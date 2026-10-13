/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_chunk.c: the chunk-backed trace accumulates to the same
 * (key, val) -> summed-diff map as a direct reference, including after a
 * spill/reload.  A random sequence of (key, val, time, diff) updates is
 * consolidated into sorted chunks (as the arrange path does), sealed into a
 * ChunkBatch through the builder with a tiny spill budget installed -- so most
 * chunks page out to the file store and are fetched back -- then read through
 * the straddle cursor.  Each (key, val)'s accumulated diff must equal the
 * reference, and every non-zero reference cell must be present in the cursor.
 * Small by design: one batch, few keys/vals, a case well under a second.
 */
#include "pbt.h"
#include "dd/trace/chunk_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)
#define MAXN 40
#define NKEY 4
#define NVAL 4
#define NTIME 4

typedef struct upd {
	uint64_t k, v, t;
	int64_t r;
} upd;

/* The accumulated diff of (k, v) over all updates (query covers all times). */
static int64_t
ref_sum(const upd *u, size_t n, uint64_t k, uint64_t v)
{
	int64_t s = 0;
	size_t i;

	for (i = 0; i < n; i++)
		if (u[i].k == k && u[i].v == v)
			s += u[i].r;
	return s;
}

struct ctx {
	int64_t sum;
};

static void
accumulate(const uint64_t *time, const void *diff, void *vp)
{
	struct ctx *c = vp;
	int64_t r;

	(void)time;
	memcpy(&r, diff, 8);
	c->sum += r;
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
free_batch(void *p)
{
	__dtfl_chunk_batch_release(p);
}

static void
free_store(void *p)
{
	__dtfl_spill_file_store_free(p);
}

static void
prop_chunk_trace_equals_reference(pbt_tc *tc)
{
	upd u[MAXN];
	size_t n = (size_t)pbt_int(tc, 1, MAXN), i;
	dtfl_time_class *tcl = dtfl_time_u64();
	const dtfl_type *rt;
	__dtfl_chunk_ctx x;
	__dtfl_spill_state st;
	__dtfl_spill_stats stats = { 0, 0, 0, 0 };
	__dtfl_spill_store *store = __dtfl_spill_file_store_new();
	__dtfl_consolidator con;
	__dtfl_chunk_builder bld;
	__dtfl_chunk_batch *b;
	__dtfl_chunk *chunk;
	__dtfl_chunk_cursor cur;
	__dtfl_antichain lower, upper, since;
	__dtfl_description desc;
	uint64_t lo = 0, hi = 1, s0 = 0;
	size_t len;

	pbt_defer(tc, free_tc, tcl);
	pbt_defer(tc, free_store, store);
	rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	pbt_defer(tc, free_type, (void *)rt);

	for (i = 0; i < n; i++) {
		u[i].k = (uint64_t)pbt_int(tc, 0, NKEY - 1);
		u[i].v = (uint64_t)pbt_int(tc, 0, NVAL - 1);
		u[i].t = (uint64_t)pbt_int(tc, 0, NTIME - 1);
		u[i].r = pbt_int(tc, -3, 3);
	}

	/*
	 * Consolidate the raw stream into sorted (record, time) chunks, then
	 * seal them into a ChunkBatch with a tiny spill budget so most chunks
	 * page out and are fetched back during settle.  Budget zero pages every
	 * committed chunk.
	 */
	__dtfl_chunk_ctx_init(&x, false, rt, dtfl_type_u64(), dtfl_type_u64(),
	    tcl, I64);
	__dtfl_spill_install(&x, &st, 0, store, &stats);
	__dtfl_consolidator_init(&con, rt, 1, I64);
	for (i = 0; i < n; i++) {
		unsigned char rec[16];

		dtfl_put_u64(rec, u[i].k);
		dtfl_put_u64(rec + 8, u[i].v);
		__dtfl_consolidator_push(&con, rec, 16, &u[i].t, &u[i].r);
	}
	__dtfl_chunk_builder_init(&bld, &x);
	while ((chunk = __dtfl_consolidator_finish(&con)) != NULL)
		__dtfl_chunk_builder_push(&bld, chunk);
	__dtfl_consolidator_free(&con);
	__dtfl_antichain_init_elem(&lower, tcl, &lo);
	__dtfl_antichain_init_elem(&upper, tcl, &hi);
	__dtfl_antichain_init_elem(&since, tcl, &s0);
	__dtfl_description_init(&desc, &lower, &upper, &since);
	b = __dtfl_chunk_builder_done(&bld, &desc);
	pbt_defer(tc, free_batch, b);

	/* Forward: every cursor cell's accumulation matches the reference. */
	__dtfl_chunk_cursor_init(&cur, b);
	while (__dtfl_chunk_cursor_key_valid(&cur, b)) {
		uint64_t k = dtfl_get_u64(__dtfl_chunk_cursor_key(&cur, b,
		    &len));

		while (__dtfl_chunk_cursor_val_valid(&cur, b)) {
			uint64_t v = dtfl_get_u64(
			    __dtfl_chunk_cursor_val(&cur, b, &len));
			struct ctx c = { 0 };

			__dtfl_chunk_cursor_map_times(&cur, b, accumulate, &c);
			PBT_ASSERT(c.sum == ref_sum(u, n, k, v));
			__dtfl_chunk_cursor_step_val(&cur, b);
		}
		__dtfl_chunk_cursor_step_key(&cur, b);
	}

	/* Reverse: every non-zero reference cell is present via seek. */
	for (i = 0; i < (size_t)NKEY * NVAL; i++) {
		uint64_t k = i / NVAL, v = i % NVAL;
		int64_t want = ref_sum(u, n, k, v);
		unsigned char kb[8], vb[8];
		struct ctx c = { 0 };

		if (want == 0)
			continue;
		dtfl_put_u64(kb, k);
		dtfl_put_u64(vb, v);
		__dtfl_chunk_cursor_rewind_keys(&cur, b);
		__dtfl_chunk_cursor_seek_key(&cur, b, kb, 8);
		PBT_ASSERT(__dtfl_chunk_cursor_key_valid(&cur, b));
		PBT_ASSERT(dtfl_get_u64(__dtfl_chunk_cursor_key(&cur, b, &len))
		    == k);
		__dtfl_chunk_cursor_seek_val(&cur, b, vb, 8);
		PBT_ASSERT(__dtfl_chunk_cursor_val_valid(&cur, b));
		PBT_ASSERT(dtfl_get_u64(__dtfl_chunk_cursor_val(&cur, b, &len))
		    == v);
		__dtfl_chunk_cursor_map_times(&cur, b, accumulate, &c);
		PBT_ASSERT(c.sum == want);
	}
	__dtfl_spill_uninstall(&x);
}

static const pbt_prop props[] = {
	{ "chunk_trace_equals_reference",
	    prop_chunk_trace_equals_reference, 0 },
};

PBT_MAIN("chunk", props)
