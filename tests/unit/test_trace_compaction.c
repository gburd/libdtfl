/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_trace_compaction.c: the ord_neu merger must keep an edit
 * readable after compaction toward a multi-element `since` antichain.
 *
 * This isolates the trace-layer compaction readback that blocks scc without
 * the full nested-iterate stack.  The time class is Product<Product<u64,
 * u64>, u64> (depth 3), the shape scc produces (an outer iterate loop
 * coordinate, an inner propagate loop coordinate).  An output edit physically
 * present at ((0,0),0) must still be surfaced by a cursor reading the merged
 * batch at a query time ((0,0),3) when the compaction `since` is the
 * two-element antichain {((0,0),3), ((0,1),1)}.
 *
 * advance_by(((0,0),0), {((0,0),3), ((0,1),1)})
 *   = meet(join(((0,0),0),((0,0),3)), join(((0,0),0),((0,1),1)))
 *   = meet(((0,0),3), ((0,1),1)) = ((0,0),1),
 * and ((0,0),1) <= ((0,0),3), so the edit stays readable.  The regression:
 * the merge dropped or misplaced the edit under the multi-coordinate since,
 * so Reduce re-emitted without retracting and the inner loop never closed.
 */
#include "dtfl_test.h"
#include "dd/trace/trace_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

/* Build a depth-3 time ((o,i),j) into t[0..2]. */
static void
mk_time(uint64_t *t, uint64_t o, uint64_t i, uint64_t j)
{
	t[0] = o;
	t[1] = i;
	t[2] = j;
}

/* Seal one batch from a set of (val, time, diff) updates for a single key.
 * lower/upper bound the batch's description.  Returns a new ord batch. */
typedef struct vtd {
	uint64_t v;
	uint64_t time[3];
	int64_t r;
} vtd;

static __dtfl_batch_ord *
seal_batch(dtfl_time_class *tc, const dtfl_type *rt, const vtd *u, size_t n,
    const uint64_t *lower, size_t nlower, const uint64_t *upper, size_t nupper)
{
	__dtfl_merge_batcher mb;
	__dtfl_chunker ch;
	__dtfl_batch *chunk;
	__dtfl_antichain up;
	__dtfl_description desc;
	__dtfl_batch **out;
	size_t nout, i;
	__dtfl_batch_ord *batch;
	uint64_t key = 7;	/* one shared key for all updates */

	__dtfl_chunker_init(&ch, rt, 3, I64);
	for (i = 0; i < n; i++) {
		unsigned char rec[16];

		dtfl_put_u64(rec, key);
		dtfl_put_u64(rec + 8, u[i].v);
		__dtfl_chunker_push(&ch, rec, 16, u[i].time, &u[i].r);
	}
	__dtfl_merge_batcher_init(&mb, rt, tc, I64);
	while ((chunk = __dtfl_chunker_finish(&ch)) != NULL)
		__dtfl_merge_batcher_push(&mb, chunk);
	__dtfl_chunker_free(&ch);

	/* The batcher starts its lower at the time minimum; seal at `upper`
	 * then overwrite the description's bounds with the requested ones so
	 * the two batches are contiguous (upper1 == lower2). */
	__dtfl_antichain_init(&up, tc);
	for (i = 0; i < nupper; i++)
		__dtfl_antichain_insert(&up, upper + i * 3);
	__dtfl_merge_batcher_seal(&mb, &up, &out, &nout, &desc);
	__dtfl_description_free(&desc);
	{
		__dtfl_antichain lo, hi, since;
		uint64_t min[3];

		__dtfl_antichain_init(&lo, tc);
		for (i = 0; i < nlower; i++)
			__dtfl_antichain_insert(&lo, lower + i * 3);
		__dtfl_antichain_init(&hi, tc);
		for (i = 0; i < nupper; i++)
			__dtfl_antichain_insert(&hi, upper + i * 3);
		dtfl_time_minimum(tc, min);
		__dtfl_antichain_init_elem(&since, tc, min);
		__dtfl_description_init(&desc, &lo, &hi, &since);
	}
	batch = __dtfl_builder_ord_seal(false, dtfl_type_u64(),
	    dtfl_type_u64(), tc, I64, out, nout, &desc);
	for (i = 0; i < nout; i++)
		__dtfl_batch_release(out[i]);
	__dtfl_free(out);
	__dtfl_antichain_free(&up);
	__dtfl_merge_batcher_free(&mb);
	return batch;
}

/* Accumulate the diff of the current (key, val) at times <= query. */
struct acc {
	const dtfl_time_class *tc;
	uint64_t query[3];
	int64_t sum;
};

static void
accumulate(const uint64_t *time, const void *diff, void *vp)
{
	struct acc *a = vp;
	int64_t r;

	if (dtfl_time_less_equal(a->tc, time, a->query)) {
		memcpy(&r, diff, 8);
		a->sum += r;
	}
}

DTFL_TEST(trace_compaction__multi_since_keeps_edit)
{
	dtfl_time_class *u1 = dtfl_time_u64();
	dtfl_time_class *u2 = dtfl_time_u64();
	dtfl_time_class *u3 = dtfl_time_u64();
	dtfl_time_class *mid = dtfl_time_product(u1, u2);
	dtfl_time_class *tc = dtfl_time_product(mid, u3);
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_batch_ord *b1, *b2, *merged;
	__dtfl_merger_ord m;
	__dtfl_antichain compaction;
	uint64_t t000[3], t001[3], t003[3], t011[3], t100[3];
	int64_t fuel = 1 << 20;
	__dtfl_cursor_list cl;
	size_t len;
	bool found_key = false;
	struct acc a;

	(void)params; (void)data;

	mk_time(t000, 0, 0, 0);
	mk_time(t001, 0, 0, 1);
	mk_time(t003, 0, 0, 3);
	mk_time(t011, 0, 1, 1);
	mk_time(t100, 1, 0, 0);

	/* batch1: an output edit at ((0,0),0) with diff +1, over [min, ((0,0),1)). */
	{
		vtd u[] = { { 5, { 0, 0, 0 }, 1 } };

		b1 = seal_batch(tc, rt, u, 1, t000, 1, t001, 1);
	}
	/* batch2: a later edit at ((1,0),0), over [((0,0),1), ((1,0),1)).  It
	 * keeps the batches contiguous and gives the merger a second key range
	 * to walk; its presence does not change the retraction at ((0,0),0). */
	{
		vtd u[] = { { 5, { 1, 0, 0 }, -1 } };

		b2 = seal_batch(tc, rt, u, 1, t001, 1, t100, 1);
	}

	/* compaction_frontier = {((0,0),3), ((0,1),1)}. */
	__dtfl_antichain_init(&compaction, tc);
	__dtfl_antichain_insert(&compaction, t003);
	__dtfl_antichain_insert(&compaction, t011);
	assert_size(__dtfl_antichain_len(&compaction), ==, 2);

	__dtfl_merger_ord_new(&m, b1, b2, &compaction);
	__dtfl_merger_ord_work(&m, &fuel);
	merged = __dtfl_merger_ord_done(&m);

	/* Read the merged batch at the query time ((0,0),3): the +1 edit must
	 * be surfaced, accumulating to +1 (the ((1,0),0) retraction is not
	 * <= ((0,0),3), so it does not count here). */
	{
		__dtfl_batch_ord *bs[1] = { merged };

		__dtfl_cursor_list_init(&cl, bs, 1, dtfl_type_u64(),
		    dtfl_type_u64());
	}
	a.tc = tc;
	memcpy(a.query, t003, sizeof t003);
	a.sum = 0;
	while (__dtfl_cursor_list_key_valid(&cl)) {
		while (__dtfl_cursor_list_val_valid(&cl)) {
			uint64_t v = dtfl_get_u64(
			    __dtfl_cursor_list_val(&cl, &len));

			if (v == 5) {
				found_key = true;
				__dtfl_cursor_list_map_times(&cl, accumulate,
				    &a);
			}
			__dtfl_cursor_list_step_val(&cl);
		}
		__dtfl_cursor_list_step_key(&cl);
	}
	__dtfl_cursor_list_free(&cl);

	assert_true(found_key);
	assert_int64(a.sum, ==, 1);

	__dtfl_batch_ord_release(b1);
	__dtfl_batch_ord_release(b2);
	__dtfl_antichain_free(&compaction);
	dtfl_time_class_free(tc);
	dtfl_time_class_free(mid);
	dtfl_time_class_free(u1);
	dtfl_time_class_free(u2);
	dtfl_time_class_free(u3);
	dtfl_type_free(rt);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(trace_compaction__multi_since_keeps_edit),
	DTFL_T_END,
};

DTFL_TEST_MAIN("trace_compaction", tests)
