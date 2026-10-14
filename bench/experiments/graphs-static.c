/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   experiments/src/bin/graphs-static.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * bench/experiments/graphs-static.c: the static-graph experiment.  Like
 * graphs.c it shares one arrange_by_key().trace across dataflows, but the
 * graph is loaded once and never updated, and each later phase runs against
 * that frozen imported trace.  It times five phases:
 *
 *   FORWARD  build the forward (src -> dst) index: a (src, dst) collection,
 *            arrange_by_key, keep the trace.
 *   REACH    import the forward trace, reachability from the root: iterate
 *            a Variable, join the frontier (arranged by self) against the
 *            graph trace, concat the root, distinct.
 *   BFS      import the forward trace, breadth-first distance labeling from
 *            the root: iterate a Variable, join the frontier (arranged by
 *            key) against the graph trace at dist + 1, concat the root,
 *            reduce to the minimum distance.
 *   REVERSE  import the forward trace, read it back as a collection with
 *            (k, v) -> (v, k), arrange_by_key, keep the reverse trace.
 *   CC       import both the forward and reverse traces and label every node
 *            with the minimum node id in its connected component.
 *
 * The reach, bfs and cc helpers port the functions of the same name in
 * graphs-static.rs; each imports the shared trace into the phase's scope
 * (trace.import(scope)) and iterates with an explicit Variable (upstream's
 * outer.iterative + Variable::new + inner.set + result.leave).  The point of
 * the workload is that one static index serves every later phase.
 *
 * Divergences from upstream, under the port's decisions:
 *  - Upstream reads a GraphMMap and seeds a single root; this port builds
 *    the graph in memory from a deterministic xorshift64* RNG (as arrange.c
 *    does) and roots at node 0.  A run is reproducible given the arguments.
 *  - Upstream's unit () top time makes the iterative inner time totally
 *    ordered, so reach ends in threshold_total.  This port uses a u64 top
 *    time so the graph input advances through normal epochs; the inner
 *    Product<u64, u32> is then not totally ordered, so reach ends in the
 *    general distinct and bfs in the general reduce.  Both compute the same
 *    fixed point.
 *  - When joining an in-scope loop arrangement against the imported trace,
 *    the in-scope operand is a1 and the imported trace a2 (as the
 *    reachability plugin does), so the join's output capability tracks the
 *    in-scope operand.
 *  - connected_components drops upstream's leading-zeros delay priority (a
 *    plain enter), the same simplification src/algo/propagate.c makes; the
 *    min-label fixed point is identical, only the round schedule differs.
 *
 * A benchmark, not a test: not run by make check.  With `nodes <= 32` it
 * also prints the reach set, the bfs distances and the component labels so
 * a tiny run is hand-checkable.
 *
 * Run as: bench/experiments/graphs-static [nodes [edges [root]]]
 */
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
/* reach: nodes reachable from the roots (graphs-static.rs reach).     */
/* ------------------------------------------------------------------ */

/*
 * join_core(inner.arrange_by_self(), graph, |_src, &(), &dst| Some(dst)):
 * key is the source node, val1 is unit (the in-scope frontier), val2 is the
 * edge destination (the imported graph trace).  Emit dst.
 */
static void
emit_dst(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)v1; (void)v1l; (void)v2l;
	dtfl_join_emit_record(out, v2, 8);
}

typedef struct reach_ctx {
	dtfl_trace *trace;	/* the forward graph trace */
	dtfl_collection *roots;	/* the root nodes, outer scope */
	dtfl_diff *i64;
	dtfl_collection *result;	/* out: reachable set, outer scope */
} reach_ctx;

static void
reach_build(dtfl_scope *inner, void *arg)
{
	reach_ctx *c = arg;
	dtfl_scope *outer = dtfl_scope_parent(inner);
	dtfl_arranged *graph = dtfl_trace_import(c->trace, outer, "reach graph");
	dtfl_arranged *graph_in = dtfl_arranged_enter(graph, inner);
	dtfl_collection *roots = dtfl_collection_enter(c->roots, inner);
	dtfl_collection *inner_coll;
	dtfl_variable *var = dtfl_variable_new(inner, dtfl_type_u64(), c->i64,
	    &inner_coll);
	dtfl_arranged *inner_arr = dtfl_collection_arrange_by_key(inner_coll,
	    dtfl_type_u64(), dtfl_type_unit(), "reach frontier");
	dtfl_collection *reached = dtfl_arranged_join_core(inner_arr, graph_in,
	    dtfl_type_u64(), emit_dst, DTFL_NOCTX);
	dtfl_collection *merged = dtfl_collection_concat(reached, roots);
	dtfl_collection *result = dtfl_collection_distinct(merged, c->i64);

	c->result = dtfl_collection_leave(result);
	dtfl_variable_set(var, result);

	dtfl_collection_free(merged);
	dtfl_collection_free(reached);
	dtfl_arranged_free(inner_arr);
	dtfl_collection_free(inner_coll);
	dtfl_collection_free(roots);
	dtfl_arranged_free(graph_in);
	dtfl_arranged_free(graph);
}

/* ------------------------------------------------------------------ */
/* bfs: breadth-first distance labeling (graphs-static.rs bfs).        */
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
 * distance (u32), val2 is the edge destination (the imported graph trace).
 * Emit (dest, dist + 1); the add is u32 and wraps at 2^32.
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
 * distances for one node are sorted by value, so the smallest is first.
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
	dtfl_trace *trace;	/* the forward graph trace */
	dtfl_collection *roots;	/* the root nodes, outer scope */
	dtfl_diff *i64;
	dtfl_collection *result;	/* out: (node, dist), outer scope */
} bfs_ctx;

static void
bfs_build(dtfl_scope *inner, void *arg)
{
	bfs_ctx *c = arg;
	dtfl_scope *outer = dtfl_scope_parent(inner);
	const dtfl_type *nd = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u32());
	dtfl_arranged *graph = dtfl_trace_import(c->trace, outer, "bfs graph");
	dtfl_arranged *graph_in = dtfl_arranged_enter(graph, inner);
	/* roots.map(|r| (r, 0)) in the outer scope, then enter. */
	dtfl_collection *seeded = dtfl_collection_map(c->roots, nd, seed_root,
	    DTFL_NOCTX);
	dtfl_collection *roots = dtfl_collection_enter(seeded, inner);
	dtfl_collection *inner_coll;
	dtfl_variable *var = dtfl_variable_new(inner, nd, c->i64, &inner_coll);
	dtfl_arranged *inner_arr = dtfl_collection_arrange_by_key(inner_coll,
	    dtfl_type_u64(), dtfl_type_u32(), "bfs frontier");
	dtfl_collection *stepped = dtfl_arranged_join_core(inner_arr, graph_in,
	    nd, step_edge, DTFL_NOCTX);
	dtfl_collection *merged = dtfl_collection_concat(stepped, roots);
	dtfl_collection *result = dtfl_collection_reduce(merged,
	    dtfl_type_u64(), dtfl_type_u32(), dtfl_type_u32(), c->i64, keep_min,
	    DTFL_NOCTX);

	c->result = dtfl_collection_leave(result);
	dtfl_variable_set(var, result);

	dtfl_collection_free(merged);
	dtfl_collection_free(stepped);
	dtfl_arranged_free(inner_arr);
	dtfl_collection_free(inner_coll);
	dtfl_collection_free(roots);
	dtfl_collection_free(seeded);
	dtfl_arranged_free(graph_in);
	dtfl_arranged_free(graph);
	dtfl_type_free(nd);
}

/* ------------------------------------------------------------------ */
/* connected_components (graphs-static.rs connected_components).       */
/* ------------------------------------------------------------------ */

/* as_collection(|&k, &v| (v, k)): swap the stored (src, dst) to (dst, src). */
static void
swap_kv(void *ctx, const void *key, size_t klen, const void *val, size_t vlen,
    dtfl_arranged_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)klen; (void)vlen;
	memcpy(buf, val, 8);		/* v -> key */
	memcpy(buf + 8, key, 8);	/* k -> value */
	dtfl_arranged_emit_record(out, buf, sizeof buf);
}

/*
 * as_collection(|&k, &v| if k < v { Some(k) } ...) over an arrangement:
 * emit the key when it is the smaller endpoint, as a (node, node) seed.
 * The returned collection is filtered to the k < v rows and mapped to
 * (min, min); here we emit (k, k) when k < v and nothing otherwise, which
 * is upstream's nodes_f / nodes_r followed by map(|x| (x, x)).
 */
static void
seed_label(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)klen; (void)vlen;
	if (dtfl_get_u64(key) < dtfl_get_u64(val)) {
		memcpy(buf, key, 8);		/* node */
		memcpy(buf + 8, key, 8);	/* label = node */
		dtfl_arranged_emit_record(out, buf, sizeof buf);
	}
}

/*
 * join_core(labels.arrange_by_key(), edges, |_k, &label, &d| Some((d, label))):
 * key is the node, val1 is its current label (u64, in-scope), val2 is the
 * edge destination (the imported trace).  Emit (dst, label): the label flows
 * along the edge.
 */
static void
flow_label(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 8);		/* destination node */
	memcpy(buf + 8, v1, 8);		/* label */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* reduce(min): labels for one node sorted ascending, keep the smallest. */
static void
keep_min_label(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_reduce_emit_pair(out, vals[0], 8, &one);
}

typedef struct cc_ctx {
	dtfl_trace *forward;	/* forward graph trace */
	dtfl_trace *reverse;	/* reverse graph trace */
	dtfl_collection *nodes;	/* (node, node) seeds, outer scope */
	const dtfl_type *nl;	/* (u64 node, u64 label) record type */
	dtfl_diff *i64;
	dtfl_collection *result;	/* out: (node, label), outer scope */
} cc_ctx;

static void
cc_build(dtfl_scope *inner, void *arg)
{
	cc_ctx *c = arg;
	dtfl_scope *outer = dtfl_scope_parent(inner);
	dtfl_arranged *fwd = dtfl_trace_import(c->forward, outer, "cc forward");
	dtfl_arranged *rev = dtfl_trace_import(c->reverse, outer, "cc reverse");
	dtfl_arranged *fwd_in = dtfl_arranged_enter(fwd, inner);
	dtfl_arranged *rev_in = dtfl_arranged_enter(rev, inner);
	dtfl_collection *nodes = dtfl_collection_enter(c->nodes, inner);
	dtfl_collection *inner_coll;
	dtfl_variable *var = dtfl_variable_new(inner, c->nl, c->i64,
	    &inner_coll);
	/* labels = (prior labels, concat nodes) reduced to the min per node. */
	dtfl_collection *merged = dtfl_collection_concat(inner_coll, nodes);
	dtfl_collection *labels = dtfl_collection_reduce(merged,
	    dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64(), c->i64,
	    keep_min_label, DTFL_NOCTX);
	dtfl_arranged *labels_arr = dtfl_collection_arrange_by_key(labels,
	    dtfl_type_u64(), dtfl_type_u64(), "cc labels");
	dtfl_collection *f_prop = dtfl_arranged_join_core(labels_arr, fwd_in,
	    c->nl, flow_label, DTFL_NOCTX);
	dtfl_collection *r_prop = dtfl_arranged_join_core(labels_arr, rev_in,
	    c->nl, flow_label, DTFL_NOCTX);
	dtfl_collection *props = dtfl_collection_concat(f_prop, r_prop);

	c->result = dtfl_collection_leave(labels);
	dtfl_variable_set(var, props);

	dtfl_collection_free(f_prop);
	dtfl_collection_free(r_prop);
	dtfl_arranged_free(labels_arr);
	dtfl_collection_free(labels);
	dtfl_collection_free(merged);
	dtfl_collection_free(inner_coll);
	dtfl_collection_free(nodes);
	dtfl_arranged_free(fwd_in);
	dtfl_arranged_free(rev_in);
	dtfl_arranged_free(fwd);
	dtfl_arranged_free(rev);
}

/* ------------------------------------------------------------------ */
/* inspect sinks for the hand-checkable small-graph output.            */
/* ------------------------------------------------------------------ */

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

static void
inspect_cc(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	printf("\tcc\t%llu\t%llu\t%lld\n",
	    (unsigned long long)dtfl_get_u64(rec),
	    (unsigned long long)dtfl_get_u64((const unsigned char *)rec + 8),
	    (long long)d);
}

/* ------------------------------------------------------------------ */
/* Phases.                                                             */
/* ------------------------------------------------------------------ */

/* Phase 1: build (src, dst) collection, arrange by key, keep the trace. */
typedef struct forward_ctx {
	dtfl_input_session *graph;
	dtfl_probe *probe;
	dtfl_trace *trace;	/* out */
} forward_ctx;

static void
build_forward(dtfl_scope *scope, void *arg)
{
	forward_ctx *c = arg;
	const dtfl_type *edge = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_collection *data = dtfl_input_session_to_collection(c->graph,
	    scope);
	dtfl_arranged *arr = dtfl_collection_arrange_by_key(data,
	    dtfl_type_u64(), dtfl_type_u64(), "forward");
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

/* Phase 2: import the forward trace, run reach from the root. */
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
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	reach_ctx rc = { c->trace, roots, c->i64, NULL };
	dtfl_collection *tail;
	dtfl_collection *ins = NULL;
	dtfl_collection *probed;

	dtfl_scope_iterative(scope, "reach", reach_build, &rc);
	tail = rc.result;
	if (c->verbose) {
		dtfl_collection *con = dtfl_collection_consolidate(rc.result);

		ins = dtfl_collection_inspect(con, inspect_reach, DTFL_NOCTX);
		dtfl_collection_free(con);
		tail = ins;
	}
	probed = dtfl_collection_probe_with(tail, c->probe);
	dtfl_collection_free(probed);
	if (ins != NULL)
		dtfl_collection_free(ins);
	dtfl_collection_free(rc.result);
	dtfl_collection_free(roots);
}

/* Phase 3: import the forward trace, run bfs from the root. */
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
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	bfs_ctx bc = { c->trace, roots, c->i64, NULL };
	dtfl_collection *tail;
	dtfl_collection *ins = NULL;
	dtfl_collection *probed;

	dtfl_scope_iterative(scope, "bfs", bfs_build, &bc);
	tail = bc.result;
	if (c->verbose) {
		dtfl_collection *con = dtfl_collection_consolidate(bc.result);

		ins = dtfl_collection_inspect(con, inspect_bfs, DTFL_NOCTX);
		dtfl_collection_free(con);
		tail = ins;
	}
	probed = dtfl_collection_probe_with(tail, c->probe);
	dtfl_collection_free(probed);
	if (ins != NULL)
		dtfl_collection_free(ins);
	dtfl_collection_free(bc.result);
	dtfl_collection_free(roots);
}

/* Phase 4: import the forward trace, build the reverse index. */
typedef struct reverse_ctx {
	dtfl_trace *forward;
	dtfl_probe *probe;
	dtfl_trace *trace;	/* out: reverse trace */
} reverse_ctx;

static void
build_reverse(dtfl_scope *scope, void *arg)
{
	reverse_ctx *c = arg;
	const dtfl_type *edge = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_arranged *fwd = dtfl_trace_import(c->forward, scope, "reverse in");
	dtfl_collection *swapped = dtfl_arranged_as_collection(fwd, edge,
	    swap_kv, DTFL_NOCTX);
	dtfl_arranged *rev = dtfl_collection_arrange_by_key(swapped,
	    dtfl_type_u64(), dtfl_type_u64(), "reverse");
	dtfl_collection *ac = dtfl_arranged_as_collection(rev, edge, NULL,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ac, c->probe);

	c->trace = dtfl_arranged_trace(rev);
	dtfl_collection_free(probed);
	dtfl_collection_free(ac);
	dtfl_arranged_free(rev);
	dtfl_collection_free(swapped);
	dtfl_arranged_free(fwd);
	dtfl_type_free(edge);
}

/* Phase 5: import both traces, run connected components. */
typedef struct cc_phase_ctx {
	dtfl_trace *forward;
	dtfl_trace *reverse;
	dtfl_probe *probe;
	dtfl_diff *i64;
	int verbose;
} cc_phase_ctx;

static void
build_cc(dtfl_scope *scope, void *arg)
{
	cc_phase_ctx *c = arg;
	const dtfl_type *nl = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	/* nodes = (nodes_f concat nodes_r).consolidate().map(|x| (x, x)): seed
	 * each endpoint that is the smaller of an edge with (node, node). */
	dtfl_arranged *fwd = dtfl_trace_import(c->forward, scope, "cc seed f");
	dtfl_arranged *rev = dtfl_trace_import(c->reverse, scope, "cc seed r");
	dtfl_collection *seed_f = dtfl_arranged_as_collection(fwd, nl,
	    seed_label, DTFL_NOCTX);
	dtfl_collection *seed_r = dtfl_arranged_as_collection(rev, nl,
	    seed_label, DTFL_NOCTX);
	dtfl_collection *cat = dtfl_collection_concat(seed_f, seed_r);
	dtfl_collection *nodes = dtfl_collection_consolidate(cat);
	cc_ctx cc = { c->forward, c->reverse, nodes, nl, c->i64, NULL };
	dtfl_collection *tail;
	dtfl_collection *ins = NULL;
	dtfl_collection *probed;

	dtfl_scope_iterative(scope, "cc", cc_build, &cc);
	tail = cc.result;
	if (c->verbose) {
		dtfl_collection *con = dtfl_collection_consolidate(cc.result);

		ins = dtfl_collection_inspect(con, inspect_cc, DTFL_NOCTX);
		dtfl_collection_free(con);
		tail = ins;
	}
	probed = dtfl_collection_probe_with(tail, c->probe);
	dtfl_collection_free(probed);
	if (ins != NULL)
		dtfl_collection_free(ins);
	dtfl_collection_free(cc.result);
	dtfl_collection_free(nodes);
	dtfl_collection_free(cat);
	dtfl_collection_free(seed_f);
	dtfl_collection_free(seed_r);
	dtfl_arranged_free(fwd);
	dtfl_arranged_free(rev);
	dtfl_type_free(nl);
}

/* ------------------------------------------------------------------ */
/* Driver.                                                             */
/* ------------------------------------------------------------------ */

/*
 * A fixed directed graph used when `nodes <= 32`, so a tiny run is
 * hand-checkable.  From root 0 the reach set is {0,1,2,3,4,5} (node 6 is
 * not reachable along edges), the bfs distances are 0:0 1:1 2:1 3:2 4:2
 * 5:3, and every node (0..6) is in one undirected component labelled 0
 * (6 - 0 undirected via 6 -> 0).
 */
static const uint64_t fixed_edges[][2] = {
	{ 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 2, 4 }, { 3, 5 },
	{ 4, 5 }, { 6, 0 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])

typedef struct run_cfg {
	uint64_t nodes, edges, root;
} run_cfg;

static void
settle(dtfl_worker *w, dtfl_input_session *in, dtfl_probe *probe)
{
	uint64_t t = *dtfl_input_session_time(in) + 1;

	dtfl_input_session_advance_to(in, &t);
	dtfl_input_session_flush(in);
	while (dtfl_probe_less_than(probe, dtfl_input_session_time(in)))
		dtfl_worker_step(w);
}

/* Settle, then close the per-phase roots input (roots.close()). */
static void
drain(dtfl_worker *w, dtfl_input_session *in, dtfl_probe *probe)
{
	settle(w, in, probe);
	dtfl_input_session_close(in);
}

static void
seed_root_input(dtfl_worker *w, dtfl_input_session *roots, uint64_t root)
{
	if (dtfl_worker_index(w) == 0) {
		unsigned char buf[8];

		dtfl_put_u64(buf, root);
		dtfl_input_session_insert(roots, buf, sizeof buf);
	}
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
	forward_ctx fc;
	reach_phase_ctx rc;
	bfs_phase_ctx bc;
	reverse_ctx vc;
	cc_phase_ctx cc;
	struct timespec t0;
	uint64_t i;
	double s;

	/* Phase 1: forward index.  Keep the graph input open through every
	 * phase that imports its trace (closing it would seal the trace). */
	fc.graph = dtfl_input_session_new(edge, i64);
	fc.probe = dtfl_probe_new();
	fc.trace = NULL;
	dtfl_worker_dataflow(w, "forward", tc, build_forward, &fc);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (verbose) {
		for (i = 0; i < FIXED_E; i++) {
			unsigned char buf[16];

			dtfl_put_u64(buf, fixed_edges[i][0]);
			dtfl_put_u64(buf + 8, fixed_edges[i][1]);
			dtfl_input_session_insert(fc.graph, buf, sizeof buf);
		}
	} else {
		for (i = 0; i < cfg->edges; i++) {
			unsigned char buf[16];

			dtfl_put_u64(buf, next_node(cfg->nodes));
			dtfl_put_u64(buf + 8, next_node(cfg->nodes));
			dtfl_input_session_insert(fc.graph, buf, sizeof buf);
		}
	}
	settle(w, fc.graph, fc.probe);
	s = secs_since(t0);
	printf("GRAPHS-STATIC\tFORWARD\t1\t%.1f\n",
	    (double)(verbose ? FIXED_E : cfg->edges) / s);
	fflush(stdout);

	/* Phase 2: reachability from the root. */
	rc.trace = fc.trace;
	rc.roots = dtfl_input_session_new(dtfl_type_u64(), i64);
	rc.probe = dtfl_probe_new();
	rc.i64 = i64;
	rc.verbose = verbose;
	dtfl_worker_dataflow(w, "reach", tc, build_reach, &rc);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	seed_root_input(w, rc.roots, cfg->root);
	drain(w, rc.roots, rc.probe);
	printf("GRAPHS-STATIC\tREACH\t1\t%.3f\n", secs_since(t0));
	fflush(stdout);

	/* Phase 3: breadth-first distance labeling from the root. */
	bc.trace = fc.trace;
	bc.roots = dtfl_input_session_new(dtfl_type_u64(), i64);
	bc.probe = dtfl_probe_new();
	bc.i64 = i64;
	bc.verbose = verbose;
	dtfl_worker_dataflow(w, "bfs", tc, build_bfs, &bc);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	seed_root_input(w, bc.roots, cfg->root);
	drain(w, bc.roots, bc.probe);
	printf("GRAPHS-STATIC\tBFS\t1\t%.3f\n", secs_since(t0));
	fflush(stdout);

	/* Phase 4: reverse index. */
	vc.forward = fc.trace;
	vc.probe = dtfl_probe_new();
	vc.trace = NULL;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	dtfl_worker_dataflow(w, "reverse", tc, build_reverse, &vc);
	while (dtfl_worker_step(w) &&
	    dtfl_probe_less_than(vc.probe, dtfl_input_session_time(fc.graph)))
		;
	printf("GRAPHS-STATIC\tREVERSE\t1\t%.3f\n", secs_since(t0));
	fflush(stdout);

	/* Phase 5: undirected connectivity. */
	cc.forward = fc.trace;
	cc.reverse = vc.trace;
	cc.probe = dtfl_probe_new();
	cc.i64 = i64;
	cc.verbose = verbose;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	dtfl_worker_dataflow(w, "cc", tc, build_cc, &cc);
	while (dtfl_worker_step(w) &&
	    dtfl_probe_less_than(cc.probe, dtfl_input_session_time(fc.graph)))
		;
	printf("GRAPHS-STATIC\tCC\t1\t%.3f\n", secs_since(t0));
	fflush(stdout);

	/* Close the shared graph input now that every importer is built. */
	dtfl_input_session_close(fc.graph);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(cc.probe);
	dtfl_trace_free(vc.trace);
	dtfl_probe_free(vc.probe);
	dtfl_probe_free(bc.probe);
	dtfl_probe_free(rc.probe);
	dtfl_trace_free(fc.trace);
	dtfl_probe_free(fc.probe);
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
	cfg.root = argc > 3 ? strtoull(argv[3], NULL, 10) : 0;
	return dtfl_execute_directly(worker, &cfg);
}
