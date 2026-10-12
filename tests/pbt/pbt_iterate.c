/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_iterate.c: the iterate fixpoint equals a reference
 * transitive closure.
 *
 * The property draws a small random directed graph (at most 8 nodes and
 * 16 edges) and a random non-empty root set, computes the set of nodes
 * reachable from the roots with dtfl_collection_iterate (a join against
 * the edges, concatenated with the roots, made idempotent by distinct),
 * and checks it equals the reference closure a plain breadth-first search
 * computes.  The generators are deliberately tiny: a reachable set over 8
 * nodes converges in at most 8 iterations and each case finishes in well
 * under a second.  The test asserts both the exact closure and that the
 * loop terminates (bounded worker steps plus probe done), so a Variable
 * that fails to negate its source or a wrong loop summary, which would
 * diverge, is caught as a non-terminating loop, not just a wrong answer.
 */
#include "pbt.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#define MAXNODE 8
#define MAXEDGE 16

typedef struct graph {
	unsigned nnodes;
	unsigned nedges;
	uint8_t src[MAXEDGE];
	uint8_t dst[MAXEDGE];
	bool root[MAXNODE];
} graph;

/* Reference transitive closure: BFS from the roots over the edges. */
static void
ref_closure(const graph *g, bool out[MAXNODE])
{
	bool changed = true;
	unsigned i, e;

	for (i = 0; i < MAXNODE; i++)
		out[i] = (i < g->nnodes) && g->root[i];
	while (changed) {
		changed = false;
		for (e = 0; e < g->nedges; e++)
			if (out[g->src[e]] && !out[g->dst[e]]) {
				out[g->dst[e]] = true;
				changed = true;
			}
	}
}

/* ------------------------------------------------------------------ */
/* The dataflow: reachable = iterate(roots, join edges + roots, distinct). */
/* ------------------------------------------------------------------ */

typedef struct collected {
	int64_t net[MAXNODE];	/* net diff per reachable node */
} collected;

static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	uint64_t node;
	int64_t d;

	(void)time;
	if (len < 8)
		return;
	node = dtfl_get_u64(rec);
	memcpy(&d, diff, sizeof d);
	if (node < MAXNODE)
		c->net[node] += d;
}

static void
emit_dst(void *ctx, const void *key, size_t klen, const void *v1, size_t v1len,
    const void *v2, size_t v2len, dtfl_join_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)v1; (void)v1len; (void)v2len;
	dtfl_join_emit_record(out, v2, v2len);
}

typedef struct reach_ctx {
	dtfl_collection *roots;
	dtfl_collection *edges;
} reach_ctx;

static dtfl_collection *
reach_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	reach_ctx *rc = vctx;
	dtfl_collection *g = dtfl_collection_enter(rc->edges, inner);
	dtfl_collection *stepped = dtfl_collection_join_map(var, g,
	    dtfl_type_u64(), dtfl_type_unit(), dtfl_type_u64(),
	    dtfl_type_u64(), emit_dst, DTFL_NOCTX);
	dtfl_collection *rts = dtfl_collection_enter(rc->roots, inner);
	dtfl_collection *all = dtfl_collection_concat(stepped, rts);
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *dist = dtfl_collection_distinct(all, i64);

	dtfl_diff_free(i64);
	dtfl_collection_free(all);
	dtfl_collection_free(rts);
	dtfl_collection_free(stepped);
	dtfl_collection_free(g);
	return dist;
}

typedef struct build_ctx {
	dtfl_input_session *roots;
	dtfl_input_session *graph;
	dtfl_probe *probe;
	collected *out;
} build_ctx;

static void
build_reach(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	dtfl_collection *gcoll = dtfl_input_session_to_collection(c->graph,
	    scope);
	reach_ctx rc = { roots, gcoll };
	dtfl_collection *fix = dtfl_collection_iterate(roots, reach_logic,
	    DTFL_CTX(&rc, NULL));
	dtfl_collection *ins = dtfl_collection_inspect(fix, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(fix);
	dtfl_collection_free(gcoll);
	dtfl_collection_free(roots);
}

static void free_worker(void *w) { dtfl_worker_free(w); }
static void free_tc(void *tc) { dtfl_time_class_free(tc); }
static void free_diff(void *d) { dtfl_diff_free(d); }
static void free_probe(void *p) { dtfl_probe_free(p); }
static void free_type(void *t) { dtfl_type_free(t); }

/* Runs the reachability dataflow for `g`, returning the reachable set. */
static bool
run_reach(pbt_tc *tc, const graph *g, collected *out)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tcl = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	build_ctx c;
	uint64_t one = 1;
	unsigned i;
	int steps;

	memset(out, 0, sizeof *out);
	pbt_defer(tc, free_worker, w);
	pbt_defer(tc, free_tc, tcl);
	pbt_defer(tc, free_diff, r);
	pbt_defer(tc, free_type, (void *)pair);
	c.roots = dtfl_input_session_new(dtfl_type_u64(), r);
	c.graph = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	pbt_defer(tc, free_probe, c.probe);
	c.out = out;
	PBT_ASSERT(dtfl_worker_dataflow(w, "reach", tcl, build_reach, &c) == 0);

	for (i = 0; i < g->nnodes; i++)
		if (g->root[i]) {
			unsigned char buf[8];

			dtfl_put_u64(buf, i);
			dtfl_input_session_insert(c.roots, buf, 8);
		}
	for (i = 0; i < g->nedges; i++) {
		unsigned char buf[16];

		dtfl_put_u64(buf, g->src[i]);
		dtfl_put_u64(buf + 8, g->dst[i]);
		dtfl_input_session_insert(c.graph, buf, 16);
	}
	dtfl_input_session_advance_to(c.roots, &one);
	dtfl_input_session_advance_to(c.graph, &one);
	dtfl_input_session_flush(c.roots);
	dtfl_input_session_flush(c.graph);
	dtfl_input_session_close(c.roots);
	dtfl_input_session_close(c.graph);

	/* A tiny graph converges in <= nnodes rounds; the bound catches a
	 * diverging loop (a wrong summary or an un-negated source). */
	for (steps = 0; steps < 100000 && dtfl_worker_step(w); steps++)
		;
	PBT_ASSERT(steps < 100000);
	return dtfl_probe_done(c.probe);
}

static void
prop_reach_equals_closure(pbt_tc *tc)
{
	graph g;
	collected out;
	bool ref[MAXNODE];
	unsigned i, nroots = 0;

	g.nnodes = (unsigned)pbt_int(tc, 1, MAXNODE);
	g.nedges = (unsigned)pbt_int(tc, 0, MAXEDGE);
	for (i = 0; i < g.nedges; i++) {
		g.src[i] = (uint8_t)pbt_int(tc, 0, (int)g.nnodes - 1);
		g.dst[i] = (uint8_t)pbt_int(tc, 0, (int)g.nnodes - 1);
	}
	for (i = 0; i < g.nnodes; i++) {
		g.root[i] = pbt_bool(tc);
		nroots += g.root[i];
	}
	/* A non-empty root set keeps the closure non-trivial. */
	if (nroots == 0)
		g.root[0] = true;

	PBT_ASSERT(run_reach(tc, &g, &out));
	ref_closure(&g, ref);
	for (i = 0; i < MAXNODE; i++)
		PBT_ASSERT(out.net[i] == (ref[i] ? 1 : 0));
}

static const pbt_prop props[] = {
	{ "reach_equals_closure", prop_reach_equals_closure, 100 },
};

PBT_MAIN("iterate", props)
