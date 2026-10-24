/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   experiments/src/bin/graphs.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * bench/experiments/graphs.c: the graphs multi-phase experiment.  It
 * shares one arrange_by_key().trace across three dataflows on the same
 * worker, timing each phase:
 *
 *   LOAD   build a (src, dst) collection in dataflow 1, arrange it by key,
 *          keep the trace, insert `edges` random edges over `nodes`, close
 *          the input and drain.  Times the graph-load rate.
 *   REACH  in a new dataflow, import the trace and run reachability from
 *          root 0: iterate the reachable set, join the frontier (arranged
 *          by self) against the graph trace to reach each destination,
 *          concat the root back in, distinct.  Times the fixed point.
 *   BFS    in a third dataflow, import the trace and run breadth-first
 *          distance labeling from root 0: iterate (node -> dist), join the
 *          frontier against the graph trace to reach each destination at
 *          dist + 1, concat the root, reduce to the minimum distance.
 *
 * reach() and bfs() port the helper functions of the same name in
 * graphs.rs; each imports the shared trace into the phase's scope
 * (trace.import(scope)) and iterates against it, which is the point of the
 * workload: one index built once in phase 1 serves both later phases.
 *
 * A benchmark, not a test: not run by make check.  It uses the public API
 * and a deterministic xorshift64* RNG (upstream seeds a StdRng per worker),
 * so a run is reproducible given the arguments.  Upstream reads a random
 * graph from a GraphMMap; this port builds the graph in memory from the
 * RNG, matching arrange.c.  With `nodes <= 32` it also prints the reach set
 * and the bfs distances so a tiny run is hand-checkable.
 *
 * Upstream's dataflows use a unit () top time, which makes the iterative
 * Product<(), u32> inner time totally ordered, so reach ends in
 * distinct_total.  This port uses a u64 top time so the graph input
 * advances through normal epochs; the inner Product<u64, u32> is then not
 * totally ordered, so reach ends in the general distinct and bfs in the
 * general reduce (the already-ported bfs.c and the reachability plugin make
 * the same choice).  Both compute the identical fixed point.
 *
 * Run as: bench/experiments/graphs [nodes [edges]]
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A deterministic xorshift64* RNG (upstream seeds a StdRng per worker). */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t
next_node(uint64_t nodes)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (rng_state * 0x2545F4914F6CDD1DULL) % nodes;
}

static double
secs_since(struct timespec t0)
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
}

/* ------------------------------------------------------------------ */
/* reach: nodes reachable from the roots (graphs.rs reach).            */
/* ------------------------------------------------------------------ */

/*
 * join_core(inner.arrange_by_self(), graph, |_src, &(), &dst| Some(dst)):
 * key is the source node, val1 is unit (from the frontier arranged by
 * self), val2 is the edge destination (from the graph trace).  Emit dst.
 * Upstream writes graph.join_core(inner...); this port puts the in-scope
 * frontier arrangement first (a1) and the imported graph trace second
 * (a2), matching the reachability plugin, so the join's output capability
 * tracks the in-scope operand.
 */
static void
emit_dst(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)v1; (void)v1l; (void)v2l;
	dtfl_join_emit_record(out, v2, 8);
}

typedef struct reach_ctx {
	dtfl_arranged *graph;	/* imported shared graph trace, outer scope */
	dtfl_collection *roots;	/* the root nodes, outer scope */
	dtfl_diff *i64;
} reach_ctx;

static dtfl_collection *
reach_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	reach_ctx *c = vctx;
	dtfl_arranged *graph = dtfl_arranged_enter(c->graph, inner);
	/* inner.arrange_by_self(): key is the node, value is unit. */
	dtfl_arranged *inner_arr = dtfl_collection_arrange_by_key(var,
	    dtfl_type_u64(), dtfl_type_unit(), "reach frontier");
	dtfl_collection *reached = dtfl_arranged_join_core(inner_arr, graph,
	    dtfl_type_u64(), emit_dst, DTFL_NOCTX);
	dtfl_collection *roots = dtfl_collection_enter(c->roots, inner);
	dtfl_collection *merged = dtfl_collection_concat(reached, roots);
	dtfl_collection *distinct = dtfl_collection_distinct(merged,
	    c->i64);

	dtfl_collection_free(merged);
	dtfl_collection_free(roots);
	dtfl_collection_free(reached);
	dtfl_arranged_free(inner_arr);
	dtfl_arranged_free(graph);
	return distinct;
}

/* reach(graph, roots): graphs.rs reach, roots.clone().iterate(...). */
static dtfl_collection *
reach(dtfl_arranged *graph, dtfl_collection *roots, dtfl_diff *i64)
{
	reach_ctx c = { graph, roots, i64 };

	return dtfl_collection_iterate(roots, reach_logic, DTFL_CTX(&c, NULL));
}

/* ------------------------------------------------------------------ */
/* bfs: breadth-first distance labeling (graphs.rs bfs).               */
/* ------------------------------------------------------------------ */

/* map a bare u64 node to (node, 0): the root at distance 0. */
static void
seed_root(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[12];

	(void)ctx; (void)len;
	memcpy(buf, rec, 8);		/* node */
	dtfl_put_u32(buf + 8, 0);	/* distance 0 */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/*
 * join_core(inner.arrange_by_key(), graph, |_src, &dist, &dest|
 * [(dest, dist + 1)]): key is the source node, val1 is the frontier
 * distance (u32), val2 is the edge destination (from the graph trace).
 * Emit (dest, dist + 1); the add is u32 and wraps at 2^32, as upstream's
 * u32 distance does.  Upstream writes graph.join_core(inner...); this port
 * puts the in-scope frontier first (a1) and the imported graph second
 * (a2), so the join's output capability tracks the in-scope operand.
 */
static void
step_edge(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[12];
	uint32_t dist = dtfl_get_u32(v1);

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 8);			/* destination node */
	dtfl_put_u32(buf + 8, dist + 1);	/* distance + 1 (u32 wrap) */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * reduce(|_key, input, output| output.push((*input[0].0, 1))): the input
 * multiset of distances for one node is sorted by value, so the smallest
 * distance is first.  Keep it with diff 1.
 */
static void
keep_min(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_reduce_emit_pair(out, vals[0], 4, &one);
}

typedef struct bfs_ctx {
	dtfl_arranged *graph;	/* imported shared graph trace, outer scope */
	dtfl_collection *roots;	/* seeded (node, 0) roots, outer scope */
	dtfl_diff *i64;
} bfs_ctx;

static dtfl_collection *
bfs_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	bfs_ctx *c = vctx;
	dtfl_arranged *graph = dtfl_arranged_enter(c->graph, inner);
	/* inner.arrange_by_key(): key is the node, value is the u32 dist. */
	dtfl_arranged *inner_arr = dtfl_collection_arrange_by_key(var,
	    dtfl_type_u64(), dtfl_type_u32(), "bfs frontier");
	const dtfl_type *nd = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u32());
	dtfl_collection *stepped = dtfl_arranged_join_core(inner_arr, graph,
	    nd, step_edge, DTFL_NOCTX);
	dtfl_collection *roots = dtfl_collection_enter(c->roots, inner);
	dtfl_collection *merged = dtfl_collection_concat(stepped, roots);
	dtfl_collection *reduced = dtfl_collection_reduce(merged,
	    dtfl_type_u64(), dtfl_type_u32(), dtfl_type_u32(), c->i64, keep_min,
	    DTFL_NOCTX);

	dtfl_collection_free(merged);
	dtfl_collection_free(roots);
	dtfl_collection_free(stepped);
	dtfl_type_free(nd);
	dtfl_arranged_free(inner_arr);
	dtfl_arranged_free(graph);
	return reduced;
}

/* bfs(graph, roots): graphs.rs bfs, roots.map(|r| (r,0)).iterate(...). */
static dtfl_collection *
bfs(dtfl_arranged *graph, dtfl_collection *roots, dtfl_diff *i64)
{
	const dtfl_type *nd = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u32());
	dtfl_collection *seeded = dtfl_collection_map(roots, nd, seed_root,
	    DTFL_NOCTX);
	bfs_ctx c = { graph, seeded, i64 };
	dtfl_collection *result = dtfl_collection_iterate(seeded, bfs_logic,
	    DTFL_CTX(&c, NULL));

	dtfl_collection_free(seeded);
	dtfl_type_free(nd);
	return result;
}

/* ------------------------------------------------------------------ */
/* inspect sinks for the hand-checkable small-graph output.            */
/* ------------------------------------------------------------------ */

/* reach inspect: print "\treach <node> <diff>" for a node in the set. */
static void
inspect_reach(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	printf("\treach\t%llu\t%lld\n", (unsigned long long)dtfl_get_u64(rec),
	    (long long)d);
}

/* bfs inspect: print "\tbfs <node> <dist> <diff>" for each label. */
static void
inspect_bfs(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	printf("\tbfs\t%llu\t%llu\t%lld\n",
	    (unsigned long long)dtfl_get_u64(rec),
	    (unsigned long long)dtfl_get_u32((const unsigned char *)rec + 8),
	    (long long)d);
}

/* ------------------------------------------------------------------ */
/* The multi-phase worker.                                             */
/* ------------------------------------------------------------------ */

typedef struct run_cfg {
	uint64_t nodes, edges;
} run_cfg;

/* Phase 1: build (src, dst) collection, arrange by key, keep the trace. */
typedef struct load_ctx {
	dtfl_input_session *graph;
	dtfl_trace *trace;	/* out: the shared graph trace */
	dtfl_probe *probe;
} load_ctx;

static void
build_load(dtfl_scope *scope, void *arg)
{
	load_ctx *c = arg;
	const dtfl_type *edge = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_collection *data = dtfl_input_session_to_collection(c->graph,
	    scope);
	dtfl_arranged *arr = dtfl_collection_arrange_by_key(data,
	    dtfl_type_u64(), dtfl_type_u64(), "graph");
	/* as_collection(NULL fn) re-emits the stored key||value record, so its
	 * out_type is the (src, dst) edge type; probe it to drain the load. */
	dtfl_collection *ac = dtfl_arranged_as_collection(arr, edge, NULL,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ac, c->probe);

	c->trace = dtfl_arranged_trace(arr);
	dtfl_collection_free(probed);
	dtfl_collection_free(ac);
	dtfl_arranged_free(arr);
	dtfl_collection_free(data);
	dtfl_type_free(edge);
}

/* Phase 2: import the trace, run reach from root 0. */
typedef struct reach_phase_ctx {
	dtfl_trace *trace;
	dtfl_input_session *roots;
	dtfl_probe *probe;
	dtfl_diff *i64;
	int verbose;
} reach_phase_ctx;

static void
build_reach(dtfl_scope *scope, void *arg)
{
	reach_phase_ctx *c = arg;
	dtfl_arranged *graph = dtfl_trace_import(c->trace, scope, "reach graph");
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	dtfl_collection *result = reach(graph, roots, c->i64);
	dtfl_collection *tail = result;
	dtfl_collection *ins = NULL;
	dtfl_collection *probed;

	if (c->verbose) {
		dtfl_collection *con = dtfl_collection_consolidate(result);

		ins = dtfl_collection_inspect(con, inspect_reach, DTFL_NOCTX);
		dtfl_collection_free(con);
		tail = ins;
	}
	probed = dtfl_collection_probe_with(tail, c->probe);
	dtfl_collection_free(probed);
	if (ins != NULL)
		dtfl_collection_free(ins);
	dtfl_collection_free(result);
	dtfl_collection_free(roots);
	dtfl_arranged_free(graph);
}

/* Phase 3: import the trace, run bfs from root 0. */
typedef struct bfs_phase_ctx {
	dtfl_trace *trace;
	dtfl_input_session *roots;
	dtfl_probe *probe;
	dtfl_diff *i64;
	int verbose;
} bfs_phase_ctx;

static void
build_bfs(dtfl_scope *scope, void *arg)
{
	bfs_phase_ctx *c = arg;
	dtfl_arranged *graph = dtfl_trace_import(c->trace, scope, "bfs graph");
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	dtfl_collection *result = bfs(graph, roots, c->i64);
	dtfl_collection *tail = result;
	dtfl_collection *ins = NULL;
	dtfl_collection *probed;

	if (c->verbose) {
		dtfl_collection *con = dtfl_collection_consolidate(result);

		ins = dtfl_collection_inspect(con, inspect_bfs, DTFL_NOCTX);
		dtfl_collection_free(con);
		tail = ins;
	}
	probed = dtfl_collection_probe_with(tail, c->probe);
	dtfl_collection_free(probed);
	if (ins != NULL)
		dtfl_collection_free(ins);
	dtfl_collection_free(result);
	dtfl_collection_free(roots);
	dtfl_arranged_free(graph);
}

/*
 * A fixed directed graph used when `nodes <= 32`, so a tiny run is
 * hand-checkable.  From root 0 the reach set is {0,1,2,3,4,5} (node 6 is
 * not reachable), and the bfs distances are 0:0 1:1 2:1 3:2 4:2 5:3.
 */
static const uint64_t fixed_edges[][2] = {
	{ 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 2, 4 }, { 3, 5 },
	{ 4, 5 }, { 6, 0 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])

static void
settle(dtfl_worker *w, dtfl_input_session *in, dtfl_probe *probe)
{
	uint64_t t = *dtfl_input_session_time(in) + 1;

	dtfl_input_session_advance_to(in, &t);
	dtfl_input_session_flush(in);
	while (dtfl_probe_less_than(probe, dtfl_input_session_time(in)))
		dtfl_worker_step(w);
}

/*
 * Settle `in`, then close it.  Used for the per-phase roots inputs
 * (roots.close() in graphs.rs).  No unbounded step loop afterwards: the
 * shared graph input is still open, so dtfl_worker_step never reports the
 * worker idle until the graph input is closed at the very end.
 */
static void
drain(dtfl_worker *w, dtfl_input_session *in, dtfl_probe *probe)
{
	settle(w, in, probe);
	dtfl_input_session_close(in);
}

static void
worker(dtfl_worker *w, void *arg)
{
	run_cfg *cfg = arg;
	int verbose = cfg->nodes <= 32;
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *edge = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	load_ctx lc;
	reach_phase_ctx rc;
	bfs_phase_ctx bc;
	struct timespec t0;
	uint64_t i;
	double s;

	/* Phase 1: load the graph and arrange it by key. */
	lc.graph = dtfl_input_session_new(edge, i64);
	lc.probe = dtfl_probe_new();
	lc.trace = NULL;
	dtfl_worker_dataflow(w, "graphs load", tc, build_load, &lc);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (verbose) {
		for (i = 0; i < FIXED_E; i++) {
			unsigned char buf[16];

			dtfl_put_u64(buf, fixed_edges[i][0]);
			dtfl_put_u64(buf + 8, fixed_edges[i][1]);
			dtfl_input_session_insert(lc.graph, buf, sizeof buf);
		}
	} else {
		for (i = 0; i < cfg->edges; i++) {
			unsigned char buf[16];

			dtfl_put_u64(buf, next_node(cfg->nodes));
			dtfl_put_u64(buf + 8, next_node(cfg->nodes));
			dtfl_input_session_insert(lc.graph, buf, sizeof buf);
		}
	}
	/* Phase 1 keeps the graph input open: closing it would seal the
	 * trace to the empty frontier before the reach and bfs phases import
	 * it.  Settle it so the arrangement reflects every edge, then run the
	 * importing phases, and close it at the end. */
	settle(w, lc.graph, lc.probe);
	s = secs_since(t0);
	printf("GRAPHS\tLOAD\t1\t%.1f\n",
	    (double)(verbose ? FIXED_E : cfg->edges) / s);
	fflush(stdout);

	/* Phase 2: reachability from root 0. */
	rc.trace = lc.trace;
	rc.roots = dtfl_input_session_new(dtfl_type_u64(), i64);
	rc.probe = dtfl_probe_new();
	rc.i64 = i64;
	rc.verbose = verbose;
	dtfl_worker_dataflow(w, "graphs reach", tc, build_reach, &rc);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (dtfl_worker_index(w) == 0) {
		unsigned char buf[8];

		dtfl_put_u64(buf, 0);
		dtfl_input_session_insert(rc.roots, buf, sizeof buf);
	}
	drain(w, rc.roots, rc.probe);
	printf("GRAPHS\tREACH\t1\t%.3f\n", secs_since(t0));
	fflush(stdout);

	/* Phase 3: breadth-first distance labeling from root 0. */
	bc.trace = lc.trace;
	bc.roots = dtfl_input_session_new(dtfl_type_u64(), i64);
	bc.probe = dtfl_probe_new();
	bc.i64 = i64;
	bc.verbose = verbose;
	dtfl_worker_dataflow(w, "graphs bfs", tc, build_bfs, &bc);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (dtfl_worker_index(w) == 0) {
		unsigned char buf[8];

		dtfl_put_u64(buf, 0);
		dtfl_input_session_insert(bc.roots, buf, sizeof buf);
	}
	drain(w, bc.roots, bc.probe);
	printf("GRAPHS\tBFS\t1\t%.3f\n", secs_since(t0));
	fflush(stdout);

	dtfl_input_session_close(lc.graph);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(bc.probe);
	dtfl_probe_free(rc.probe);
	dtfl_trace_free(lc.trace);
	dtfl_probe_free(lc.probe);
	dtfl_type_free(edge);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	run_cfg cfg;

	cfg.nodes = argc > 1 ? strtoull(argv[1], NULL, 10) : 8;
	cfg.edges = argc > 2 ? strtoull(argv[2], NULL, 10) : 8;
	return dtfl_execute_directly(worker, &cfg);
}
