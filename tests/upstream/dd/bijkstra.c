/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/graphs/bijkstra.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dd/bijkstra.c: end-to-end test for dtfl_algo_bidijkstra.
 *
 * Upstream bijkstra.rs has no integration test of its own.  This test
 * builds a fixed directed graph of unit-length edges and a set of
 * (source, target) goals, runs the bidirectional search, and compares
 * each goal's reported distance against a plain breadth-first oracle
 * over the same edge set.  Goals whose target is unreachable from the
 * source must be absent from the result, which the oracle also checks.
 */
#include "dtfl_test.h"

#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAXN 32
#define INF 0xffffffffu

typedef struct edge {
	uint64_t src, dst;
} edge;

typedef struct goal {
	uint64_t src, dst;
} goal;

/*
 * Breadth-first shortest distance from `src` to `dst` over the directed
 * unit-length edge set, or INF if unreachable.
 */
static uint32_t
bfs_dist(size_t nodes, const edge *edges, size_t ne, uint64_t src,
    uint64_t dst)
{
	uint32_t dist[MAXN];
	size_t i;
	bool changed = true;

	for (i = 0; i < nodes; i++)
		dist[i] = INF;
	dist[src] = 0;
	while (changed) {
		changed = false;
		for (i = 0; i < ne; i++) {
			uint64_t s = edges[i].src, d = edges[i].dst;

			if (dist[s] != INF && dist[d] > dist[s] + 1) {
				dist[d] = dist[s] + 1;
				changed = true;
			}
		}
	}
	return dist[dst];
}

/* The differential result: ((src, dst), dist) records with diffs. */
typedef struct result {
	uint64_t src, dst;
	uint32_t dist;
	int64_t diff;
} result;

typedef struct resultlog {
	result r[256];
	size_t n;
} resultlog;

static void
sink_log(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	resultlog *c = ctx;
	const unsigned char *p = rec;

	(void)time; (void)len;
	if (c->n >= 256)
		return;
	c->r[c->n].src = dtfl_get_u64(p);
	c->r[c->n].dst = dtfl_get_u64(p + 8);
	c->r[c->n].dist = dtfl_get_u32(p + 16);
	memcpy(&c->r[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* The net diff of a ((src, dst), dist) tuple across the result log. */
static int64_t
net_at(const resultlog *c, uint64_t src, uint64_t dst, uint32_t dist)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->r[i].src == src && c->r[i].dst == dst &&
		    c->r[i].dist == dist)
			s += c->r[i].diff;
	return s;
}

/* The net diff of any ((src, dst), *) across the result log. */
static int64_t
net_pair(const resultlog *c, uint64_t src, uint64_t dst)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->r[i].src == src && c->r[i].dst == dst)
			s += c->r[i].diff;
	return s;
}

typedef struct build_ctx {
	dtfl_input_session *edges;
	dtfl_input_session *goals;
	dtfl_probe *probe;
	resultlog *out;
} build_ctx;

static void
build_bij(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	dtfl_collection *goals = dtfl_input_session_to_collection(c->goals,
	    scope);
	dtfl_collection *bij = dtfl_algo_bidijkstra(edges, goals);
	dtfl_collection *ins = dtfl_collection_inspect(bij, sink_log,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(bij);
	dtfl_collection_free(goals);
	dtfl_collection_free(edges);
}

static MunitResult
run_case(size_t nodes, const edge *edges, size_t ne, const goal *goals,
    size_t ng)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *rdiff = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	build_ctx c;
	resultlog out;
	uint64_t one = 1;
	size_t i;
	int steps;

	memset(&out, 0, sizeof out);
	c.edges = dtfl_input_session_new(pair, rdiff);
	c.goals = dtfl_input_session_new(pair, rdiff);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "bijkstra", tc, build_bij, &c),
	    ==, 0);

	for (i = 0; i < ne; i++) {
		unsigned char buf[16];

		dtfl_put_u64(buf, edges[i].src);
		dtfl_put_u64(buf + 8, edges[i].dst);
		dtfl_input_session_insert(c.edges, buf, 16);
	}
	for (i = 0; i < ng; i++) {
		unsigned char buf[16];

		dtfl_put_u64(buf, goals[i].src);
		dtfl_put_u64(buf + 8, goals[i].dst);
		dtfl_input_session_insert(c.goals, buf, 16);
	}
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_advance_to(c.goals, &one);
	dtfl_input_session_flush(c.edges);
	dtfl_input_session_flush(c.goals);
	dtfl_input_session_close(c.edges);
	dtfl_input_session_close(c.goals);

	for (steps = 0; steps < 2000000 && dtfl_worker_step(w); steps++)
		;
	assert_true(dtfl_probe_done(c.probe));
	assert_int(steps, <, 2000000);

	for (i = 0; i < ng; i++) {
		uint32_t d = bfs_dist(nodes, edges, ne, goals[i].src,
		    goals[i].dst);

		if (d == INF) {
			/* Unreachable goal: absent from the result. */
			assert_int64(net_pair(&out, goals[i].src, goals[i].dst),
			    ==, 0);
		} else {
			assert_int64(net_at(&out, goals[i].src, goals[i].dst, d),
			    ==, 1);
			assert_int64(net_pair(&out, goals[i].src, goals[i].dst),
			    ==, 1);
		}
	}

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(rdiff);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/*
 * A fixed directed graph:
 *   0 -> 1 -> 2 -> 3 -> 4   (a chain)
 *   0 -> 5 -> 4             (a shortcut of length 2 to node 4)
 *   6 -> 7                  (a disconnected component)
 * Hand-computed distances: 0->4 is 2 (via 5), 0->3 is 3, 1->4 is 3,
 * 2->2 is 0, 0->6 is unreachable, 6->4 is unreachable.
 */
DTFL_TEST(bijkstra__fixed_graph)
{
	static const edge e[] = {
	    { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 },
	    { 0, 5 }, { 5, 4 },
	    { 6, 7 },
	};
	static const goal g[] = {
	    { 0, 4 }, { 0, 3 }, { 1, 4 }, { 2, 2 }, { 0, 6 }, { 6, 4 },
	};

	(void)params; (void)data;
	return run_case(8, e, sizeof e / sizeof e[0], g,
	    sizeof g / sizeof g[0]);
}

/*
 * A diamond with two equal-length paths; the shortest distance is the
 * same whichever side the search explores first.
 */
DTFL_TEST(bijkstra__diamond)
{
	static const edge e[] = {
	    { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 3, 4 },
	};
	static const goal g[] = {
	    { 0, 3 }, { 0, 4 }, { 1, 4 },
	};

	(void)params; (void)data;
	return run_case(5, e, sizeof e / sizeof e[0], g,
	    sizeof g / sizeof g[0]);
}

static MunitTest tests[] = {
	DTFL_T(bijkstra__fixed_graph),
	DTFL_T(bijkstra__diamond),
	DTFL_T_END
};

DTFL_TEST_MAIN("dd/bijkstra", tests)
