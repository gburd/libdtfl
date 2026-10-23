/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_batch.c: the record batch (container/src/lib.rs, D2).
 */
#include "dtfl_test.h"
#include "base/batch_int.h"

#include <pthread.h>

/* Allocation hooks that count calls, for the one-block batch tests. */
static size_t n_malloc, n_realloc, n_free;

static void *
count_malloc(size_t n, void *ctx)
{
	(void)ctx;
	n_malloc++;
	return malloc(n);
}

static void *
count_realloc(void *p, size_t n, void *ctx)
{
	(void)ctx;
	n_realloc++;
	return realloc(p, n);
}

static void
count_free(void *p, void *ctx)
{
	(void)ctx;
	n_free++;
	free(p);
}

static void
count_hooks(void)
{
	dtfl_alloc_hooks h = { count_malloc, count_realloc, count_free, NULL };

	assert_int(dtfl_set_alloc_hooks(&h), ==, DTFL_OK);
	n_malloc = n_realloc = n_free = 0;
}

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

/*
 * drain_front keeps the records after k, in order, with every column
 * moved to the front and the storage kept.
 */
DTFL_TEST(batch_drain_front_keeps_tail)
{
	__dtfl_batch *b = sample(), *c;
	static const size_t bytes[4] = { 4, 3, 3, 0 };
	const unsigned char *arena;
	size_t k, len;

	(void)params; (void)data;
	for (k = 0; k <= 3; k++) {
		c = __dtfl_batch_copy(b);
		arena = c->arena.data;
		__dtfl_batch_drain_front(c, k);
		assert_size(__dtfl_batch_len(c), ==, 3 - k);
		assert_size(c->times.len, ==, (3 - k) * 2);
		assert_size(c->diffs.len, ==, (3 - k) * 4);
		assert_ptr_equal(c->arena.data, arena);
		assert_size(((const size_t *)(void *)c->offs.data)[0], ==, 0);
		if (k <= 2) {
			const void *r = __dtfl_batch_rec(c, 2 - k, &len);

			assert_size(len, ==, 3);
			assert_memory_equal(3, r, "xyz");
			assert_uint64(__dtfl_batch_time(c, 2 - k)[1], ==, 6);
		}
		if (k <= 1) {
			(void)__dtfl_batch_rec(c, 1 - k, &len);
			assert_size(len, ==, 0);
			assert_uint64(__dtfl_batch_time(c, 1 - k)[0], ==, 3);
		}
		assert_size(__dtfl_batch_bytes(c), ==, bytes[k]);
		__dtfl_batch_release(c);
	}
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

/* Pushes rows 0..n-1 of the form ("r" + i as 8 bytes, t = i, d = -i). */
static void
fill(__dtfl_batch *b, size_t from, size_t n)
{
	size_t i;

	for (i = from; i < from + n; i++) {
		uint64_t rec = i * 7, t[2] = { i, i + 1 };
		int32_t d = -(int32_t)i;

		__dtfl_batch_push(b, &rec, 8, t, &d);
	}
}

static void
check_fill(const __dtfl_batch *b, size_t n)
{
	size_t i, len;

	assert_size(__dtfl_batch_len(b), ==, n);
	assert_size(b->arena.len, ==, 8 * n);
	assert_size(b->times.len, ==, 2 * n);
	assert_size(b->diffs.len, ==, 4 * n);
	for (i = 0; i < n; i++) {
		uint64_t rec;
		int32_t d;

		memcpy(&rec, __dtfl_batch_rec(b, i, &len), 8);
		assert_size(len, ==, 8);
		assert_uint64(rec, ==, i * 7);
		assert_uint64(__dtfl_batch_time(b, i)[0], ==, i);
		assert_uint64(__dtfl_batch_time(b, i)[1], ==, i + 1);
		memcpy(&d, __dtfl_batch_diff(b, i), 4);
		assert_int32(d, ==, -(int32_t)i);
	}
}

/*
 * A reserved batch is one allocation and one free, and filling it within
 * its reservation allocates nothing more.
 */
DTFL_TEST(batch_reserved_is_one_block)
{
	__dtfl_batch *b;

	(void)params; (void)data;
	count_hooks();
	b = __dtfl_batch_new_reserved(2, 4, 64, 8 * 64);
	fill(b, 0, 64);
	assert_size(n_malloc, ==, 1);
	assert_size(n_realloc, ==, 0);
	check_fill(b, 64);
	__dtfl_batch_release(b);
	assert_size(n_free, ==, 1);
	/* An unsized batch is one block too, until it is pushed into. */
	b = __dtfl_batch_new(0, 0);
	__dtfl_batch_release(b);
	assert_size(n_malloc, ==, 2);
	assert_size(n_free, ==, 2);
	return MUNIT_OK;
}

/*
 * Pushing past the reservation moves each overflowing column out of the
 * block once; the rows read back unchanged, and release frees the block
 * and every moved column, nothing twice (ASan checks the frees).
 */
DTFL_TEST(batch_reserved_grows_past_block)
{
	__dtfl_batch *b;
	const unsigned char *block_arena;

	(void)params; (void)data;
	count_hooks();
	b = __dtfl_batch_new_reserved(2, 4, 8, 8 * 8);
	fill(b, 0, 8);
	block_arena = b->arena.data;
	assert_true(b->arena.borrowed && b->offs.borrowed);
	fill(b, 8, 1000);
	assert_ptr_not_equal(b->arena.data, block_arena);
	assert_false(b->arena.borrowed || b->offs.borrowed ||
	    b->times.borrowed || b->diffs.borrowed);
	check_fill(b, 1008);
	/* Only the one malloc per column that left the block, then reallocs. */
	assert_size(n_malloc, ==, 5);
	__dtfl_batch_release(b);
	assert_size(n_free, ==, 5);
	/* Clear keeps the grown storage, and reuse needs no allocation. */
	b = __dtfl_batch_new_reserved(2, 4, 4, 32);
	fill(b, 0, 100);
	__dtfl_batch_clear(b);
	n_malloc = n_realloc = 0;
	fill(b, 0, 100);
	assert_size(n_malloc + n_realloc, ==, 0);
	check_fill(b, 100);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

/*
 * A batch recycled after clear comes back with zero lengths in every
 * column, whether its columns are in the block or moved out.
 */
DTFL_TEST(batch_reserved_clear_resets_lengths)
{
	__dtfl_batch *b = __dtfl_batch_new_reserved(2, 4, 16, 128);

	(void)params; (void)data;
	fill(b, 0, 16);
	__dtfl_batch_clear(b);
	assert_size(__dtfl_batch_len(b), ==, 0);
	assert_size(b->arena.len, ==, 0);
	assert_size(b->times.len, ==, 0);
	assert_size(b->diffs.len, ==, 0);
	fill(b, 0, 3);
	check_fill(b, 3);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

static void *
release_on_thread(void *arg)
{
	__dtfl_batch_release(arg);
	return NULL;
}

/*
 * A one-block batch shared by two holders: the other thread's release
 * drops a reference, the last release (on whichever thread) frees the
 * block through the hooks once.  Run both orders.
 */
DTFL_TEST(batch_reserved_shared_release_other_thread)
{
	int order;

	(void)params; (void)data;
	count_hooks();
	for (order = 0; order < 2; order++) {
		__dtfl_batch *b = __dtfl_batch_new_reserved(2, 4, 32, 256);
		pthread_t th;

		fill(b, 0, 32);
		(void)__dtfl_batch_retain(b);
		if (order == 0) {
			assert_int(pthread_create(&th, NULL, release_on_thread,
			    b), ==, 0);
			assert_int(pthread_join(th, NULL), ==, 0);
			assert_size(__dtfl_arc_count(&b->ref), ==, 1);
			check_fill(b, 32);
			__dtfl_batch_release(b);
		} else {
			__dtfl_batch_release(b);
			check_fill(b, 32);
			assert_int(pthread_create(&th, NULL, release_on_thread,
			    b), ==, 0);
			assert_int(pthread_join(th, NULL), ==, 0);
		}
	}
	assert_size(n_malloc, ==, 2);
	assert_size(n_free, ==, 2);
	return MUNIT_OK;
}

/*
 * move_rows between batches whose columns are in their blocks or moved
 * out: dst gets src's rows, src is left empty, and neither points into
 * the other's block (each is then freed and refilled independently).
 */
DTFL_TEST(batch_move_rows_keeps_blocks_apart)
{
	int kind;

	(void)params; (void)data;
	for (kind = 0; kind < 4; kind++) {
		/* kind bit 0: dst grown out of its block; bit 1: src grown. */
		__dtfl_batch *dst = __dtfl_batch_new_reserved(2, 4, 8, 64);
		__dtfl_batch *src = __dtfl_batch_new_reserved(2, 4, 8, 64);

		fill(dst, 0, (kind & 1) ? 100 : 3);
		fill(src, 0, (kind & 2) ? 50 : 5);
		__dtfl_batch_move_rows(dst, src);
		check_fill(dst, (kind & 2) ? 50 : 5);
		assert_size(__dtfl_batch_len(src), ==, 0);
		assert_size(src->arena.len, ==, 0);
		assert_size(src->times.len, ==, 0);
		assert_size(src->diffs.len, ==, 0);
		/* src's spare storage is its own: refill it, free dst first. */
		fill(src, 0, 20);
		__dtfl_batch_release(dst);
		check_fill(src, 20);
		__dtfl_batch_release(src);
	}
	return MUNIT_OK;
}

/* A copy is one block holding the same rows. */
DTFL_TEST(batch_copy_is_one_block)
{
	__dtfl_batch *b, *c;

	(void)params; (void)data;
	count_hooks();	/* before the first allocation: the hooks latch */
	b = __dtfl_batch_new_reserved(2, 4, 4, 32);
	fill(b, 0, 40);
	n_malloc = n_realloc = n_free = 0;
	c = __dtfl_batch_copy(b);
	assert_size(n_malloc + n_realloc, ==, 1);
	check_fill(c, 40);
	__dtfl_batch_release(c);
	assert_size(n_free, ==, 1);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(batch_push_read),
	DTFL_T(batch_drain_front_keeps_tail),
	DTFL_T(batch_make_mut_copy_on_write),
	DTFL_T(batch_encode_decode_round_trip),
	DTFL_T(batch_decode_rejects_malformed),
	DTFL_T(batch_reserved_is_one_block),
	DTFL_T(batch_reserved_grows_past_block),
	DTFL_T(batch_reserved_clear_resets_lengths),
	DTFL_T(batch_reserved_shared_release_other_thread),
	DTFL_T(batch_move_rows_keeps_blocks_apart),
	DTFL_T(batch_copy_is_one_block),
	DTFL_T_END
};

DTFL_TEST_MAIN("batch", tests)
