/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/pagerank.c: mirrors differential-dataflow/examples/pagerank.rs.
 *
 * A weighted collection whose weight at each node is proportional to its
 * PageRank: initialize surfers at each node, then in an iterative scope
 * match each surfer with its source out-degree, scale the count down by
 * 5/6, push surfers along edges, and blend in a constant reset each round
 * until the integer surfer counts reach a fixed point.  Upstream reads the
 * graph from a GraphMMap file, caps the loop with an inner-timestamp filter
 * (t.inner < iters), and reports timings; a C runtime cannot reproduce that
 * stream and the public collection filter sees only the record, not the
 * time, so this port feeds a small fixed graph and runs the loop to its
 * natural integer fixed point (upstream's iters == 0 path), inspecting the
 * final (node, weight) multiset.
 */
#include "example_args.h"

#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

/*
 * A fixed directed graph on five nodes.  Out-degrees:
 *   0:2 (->1,->2)  1:1 (->2)  2:1 (->0)  3:1 (->0)  4:0.
 */
static const uint32_t fixed_edges[][2] = {
	{ 0, 1 }, { 0, 2 }, { 1, 2 }, { 2, 0 }, { 3, 0 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])

/* flat_map (x, y) -> {x, y}: both endpoints of an edge are nodes. */
static void
edge_endpoints(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 4);
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 4, 4);
}

/* map (src, dst) -> src. */
static void
edge_to_src(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 4);
}

/* map (node, degr) -> node (drop the degree after the threshold). */
static void
drop_degr(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 4);
}

/* map (src, dst) -> dst (where surfers land after a push). */
static void
edge_to_dst(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 4, 4);
}

/* explode node -> (node, factor): assign each node `factor` surfers. */
static void
explode_inits(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	int64_t factor = 6000000;

	(void)ctx; (void)len;
	dtfl_collection_emit_pair(out, rec, 4, &factor);
}

static void
explode_reset(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	int64_t factor = 1000000;

	(void)ctx; (void)len;
	dtfl_collection_emit_pair(out, rec, 4, &factor);
}

/*
 * threshold on the semijoin (node, degr) with accumulated surfer count
 * `rank`: keep (5 * rank) / (6 * degr) copies.  The key is node || degr;
 * degr is the u32 following the node.
 */
static bool
scale_down(void *ctx, const void *key, size_t klen, const void *acc_diff,
    void *out)
{
	int64_t rank, degr, scaled;

	(void)ctx; (void)klen;
	memcpy(&rank, acc_diff, sizeof rank);
	degr = dtfl_get_i64((const unsigned char *)key + 4);
	if (degr == 0)
		return false;
	scaled = (5 * rank) / (6 * degr);
	if (scaled == 0)
		return false;
	memcpy(out, &scaled, sizeof scaled);
	return true;
}

static void
inspect_rank(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	printf("(%llu, %lld)\n", (unsigned long long)dtfl_get_u32(rec),
	    (long long)d);
}

/* ------------------------------------------------------------------ */
/* The iterative PageRank loop.                                        */
/* ------------------------------------------------------------------ */

typedef struct pr_ctx {
	dtfl_collection *edges;	/* (u32 src, u32 dst), outer scope */
	dtfl_collection *nodes;	/* distinct u32 nodes, outer scope */
	dtfl_collection *degrs;	/* (u32 node, i64 count) via count, outer */
	dtfl_collection *result;	/* out: final ranks, outer scope */
	const dtfl_type *u32;
	const dtfl_type *pair;	/* (u32, u32) */
	const dtfl_type *node_degr;	/* (u32 node, i64 degr) */
} pr_ctx;

static void
build_loop(dtfl_scope *inner, void *arg)
{
	pr_ctx *c = arg;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *edges = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *nodes = dtfl_collection_enter(c->nodes, inner);
	dtfl_collection *degrs = dtfl_collection_enter(c->degrs, inner);
	dtfl_collection *inits = dtfl_collection_explode(nodes, c->u32, i64,
	    explode_inits, DTFL_NOCTX);
	dtfl_collection *reset = dtfl_collection_explode(nodes, c->u32, i64,
	    explode_reset, DTFL_NOCTX);
	dtfl_collection *ranks;
	dtfl_variable *ranks_bind = dtfl_variable_new_from(inits, &ranks);
	/* Match each surfer with its source degree and scale 5/6 down. */
	dtfl_collection *matched = dtfl_collection_semijoin(degrs, ranks,
	    c->u32, dtfl_type_i64(), c->node_degr);
	dtfl_collection *to_push_kv = dtfl_collection_threshold(matched, i64,
	    scale_down, DTFL_NOCTX);
	dtfl_collection *to_push = dtfl_collection_map(to_push_kv, c->u32,
	    drop_degr, DTFL_NOCTX);
	/* Push surfers along links, blend in the reset, consolidate. */
	dtfl_collection *pushed_kv = dtfl_collection_semijoin(edges, to_push,
	    c->u32, dtfl_type_u32(), c->pair);
	dtfl_collection *pushed_dst = dtfl_collection_map(pushed_kv, c->u32,
	    edge_to_dst, DTFL_NOCTX);
	dtfl_collection *blended = dtfl_collection_concat(pushed_dst, reset);
	dtfl_collection *pushed = dtfl_collection_consolidate(blended);

	c->result = dtfl_collection_leave(pushed);
	dtfl_variable_set(ranks_bind, pushed);

	dtfl_collection_free(blended);
	dtfl_collection_free(pushed_dst);
	dtfl_collection_free(pushed_kv);
	dtfl_collection_free(to_push);
	dtfl_collection_free(to_push_kv);
	dtfl_collection_free(matched);
	dtfl_collection_free(ranks);
	dtfl_collection_free(inits);
	dtfl_collection_free(reset);
	dtfl_collection_free(degrs);
	dtfl_collection_free(nodes);
	dtfl_collection_free(edges);
	dtfl_diff_free(i64);
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	pr_ctx *pc;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	pr_ctx *pc = c->pc;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *edges = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *endpoints = dtfl_collection_flat_map(edges, pc->u32,
	    edge_endpoints, DTFL_NOCTX);
	dtfl_collection *nodes = dtfl_collection_distinct(endpoints, i64);
	dtfl_collection *srcs = dtfl_collection_map(edges, pc->u32,
	    edge_to_src, DTFL_NOCTX);
	dtfl_collection *degrs = dtfl_collection_count(srcs, i64);
	dtfl_collection *con, *ins, *probed;

	pc->edges = edges;
	pc->nodes = nodes;
	pc->degrs = degrs;
	pc->result = NULL;
	dtfl_scope_iterative(scope, "pagerank", build_loop, pc);

	con = dtfl_collection_consolidate(pc->result);
	ins = dtfl_collection_inspect(con, inspect_rank, DTFL_NOCTX);
	probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(pc->result);
	dtfl_collection_free(degrs);
	dtfl_collection_free(srcs);
	dtfl_collection_free(nodes);
	dtfl_collection_free(endpoints);
	dtfl_collection_free(edges);
	dtfl_diff_free(i64);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u32();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	const dtfl_type *node_degr = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_i64());
	build_ctx c;
	pr_ctx pc;
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	memset(&pc, 0, sizeof pc);
	pc.u32 = u32;
	pc.pair = pair;
	pc.node_degr = node_degr;
	c.in = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	c.pc = &pc;
	dtfl_worker_dataflow(w, "pagerank", tc, build, &c);

	if (dtfl_worker_index(w) == 0)
		for (i = 0; i < FIXED_E; i++) {
			unsigned char buf[8];

			dtfl_put_u32(buf, fixed_edges[i][0]);
			dtfl_put_u32(buf + 4, fixed_edges[i][1]);
			dtfl_input_session_insert(c.in, buf, sizeof buf);
		}
	dtfl_input_session_advance_to(c.in, &one);
	dtfl_input_session_flush(c.in);
	while (dtfl_probe_less_than(c.probe, dtfl_input_session_time(c.in)))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_type_free(node_degr);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	/* Positional args (filename iterations inspect) are kept for parity. */
	int skip = argc > 3 ? 3 : 0;
	dtfl_config *cfg = ex_config(argc, argv, skip);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
