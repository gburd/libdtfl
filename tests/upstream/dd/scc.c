/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/tests/scc.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dd/scc.c: the ported scc.rs integration tests.
 *
 * Each test builds a random directed graph with a fixed seed, runs the
 * differential strongly_connected (dtfl_algo_scc), and compares the set
 * of surviving intra-component edges against a plain Kosaraju oracle over
 * the same final edge set.  The differential result must equal the
 * oracle: an edge survives iff both endpoints share a strongly connected
 * component.
 *
 * The upstream test drives Rust's StdRng and three process workers; a C
 * runtime cannot reproduce that stream, so this test uses its own
 * deterministic generator and feeds the identical edge list to the
 * differential program and the oracle.  The scc_at tests exercise the
 * prioritized variant (dtfl_algo_scc_at / strongly_connected_at), which
 * introduces node labels in log-bucketed enter_at rounds; it produces the
 * same intra-component edge set as the unprioritized variant, so both are
 * checked against the same oracle.
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

typedef struct edge {
	uint64_t src, dst;
} edge;

/* ------------------------------------------------------------------ */
/* Kosaraju SCC oracle.                                                 */
/* ------------------------------------------------------------------ */

typedef struct adj {
	uint64_t to[MAXE];
	size_t n;
} adj;

static void
visit1(uint64_t v, const adj *fwd, bool *seen, uint64_t *order, size_t *oi)
{
	size_t i;

	if (seen[v])
		return;
	seen[v] = true;
	for (i = 0; i < fwd[v].n; i++)
		visit1(fwd[v].to[i], fwd, seen, order, oi);
	order[(*oi)++] = v;
}

static void
visit2(uint64_t v, uint64_t root, const adj *rev, uint64_t *comp)
{
	size_t i;

	if (comp[v] != UINT64_MAX)
		return;
	comp[v] = root;
	for (i = 0; i < rev[v].n; i++)
		visit2(rev[v].to[i], root, rev, comp);
}

/*
 * Computes the SCC id of each node (component representative) over the
 * final live edge set, then marks each edge whose endpoints share a
 * component.  Returns the count of surviving edges and fills `keep`.
 */
static void
scc_oracle(size_t nodes, const edge *edges, size_t ne, uint64_t *comp)
{
	static adj fwd[MAXN], rev[MAXN];
	static bool seen[MAXN];
	uint64_t order[MAXN];
	size_t oi = 0;
	size_t i;
	long k;

	for (i = 0; i < nodes; i++) {
		fwd[i].n = rev[i].n = 0;
		seen[i] = false;
		comp[i] = UINT64_MAX;
	}
	for (i = 0; i < ne; i++) {
		fwd[edges[i].src].to[fwd[edges[i].src].n++] = edges[i].dst;
		rev[edges[i].dst].to[rev[edges[i].dst].n++] = edges[i].src;
	}
	for (i = 0; i < nodes; i++)
		visit1(i, fwd, seen, order, &oi);
	for (k = (long)oi - 1; k >= 0; k--)
		visit2(order[k], order[k], rev, comp);
}

/* ------------------------------------------------------------------ */
/* Differential scc driver.                                             */
/* ------------------------------------------------------------------ */

typedef struct eupd {
	uint64_t src, dst;
	int64_t diff;
} eupd;

typedef struct elog {
	eupd u[MAXE];
	size_t n;
} elog;

static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	elog *c = ctx;
	const unsigned char *p = rec;

	(void)time; (void)len;
	if (c->n >= MAXE)
		return;
	c->u[c->n].src = dtfl_get_u64(p);
	c->u[c->n].dst = dtfl_get_u64(p + 8);
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

static int64_t
net_edge(const elog *c, uint64_t s, uint64_t d)
{
	size_t i;
	int64_t sum = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].src == s && c->u[i].dst == d)
			sum += c->u[i].diff;
	return sum;
}

typedef struct build_ctx {
	dtfl_input_session *edges;
	dtfl_probe *probe;
	elog *out;
	bool prioritized;
} build_ctx;

/* The scc_at prioritization logic: a node labels into a log-bucketed
 * round by its own id (upstream |x| *x as u64). */
static uint64_t
scc_at_logic(void *ctx, uint64_t node)
{
	(void)ctx;
	return node;
}

static void
build_scc(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	dtfl_collection *scc = c->prioritized ?
	    dtfl_algo_scc_at(edges, scc_at_logic, DTFL_NOCTX) :
	    dtfl_algo_scc(edges);
	dtfl_collection *con = dtfl_collection_consolidate(scc);
	dtfl_collection *ins = dtfl_collection_inspect(con, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(scc);
	dtfl_collection_free(edges);
}

static MunitResult
run_case(size_t nodes, size_t nedges, uint64_t seed, bool prioritized)
{
	rng r = { seed };
	edge edges[MAXE];
	uint64_t comp[MAXN];
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *rdiff = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	build_ctx c;
	elog out;
	uint64_t one = 1;
	size_t i, j;
	int steps;

	memset(&out, 0, sizeof out);
	for (i = 0; i < nedges; i++) {
		edges[i].src = rng_range(&r, nodes);
		edges[i].dst = rng_range(&r, nodes);
	}

	c.edges = dtfl_input_session_new(pair, rdiff);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.prioritized = prioritized;
	assert_int(dtfl_worker_dataflow(w, "scc", tc, build_scc, &c), ==, 0);

	for (i = 0; i < nedges; i++) {
		unsigned char buf[16];

		dtfl_put_u64(buf, edges[i].src);
		dtfl_put_u64(buf + 8, edges[i].dst);
		dtfl_input_session_insert(c.edges, buf, 16);
	}
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_flush(c.edges);
	dtfl_input_session_close(c.edges);

	for (steps = 0; steps < 4000000 && dtfl_worker_step(w); steps++)
		;
	assert_true(dtfl_probe_done(c.probe));
	assert_int(steps, <, 4000000);

	scc_oracle(nodes, edges, nedges, comp);

	/*
	 * For every distinct live edge, the oracle keeps it iff its endpoints
	 * share a component.  The differential net for a kept edge is its
	 * multiplicity; for a dropped edge the net is zero.  Compare both.
	 */
	for (i = 0; i < nedges; i++) {
		uint64_t s = edges[i].src, d = edges[i].dst;
		bool dup = false;
		int64_t mult = 0;

		for (j = 0; j < i; j++)
			if (edges[j].src == s && edges[j].dst == d) {
				dup = true;
				break;
			}
		if (dup)
			continue;
		for (j = 0; j < nedges; j++)
			if (edges[j].src == s && edges[j].dst == d)
				mult++;
		if (comp[s] == comp[d])
			assert_int64(net_edge(&out, s, d), ==, mult);
		else
			assert_int64(net_edge(&out, s, d), ==, 0);
	}

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(rdiff);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

DTFL_TEST(scc__scc_10_20_1000)
{
	(void)params; (void)data;
	return run_case(10, 20, 1, false);
}

DTFL_TEST(scc__scc_100_200_10)
{
	(void)params; (void)data;
	return run_case(100, 200, 2, false);
}

DTFL_TEST(scc__scc_100_2000_1)
{
	(void)params; (void)data;
	return run_case(100, 2000, 3, false);
}

DTFL_TEST(scc__scc_at_10_20_1000)
{
	(void)params; (void)data;
	return run_case(10, 20, 1, true);
}

DTFL_TEST(scc__scc_at_100_200_10)
{
	(void)params; (void)data;
	return run_case(100, 200, 2, true);
}

DTFL_TEST(scc__scc_at_100_2000_1)
{
	(void)params; (void)data;
	return run_case(100, 2000, 3, true);
}

static MunitTest tests[] = {
	DTFL_T(scc__scc_10_20_1000),
	DTFL_T(scc__scc_100_200_10),
	DTFL_T(scc__scc_100_2000_1),
	DTFL_T(scc__scc_at_10_20_1000),
	DTFL_T(scc__scc_at_100_200_10),
	DTFL_T(scc__scc_at_100_2000_1),
	DTFL_T_END
};

DTFL_TEST_MAIN("dd/scc", tests)
