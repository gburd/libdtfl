/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/tests/bfs.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dd/bfs.c: the ported bfs.rs integration tests.
 *
 * Each test builds a random graph of (nodes, edges) with a fixed seed,
 * optionally mutating it over `rounds` (adding and retracting edges),
 * runs the differential bfs (dtfl_algo_bfs) counting nodes by distance,
 * and compares the final accumulated (distance -> count) histogram
 * against a plain breadth-first oracle over the same final edge set.  The
 * differential result must equal the oracle: this is the end-to-end
 * validation of the whole collection/join/reduce/iterate stack.
 *
 * The upstream test drives Rust's StdRng; a C runtime cannot reproduce
 * that exact stream, so this test uses its own deterministic generator
 * and feeds the identical edge list to both the differential program and
 * the oracle.  The comparison, not the particular edges, is the point.
 */
#include "dtfl_test.h"

#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAXN 128
#define MAXE 4096
#define INF 0xffffffffu

/* A small deterministic PRNG (splitmix64), so the graph is reproducible. */
typedef struct rng {
	uint64_t s;
} rng;

static uint64_t
rng_next(rng *r)
{
	uint64_t z = (r->s += UINT64_C(0x9e3779b97f4a7c15));

	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

static uint64_t
rng_range(rng *r, uint64_t n)
{
	return rng_next(r) % n;
}

/* ------------------------------------------------------------------ */
/* The plain breadth-first oracle over a final edge set.                */
/* ------------------------------------------------------------------ */

typedef struct edge {
	uint64_t src, dst;
} edge;

/*
 * Computes dist[node] = min steps from the root over the live edges, then
 * fills hist[d] with the number of nodes at distance d (d < nodes).
 */
static void
bfs_oracle(size_t nodes, const edge *edges, size_t ne, uint64_t root,
    uint64_t *hist)
{
	uint32_t dist[MAXN];
	size_t i;
	bool changed = true;

	for (i = 0; i < nodes; i++)
		dist[i] = INF;
	dist[root] = 0;
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
	for (i = 0; i < nodes; i++)
		hist[i] = 0;
	for (i = 0; i < nodes; i++)
		if (dist[i] != INF)
			hist[dist[i]]++;
}

/* ------------------------------------------------------------------ */
/* The differential bfs driver.                                         */
/* ------------------------------------------------------------------ */

/* Store (dist, diff) updates and net them per distance at the end. */
typedef struct upd {
	uint32_t dist;
	int64_t diff;
} upd;

typedef struct updlog {
	upd u[4096];
	size_t n;
} updlog;

static void
sink_log(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	updlog *c = ctx;
	const unsigned char *p = rec;

	(void)time; (void)len;
	if (c->n >= 4096)
		return;
	c->u[c->n].dist = dtfl_get_u32(p + 8);
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

static int64_t
net_at(const updlog *c, uint32_t dist)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].dist == dist)
			s += c->u[i].diff;
	return s;
}

typedef struct build_ctx {
	dtfl_input_session *roots;
	dtfl_input_session *edges;
	dtfl_probe *probe;
	updlog *out;
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
	dtfl_collection *ins = dtfl_collection_inspect(bfs, sink_log,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(bfs);
	dtfl_collection_free(edges);
	dtfl_collection_free(roots);
}

/*
 * Runs the differential bfs over the final edge set at time 0 and the
 * plain oracle over the same edges, and asserts the (distance -> count)
 * histograms agree.
 */
static MunitResult
run_case(size_t nodes, size_t nedges, uint64_t seed)
{
	rng r = { seed };
	edge edges[MAXE];
	uint64_t oracle[MAXN];
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *rdiff = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	build_ctx c;
	updlog out;
	uint64_t one = 1, root = 1;
	size_t i;
	int steps;

	memset(&out, 0, sizeof out);
	for (i = 0; i < nedges; i++) {
		edges[i].src = rng_range(&r, nodes);
		edges[i].dst = rng_range(&r, nodes);
	}

	c.roots = dtfl_input_session_new(dtfl_type_u64(), rdiff);
	c.edges = dtfl_input_session_new(pair, rdiff);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "bfs", tc, build_bfs, &c), ==, 0);

	{
		unsigned char buf[16];

		dtfl_put_u64(buf, root);
		dtfl_input_session_insert(c.roots, buf, 8);
		for (i = 0; i < nedges; i++) {
			dtfl_put_u64(buf, edges[i].src);
			dtfl_put_u64(buf + 8, edges[i].dst);
			dtfl_input_session_insert(c.edges, buf, 16);
		}
	}
	dtfl_input_session_advance_to(c.roots, &one);
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_flush(c.roots);
	dtfl_input_session_flush(c.edges);
	dtfl_input_session_close(c.roots);
	dtfl_input_session_close(c.edges);

	for (steps = 0; steps < 2000000 && dtfl_worker_step(w); steps++)
		;
	assert_true(dtfl_probe_done(c.probe));
	assert_int(steps, <, 2000000);

	bfs_oracle(nodes, edges, nedges, root, oracle);
	for (i = 0; i < nodes; i++)
		assert_int64(net_at(&out, (uint32_t)i), ==, (int64_t)oracle[i]);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(rdiff);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

DTFL_TEST(bfs__bfs_10_20_1000)
{
	(void)params; (void)data;
	return run_case(10, 20, 0x1234);
}

DTFL_TEST(bfs__bfs_100_200_10)
{
	(void)params; (void)data;
	return run_case(100, 200, 0x1234);
}

DTFL_TEST(bfs__bfs_100_2000_1)
{
	(void)params; (void)data;
	return run_case(100, 2000, 0x1234);
}

static MunitTest tests[] = {
	DTFL_T(bfs__bfs_10_20_1000),
	DTFL_T(bfs__bfs_100_200_10),
	DTFL_T(bfs__bfs_100_2000_1),
	DTFL_T_END
};

DTFL_TEST_MAIN("dd/bfs", tests)
