/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dogs/ngo.c: mirrors dogsdogsdogs/examples/ngo.rs.
 *
 * The worst-case-optimal triangle query by the count-min dataflow join
 * (not the delta-query extend() path of delta_query_wcoj.c).  The edge
 * relation, filtered to forward-pointing edges (src < dst), is arranged
 * three ways -- by self (the whole edge as key), forward (src -> dst) and
 * reverse (dst -> src) -- plus a count arrangement (the source of each edge
 * arranged by self, so its weight is the source's out-degree).  For each
 * (src, dst) the two candidate plans are weighted by how many extensions
 * each index would propose, a reduce picks the index offering the fewest,
 * and that index extends the pair and the other validates, yielding the
 * triangle (a, b, c) with a -> b, a -> c, b -> c.  Upstream reads the graph
 * from a GraphMMap file and reports a count; this port feeds a small fixed
 * graph, so the triangle set is exact and printed as delta_query_wcoj.c
 * does ("\tTriangle: (a, b, c)").
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * A fixed directed graph (the same one delta_query_wcoj.c uses).  All edges
 * already point forward (src < dst), so the filter keeps them all.
 * Triangles (a, b, c) with a -> b, b -> c, a -> c:
 *   (0,1,2): 0->1, 1->2, 0->2
 *   (0,1,3): 0->1, 1->3, 0->3
 * Edge 4->5 forms no triangle.
 */
static const uint32_t fixed_edges[][2] = {
	{ 0, 1 }, { 1, 2 }, { 0, 2 }, { 1, 3 }, { 0, 3 }, { 4, 5 },
};
#define N_EDGES (sizeof fixed_edges / sizeof fixed_edges[0])

/* ------------------------------------------------------------------ */
/* map / filter closures over pair(u32, u32) edges.                    */
/* ------------------------------------------------------------------ */

/* filter |&(src, dst)| src < dst. */
static bool
edge_forward(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)len;
	return dtfl_get_u32(rec) < dtfl_get_u32((const unsigned char *)rec + 4);
}

/* map |x| swap(x.0, x.1): reverse an edge. */
static void
map_reverse(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 4, 4);
	memcpy(buf + 4, rec, 4);
	dtfl_collection_emit_record(out, buf, 8);
}

/* map |(src, _dst)| src: the source of each edge. */
static void
map_source(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 4);
}

/* ------------------------------------------------------------------ */
/* join_core result closures.                                          */
/* ------------------------------------------------------------------ */

/*
 * cand_count1: forward (src -> dst) join counts (src), as
 * |&src, &dst, &()| ((src, dst), 1).  The output record is
 * (src, dst, idx) with idx = 1; its weight is the product of the two
 * diffs, i.e. the out-degree of src (how many extensions forward offers).
 */
static void
join_cand1(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[12]; /* (src, dst, idx) */

	(void)ctx; (void)klen; (void)v1len; (void)val2; (void)v2len;
	memcpy(buf, key, 4);		/* src */
	memcpy(buf + 4, val1, 4);	/* dst */
	dtfl_put_u32(buf + 8, 1u);	/* idx */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * cand_count2: reverse (dst -> src) join counts (src, keyed by its value),
 * as |&dst, &src, &()| ((src, dst), 2).  The key is dst, the reverse value
 * is src; the output record is (src, dst, 2) and its weight is the
 * out-degree of dst.
 */
static void
join_cand2(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[12]; /* (src, dst, idx) */

	(void)ctx; (void)klen; (void)v1len; (void)val2; (void)v2len;
	memcpy(buf, val1, 4);		/* src */
	memcpy(buf + 4, key, 4);	/* dst */
	dtfl_put_u32(buf + 8, 2u);	/* idx */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * winners1 first join: prefix (src, dst) join forward (src -> ext), as
 * |&src, &dst, &ext| ((dst, ext), src).  Key is src, prefix value is dst,
 * forward value is ext.  Output record is (dst, ext, src).
 */
static void
join_w1a(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[12]; /* (dst, ext, src) */

	(void)ctx; (void)klen; (void)v1len; (void)v2len;
	memcpy(buf, val1, 4);		/* dst (prefix value) */
	memcpy(buf + 4, val2, 4);	/* ext (forward value under src) */
	memcpy(buf + 8, key, 4);	/* src */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * winners1 second join: ((dst, ext), src) validated against as_self, as
 * |&(dst, ext), &src, &()| ((dst, ext), src).  Key is the edge (dst, ext),
 * the left value is src; the edge must exist in as_self.  Output record is
 * (dst, ext, src), unchanged.
 */
static void
join_w1b(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[12]; /* (dst, ext, src) */

	(void)ctx; (void)klen; (void)v1len; (void)val2; (void)v2len;
	memcpy(buf, key, 8);		/* (dst, ext) */
	memcpy(buf + 8, val1, 4);	/* src */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* winners1 final map: |((dst, ext), src)| (src, dst, ext). */
static void
map_w1(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[12]; /* (src, dst, ext) */

	(void)ctx; (void)len;
	memcpy(buf, p + 8, 4);		/* src */
	memcpy(buf + 4, p, 4);		/* dst */
	memcpy(buf + 8, p + 4, 4);	/* ext */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/*
 * winners2 first join: prefix (dst, src) join forward (dst -> ext), as
 * |&dst, &src, &ext| ((src, ext), dst).  Key is dst, prefix value is src,
 * forward value is ext.  Output record is (src, ext, dst).
 */
static void
join_w2a(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[12]; /* (src, ext, dst) */

	(void)ctx; (void)klen; (void)v1len; (void)v2len;
	memcpy(buf, val1, 4);		/* src (prefix value) */
	memcpy(buf + 4, val2, 4);	/* ext (forward value under dst) */
	memcpy(buf + 8, key, 4);	/* dst */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * winners2 second join: ((src, ext), dst) validated against as_self, as
 * |&(src, ext), &dst, &()| ((src, ext), dst).  Key is the edge (src, ext).
 * Output record is (src, ext, dst), unchanged.
 */
static void
join_w2b(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[12]; /* (src, ext, dst) */

	(void)ctx; (void)klen; (void)v1len; (void)val2; (void)v2len;
	memcpy(buf, key, 8);		/* (src, ext) */
	memcpy(buf + 8, val1, 4);	/* dst */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* winners2 final map: |((src, ext), dst)| (src, dst, ext). */
static void
map_w2(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[12]; /* (src, dst, ext) */

	(void)ctx; (void)len;
	memcpy(buf, p, 4);		/* src */
	memcpy(buf + 4, p + 8, 4);	/* dst */
	memcpy(buf + 8, p + 4, 4);	/* ext */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* ------------------------------------------------------------------ */
/* reduce logic: pick the index proposing the fewest extensions.       */
/* ------------------------------------------------------------------ */

/*
 * winners reduce (upstream |_srcdst, counts, output|): the input key is a
 * (src, dst) pair; the input values are the candidate indices (1 and 2),
 * one per candidate relation, each carrying the extension count as its
 * i64 diff.  When both indices are present (counts.len() == 2), emit the
 * index with the smaller count, with diff 1.  The value type is u32 (the
 * index); the input diff is i64 (the count).
 */
static void
winners_reduce(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	const unsigned char *dcol = diffs;
	int64_t min_cnt = INT64_MAX;
	uint32_t min_idx = 0;
	int64_t one = 1;
	size_t i;

	(void)ctx; (void)key; (void)klen; (void)vlens;
	if (n != 2)
		return;
	for (i = 0; i < n; i++) {
		int64_t cnt = dtfl_get_i64(dcol + i * sizeof(int64_t));

		if (min_cnt > cnt) {
			min_cnt = cnt;
			min_idx = dtfl_get_u32(vals[i]);
		}
	}
	{
		unsigned char vbuf[4];

		dtfl_put_u32(vbuf, min_idx);
		dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &one);
	}
}

/* ------------------------------------------------------------------ */
/* winners flat_map: route each winner to its chosen plan.             */
/* ------------------------------------------------------------------ */

/*
 * winners1 flat_map |((src, dst), idx)| if idx == 1 { Some((src, dst)) }.
 * Input record is (src, dst, idx); emit (src, dst) when idx == 1.
 */
static void
flat_pick1(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;

	(void)ctx; (void)len;
	if (dtfl_get_u32(p + 8) == 1u)
		dtfl_collection_emit_record(out, p, 8); /* (src, dst) */
}

/*
 * winners2 flat_map |((src, dst), idx)| if idx == 2 { Some((dst, src)) }.
 * Input record is (src, dst, idx); emit (dst, src) when idx == 2.
 */
static void
flat_pick2(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[8]; /* (dst, src) */

	(void)ctx; (void)len;
	if (dtfl_get_u32(p + 8) == 2u) {
		memcpy(buf, p + 4, 4);		/* dst */
		memcpy(buf + 4, p, 4);		/* src */
		dtfl_collection_emit_record(out, buf, sizeof buf);
	}
}

/* ------------------------------------------------------------------ */
/* Output.                                                             */
/* ------------------------------------------------------------------ */

static void
inspect_triangle(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	const unsigned char *p = rec;
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	if (d <= 0)
		return;
	printf("\tTriangle: (%llu, %llu, %llu)\n",
	    (unsigned long long)dtfl_get_u32(p),
	    (unsigned long long)dtfl_get_u32(p + 4),
	    (unsigned long long)dtfl_get_u32(p + 8));
}

/* ------------------------------------------------------------------ */
/* The triangle dataflow.                                              */
/* ------------------------------------------------------------------ */

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	const dtfl_type *u32;
	const dtfl_type *unit;
	const dtfl_type *pair;	/* (u32, u32) */
	const dtfl_type *tri;	/* (u32, u32, u32) */
	const dtfl_diff *i64;
} build_ctx;

static void
build_dataflow(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u32 = c->u32, *unit = c->unit, *pair = c->pair;
	const dtfl_type *tri = c->tri;
	dtfl_collection *raw = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *edges = dtfl_collection_filter(raw, edge_forward,
	    DTFL_NOCTX);
	dtfl_collection *reverse_edges = dtfl_collection_map(edges, pair,
	    map_reverse, DTFL_NOCTX);
	dtfl_collection *src_col = dtfl_collection_map(edges, u32, map_source,
	    DTFL_NOCTX);

	/* arrange the edge relation three ways, plus the count arrangement. */
	dtfl_arranged *as_self = dtfl_collection_arrange(edges, "as_self");
	dtfl_arranged *forward = dtfl_collection_arrange_by_key(edges, u32, u32,
	    "forward");
	dtfl_arranged *reverse = dtfl_collection_arrange_by_key(reverse_edges,
	    u32, u32, "reverse");
	dtfl_arranged *counts = dtfl_collection_arrange(src_col, "counts");

	/* ((src, dst), idx) with weight equal to the number of extensions. */
	const dtfl_type *tf3[3] = { u32, u32, u32 };
	const dtfl_type *cand_t = dtfl_type_tuple(tf3, 3);
	dtfl_collection *cand1 = dtfl_arranged_join_core(forward, counts,
	    cand_t, join_cand1, DTFL_NOCTX);
	dtfl_collection *cand2 = dtfl_arranged_join_core(reverse, counts,
	    cand_t, join_cand2, DTFL_NOCTX);
	dtfl_collection *cand = dtfl_collection_concat(cand1, cand2);

	/*
	 * reduce the (src, dst) key over its candidate indices to the index
	 * proposing the fewest extensions.  Record is (src, dst, idx); key is
	 * the pair, value is the index.
	 */
	dtfl_collection *winners = dtfl_collection_reduce(cand, pair, u32, u32,
	    c->i64, winners_reduce, DTFL_NOCTX);

	/* winners1: index 1 wins -> extend forward from src, validate edge. */
	dtfl_collection *w1pre = dtfl_collection_flat_map(winners, pair,
	    flat_pick1, DTFL_NOCTX);
	dtfl_arranged *w1arr = dtfl_collection_arrange_by_key(w1pre, u32, u32,
	    "w1pre");
	dtfl_collection *w1j1 = dtfl_arranged_join_core(w1arr, forward, cand_t,
	    join_w1a, DTFL_NOCTX);
	dtfl_arranged *w1j1arr = dtfl_collection_arrange_by_key(w1j1, pair, u32,
	    "w1j1");
	dtfl_collection *w1j2 = dtfl_arranged_join_core(w1j1arr, as_self,
	    cand_t, join_w1b, DTFL_NOCTX);
	dtfl_collection *winners1 = dtfl_collection_map(w1j2, tri, map_w1,
	    DTFL_NOCTX);

	/* winners2: index 2 wins -> extend forward from dst, validate edge. */
	dtfl_collection *w2pre = dtfl_collection_flat_map(winners, pair,
	    flat_pick2, DTFL_NOCTX);
	dtfl_arranged *w2arr = dtfl_collection_arrange_by_key(w2pre, u32, u32,
	    "w2pre");
	dtfl_collection *w2j1 = dtfl_arranged_join_core(w2arr, forward, cand_t,
	    join_w2a, DTFL_NOCTX);
	dtfl_arranged *w2j1arr = dtfl_collection_arrange_by_key(w2j1, pair, u32,
	    "w2j1");
	dtfl_collection *w2j2 = dtfl_arranged_join_core(w2j1arr, as_self,
	    cand_t, join_w2b, DTFL_NOCTX);
	dtfl_collection *winners2 = dtfl_collection_map(w2j2, tri, map_w2,
	    DTFL_NOCTX);

	dtfl_collection *result = dtfl_collection_concat(winners1, winners2);
	dtfl_collection *ins = dtfl_collection_inspect(result,
	    inspect_triangle, DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	(void)unit;
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(result);
	dtfl_collection_free(winners1);
	dtfl_collection_free(w2j2);
	dtfl_arranged_free(w2j1arr);
	dtfl_collection_free(w2j1);
	dtfl_arranged_free(w2arr);
	dtfl_collection_free(w2pre);
	dtfl_collection_free(winners2);
	dtfl_collection_free(w1j2);
	dtfl_arranged_free(w1j1arr);
	dtfl_collection_free(w1j1);
	dtfl_arranged_free(w1arr);
	dtfl_collection_free(w1pre);
	dtfl_collection_free(winners);
	dtfl_collection_free(cand);
	dtfl_collection_free(cand1);
	dtfl_collection_free(cand2);
	dtfl_type_free(cand_t);
	dtfl_arranged_free(counts);
	dtfl_arranged_free(reverse);
	dtfl_arranged_free(forward);
	dtfl_arranged_free(as_self);
	dtfl_collection_free(src_col);
	dtfl_collection_free(reverse_edges);
	dtfl_collection_free(edges);
	dtfl_collection_free(raw);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *unit = dtfl_type_unit();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	const dtfl_type *tf[3] = { dtfl_type_u32(), dtfl_type_u32(),
	    dtfl_type_u32() };
	const dtfl_type *tri = dtfl_type_tuple(tf, 3);
	build_ctx c;
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	memset(&c, 0, sizeof c);
	c.u32 = u32; c.unit = unit; c.pair = pair; c.tri = tri; c.i64 = i64;
	c.in = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "ngo", tc, build_dataflow, &c);

	for (i = 0; i < N_EDGES; i++) {
		unsigned char rec[8];

		dtfl_put_u32(rec, fixed_edges[i][0]);
		dtfl_put_u32(rec + 4, fixed_edges[i][1]);
		dtfl_input_session_insert(c.in, rec, sizeof rec);
	}
	dtfl_input_session_advance_to(c.in, &one);
	dtfl_input_session_flush(c.in);
	while (dtfl_probe_less_than(c.probe, &one))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_type_free(tri);
	dtfl_type_free(unit);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc;

	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
