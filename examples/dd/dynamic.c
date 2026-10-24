/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/dynamic.c: mirrors differential-dataflow/examples/dynamic.rs.
 *
 * Breadth-first distance labeling run in a dynamic iterative scope whose
 * inner time is a PointStamp (dynamic/mod.rs), rather than the static
 * Product<outer, u64> scope.  The label Variable is created at PointStamp
 * coordinate 1 with the feedback summary feedback_summary(1, 1); each round
 * joins labelled nodes with edges to reach destinations at distance + 1,
 * concatenates the roots, and reduces to the minimum distance per node;
 * then leave_dynamic(1) strips the inner PointStamp coordinate.  Upstream
 * builds a random graph with a seeded StdRng and reports per-round timings;
 * a C runtime cannot reproduce that stream, so this port feeds a small fixed
 * graph and inspects the (distance -> count) histogram, which is exact.
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

/*
 * A fixed directed graph.  From root 0 the BFS distances are:
 *   0:0  1:1  2:1  3:2  4:2  5:3.  Node 6 is unreachable.
 * Distance histogram: dist 0 -> 1, dist 1 -> 2, dist 2 -> 2, dist 3 -> 1.
 */
static const uint32_t fixed_edges[][2] = {
	{ 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 2, 4 }, { 3, 5 },
	{ 4, 5 }, { 6, 0 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])

/* map root x -> (x, 0): seed each root at distance 0. */
static void
root_to_label(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	memcpy(buf, rec, 4);
	dtfl_put_u32(buf + 4, 0);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* join_map (node, dist) x (node, dst) -> (dst, dist + 1). */
static void
reach(void *ctx, const void *key, size_t klen, const void *val1, size_t v1len,
    const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)key; (void)klen; (void)v1len; (void)v2len;
	memcpy(buf, val2, 4);			/* dst */
	dtfl_put_u32(buf + 4, dtfl_get_u32(val1) + 1);	/* dist + 1 */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* reduce (node -> {dist}) keeping the minimum distance (s is sorted asc). */
static void
keep_min(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)vlens; (void)diffs;
	if (n == 0)
		return;
	dtfl_reduce_emit_pair(out, vals[0], 4, &one);
}

/* map (node, dist) -> dist. */
static void
label_to_dist(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 4, 4);
}

static void
inspect_dist(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	printf("\t(%llu, %lld)\n", (unsigned long long)dtfl_get_u32(rec),
	    (long long)d);
}

/* ------------------------------------------------------------------ */
/* The dynamic BFS loop.                                               */
/* ------------------------------------------------------------------ */

typedef struct bfs_ctx {
	dtfl_collection *edges;	/* (u32, u32) outer */
	dtfl_collection *nodes;	/* (u32 node, u32 dist) outer */
	dtfl_collection *result;	/* out: labels, outer */
	const dtfl_type *u32;
	const dtfl_type *pair;	/* (u32, u32) */
} bfs_ctx;

static void
build_loop(dtfl_scope *inner, void *arg)
{
	bfs_ctx *c = arg;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *edges = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *nodes = dtfl_collection_enter(c->nodes, inner);
	dtfl_collection *label;
	/* Variable::new_from(nodes, Product { inner: feedback_summary(1, 1) }).
	 * new_at builds the plain Variable at PointStamp coordinate 1; the
	 * from-behaviour (concat the source, feed back its negation) is wired
	 * by hand, as the dynamic test does. */
	dtfl_variable *label_bind = dtfl_variable_new_at(inner, c->pair, i64, 1,
	    &label);
	dtfl_collection *label_src;
	dtfl_collection *reached, *body, *next, *leaving;
	dtfl_collection *neg, *fb;

	{
		dtfl_collection *fed = label;

		label_src = dtfl_collection_concat(fed, nodes);
		dtfl_collection_free(fed);
		label = NULL;
	}
	reached = dtfl_collection_join_map(label_src, edges, c->u32, c->u32,
	    c->u32, c->pair, reach, DTFL_NOCTX);
	body = dtfl_collection_concat(reached, nodes);
	next = dtfl_collection_reduce(body, c->u32, c->u32, c->u32, i64,
	    keep_min, DTFL_NOCTX);
	leaving = dtfl_collection_leave_dynamic(next, 1);
	c->result = dtfl_collection_leave(leaving);

	neg = dtfl_collection_negate(nodes);
	fb = dtfl_collection_concat(next, neg);
	dtfl_variable_set(label_bind, fb);

	dtfl_collection_free(neg);
	dtfl_collection_free(leaving);
	dtfl_collection_free(next);
	dtfl_collection_free(body);
	dtfl_collection_free(reached);
	dtfl_collection_free(label_src);
	dtfl_collection_free(nodes);
	dtfl_collection_free(edges);
	dtfl_diff_free(i64);
}

typedef struct build_ctx {
	dtfl_input_session *roots;
	dtfl_input_session *edges;
	dtfl_probe *probe;
	bfs_ctx *bc;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	bfs_ctx *bc = c->bc;
	dtfl_coord u32c = { DTFL_COORD_UNSIGNED, 32 };
	dtfl_time_class *outer = dtfl_time_u32();
	dtfl_time_class *ps = dtfl_time_pointstamp(u32c, 4);
	dtfl_time_class *inner = dtfl_time_product(outer, ps);
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	dtfl_collection *nodes = dtfl_collection_map(roots, bc->pair,
	    root_to_label, DTFL_NOCTX);
	dtfl_collection *dists, *con, *ins, *probed;

	bc->edges = edges;
	bc->nodes = nodes;
	bc->result = NULL;
	dtfl_scope_scoped(scope, "bfs", inner, build_loop, bc);

	dists = dtfl_collection_map(bc->result, bc->u32, label_to_dist,
	    DTFL_NOCTX);
	con = dtfl_collection_consolidate(dists);
	ins = dtfl_collection_inspect(con, inspect_dist, DTFL_NOCTX);
	probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(dists);
	dtfl_collection_free(bc->result);
	dtfl_collection_free(nodes);
	dtfl_collection_free(edges);
	dtfl_collection_free(roots);
	dtfl_time_class_free(inner);
	dtfl_time_class_free(ps);
	dtfl_time_class_free(outer);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u32();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	build_ctx c;
	bfs_ctx bc;
	uint32_t root = 0;
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	memset(&bc, 0, sizeof bc);
	bc.u32 = u32;
	bc.pair = pair;
	c.roots = dtfl_input_session_new(u32, i64);
	c.edges = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	c.bc = &bc;
	dtfl_worker_dataflow(w, "dynamic", tc, build, &c);

	if (dtfl_worker_index(w) == 0) {
		unsigned char buf[8];

		dtfl_put_u32(buf, root);
		dtfl_input_session_insert(c.roots, buf, 4);
		for (i = 0; i < FIXED_E; i++) {
			dtfl_put_u32(buf, fixed_edges[i][0]);
			dtfl_put_u32(buf + 4, fixed_edges[i][1]);
			dtfl_input_session_insert(c.edges, buf, sizeof buf);
		}
	}
	dtfl_input_session_advance_to(c.roots, &one);
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_flush(c.roots);
	dtfl_input_session_flush(c.edges);
	while (dtfl_probe_less_than(c.probe, dtfl_input_session_time(c.edges)))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.roots);
	dtfl_input_session_close(c.edges);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	/* Positional args (nodes edges batch rounds inspect) kept for parity. */
	int skip = argc > 5 ? 5 : 0;
	dtfl_config *cfg = ex_config(argc, argv, skip);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
