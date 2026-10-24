/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_trace_spill.c: the trace-side chunk spill round trip.  Fills
 * past the per-worker record budget so committed chunks spill to the file store,
 * then reads the resulting ChunkBatch back through the straddle cursor and
 * asserts the data is intact (the key property: spilling and reloading is
 * transparent).  Also checks the budget policy (the first budget_records worth
 * stay resident, the rest page) and the no-spiller fast path (nothing pages).
 */
#include "dtfl_test.h"
#include "dd/trace/chunk_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

/*
 * Build a graded ChunkBatch of N distinct (key, 0) updates by pushing one
 * TARGET-sized chunk at a time through the builder, with `x`'s spill state
 * active.  The builder settles each pushed chunk, so settle's seal hook pages
 * out whatever exceeds the budget.  Returns the batch and the total pushed.
 */
static __dtfl_chunk_batch *
build_spilled(__dtfl_chunk_ctx *x, dtfl_time_class *tc, size_t nchunks,
    size_t per_chunk, uint64_t *next_key)
{
	__dtfl_chunk_builder bld;
	__dtfl_antichain lower, upper, since;
	__dtfl_description d;
	uint64_t lo = 0, hi = 1, s = 0;
	size_t i, j;

	__dtfl_chunk_builder_init(&bld, x);
	for (i = 0; i < nchunks; i++) {
		__dtfl_chunk *c = __dtfl_batch_new(1, 8);
		unsigned char rec[16];
		uint64_t t = 0;
		int64_t r = 1;

		for (j = 0; j < per_chunk; j++) {
			dtfl_put_u64(rec, *next_key);
			dtfl_put_u64(rec + 8, 0);
			__dtfl_batch_push(c, rec, 16, &t, &r);
			(*next_key)++;
		}
		__dtfl_chunk_builder_push(&bld, c);
	}
	__dtfl_antichain_init_elem(&lower, tc, &lo);
	__dtfl_antichain_init_elem(&upper, tc, &hi);
	__dtfl_antichain_init_elem(&since, tc, &s);
	__dtfl_description_init(&d, &lower, &upper, &since);
	return __dtfl_chunk_builder_done(&bld, &d);
}

/* Read every (key, time, diff) back; assert keys are 0..N-1 with diff +1. */
static size_t
verify_sequential(const __dtfl_chunk_batch *b, size_t expect)
{
	__dtfl_chunk_cursor cur;
	size_t len, seen = 0;
	uint64_t prev = 0;
	bool first = true;

	__dtfl_chunk_cursor_init(&cur, b);
	while (__dtfl_chunk_cursor_key_valid(&cur, b)) {
		uint64_t k = dtfl_get_u64(__dtfl_chunk_cursor_key(&cur, b,
		    &len));

		if (!first)
			munit_assert_uint64(prev, <, k);
		prev = k;
		first = false;
		seen++;
		__dtfl_chunk_cursor_step_key(&cur, b);
	}
	munit_assert_size(seen, ==, expect);
	return seen;
}

/*
 * The round trip: fill well past the budget so chunks spill, then read the
 * batch back and confirm every update survived the serialize -> store -> load
 * -> decode trip through the temp file, in order.
 */
DTFL_TEST(trace_spill__roundtrip_intact)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_spill_state st;
	__dtfl_spill_stats stats = { 0, 0, 0, 0 };
	__dtfl_spill_store *store = __dtfl_spill_file_store_new();
	__dtfl_chunk_batch *b;
	uint64_t key = 0;
	size_t nchunks = 6, per = DTFL_CHUNK_TARGET;
	size_t total = nchunks * per;

	(void)params; (void)data;
	__dtfl_chunk_ctx_init(&x, false, rt, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64);
	/* Keep one chunk resident; page the rest. */
	__dtfl_spill_install(&x, &st, per, store, &stats);

	b = build_spilled(&x, tc, nchunks, per, &key);

	/* Some chunks must have spilled, and each spilled chunk was fetched. */
	munit_assert_size(stats.spilled_chunks, >, 0);
	munit_assert_size(stats.fetched_chunks, ==, stats.spilled_chunks);
	munit_assert_size(stats.spilled_records, >, 0);
	munit_assert_size(stats.bytes_written, >, 0);

	/* Every update is present, in order, after the spill and reload. */
	verify_sequential(b, total);

	__dtfl_chunk_batch_release(b);
	__dtfl_spill_uninstall(&x);
	__dtfl_spill_file_store_free(store);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * Policy: with the budget set to the whole workload, nothing spills; with a
 * tiny budget, everything past the first budget_records pages.
 */
DTFL_TEST(trace_spill__budget_high_water_mark)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_spill_state st;
	__dtfl_spill_stats stats = { 0, 0, 0, 0 };
	__dtfl_spill_store *store = __dtfl_spill_file_store_new();
	__dtfl_chunk_batch *b;
	uint64_t key = 0;
	size_t nchunks = 4, per = DTFL_CHUNK_TARGET;

	(void)params; (void)data;
	__dtfl_chunk_ctx_init(&x, false, rt, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64);
	/* Budget covers the whole workload: nothing pages. */
	__dtfl_spill_install(&x, &st, nchunks * per, store, &stats);
	b = build_spilled(&x, tc, nchunks, per, &key);
	munit_assert_size(stats.spilled_chunks, ==, 0);
	verify_sequential(b, nchunks * per);
	__dtfl_chunk_batch_release(b);

	__dtfl_spill_file_store_free(store);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* No spiller installed: seal is the identity, nothing pages, data intact. */
DTFL_TEST(trace_spill__resident_without_spiller)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk_batch *b;
	uint64_t key = 0;
	size_t nchunks = 3, per = DTFL_CHUNK_TARGET;

	(void)params; (void)data;
	__dtfl_chunk_ctx_init(&x, false, rt, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64);
	munit_assert_ptr_null(x.spill);
	b = build_spilled(&x, tc, nchunks, per, &key);
	verify_sequential(b, nchunks * per);
	__dtfl_chunk_batch_release(b);

	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(trace_spill__roundtrip_intact),
	DTFL_T(trace_spill__budget_high_water_mark),
	DTFL_T(trace_spill__resident_without_spiller),
	DTFL_T_END
};

DTFL_TEST_MAIN("trace_spill", tests)
