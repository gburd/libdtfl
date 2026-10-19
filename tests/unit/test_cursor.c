/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_cursor.c: the single-batch cursor and the merge-of-cursors
 * list, driven directly over ord_neu storage built by the Builder.  Builds a
 * batch of (key, val, time, diff) updates and reads it back, asserting the
 * key/val/time/diff order, the singleton update encoding, and that a
 * CursorList over two batches presents their union in (key, val) order with
 * map_times firing across both.
 */
#include "dtfl_test.h"
#include "dd/trace/trace_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

/* A (key u64, val u64, time u64, i64) update. */
typedef struct kv {
	uint64_t k, v, t;
	int64_t r;
} kv;

/* A chunk of key||val records (both u64), one time word, i64 diffs. */
static __dtfl_batch *
chunk_of(const kv *u, size_t n)
{
	__dtfl_batch *b = __dtfl_batch_new(1, 8);
	unsigned char rec[16];
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_put_u64(rec, u[i].k);
		dtfl_put_u64(rec + 8, u[i].v);
		__dtfl_batch_push(b, rec, 16, &u[i].t, &u[i].r);
	}
	return b;
}

static __dtfl_batch_ord *
build_val_batch(const kv *u, size_t n, dtfl_time_class *tc)
{
	const dtfl_type *kt = dtfl_type_u64(), *vt = dtfl_type_u64();
	__dtfl_batch *chunk = chunk_of(u, n);
	__dtfl_antichain lower, upper, since;
	__dtfl_description desc;
	__dtfl_batch_ord *b;
	uint64_t zero = 0, big = 1000;

	__dtfl_antichain_init_elem(&lower, tc, &zero);
	__dtfl_antichain_init_elem(&upper, tc, &big);
	__dtfl_antichain_init_elem(&since, tc, &zero);
	__dtfl_description_init(&desc, &lower, &upper, &since);
	b = __dtfl_builder_ord_seal(false, kt, vt, tc, I64, &chunk, 1, &desc);
	__dtfl_batch_release(chunk);
	return b;
}

/* Accumulates (time, diff) into a running sum of diffs. */
struct acc {
	int64_t sum;
	uint64_t last_t;
	size_t ntimes;
	uint64_t times[16];
	int64_t diffs[16];
};

static void
collect(const uint64_t *time, const void *diff, void *ctx)
{
	struct acc *a = ctx;
	int64_t r;

	memcpy(&r, diff, 8);
	a->sum += r;
	if (a->ntimes < 16) {
		a->times[a->ntimes] = time[0];
		a->diffs[a->ntimes] = r;
		a->ntimes++;
	}
}

DTFL_TEST(cursor__single_batch_order_and_consolidation)
{
	/*
	 * Updates for key 1: val 2 at times {0:+1, 3:+2}; val 5 at time 1:+1.
	 * Key 2: val 3 at time 1:+1, then 1:-1 (cancels), time 2:+1.
	 * Fed sorted by (key, val, time); the builder consolidates per value.
	 */
	static const kv u[] = {
		{ 1, 2, 0, 1 }, { 1, 2, 3, 2 },
		{ 1, 5, 1, 1 },
		{ 2, 3, 1, 1 }, { 2, 3, 1, -1 }, { 2, 3, 2, 1 },
	};
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_batch_ord *b = build_val_batch(u, 6, tc);
	__dtfl_cursor c;
	size_t len;
	struct acc a;

	(void)params; (void)data;
	__dtfl_cursor_init(&c, b);

	/* key 1 */
	assert_true(__dtfl_cursor_key_valid(&c, b));
	assert_uint64(dtfl_get_u64(__dtfl_cursor_key(&c, b, &len)), ==, 1);
	/*   val 2: times (0,+1), (3,+2) */
	assert_true(__dtfl_cursor_val_valid(&c, b));
	assert_uint64(dtfl_get_u64(__dtfl_cursor_val(&c, b, &len)), ==, 2);
	memset(&a, 0, sizeof a);
	__dtfl_cursor_map_times(&c, b, collect, &a);
	assert_size(a.ntimes, ==, 2);
	assert_uint64(a.times[0], ==, 0);
	assert_int64(a.diffs[0], ==, 1);
	assert_uint64(a.times[1], ==, 3);
	assert_int64(a.diffs[1], ==, 2);
	__dtfl_cursor_step_val(&c, b);
	/*   val 5: time (1,+1) */
	assert_true(__dtfl_cursor_val_valid(&c, b));
	assert_uint64(dtfl_get_u64(__dtfl_cursor_val(&c, b, &len)), ==, 5);
	memset(&a, 0, sizeof a);
	__dtfl_cursor_map_times(&c, b, collect, &a);
	assert_size(a.ntimes, ==, 1);
	assert_int64(a.sum, ==, 1);
	__dtfl_cursor_step_val(&c, b);
	assert_false(__dtfl_cursor_val_valid(&c, b));

	/* key 2 */
	__dtfl_cursor_step_key(&c, b);
	assert_true(__dtfl_cursor_key_valid(&c, b));
	assert_uint64(dtfl_get_u64(__dtfl_cursor_key(&c, b, &len)), ==, 2);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_val(&c, b, &len)), ==, 3);
	memset(&a, 0, sizeof a);
	__dtfl_cursor_map_times(&c, b, collect, &a);
	/* 1:+1 and 1:-1 cancel; only 2:+1 survives. */
	assert_size(a.ntimes, ==, 1);
	assert_uint64(a.times[0], ==, 2);
	assert_int64(a.diffs[0], ==, 1);

	__dtfl_cursor_step_key(&c, b);
	assert_false(__dtfl_cursor_key_valid(&c, b));

	/* seek_key jumps to a key, rewind returns to the first. */
	__dtfl_cursor_rewind_keys(&c, b);
	{
		unsigned char kbuf[8];

		dtfl_put_u64(kbuf, 2);
		__dtfl_cursor_seek_key(&c, b, kbuf, 8);
		assert_uint64(dtfl_get_u64(__dtfl_cursor_key(&c, b, &len)),
		    ==, 2);
	}

	__dtfl_batch_ord_release(b);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(cursor__list_merges_two_batches)
{
	static const kv a1[] = { { 1, 2, 0, 1 }, { 3, 4, 1, 1 } };
	static const kv a2[] = { { 1, 2, 2, 1 }, { 2, 9, 0, 1 } };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_batch_ord *batches[2];
	__dtfl_cursor_list cl;
	size_t len;
	struct acc a;

	(void)params; (void)data;
	batches[0] = build_val_batch(a1, 2, tc);
	batches[1] = build_val_batch(a2, 2, tc);
	__dtfl_cursor_list_init(&cl, batches, 2, dtfl_type_u64(),
	    dtfl_type_u64());

	/* key 1, val 2: both batches contribute (times 0 and 2). */
	assert_true(__dtfl_cursor_list_key_valid(&cl));
	assert_uint64(dtfl_get_u64(__dtfl_cursor_list_key(&cl, &len)), ==, 1);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_list_val(&cl, &len)), ==, 2);
	memset(&a, 0, sizeof a);
	__dtfl_cursor_list_map_times(&cl, collect, &a);
	assert_size(a.ntimes, ==, 2);
	assert_int64(a.sum, ==, 2);
	__dtfl_cursor_list_step_key(&cl);

	/* key 2 (only batch 2). */
	assert_uint64(dtfl_get_u64(__dtfl_cursor_list_key(&cl, &len)), ==, 2);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_list_val(&cl, &len)), ==, 9);
	__dtfl_cursor_list_step_key(&cl);

	/* key 3 (only batch 1). */
	assert_uint64(dtfl_get_u64(__dtfl_cursor_list_key(&cl, &len)), ==, 3);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_list_val(&cl, &len)), ==, 4);
	__dtfl_cursor_list_step_key(&cl);
	assert_false(__dtfl_cursor_list_key_valid(&cl));

	__dtfl_cursor_list_free(&cl);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * A cursor list over key batches of 8-byte memcmp-ordered keys takes the
 * all8 minimize path.  A key type with a user cmp of the same order takes
 * the general one, so the same random batches walked by the same mix of
 * step_key and seek_key must visit the same keys with the same min-key
 * cursors and the same updates.  Each batch is cut on a distinct lower
 * and upper, as a spine's are.  Both lists are of key batches, so both
 * take the key-batch minimize_vals; its result is checked directly.
 */
static int
user_memcmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	size_t m = alen < blen ? alen : blen;
	int c = m != 0 ? memcmp(a, b, m) : 0;

	(void)t;
	if (c != 0)
		return c;
	return (alen > blen) - (alen < blen);
}

static __dtfl_batch_ord *
build_key_batch(const dtfl_type *kt, uint64_t *rng, size_t n, uint64_t lo,
    dtfl_time_class *tc)
{
	__dtfl_batch *chunk = __dtfl_batch_new(1, 8);
	__dtfl_antichain lower, upper, since;
	__dtfl_description desc;
	__dtfl_batch_ord *b;
	uint64_t hi = lo + 1, zero = 0;
	unsigned char rec[8];
	size_t i;

	for (i = 0; i < n; i++) {
		int64_t r;
		uint64_t t;

		*rng ^= *rng << 13; *rng ^= *rng >> 7; *rng ^= *rng << 17;
		dtfl_put_u64(rec, *rng % 40);
		t = lo;
		r = (int64_t)((*rng >> 8) % 3) + 1;
		__dtfl_batch_push(chunk, rec, 8, &t, &r);
	}
	__dtfl_consolidate_batch(chunk, dtfl_type_u64(), I64);
	__dtfl_antichain_init_elem(&lower, tc, &lo);
	__dtfl_antichain_init_elem(&upper, tc, &hi);
	__dtfl_antichain_init_elem(&since, tc, &zero);
	__dtfl_description_init(&desc, &lower, &upper, &since);
	b = __dtfl_builder_ord_seal(true, kt, dtfl_type_unit(), tc, I64,
	    &chunk, 1, &desc);
	__dtfl_batch_release(chunk);
	return b;
}

DTFL_TEST(cursor__list_all8_matches_general)
{
	dtfl_type slow = *dtfl_type_u64();
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t rng = 0x9e3779b97f4a7c15ULL;
	int round;

	(void)params; (void)data;
	slow.cmp = user_memcmp;
	for (round = 0; round < 200; round++) {
		__dtfl_batch_ord *fb[6], *gb[6];
		__dtfl_cursor_list fl, gl;
		size_t nb = 1 + (size_t)round % 6, i;
		uint64_t walk;

		for (i = 0; i < nb; i++) {
			uint64_t save = rng;
			size_t n = (size_t)(rng % 30);

			fb[i] = build_key_batch(dtfl_type_u64(), &rng, n, i, tc);
			rng = save;
			gb[i] = build_key_batch(&slow, &rng, n, i, tc);
		}
		__dtfl_cursor_list_init(&fl, fb, nb, dtfl_type_u64(),
		    dtfl_type_unit());
		__dtfl_cursor_list_init(&gl, gb, nb, &slow, dtfl_type_unit());
		walk = rng;
		for (;;) {
			struct acc fa, ga;
			size_t fl_len, gl_len;

			assert_int(__dtfl_cursor_list_key_valid(&fl), ==,
			    __dtfl_cursor_list_key_valid(&gl));
			if (!__dtfl_cursor_list_key_valid(&fl))
				break;
			assert_memory_equal(8, __dtfl_cursor_list_key(&fl, &fl_len),
			    __dtfl_cursor_list_key(&gl, &gl_len));
			assert_size(fl.min_key.len, ==, gl.min_key.len);
			assert_memory_equal(fl.min_key.len * sizeof(size_t),
			    fl.min_key.data, gl.min_key.data);
			assert_size(fl.min_val.len, ==, gl.min_val.len);
			assert_memory_equal(fl.min_val.len * sizeof(size_t),
			    fl.min_val.data, gl.min_val.data);
			memset(&fa, 0, sizeof fa);
			memset(&ga, 0, sizeof ga);
			__dtfl_cursor_list_map_times(&fl, collect, &fa);
			__dtfl_cursor_list_map_times(&gl, collect, &ga);
			assert_size(fa.ntimes, ==, ga.ntimes);
			assert_int64(fa.sum, ==, ga.sum);
			walk ^= walk << 13; walk ^= walk >> 7; walk ^= walk << 17;
			if (walk % 3 == 0) {
				unsigned char kb[8];
				uint64_t k = dtfl_get_u64(
				    __dtfl_cursor_list_key(&fl, &fl_len)) +
				    1 + (walk >> 8) % 5;

				dtfl_put_u64(kb, k);
				__dtfl_cursor_list_seek_key(&fl, kb, 8);
				__dtfl_cursor_list_seek_key(&gl, kb, 8);
			} else if (walk % 3 == 1) {
				/*
				 * A key batch has one (unit) value per key: once
				 * stepped none is valid, and a rewind makes every
				 * min-key cursor's valid again.
				 */
				__dtfl_cursor_list_step_val(&fl);
				__dtfl_cursor_list_step_val(&gl);
				assert_false(__dtfl_cursor_list_val_valid(&fl));
				assert_false(__dtfl_cursor_list_val_valid(&gl));
				__dtfl_cursor_list_rewind_vals(&fl);
				__dtfl_cursor_list_rewind_vals(&gl);
				assert_size(fl.min_val.len, ==, fl.min_key.len);
				assert_size(gl.min_val.len, ==, gl.min_key.len);
				__dtfl_cursor_list_step_val(&fl);
				__dtfl_cursor_list_step_val(&gl);
				__dtfl_cursor_list_step_key(&fl);
				__dtfl_cursor_list_step_key(&gl);
			} else {
				__dtfl_cursor_list_step_key(&fl);
				__dtfl_cursor_list_step_key(&gl);
			}
		}
		__dtfl_cursor_list_free(&fl);
		__dtfl_cursor_list_free(&gl);
	}
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(cursor__single_batch_order_and_consolidation),
	DTFL_T(cursor__list_merges_two_batches),
	DTFL_T(cursor__list_all8_matches_general),
	DTFL_T_END
};

DTFL_TEST_MAIN("cursor", tests)
