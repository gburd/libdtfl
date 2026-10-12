/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_wrappers.c: the trace-wrapper batch transforms
 * (frontier.rs, enter.rs, enter_at.rs), applied eagerly to a flat batch.
 *
 * Each test builds a small val batch with known (key, val, time, diff)
 * updates, applies one transform, and reads the result cursor back.
 */
#include "dtfl_test.h"
#include "dd/wrap_int.h"
#include "progress/time_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

/* Builds a val batch of u64 keys/vals over [lower, upper) with the updates. */
static __dtfl_batch_ord *
build_batch(dtfl_time_class *tc, const uint64_t *lower_t, const uint64_t *upper_t,
    const uint64_t *keys, const uint64_t *vals, const uint64_t *times,
    const int64_t *diffs, size_t n)
{
	__dtfl_builder_ord bld;
	__dtfl_description desc;
	__dtfl_antichain lo, up, si;
	uint64_t min = 0;
	size_t i;

	__dtfl_builder_ord_init(&bld, false, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64, 0, 0, 0);
	for (i = 0; i < n; i++) {
		uint64_t kb, vb, tb;
		int64_t db = diffs[i];

		dtfl_put_u64(&kb, keys[i]);
		dtfl_put_u64(&vb, vals[i]);
		tb = times[i];
		__dtfl_builder_ord_push(&bld, &kb, 8, &vb, 8, &tb, &db);
	}
	__dtfl_antichain_init_elem(&lo, tc, lower_t);
	__dtfl_antichain_init_elem(&up, tc, upper_t);
	__dtfl_antichain_init_elem(&si, tc, &min);
	__dtfl_description_init(&desc, &lo, &up, &si);
	return __dtfl_builder_ord_done(&bld, &desc);
}

/* Collects (key, val, time0, diff) tuples from a batch cursor. */
struct tup {
	uint64_t k, v, t;
	int64_t r;
};
struct acc {
	struct tup *v;
	size_t n;
	uint64_t k, vv;
};

static void
on_time(const uint64_t *time, const void *diff, void *ctx)
{
	struct acc *a = ctx;

	a->v[a->n].k = a->k;
	a->v[a->n].v = a->vv;
	a->v[a->n].t = time[0];
	memcpy(&a->v[a->n].r, diff, 8);
	a->n++;
}

static size_t
read_all(const __dtfl_batch_ord *b, struct tup *out)
{
	__dtfl_cursor c;
	struct acc a = { out, 0, 0, 0 };

	__dtfl_cursor_init(&c, b);
	while (__dtfl_cursor_key_valid(&c, b)) {
		size_t len;

		a.k = dtfl_get_u64(__dtfl_cursor_key(&c, b, &len));
		while (__dtfl_cursor_val_valid(&c, b)) {
			a.vv = dtfl_get_u64(__dtfl_cursor_val(&c, b, &len));
			__dtfl_cursor_map_times(&c, b, on_time, &a);
			__dtfl_cursor_step_val(&c, b);
		}
		__dtfl_cursor_step_key(&c, b);
	}
	return a.n;
}

/* Records the two inner time words of each update of an entered batch. */
struct iacc {
	uint64_t w0, w1;
	size_t *seen;
};

static void
enter_on_time(const uint64_t *time, const void *diff, void *ctx)
{
	struct iacc *a = ctx;

	(void)diff;
	a->w0 = time[0];
	a->w1 = time[1];
	(*a->seen)++;
}

/*
 * frontier: advance times to since=[2] and suppress times >= until=[4].
 * The source has key 1 val 2 at times 0,1,3,5; advancing to 2 maps 0,1->2
 * (which consolidate: +1 +1 = +2) and 3->3, 5->5; until=[4] drops 5.
 */
DTFL_TEST(wrap__frontier_advance_suppress)
{
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t lower = 0, upper = 6;
	uint64_t keys[] = { 1, 1, 1, 1 };
	uint64_t vals[] = { 2, 2, 2, 2 };
	uint64_t times[] = { 0, 1, 3, 5 };
	int64_t diffs[] = { 1, 1, 1, 1 };
	uint64_t since = 2, until = 4;
	__dtfl_batch_ord *src, *dst;
	struct tup got[8];
	size_t n;

	(void)params;
	(void)data;
	src = build_batch(tc, &lower, &upper, keys, vals, times, diffs, 4);
	dst = __dtfl_batch_frontier(src, &since, 1, &until, 1);
	n = read_all(dst, got);
	/* times 0,1 -> 2 (sum +2), 3 -> 3; 5 suppressed by until. */
	assert_size(n, ==, 2);
	assert_uint64(got[0].t, ==, 2);
	assert_int64(got[0].r, ==, 2);
	assert_uint64(got[1].t, ==, 3);
	assert_int64(got[1].r, ==, 1);

	__dtfl_batch_ord_release(src);
	__dtfl_batch_ord_release(dst);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* frontier with empty until performs only the advance, suppressing nothing. */
DTFL_TEST(wrap__frontier_no_until)
{
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t lower = 0, upper = 6;
	uint64_t keys[] = { 1, 1 };
	uint64_t vals[] = { 2, 2 };
	uint64_t times[] = { 1, 5 };
	int64_t diffs[] = { 1, 1 };
	uint64_t since = 3;
	__dtfl_batch_ord *src, *dst;
	struct tup got[8];
	size_t n;

	(void)params;
	(void)data;
	src = build_batch(tc, &lower, &upper, keys, vals, times, diffs, 2);
	dst = __dtfl_batch_frontier(src, &since, 1, NULL, 0);
	n = read_all(dst, got);
	/* 1 -> 3, 5 -> 5; both kept. */
	assert_size(n, ==, 2);
	assert_uint64(got[0].t, ==, 3);
	assert_uint64(got[1].t, ==, 5);

	__dtfl_batch_ord_release(src);
	__dtfl_batch_ord_release(dst);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * enter: lift u64 times into PRODUCT(u64, u64) with to_inner, which appends
 * the inner minimum (0).  So time t becomes (t, 0): the inner coordinate.
 */
DTFL_TEST(wrap__enter_to_inner)
{
	dtfl_time_class *outer = dtfl_time_u64();
	dtfl_time_class *innerb = dtfl_time_u64();
	dtfl_time_class *inner = dtfl_time_product(outer, innerb);
	uint64_t lower = 0, upper = 3;
	uint64_t keys[] = { 1, 2 };
	uint64_t vals[] = { 7, 8 };
	uint64_t times[] = { 1, 2 };
	int64_t diffs[] = { 1, -1 };
	__dtfl_batch_ord *src, *dst;
	__dtfl_cursor c;
	size_t len, seen = 0;
	uint64_t last_inner0 = 0, last_inner1 = 0;

	(void)params;
	(void)data;
	src = build_batch(outer, &lower, &upper, keys, vals, times, diffs, 2);
	dst = __dtfl_batch_enter(src, inner);

	/* The inner class has depth 2 (u64 outer + u64 inner). */
	assert_uint(dtfl_time_depth(inner), ==, 2);

	/* Walk, recording the two inner time words of each update. */
	__dtfl_cursor_init(&c, dst);
	while (__dtfl_cursor_key_valid(&c, dst)) {
		uint64_t k = dtfl_get_u64(__dtfl_cursor_key(&c, dst, &len));

		(void)k;
		while (__dtfl_cursor_val_valid(&c, dst)) {
			struct iacc ia = { 0, 0, &seen };

			__dtfl_cursor_map_times(&c, dst, enter_on_time, &ia);
			last_inner0 = ia.w0;
			last_inner1 = ia.w1;
			__dtfl_cursor_step_val(&c, dst);
		}
		__dtfl_cursor_step_key(&c, dst);
	}
	assert_size(seen, ==, 2);
	/* Last update was key 2 val 8 at outer time 2 -> inner (2, 0). */
	assert_uint64(last_inner0, ==, 2);
	assert_uint64(last_inner1, ==, 0);

	__dtfl_batch_ord_release(src);
	__dtfl_batch_ord_release(dst);
	dtfl_time_class_free(inner);
	dtfl_time_class_free(innerb);
	dtfl_time_class_free(outer);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(wrap__frontier_advance_suppress),
	DTFL_T(wrap__frontier_no_until),
	DTFL_T(wrap__enter_to_inner),
	DTFL_T_END
};

DTFL_TEST_MAIN("wrappers", tests)
