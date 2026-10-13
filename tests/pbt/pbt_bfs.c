/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_bfs.c: differential bfs distances equal a reference BFS.
 *
 * The property draws a small random directed graph (at most 10 nodes and
 * 20 edges) and a single root, computes each node's minimum distance from
 * the root with dtfl_algo_bfs, and checks it equals the distance a plain
 * breadth-first search computes.  The generators are deliberately tiny: a
 * distance labeling over 10 nodes converges in at most 10 iterations and
 * each case finishes in well under a second.  The test asserts both the
 * exact distance map and that the loop terminates (bounded worker steps
 * plus probe done), so a diverging fixpoint shows up as a timeout, not a
 * wrong answer.
 */
#include "pbt.h"

#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#define MAXNODE 10
#define MAXEDGE 20
#define INF 0xffffffffu

typedef struct graph {
	unsigned nnodes;
	unsigned nedges;
	uint8_t src[MAXEDGE];
	uint8_t dst[MAXEDGE];
	unsigned root;
} graph;

/* Reference BFS: minimum distance from the root over the edges. */
static void
ref_bfs(const graph *g, uint32_t dist[MAXNODE])
{
	bool changed = true;
	unsigned i, e;

	for (i = 0; i < MAXNODE; i++)
		dist[i] = INF;
	dist[g->root] = 0;
	while (changed) {
		changed = false;
		for (e = 0; e < g->nedges; e++) {
			uint8_t s = g->src[e], d = g->dst[e];

			if (dist[s] != INF && dist[d] > dist[s] + 1) {
				dist[d] = dist[s] + 1;
				changed = true;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* The dataflow: dtfl_algo_bfs over the graph.                          */
/* ------------------------------------------------------------------ */

typedef struct collected {
	/* net count of (node, dist) updates: distances[node] holds the one
	 * surviving distance with positive net, or INF if the node is absent. */
	int64_t net[MAXNODE][MAXNODE];
} collected;

static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	const unsigned char *p = rec;
	uint64_t node;
	uint32_t dist;
	int64_t d;

	(void)time; (void)len;
	node = dtfl_get_u64(p);
	dist = (uint32_t)dtfl_get_u64(p + 8);
	memcpy(&d, diff, sizeof d);
	if (node < MAXNODE && dist < MAXNODE)
		c->net[node][dist] += d;
}

typedef struct build_ctx {
	dtfl_input_session *roots;
	dtfl_input_session *edges;
	dtfl_probe *probe;
	collected *out;
} build_ctx;

static void
build_bfs(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	dtfl_collection *bfs = dtfl_algo_bfs(edges, roots);
	dtfl_collection *ins = dtfl_collection_inspect(bfs, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(bfs);
	dtfl_collection_free(edges);
	dtfl_collection_free(roots);
}

static void free_worker(void *w) { dtfl_worker_free(w); }
static void free_tc(void *tc) { dtfl_time_class_free(tc); }
static void free_diff(void *d) { dtfl_diff_free(d); }
static void free_probe(void *p) { dtfl_probe_free(p); }
static void free_type(void *t) { dtfl_type_free(t); }

static bool
run_bfs(pbt_tc *tc, const graph *g, collected *out)
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
	unsigned char buf[16];

	memset(out, 0, sizeof *out);
	pbt_defer(tc, free_worker, w);
	pbt_defer(tc, free_tc, tcl);
	pbt_defer(tc, free_diff, r);
	pbt_defer(tc, free_type, (void *)pair);
	c.roots = dtfl_input_session_new(dtfl_type_u64(), r);
	c.edges = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	pbt_defer(tc, free_probe, c.probe);
	c.out = out;
	PBT_ASSERT(dtfl_worker_dataflow(w, "bfs", tcl, build_bfs, &c) == 0);

	dtfl_put_u64(buf, g->root);
	dtfl_input_session_insert(c.roots, buf, 8);
	for (i = 0; i < g->nedges; i++) {
		dtfl_put_u64(buf, g->src[i]);
		dtfl_put_u64(buf + 8, g->dst[i]);
		dtfl_input_session_insert(c.edges, buf, 16);
	}
	dtfl_input_session_advance_to(c.roots, &one);
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_flush(c.roots);
	dtfl_input_session_flush(c.edges);
	dtfl_input_session_close(c.roots);
	dtfl_input_session_close(c.edges);

	for (steps = 0; steps < 200000 && dtfl_worker_step(w); steps++)
		;
	PBT_ASSERT(steps < 200000);
	return dtfl_probe_done(c.probe);
}

static void
prop_bfs_equals_reference(pbt_tc *tc)
{
	graph g;
	collected out;
	uint32_t ref[MAXNODE];
	unsigned i, node, dist;

	g.nnodes = (unsigned)pbt_int(tc, 1, MAXNODE);
	g.nedges = (unsigned)pbt_int(tc, 0, MAXEDGE);
	g.root = (unsigned)pbt_int(tc, 0, (int)g.nnodes - 1);
	for (i = 0; i < g.nedges; i++) {
		g.src[i] = (uint8_t)pbt_int(tc, 0, (int)g.nnodes - 1);
		g.dst[i] = (uint8_t)pbt_int(tc, 0, (int)g.nnodes - 1);
	}

	PBT_ASSERT(run_bfs(tc, &g, &out));
	ref_bfs(&g, ref);

	/*
	 * For each node, the reference distance (if finite and within the
	 * node range) must appear with net 1 and every other distance with
	 * net 0; an unreachable node must have no surviving distance.
	 */
	for (node = 0; node < g.nnodes; node++)
		for (dist = 0; dist < MAXNODE; dist++) {
			int64_t want = (ref[node] != INF && ref[node] == dist)
			    ? 1 : 0;

			PBT_ASSERT(out.net[node][dist] == want);
		}
}

static const pbt_prop props[] = {
	{ "bfs_equals_reference", prop_bfs_equals_reference, 100 },
};

PBT_MAIN("bfs", props)
