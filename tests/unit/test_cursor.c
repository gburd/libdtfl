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

static MunitTest tests[] = {
	DTFL_T(cursor__single_batch_order_and_consolidation),
	DTFL_T(cursor__list_merges_two_batches),
	DTFL_T_END
};

DTFL_TEST_MAIN("cursor", tests)
