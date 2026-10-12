/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_merge_batcher.c: the MergeBatcher frontier tests.  Ports
 * trace_implementations_merge_batcher_test__frontier_is_post_consolidation
 * and __frontier_survivor_minimum from
 * differential-dataflow/src/trace/implementations/merge_batcher.rs
 * (differential-dataflow-v0.25.1).  Upstream uses (u64, u64, i64) updates;
 * here records are order-preserving u64 bytes, times a single u64 word, and
 * diffs the i64 class.  The sealed frontier must reflect the set of distinct
 * kept times AFTER consolidation, so a kept time whose updates cancel to zero
 * holds no capability.
 */
#include "dtfl_test.h"
#include "dd/trace/trace_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

/* A one-chunk chain carrying the given (record u64, time u64, i64) updates. */
typedef struct u3 {
	uint64_t d, t;
	int64_t r;
} u3;

static __dtfl_batch *
chunk_of(const u3 *u, size_t n)
{
	__dtfl_batch *b = __dtfl_batch_new(1, 8);
	unsigned char rec[8];
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_put_u64(rec, u[i].d);
		__dtfl_batch_push(b, rec, 8, &u[i].t, &u[i].r);
	}
	return b;
}

/* Pushes one chain of a single chunk into the batcher. */
static void
push_chain(__dtfl_merge_batcher *mb, const u3 *u, size_t n)
{
	__dtfl_batch *chunk = chunk_of(u, n);

	__dtfl_merge_batcher_chain_push(mb, &chunk, 1);
}

/* The sorted set of frontier times after a seal. */
static void
frontier_times(const __dtfl_antichain *f, uint64_t *out, size_t *n)
{
	size_t i;

	*n = __dtfl_antichain_len(f);
	for (i = 0; i < *n; i++)
		out[i] = __dtfl_antichain_at(f, i)[0];
	/* Sort ascending (small). */
	for (i = 1; i < *n; i++) {
		uint64_t key = out[i];
		size_t j = i;

		while (j > 0 && out[j - 1] > key) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = key;
	}
}

static void
release_chunks(__dtfl_batch **out, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		__dtfl_batch_release(out[i]);
	__dtfl_free(out);
}

DTFL_TEST(trace_implementations_merge_batcher_test__frontier_is_post_consolidation)
{
	static const u3 chain1[] = { { 100, 5, 1 }, { 200, 7, 1 } };
	static const u3 chain2[] = { { 100, 5, -1 } };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_merge_batcher mb;
	__dtfl_antichain upper;
	__dtfl_description desc;
	__dtfl_batch **out;
	size_t nout, nf;
	uint64_t ft[8], three = 3;

	(void)params; (void)data;
	__dtfl_merge_batcher_init(&mb, dtfl_type_u64(), tc, I64);
	push_chain(&mb, chain1, 2);
	push_chain(&mb, chain2, 1);
	__dtfl_antichain_init_elem(&upper, tc, &three);
	__dtfl_merge_batcher_seal(&mb, &upper, &out, &nout, &desc);

	/* (100, 5) nets to zero; only (200, 7) survives, so frontier = {7}. */
	frontier_times(__dtfl_merge_batcher_frontier(&mb), ft, &nf);
	assert_size(nf, ==, 1);
	assert_uint64(ft[0], ==, 7);

	release_chunks(out, nout);
	__dtfl_description_free(&desc);
	__dtfl_antichain_free(&upper);
	__dtfl_merge_batcher_free(&mb);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(trace_implementations_merge_batcher_test__frontier_survivor_minimum)
{
	static const u3 chain1[] = { { 100, 5, 1 } };
	static const u3 chain2[] = { { 200, 7, 1 } };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_merge_batcher mb;
	__dtfl_antichain upper;
	__dtfl_description desc;
	__dtfl_batch **out;
	size_t nout, nf;
	uint64_t ft[8], three = 3;

	(void)params; (void)data;
	__dtfl_merge_batcher_init(&mb, dtfl_type_u64(), tc, I64);
	push_chain(&mb, chain1, 1);
	push_chain(&mb, chain2, 1);
	__dtfl_antichain_init_elem(&upper, tc, &three);
	__dtfl_merge_batcher_seal(&mb, &upper, &out, &nout, &desc);

	/* No cancellation; the frontier is the minimal kept time, {5}. */
	frontier_times(__dtfl_merge_batcher_frontier(&mb), ft, &nf);
	assert_size(nf, ==, 1);
	assert_uint64(ft[0], ==, 5);

	release_chunks(out, nout);
	__dtfl_description_free(&desc);
	__dtfl_antichain_free(&upper);
	__dtfl_merge_batcher_free(&mb);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(trace_implementations_merge_batcher_test__frontier_is_post_consolidation),
	DTFL_T(trace_implementations_merge_batcher_test__frontier_survivor_minimum),
	DTFL_T_END
};

DTFL_TEST_MAIN("merge_batcher", tests)
