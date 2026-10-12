/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ord_neu.c: the ord_neu Builder and Merger over the layered
 * trie.  Builds two adjacent batches and merges them, then reads the merged
 * batch through a cursor to assert that equal (key, val) updates are summed,
 * cancelling updates vanish, keys with no surviving updates are dropped, and
 * copied times are advanced to the merged since.  Also exercises the key
 * batch (OrdKeyBatch), whose single value per key is the unit value.
 */
#include "dtfl_test.h"
#include "dd/trace/trace_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

typedef struct kv {
	uint64_t k, v, t;
	int64_t r;
} kv;

static __dtfl_batch *
chunk_of(const kv *u, size_t n, bool keyed)
{
	__dtfl_batch *b = __dtfl_batch_new(1, 8);
	unsigned char rec[16];
	size_t i, len;

	for (i = 0; i < n; i++) {
		dtfl_put_u64(rec, u[i].k);
		if (keyed) {
			len = 8;
		} else {
			dtfl_put_u64(rec + 8, u[i].v);
			len = 16;
		}
		__dtfl_batch_push(b, rec, len, &u[i].t, &u[i].r);
	}
	return b;
}

static __dtfl_batch_ord *
build(const kv *u, size_t n, bool keyed, uint64_t lo, uint64_t hi,
    uint64_t since_t, dtfl_time_class *tc)
{
	__dtfl_batch *chunk = chunk_of(u, n, keyed);
	__dtfl_antichain lower, upper, since;
	__dtfl_description desc;
	__dtfl_batch_ord *b;

	__dtfl_antichain_init_elem(&lower, tc, &lo);
	__dtfl_antichain_init_elem(&upper, tc, &hi);
	__dtfl_antichain_init_elem(&since, tc, &since_t);
	__dtfl_description_init(&desc, &lower, &upper, &since);
	b = __dtfl_builder_ord_seal(keyed, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64, &chunk, 1, &desc);
	__dtfl_batch_release(chunk);
	return b;
}

struct acc {
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
	if (a->ntimes < 16) {
		a->times[a->ntimes] = time[0];
		a->diffs[a->ntimes] = r;
		a->ntimes++;
	}
}

DTFL_TEST(ord_neu__merge_consolidates_and_advances)
{
	/* Batch 1: [0, 5); key 1 val 2 at 0:+1, key 3 val 3 at 1:+1. */
	static const kv b1[] = { { 1, 2, 0, 1 }, { 3, 3, 1, 1 } };
	/* Batch 2: [5, 10); key 1 val 2 at 6:+1, key 3 val 3 at 7:-1. */
	static const kv b2[] = { { 1, 2, 6, 1 }, { 3, 3, 7, -1 } };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_batch_ord *x = build(b1, 2, false, 0, 5, 0, tc);
	__dtfl_batch_ord *y = build(b2, 2, false, 5, 10, 0, tc);
	__dtfl_antichain compaction;
	__dtfl_merger_ord m;
	int64_t fuel = 1000000;
	__dtfl_batch_ord *merged;
	__dtfl_cursor c;
	size_t len;
	struct acc a;
	uint64_t since8 = 8;

	(void)params; (void)data;
	/* Compact to 8: times <= 8 round up to 8. */
	__dtfl_antichain_init_elem(&compaction, tc, &since8);
	__dtfl_merger_ord_new(&m, x, y, &compaction);
	__dtfl_merger_ord_work(&m, &fuel);
	assert_true(fuel > 0);
	merged = __dtfl_merger_ord_done(&m);

	__dtfl_cursor_init(&c, merged);
	/* key 1 val 2: 0 and 6 both advance to 8 and sum to +2. */
	assert_uint64(dtfl_get_u64(__dtfl_cursor_key(&c, merged, &len)), ==, 1);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_val(&c, merged, &len)), ==, 2);
	memset(&a, 0, sizeof a);
	__dtfl_cursor_map_times(&c, merged, collect, &a);
	assert_size(a.ntimes, ==, 1);
	assert_uint64(a.times[0], ==, 8);
	assert_int64(a.diffs[0], ==, 2);
	__dtfl_cursor_step_key(&c, merged);
	/* key 3 val 3: 1:+1 and 7:-1 both advance to 8 and cancel; key gone. */
	assert_false(__dtfl_cursor_key_valid(&c, merged));

	__dtfl_batch_ord_release(merged);
	__dtfl_antichain_free(&compaction);
	__dtfl_batch_ord_release(x);
	__dtfl_batch_ord_release(y);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(ord_neu__key_batch_single_value)
{
	/* Key batch: keys 1, 2 (value-less); key 1 at 0:+1, key 2 at 0:+1. */
	static const kv u[] = { { 1, 0, 0, 1 }, { 2, 0, 0, 1 } };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_batch_ord *b = build(u, 2, true, 0, 5, 0, tc);
	__dtfl_cursor c;
	size_t len;
	struct acc a;

	(void)params; (void)data;
	__dtfl_cursor_init(&c, b);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_key(&c, b, &len)), ==, 1);
	/* The value is the unit value (zero length). */
	assert_true(__dtfl_cursor_val_valid(&c, b));
	(void)__dtfl_cursor_val(&c, b, &len);
	assert_size(len, ==, 0);
	memset(&a, 0, sizeof a);
	__dtfl_cursor_map_times(&c, b, collect, &a);
	assert_size(a.ntimes, ==, 1);
	/* Only one value per key. */
	__dtfl_cursor_step_val(&c, b);
	assert_false(__dtfl_cursor_val_valid(&c, b));
	__dtfl_cursor_step_key(&c, b);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_key(&c, b, &len)), ==, 2);
	__dtfl_cursor_step_key(&c, b);
	assert_false(__dtfl_cursor_key_valid(&c, b));

	__dtfl_batch_ord_release(b);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(ord_neu__merge_consolidates_and_advances),
	DTFL_T(ord_neu__key_batch_single_value),
	DTFL_T_END
};

DTFL_TEST_MAIN("ord_neu", tests)
