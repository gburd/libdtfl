/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_bytes.c: __dtfl_bytes (bytes/src/lib.rs with #821).
 * bytes__arc_doc_example and bytes__try_regenerate_doc_example port the
 * upstream doc tests; the rest cover the aliasing rules #821 hardens.
 */
#include "dtfl_test.h"
#include "base/batch_int.h"

/* Upstream crate doc: split [4, 2, 3, 1], write slab 1, merge back. */
DTFL_TEST(bytes__arc_doc_example)
{
	__dtfl_bytes s1, s2, s3, s4;
	size_t i;

	(void)params; (void)data;
	__dtfl_bytes_new(&s1, 1024);
	memset(s1.ptr, 0, s1.len);
	__dtfl_bytes_extract_to(&s1, 100, &s2);
	__dtfl_bytes_extract_to(&s1, 100, &s3);
	__dtfl_bytes_extract_to(&s2, 60, &s4);
	assert_size(s1.len, ==, 824);
	assert_size(s2.len, ==, 40);
	assert_size(s3.len, ==, 100);
	assert_size(s4.len, ==, 60);
	assert_true(s1.writable);
	assert_false(s2.writable);
	for (i = 0; i < s1.len; i++)
		s1.ptr[i] = 1;
	__dtfl_bytes_freeze(&s1);
	assert_true(__dtfl_bytes_try_merge(&s2, &s3));
	assert_null(s3.store);
	assert_true(__dtfl_bytes_try_merge(&s2, &s1));
	assert_true(__dtfl_bytes_try_merge(&s4, &s2));
	assert_size(s4.len, ==, 1024);
	assert_uint8(s4.ptr[0], ==, 0);
	assert_uint8(s4.ptr[1023], ==, 1);
	assert_size(__dtfl_arc_count(&s4.store->ref), ==, 1);
	__dtfl_bytes_release(&s4);
	return MUNIT_OK;
}

/* try_regenerate doc example: after the other views drop, recover 1024. */
DTFL_TEST(bytes__try_regenerate_doc_example)
{
	__dtfl_bytes s1, s2, s3, s4;

	(void)params; (void)data;
	__dtfl_bytes_new(&s1, 1024);
	__dtfl_bytes_extract_to(&s1, 100, &s2);
	__dtfl_bytes_extract_to(&s1, 100, &s3);
	__dtfl_bytes_extract_to(&s2, 60, &s4);
	__dtfl_bytes_release(&s3);
	__dtfl_bytes_release(&s2);
	__dtfl_bytes_release(&s4);
	assert_true(__dtfl_bytes_try_regenerate(&s1));
	assert_size(s1.len, ==, 1024);
	assert_ptr_equal(s1.ptr, s1.store->data);
	__dtfl_bytes_release(&s1);
	return MUNIT_OK;
}

/* #821: regeneration while another view lives would alias it. */
DTFL_TEST(bytes_regenerate_refused_while_shared)
{
	__dtfl_bytes w, r, c;
	unsigned char *ptr;

	(void)params; (void)data;
	__dtfl_bytes_new(&w, 64);
	__dtfl_bytes_extract_to(&w, 16, &r);
	ptr = w.ptr;
	assert_false(__dtfl_bytes_try_regenerate(&w));
	/* Refusal leaves the view unchanged. */
	assert_ptr_equal(w.ptr, ptr);
	assert_size(w.len, ==, 48);
	/* A clone of the read view also blocks regeneration. */
	__dtfl_bytes_clone(&r, &c);
	__dtfl_bytes_release(&r);
	assert_false(__dtfl_bytes_try_regenerate(&w));
	__dtfl_bytes_release(&c);
	assert_true(__dtfl_bytes_try_regenerate(&w));
	assert_size(w.len, ==, 64);
	__dtfl_bytes_release(&w);
	return MUNIT_OK;
}

/* Writable views never alias: no clone, no regenerating a read view. */
DTFL_TEST(bytes_writable_views_do_not_alias)
{
	__dtfl_bytes w, r, x;

	(void)params; (void)data;
	__dtfl_bytes_new(&w, 32);
	DTFL_EXPECT_PANIC(__dtfl_bytes_clone(&w, &x));
	__dtfl_bytes_extract_to(&w, 8, &r);
	DTFL_EXPECT_PANIC((void)__dtfl_bytes_try_regenerate(&r));
	/* Merging a read view into a writable one yields a read view. */
	assert_true(__dtfl_bytes_try_merge(&r, &w));
	assert_false(r.writable);
	assert_size(r.len, ==, 32);
	DTFL_EXPECT_PANIC(__dtfl_bytes_extract_to(&r, 33, &x));
	__dtfl_bytes_release(&r);
	return MUNIT_OK;
}

DTFL_TEST(bytes_merge_refused)
{
	__dtfl_bytes a, b, c, other, o2;

	(void)params; (void)data;
	__dtfl_bytes_new(&a, 30);
	__dtfl_bytes_freeze(&a);
	__dtfl_bytes_extract_to(&a, 10, &b);	/* b = [0,10), a = [10,30) */
	__dtfl_bytes_extract_to(&a, 10, &c);	/* c = [10,20), a = [20,30) */
	/* Not adjacent: b ends at 10, a starts at 20. */
	assert_false(__dtfl_bytes_try_merge(&b, &a));
	assert_size(b.len, ==, 10);
	assert_size(a.len, ==, 10);
	/* Wrong order: a ends at 30, b starts at 0. */
	assert_false(__dtfl_bytes_try_merge(&a, &b));
	/* Self-merge is refused. */
	assert_false(__dtfl_bytes_try_merge(&b, &b));
	/* Different stores, even if the addresses happened to touch. */
	__dtfl_bytes_new(&other, 10);
	__dtfl_bytes_freeze(&other);
	assert_false(__dtfl_bytes_try_merge(&b, &other));
	__dtfl_bytes_clone(&other, &o2);
	assert_true(o2.store == other.store);
	/* Overlapping clones of one range are not adjacent. */
	assert_false(__dtfl_bytes_try_merge(&other, &o2));
	assert_true(__dtfl_bytes_try_merge(&b, &c));
	assert_true(__dtfl_bytes_try_merge(&b, &a));
	assert_size(b.len, ==, 30);
	__dtfl_bytes_release(&b);
	__dtfl_bytes_release(&other);
	__dtfl_bytes_release(&o2);
	return MUNIT_OK;
}

static int released;

static void
note_release(unsigned char *p, size_t cap, void *ctx)
{
	assert_ptr_equal(p, ctx);
	assert_size(cap, ==, 16);
	released++;
}

DTFL_TEST(bytes_wrap_releases_once)
{
	unsigned char mem[16];
	__dtfl_bytes w, r;

	(void)params; (void)data;
	__dtfl_bytes_wrap(&w, mem, sizeof mem, note_release, mem);
	__dtfl_bytes_extract_to(&w, 4, &r);
	__dtfl_bytes_release(&w);
	assert_int(released, ==, 0);
	__dtfl_bytes_release(&r);
	assert_int(released, ==, 1);
	/* Releasing a released view is a no-op; released views never merge. */
	__dtfl_bytes_release(&r);
	assert_int(released, ==, 1);
	assert_false(__dtfl_bytes_try_merge(&w, &r));
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(bytes__arc_doc_example),
	DTFL_T(bytes__try_regenerate_doc_example),
	DTFL_T(bytes_regenerate_refused_while_shared),
	DTFL_T(bytes_writable_views_do_not_alias),
	DTFL_T(bytes_merge_refused),
	DTFL_T(bytes_wrap_releases_once),
	DTFL_T_END
};

DTFL_TEST_MAIN("bytes", tests)
