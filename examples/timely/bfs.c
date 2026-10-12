/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/bfs.c: mirrors timely/examples/bfs.rs.
 *
 * Arguments: [nodes edges] [timely flags].  A breadth-first search from
 * node 0 over a directed graph, computed in a feedback loop: edges arrive
 * on one input, (node, parent) frontier updates on the other (seeded with
 * (0, 0)); each loop level visits newly-reached nodes, records their
 * parent, and emits their out-neighbours one level deeper.  With no
 * positional arguments a small fixed graph is used so the set of reached
 * nodes and their BFS depths are exact and checkable; the operator writes
 * the parent of each node into a shared array that the driver reports.
 *
 * The port assumes a single worker (peers == 1); the exchange pacts keep
 * the loop correct, and node / peers == node, so done[] is indexed by the
 * node id directly.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A fixed directed graph on 7 nodes.  From node 0 the reachable set is
 * {0,1,2,3,4,5}; node 6 is unreachable.  BFS depths from 0:
 *   0:0  1:1  2:1  3:2  4:2  5:3  6:unreached.
 */
static const uint32_t fixed_edges[][2] = {
	{ 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 2, 4 }, { 3, 5 },
	{ 4, 5 }, { 6, 0 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])
#define FIXED_N 7

/* ------------------------------------------------------------------ */
/* The BFS operator state (binary_notify)                              */
/* ------------------------------------------------------------------ */

#define U32_UNSET 0xffffffffu

typedef struct edge { uint32_t src, dst; } edge;

/* A per-time list of (node, parent) updates awaiting processing. */
typedef struct node_slot {
	uint64_t time;
	uint32_t (*pairs)[2];
	size_t n, cap;
} node_slot;

typedef struct bfs_ctx {
	size_t nodes;
	/* accumulated edges, and the CSR graph built at time 0. */
	edge *edges;
	size_t nedges, ecap;
	uint32_t *offsets;	/* nodes + 1 */
	uint32_t *targets;	/* nedges */
	bool built;
	/* bfs parent of each node, or U32_UNSET; shared with the driver. */
	uint32_t *done;
	/* pending (node, parent) updates keyed by time. */
	node_slot *slots;
	size_t nslots, scap;
} bfs_ctx;

static node_slot *
slot_for(bfs_ctx *c, dtfl_op *op, dtfl_notificator *note, const uint64_t *t)
{
	size_t i;
	node_slot s;

	for (i = 0; i < c->nslots; i++)
		if (c->slots[i].time == t[0])
			return &c->slots[i];
	if (c->nslots == c->scap) {
		c->scap = c->scap ? c->scap * 2 : 4;
		c->slots = realloc(c->slots, c->scap * sizeof *c->slots);
	}
	memset(&s, 0, sizeof s);
	s.time = t[0];
	c->slots[c->nslots] = s;
	dtfl_notificator_notify_at(note, dtfl_op_input_cap_delayed(op, 1, 0, t));
	return &c->slots[c->nslots++];
}

static void
slot_push(node_slot *s, uint32_t node, uint32_t parent)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 8;
		s->pairs = realloc(s->pairs, s->cap * sizeof *s->pairs);
	}
	s->pairs[s->n][0] = node;
	s->pairs[s->n][1] = parent;
	s->n++;
}

static int
cmp_edge(const void *va, const void *vb)
{
	const edge *a = va, *b = vb;

	if (a->src != b->src)
		return a->src < b->src ? -1 : 1;
	if (a->dst != b->dst)
		return a->dst < b->dst ? -1 : 1;
	return 0;
}

static void
bfs_logic(void *vctx, dtfl_op *op, dtfl_notificator *note)
{
	bfs_ctx *c = vctx;
	dtfl_message *m;
	dtfl_cap *cap;
	uint64_t count;

	/* input 0: edges.  Accumulate and request a notification at time 0. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		size_t i, k = dtfl_message_len(m);

		slot_for(c, op, note, t);	/* ensure time 0 is notified */
		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			edge e;

			e.src = (uint32_t)dtfl_get_u64(rec);
			e.dst = (uint32_t)dtfl_get_u64((const unsigned char *)rec + 8);
			if (c->nedges == c->ecap) {
				c->ecap = c->ecap ? c->ecap * 2 : 16;
				c->edges = realloc(c->edges, c->ecap * sizeof *c->edges);
			}
			c->edges[c->nedges++] = e;
		}
	}

	/* input 1: (node, parent) updates.  Stash per time. */
	while ((m = dtfl_op_input_next(op, 1)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		node_slot *s = slot_for(c, op, note, t);
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			uint32_t node = (uint32_t)dtfl_get_u64(rec);
			uint32_t parent =
			    (uint32_t)dtfl_get_u64((const unsigned char *)rec + 8);

			slot_push(s, node, parent);
		}
	}

	/* Deliver notifications: process each completed time. */
	while (dtfl_notificator_next(note, &cap, &count)) {
		const uint64_t *t = dtfl_cap_time(cap);
		size_t i;

		if (t[0] == 0 && !c->built) {
			/* Build the CSR graph from the accumulated edges. */
			size_t e;
			uint32_t prev = 0;

			if (c->nedges > 1)
				qsort(c->edges, c->nedges, sizeof *c->edges, cmp_edge);
			c->offsets = calloc(c->nodes + 1, sizeof *c->offsets);
			c->targets = calloc(c->nedges ? c->nedges : 1,
			    sizeof *c->targets);
			for (e = 0; e < c->nedges; e++) {
				uint32_t node = c->edges[e].src;

				while (prev < node && prev < c->nodes) {
					prev++;
					c->offsets[prev] = (uint32_t)e;
				}
				if (node < c->nodes)
					c->targets[e] = c->edges[e].dst;
			}
			while (prev < c->nodes) {
				prev++;
				c->offsets[prev] = (uint32_t)c->nedges;
			}
			c->built = true;
		}

		/* Visit the (node, parent) updates for this time. */
		for (i = 0; i < c->nslots; i++) {
			node_slot *s = &c->slots[i];
			size_t j;
			dtfl_session *sess;

			if (s->time != t[0])
				continue;
			sess = dtfl_op_session(op, 0, cap);
			for (j = 0; j < s->n; j++) {
				uint32_t node = s->pairs[j][0];
				uint32_t parent = s->pairs[j][1];
				uint32_t lo, hi, e;

				if (node >= c->nodes || c->done[node] != U32_UNSET)
					continue;
				c->done[node] = parent;
				lo = c->offsets[node];
				hi = c->offsets[node + 1];
				for (e = lo; e < hi; e++) {
					unsigned char buf[16];

					dtfl_put_u64(buf, c->targets[e]);
					dtfl_put_u64(buf + 8, node);
					dtfl_session_give(sess, buf, sizeof buf);
				}
			}
			dtfl_session_close(sess);
			free(s->pairs);
			s->pairs = NULL;
			s->n = s->cap = 0;
			break;
		}
		dtfl_cap_drop(cap);
	}
}

static void
bfs_free(void *vctx)
{
	bfs_ctx *c = vctx;
	size_t i;

	free(c->edges);
	free(c->offsets);
	free(c->targets);
	for (i = 0; i < c->nslots; i++)
		free(c->slots[i].pairs);
	free(c->slots);
	free(c);
}

/* ------------------------------------------------------------------ */
/* The graph source and seed                                           */
/* ------------------------------------------------------------------ */

typedef struct graph_src {
	const uint32_t (*edges)[2];
	size_t n, i;
} graph_src;

static bool
graph_next(void *ctx, dtfl_emit *out)
{
	graph_src *g = ctx;
	unsigned char buf[16];

	if (g->i >= g->n)
		return false;
	dtfl_put_u64(buf, g->edges[g->i][0]);
	dtfl_put_u64(buf + 8, g->edges[g->i][1]);
	dtfl_emit_record(out, buf, sizeof buf);
	g->i++;
	return true;
}

/* The seed stream: a single (0, 0) record at the minimum time. */
static bool
seed_next(void *ctx, dtfl_emit *out)
{
	bool *done = ctx;
	unsigned char buf[16];

	if (*done)
		return false;
	dtfl_put_u64(buf, 0);
	dtfl_put_u64(buf + 8, 0);
	dtfl_emit_record(out, buf, sizeof buf);
	*done = true;
	return true;
}

static uint64_t
route_by_src(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

typedef struct build_ctx {
	bfs_ctx *bfs;
	graph_src *gsrc;
	bool *seed_done;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	uint64_t summary[DTFL_TIME_MAX];
	dtfl_feedback *handle;
	dtfl_stream *graph, *cycle, *bfs_out, *seed, *merged;

	graph = dtfl_to_stream(scope, pair, graph_next, DTFL_CTX(c->gsrc, NULL));
	dtfl_summary_default(dtfl_scope_time_class(scope), summary);
	summary[0] = 1;
	cycle = dtfl_feedback_new(scope, pair, summary, &handle);
	bfs_out = dtfl_stream_binary_notify(graph, cycle,
	    dtfl_pact_exchange(route_by_src, NULL),
	    dtfl_pact_exchange(route_by_src, NULL), "BFS", NULL, 0, pair,
	    bfs_logic, DTFL_CTX(c->bfs, bfs_free));
	seed = dtfl_to_stream(scope, pair, seed_next,
	    DTFL_CTX(c->seed_done, NULL));
	merged = dtfl_stream_concat(bfs_out, seed);
	dtfl_feedback_connect(handle, merged);
	dtfl_type_free(pair);
}

typedef struct worker_ctx {
	bool fixed;
	size_t nodes, edges;
	uint32_t *done;		/* shared result array (driver owns) */
} worker_ctx;

static void
worker(dtfl_worker *w, void *ctx)
{
	worker_ctx *wc = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	bfs_ctx *bfs = calloc(1, sizeof *bfs);
	graph_src *gsrc = calloc(1, sizeof *gsrc);
	bool *seed_done = calloc(1, sizeof *seed_done);
	build_ctx bc;

	bfs->nodes = wc->nodes;
	bfs->done = wc->done;
	gsrc->edges = fixed_edges;
	gsrc->n = wc->fixed ? FIXED_E : 0;
	bc.bfs = bfs;
	bc.gsrc = gsrc;
	bc.seed_done = seed_done;
	dtfl_worker_dataflow(w, "bfs", tc, build, &bc);
	while (dtfl_worker_step(w))
		;
	free(gsrc);
	free(seed_done);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	worker_ctx wc;
	dtfl_config *cfg;
	int rc;
	size_t i, reached = 0;

	memset(&wc, 0, sizeof wc);
	wc.fixed = argc < 3;
	wc.nodes = wc.fixed ? FIXED_N : ex_size(argc, argv, 1, 100);
	wc.edges = wc.fixed ? FIXED_E : ex_size(argc, argv, 2, 1000);
	wc.done = malloc(wc.nodes * sizeof *wc.done);
	for (i = 0; i < wc.nodes; i++)
		wc.done[i] = U32_UNSET;

	cfg = ex_config(argc, argv, wc.fixed ? 0 : 2);
	/* The port's done[] indexing assumes a single worker. */
	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, &wc, NULL);
	dtfl_config_free(cfg);

	for (i = 0; i < wc.nodes; i++)
		if (wc.done[i] != U32_UNSET)
			reached++;
	printf("reached %zu nodes\n", reached);
	if (wc.fixed)
		for (i = 0; i < wc.nodes; i++)
			if (wc.done[i] != U32_UNSET)
				printf("reached node %zu\n", i);
	free(wc.done);
	return rc;
}
