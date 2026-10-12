/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/tests/trace.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dd/trace.c: trace__test_trace, the end-to-end trace test.
 * Three updates ((1,2) at 0:+1, (2,3) at 1:+1, (2,3) at 2:-1) are batched and
 * sealed at successive uppers [1], [2], [3], each sealed chain built into a
 * batch and inserted into a ValSpine.  Cursors cut at [1], [2], [3] and the
 * whole trace must present exactly the updates with time strictly below the
 * cut, so (2,3) carries +1 at [2] and both +1 and -1 at [3].
 */
#include "dtfl_test.h"
#include "dd/trace/trace_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

typedef struct kvtr {
	uint64_t k, v, t;
	int64_t r;
} kvtr;

/* Seal one epoch: feed the chunker, push into the batcher, seal at [upper],
 * build the sealed chain into a batch, and insert it into the trace. */
static void
insert_epoch(__dtfl_trace *tr, __dtfl_merge_batcher *mb, dtfl_time_class *tc,
    uint64_t upper_t)
{
	__dtfl_antichain upper;
	__dtfl_description desc;
	__dtfl_batch **out;
	size_t nout;
	__dtfl_batch_ord *batch;

	__dtfl_antichain_init_elem(&upper, tc, &upper_t);
	__dtfl_merge_batcher_seal(mb, &upper, &out, &nout, &desc);
	batch = __dtfl_builder_ord_seal(false, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64, out, nout, &desc);
	__dtfl_trace_insert(tr, batch);
	{
		size_t i;

		for (i = 0; i < nout; i++)
			__dtfl_batch_release(out[i]);
		__dtfl_free(out);
	}
	__dtfl_antichain_free(&upper);
}

/* Collects the current (key, val)'s (time, diff) pairs. */
struct kvacc {
	size_t n;
	uint64_t t[8];
	int64_t r[8];
};

static void
collect(const uint64_t *time, const void *diff, void *ctx)
{
	struct kvacc *a = ctx;

	if (a->n < 8) {
		a->t[a->n] = time[0];
		memcpy(&a->r[a->n], diff, 8);
		a->n++;
	}
}

/*
 * Reads the whole cursor into a flat list of (k, v, [(t, r)...]) and checks
 * it equals the expected updates.  Expected is a sorted list by (k, v, t).
 */
static void
check_cursor(__dtfl_cursor_list *cl, const kvtr *exp, size_t nexp)
{
	size_t idx = 0, len;

	__dtfl_cursor_list_rewind_keys(cl);
	while (__dtfl_cursor_list_key_valid(cl)) {
		uint64_t k = dtfl_get_u64(__dtfl_cursor_list_key(cl, &len));

		__dtfl_cursor_list_rewind_vals(cl);
		while (__dtfl_cursor_list_val_valid(cl)) {
			uint64_t v = dtfl_get_u64(
			    __dtfl_cursor_list_val(cl, &len));
			struct kvacc a;
			size_t i;

			memset(&a, 0, sizeof a);
			__dtfl_cursor_list_map_times(cl, collect, &a);
			for (i = 0; i < a.n; i++) {
				assert_size(idx, <, nexp);
				assert_uint64(k, ==, exp[idx].k);
				assert_uint64(v, ==, exp[idx].v);
				assert_uint64(a.t[i], ==, exp[idx].t);
				assert_int64(a.r[i], ==, exp[idx].r);
				idx++;
			}
			__dtfl_cursor_list_step_val(cl);
		}
		__dtfl_cursor_list_step_key(cl);
	}
	assert_size(idx, ==, nexp);
}

DTFL_TEST(trace__test_trace)
{
	static const kvtr in[] = {
		{ 1, 2, 0, 1 }, { 2, 3, 1, 1 }, { 2, 3, 2, -1 },
	};
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_trace *tr = __dtfl_trace_new(false, dtfl_type_u64(),
	    dtfl_type_u64(), tc, I64);
	__dtfl_merge_batcher mb;
	__dtfl_chunker ch;
	__dtfl_batch *chunk;
	size_t i;
	__dtfl_cursor_list cl;
	__dtfl_antichain cut;
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	uint64_t one = 1, two = 2, three = 3;

	(void)params; (void)data;

	/* Feed all three updates through the chunker into one chunk. */
	__dtfl_chunker_init(&ch, rt, 1, I64);
	for (i = 0; i < 3; i++) {
		unsigned char rec[16];

		dtfl_put_u64(rec, in[i].k);
		dtfl_put_u64(rec + 8, in[i].v);
		__dtfl_chunker_push(&ch, rec, 16, &in[i].t, &in[i].r);
	}
	__dtfl_merge_batcher_init(&mb, rt, tc, I64);
	while ((chunk = __dtfl_chunker_finish(&ch)) != NULL)
		__dtfl_merge_batcher_push(&mb, chunk);
	__dtfl_chunker_free(&ch);

	/* Seal at [1], [2], [3]. */
	insert_epoch(tr, &mb, tc, 1);
	insert_epoch(tr, &mb, tc, 2);
	insert_epoch(tr, &mb, tc, 3);
	__dtfl_merge_batcher_free(&mb);

	/* Cursor cut at [1]: only (1,2) at 0:+1. */
	{
		static const kvtr e1[] = { { 1, 2, 0, 1 } };

		__dtfl_antichain_init_elem(&cut, tc, &one);
		assert_true(__dtfl_trace_cursor_through(tr, &cut, &cl));
		check_cursor(&cl, e1, 1);
		__dtfl_cursor_list_free(&cl);
		__dtfl_antichain_free(&cut);
	}
	/* Cut at [2]: (1,2) 0:+1 and (2,3) 1:+1. */
	{
		static const kvtr e2[] = { { 1, 2, 0, 1 }, { 2, 3, 1, 1 } };

		__dtfl_antichain_init_elem(&cut, tc, &two);
		assert_true(__dtfl_trace_cursor_through(tr, &cut, &cl));
		check_cursor(&cl, e2, 2);
		__dtfl_cursor_list_free(&cl);
		__dtfl_antichain_free(&cut);
	}
	/* Cut at [3]: (1,2) 0:+1 and (2,3) 1:+1, 2:-1. */
	{
		static const kvtr e3[] = {
			{ 1, 2, 0, 1 }, { 2, 3, 1, 1 }, { 2, 3, 2, -1 },
		};

		__dtfl_antichain_init_elem(&cut, tc, &three);
		assert_true(__dtfl_trace_cursor_through(tr, &cut, &cl));
		check_cursor(&cl, e3, 3);
		__dtfl_cursor_list_free(&cl);
		__dtfl_antichain_free(&cut);
	}
	/* Whole trace cursor equals the [3] cut. */
	{
		static const kvtr e4[] = {
			{ 1, 2, 0, 1 }, { 2, 3, 1, 1 }, { 2, 3, 2, -1 },
		};

		__dtfl_trace_cursor(tr, &cl);
		check_cursor(&cl, e4, 3);
		__dtfl_cursor_list_free(&cl);
	}

	__dtfl_trace_free(tr);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(trace__test_trace),
	DTFL_T_END
};

DTFL_TEST_MAIN("trace", tests)
