/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_batch.c: the record batch (container/src/lib.rs, D2).
 */
#include "dtfl_test.h"
#include "base/batch_int.h"

/* A batch with tw = 2, dw = 4 holding "a", "", "xyz". */
static __dtfl_batch *
sample(void)
{
	__dtfl_batch *b = __dtfl_batch_new(2, 4);
	uint64_t t[2];
	int32_t d;

	t[0] = 1; t[1] = 2; d = -1;
	__dtfl_batch_push(b, "a", 1, t, &d);
	t[0] = 3; t[1] = 4; d = 5;
	__dtfl_batch_push(b, NULL, 0, t, &d);
	t[0] = 5; t[1] = 6; d = 7;
	memcpy(__dtfl_batch_push_uninit(b, 3, t, &d), "xyz", 3);
	return b;
}

static void
check_sample(const __dtfl_batch *b)
{
	const char *r;
	size_t len;
	int32_t d;

	assert_size(__dtfl_batch_len(b), ==, 3);
	r = __dtfl_batch_rec(b, 0, &len);
	assert_size(len, ==, 1);
	assert_memory_equal(1, r, "a");
	(void)__dtfl_batch_rec(b, 1, &len);
	assert_size(len, ==, 0);
	r = __dtfl_batch_rec(b, 2, &len);
	assert_size(len, ==, 3);
	assert_memory_equal(3, r, "xyz");
	assert_uint64(__dtfl_batch_time(b, 1)[0], ==, 3);
	assert_uint64(__dtfl_batch_time(b, 2)[1], ==, 6);
	memcpy(&d, __dtfl_batch_diff(b, 0), 4);
	assert_int32(d, ==, -1);
	memcpy(&d, __dtfl_batch_diff(b, 2), 4);
	assert_int32(d, ==, 7);
	assert_size(__dtfl_batch_bytes(b), ==, 4);
}

DTFL_TEST(batch_push_read)
{
	__dtfl_batch *b = sample(), *c;

	(void)params; (void)data;
	check_sample(b);
	/* push_from, including from itself. */
	c = __dtfl_batch_new(2, 4);
	__dtfl_batch_push_from(c, b, 2);
	__dtfl_batch_push_from(c, c, 0);
	assert_size(__dtfl_batch_len(c), ==, 2);
	{
		size_t len;
		const char *r = __dtfl_batch_rec(c, 1, &len);

		assert_size(len, ==, 3);
		assert_memory_equal(3, r, "xyz");
		assert_uint64(__dtfl_batch_time(c, 1)[0], ==, 5);
	}
	__dtfl_batch_clear(c);
	assert_size(__dtfl_batch_len(c), ==, 0);
	assert_size(__dtfl_batch_bytes(c), ==, 0);
	DTFL_EXPECT_PANIC(__dtfl_batch_push_from(c, b, 3));
	__dtfl_batch_release(c);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

DTFL_TEST(batch_make_mut_copy_on_write)
{
	__dtfl_batch *b = sample(), *shared, *m;

	(void)params; (void)data;
	/* Sole holder: make_mut is the identity. */
	assert_ptr_equal(__dtfl_batch_make_mut(b), b);
	shared = __dtfl_batch_retain(b);
	m = __dtfl_batch_make_mut(shared);
	assert_ptr_not_equal(m, b);
	assert_size(__dtfl_arc_count(&b->ref), ==, 1);
	__dtfl_batch_push(m, "q", 1, NULL, NULL);
	__dtfl_batch_time_mut(m, 0)[0] = 99;
	memset(__dtfl_batch_diff_mut(m, 0), 0, 4);
	/* The original is unchanged after mutation of the copy. */
	check_sample(b);
	assert_size(__dtfl_batch_len(m), ==, 4);
	assert_uint64(__dtfl_batch_time(m, 3)[0], ==, 0);
	__dtfl_batch_release(m);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

DTFL_TEST(batch_encode_decode_round_trip)
{
	__dtfl_batch *b = sample(), *d, *e;
	unsigned char *buf;
	size_t n;

	(void)params; (void)data;
	n = __dtfl_batch_encoded_len(b);
	/* 16 + 4*8 offsets + 4 arena + 6*8 times + 12 diffs = 112. */
	assert_size(n, ==, 112);
	buf = malloc(n);
	__dtfl_batch_encode(b, buf);
	assert_uint8(buf[0], ==, 3);	/* count, little-endian */
	assert_uint8(buf[8], ==, 2);	/* tw */
	assert_uint8(buf[12], ==, 4);	/* dw */
	d = __dtfl_batch_decode(buf, n);
	assert_not_null(d);
	check_sample(d);
	assert_uint(d->tw, ==, 2);
	assert_uint(d->dw, ==, 4);
	__dtfl_batch_release(d);
	free(buf);

	/* The empty batch: 16 + 8 bytes. */
	e = __dtfl_batch_new(0, 0);
	n = __dtfl_batch_encoded_len(e);
	assert_size(n, ==, 24);
	buf = malloc(n);
	__dtfl_batch_encode(e, buf);
	d = __dtfl_batch_decode(buf, n);
	assert_not_null(d);
	assert_size(__dtfl_batch_len(d), ==, 0);
	__dtfl_batch_release(d);
	__dtfl_batch_release(e);
	free(buf);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

static void
put64(unsigned char *p, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++, v >>= 8)
		p[i] = (unsigned char)v;
}

/* Decodes a mutated copy of buf and expects rejection. */
static void
expect_reject(const unsigned char *buf, size_t n, size_t at, uint64_t v,
    size_t len)
{
	unsigned char *c = malloc(n);

	memcpy(c, buf, n);
	if (at != SIZE_MAX)
		put64(c + at, v);
	assert_null(__dtfl_batch_decode(c, len));
	free(c);
}

DTFL_TEST(batch_decode_rejects_malformed)
{
	__dtfl_batch *b = sample();
	unsigned char *buf;
	size_t n;

	(void)params; (void)data;
	n = __dtfl_batch_encoded_len(b);
	buf = malloc(n);
	__dtfl_batch_encode(b, buf);
	/* Truncation, by a word and by a byte, and too short for a header. */
	expect_reject(buf, n, SIZE_MAX, 0, n - 8);
	expect_reject(buf, n, SIZE_MAX, 0, n - 1);
	expect_reject(buf, n, SIZE_MAX, 0, 15);
	assert_null(__dtfl_batch_decode(buf, 0));
	/* Counts that overflow or exceed the input. */
	expect_reject(buf, n, 0, UINT64_MAX, n);
	expect_reject(buf, n, 0, UINT64_MAX / 8, n);
	expect_reject(buf, n, 0, 4, n);
	/* tw or dw inconsistent with the remaining bytes. */
	expect_reject(buf, n, 8, (uint64_t)0xffffffff | (uint64_t)4 << 32, n);
	expect_reject(buf, n, 8, 2 | (uint64_t)0xffffffff << 32, n);
	expect_reject(buf, n, 8, 1 | (uint64_t)4 << 32, n);
	/* offs[0] != 0. */
	expect_reject(buf, n, 16, 1, n);
	/* Offsets non-monotone. */
	expect_reject(buf, n, 24, 2, n);
	/* Final offset beyond the arena (and beyond the input). */
	expect_reject(buf, n, 40, 5, n);
	expect_reject(buf, n, 40, UINT64_MAX, n);
	/* Nonzero padding, and a missing padding word's worth of slack. */
	{
		__dtfl_batch *p1 = __dtfl_batch_new(0, 0);
		unsigned char c[48];

		__dtfl_batch_push(p1, "z", 1, NULL, NULL);
		assert_size(__dtfl_batch_encoded_len(p1), ==, 40);
		__dtfl_batch_encode(p1, c);
		assert_uint8(c[39], ==, 0);
		c[39] = 1;
		assert_null(__dtfl_batch_decode(c, 40));
		c[39] = 0;
		memset(c + 40, 0, 8);
		assert_null(__dtfl_batch_decode(c, 48));
		__dtfl_batch_release(p1);
	}
	/* The unmodified buffer still decodes. */
	{
		__dtfl_batch *d = __dtfl_batch_decode(buf, n);

		assert_not_null(d);
		__dtfl_batch_release(d);
	}
	free(buf);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(batch_push_read),
	DTFL_T(batch_make_mut_copy_on_write),
	DTFL_T(batch_encode_decode_round_trip),
	DTFL_T(batch_decode_rejects_malformed),
	DTFL_T_END
};

DTFL_TEST_MAIN("batch", tests)
