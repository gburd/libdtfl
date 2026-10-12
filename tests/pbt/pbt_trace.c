/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_trace.c: the core correctness property of the trace layer.
 * A random sequence of (key, val, time, diff) updates is split into
 * contiguous time epochs, each batched (chunker -> merge batcher -> builder)
 * and inserted into the fuelled spine.  Reading the whole trace with a cursor
 * and accumulating each (key, val)'s diffs at a query time must yield the
 * same (key, val) -> summed-diff map as a direct reference accumulation over
 * the raw updates.  This exercises the batcher's consolidation, the ord_neu
 * trie, the spine's fuelled merges and the cursor together.
 */
#include "pbt.h"
#include "dd/trace/trace_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)
#define MAXN 48
#define NKEY 4
#define NVAL 4
#define NEPOCH 5	/* epoch boundaries; times in [0, NEPOCH) */

typedef struct upd {
	uint64_t k, v, t;
	int64_t r;
} upd;

/* The accumulated diff of (k, v) over updates with time <= query. */
static int64_t
ref_sum(const upd *u, size_t n, uint64_t k, uint64_t v, uint64_t query)
{
	int64_t s = 0;
	size_t i;

	for (i = 0; i < n; i++)
		if (u[i].k == k && u[i].v == v && u[i].t <= query)
			s += u[i].r;
	return s;
}

struct ctx {
	int64_t sum;
	uint64_t query;
	uint64_t cur_t;
};

/* map_times accumulates diffs of the current (key, val) at times <= query. */
static void
accumulate(const uint64_t *time, const void *diff, void *vp)
{
	struct ctx *c = vp;
	int64_t r;

	if (time[0] <= c->query) {
		memcpy(&r, diff, 8);
		c->sum += r;
	}
}

static void
free_trace(void *p)
{
	__dtfl_trace_free(p);
}

static void
free_tc(void *p)
{
	dtfl_time_class_free(p);
}

static void
free_type(void *p)
{
	dtfl_type_free(p);
}

static void
prop_cursor_accumulation_equals_reference(pbt_tc *tc)
{
	upd u[MAXN];
	size_t n = (size_t)pbt_int(tc, 1, MAXN), i, epoch;
	dtfl_time_class *tcl = dtfl_time_u64();
	const dtfl_type *rt;
	__dtfl_trace *tr;
	__dtfl_cursor_list cl;
	size_t len;
	uint64_t query;

	pbt_defer(tc, free_tc, tcl);
	rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	pbt_defer(tc, free_type, (void *)rt);
	tr = __dtfl_trace_new(false, dtfl_type_u64(), dtfl_type_u64(), tcl,
	    I64);
	pbt_defer(tc, free_trace, tr);

	for (i = 0; i < n; i++) {
		u[i].k = (uint64_t)pbt_int(tc, 0, NKEY - 1);
		u[i].v = (uint64_t)pbt_int(tc, 0, NVAL - 1);
		u[i].t = (uint64_t)pbt_int(tc, 0, NEPOCH - 1);
		u[i].r = pbt_int(tc, -3, 3);
	}

	/*
	 * Feed every update into one chunker/batcher, then seal at successive
	 * uppers [1..NEPOCH].  Each seal yields the updates with time below the
	 * upper not yet shipped, with lower = the prior upper, so the batches
	 * form the contiguous intervals the spine requires.
	 */
	{
		__dtfl_merge_batcher mb;
		__dtfl_chunker ch;
		__dtfl_batch *chunk;

		__dtfl_chunker_init(&ch, rt, 1, I64);
		for (i = 0; i < n; i++) {
			unsigned char rec[16];

			dtfl_put_u64(rec, u[i].k);
			dtfl_put_u64(rec + 8, u[i].v);
			__dtfl_chunker_push(&ch, rec, 16, &u[i].t, &u[i].r);
		}
		__dtfl_merge_batcher_init(&mb, rt, tcl, I64);
		while ((chunk = __dtfl_chunker_finish(&ch)) != NULL)
			__dtfl_merge_batcher_push(&mb, chunk);
		__dtfl_chunker_free(&ch);

		for (epoch = 0; epoch < NEPOCH; epoch++) {
			__dtfl_antichain upper;
			__dtfl_description desc;
			__dtfl_batch **out;
			size_t nout;
			__dtfl_batch_ord *batch;
			uint64_t up = epoch + 1;

			__dtfl_antichain_init_elem(&upper, tcl, &up);
			__dtfl_merge_batcher_seal(&mb, &upper, &out, &nout,
			    &desc);
			batch = __dtfl_builder_ord_seal(false, dtfl_type_u64(),
			    dtfl_type_u64(), tcl, I64, out, nout, &desc);
			__dtfl_trace_insert(tr, batch);
			for (i = 0; i < nout; i++)
				__dtfl_batch_release(out[i]);
			__dtfl_free(out);
			__dtfl_antichain_free(&upper);
		}
		__dtfl_merge_batcher_free(&mb);
	}

	/* Advance physical compaction so the spine actually merges. */
	{
		__dtfl_antichain phys;
		uint64_t mid = (uint64_t)pbt_int(tc, 0, NEPOCH);

		__dtfl_antichain_init_elem(&phys, tcl, &mid);
		__dtfl_trace_set_physical_compaction(tr, &phys);
		__dtfl_antichain_free(&phys);
	}

	query = (uint64_t)pbt_int(tc, 0, NEPOCH - 1);

	/* Read the whole trace and compare accumulation to the reference. */
	__dtfl_trace_cursor(tr, &cl);
	while (__dtfl_cursor_list_key_valid(&cl)) {
		uint64_t k = dtfl_get_u64(__dtfl_cursor_list_key(&cl, &len));

		while (__dtfl_cursor_list_val_valid(&cl)) {
			uint64_t v = dtfl_get_u64(
			    __dtfl_cursor_list_val(&cl, &len));
			struct ctx c;

			c.sum = 0;
			c.query = query;
			__dtfl_cursor_list_map_times(&cl, accumulate, &c);
			PBT_ASSERT(c.sum == ref_sum(u, n, k, v, query));
			__dtfl_cursor_list_step_val(&cl);
		}
		__dtfl_cursor_list_step_key(&cl);
	}
	__dtfl_cursor_list_free(&cl);

	/*
	 * Also check the other direction: every (key, val) the reference says
	 * has a non-zero sum at the query must appear in the cursor.  We check
	 * this by re-scanning: for each distinct (k, v) with a non-zero
	 * reference sum, the cursor's accumulation (recomputed) must match.
	 * The forward check above already covers presence of cursor keys; a
	 * missing non-zero (k, v) would leave the reference sum unmatched, so
	 * recompute the full reference total of non-zero cells and compare to
	 * the number of cursor cells visited would be redundant.  Instead we
	 * verify no non-zero reference cell is absent by seeking it.
	 */
	__dtfl_trace_cursor(tr, &cl);
	for (i = 0; i < (size_t)NKEY * NVAL; i++) {
		uint64_t k = i / NVAL, v = i % NVAL;
		int64_t want = ref_sum(u, n, k, v, query);
		unsigned char kb[8], vb[8];
		struct ctx c;

		if (want == 0)
			continue;
		dtfl_put_u64(kb, k);
		dtfl_put_u64(vb, v);
		__dtfl_cursor_list_rewind_keys(&cl);
		__dtfl_cursor_list_seek_key(&cl, kb, 8);
		PBT_ASSERT(__dtfl_cursor_list_key_valid(&cl));
		PBT_ASSERT(dtfl_get_u64(__dtfl_cursor_list_key(&cl, &len)) == k);
		__dtfl_cursor_list_seek_val(&cl, vb, 8);
		PBT_ASSERT(__dtfl_cursor_list_val_valid(&cl));
		PBT_ASSERT(dtfl_get_u64(__dtfl_cursor_list_val(&cl, &len)) == v);
		c.sum = 0;
		c.query = query;
		__dtfl_cursor_list_map_times(&cl, accumulate, &c);
		PBT_ASSERT(c.sum == want);
	}
	__dtfl_cursor_list_free(&cl);
}

static const pbt_prop props[] = {
	{ "cursor_accumulation_equals_reference",
	    prop_cursor_accumulation_equals_reference, 0 },
};

PBT_MAIN("trace", props)
