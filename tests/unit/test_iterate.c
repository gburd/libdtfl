/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_iterate.c: the iterate operator reaches a fixed point
 * and terminates.
 *
 * Three tests exercise iterate (src/dd/iterate.c): a halve-to-odd map
 * fixpoint over a fixed multiset (the upstream iterate.rs example),
 * reachability by join + distinct over a fixed small graph (transitive
 * closure), and the reachability fixpoint on three Process workers.  Each
 * test asserts both the exact fixpoint AND that the loop terminates: the
 * run is bounded by a step limit and the probe must report done, so a
 * Variable that fails to negate its source or a wrong loop summary (which
 * would circulate forever) shows up as a non-terminating loop, not just a
 * wrong answer.
 */
#include "dtfl_test.h"

#include "dtfl/collection.h"
#include "dtfl/config.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/arrange.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 256

typedef struct out_upd {
	uint64_t rec;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAX_OUT];
	size_t n;
} collected;

/* Net diff accumulated for a u64 record (over all captured times). */
static int64_t
net(const collected *c, uint64_t rec)
{
	size_t i;
	int64_t sum = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].rec == rec)
			sum += c->u[i].diff;
	return sum;
}

static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;

	(void)time;
	if (c->n >= MAX_OUT || len < 8)
		return;
	c->u[c->n].rec = dtfl_get_u64(rec);
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* ------------------------------------------------------------------ */
/* Fixpoint 1: halve even numbers until odd (iterate.rs example).       */
/* ------------------------------------------------------------------ */

/* map(x -> x even ? x/2 : x). */
static void
halve(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	uint64_t x = dtfl_get_u64(rec);
	unsigned char buf[8];

	(void)ctx; (void)len;
	dtfl_put_u64(buf, (x % 2 == 0) ? x / 2 : x);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

static dtfl_collection *
halve_logic(void *ctx, dtfl_scope *inner, dtfl_collection *var)
{
	dtfl_collection *mapped = dtfl_collection_map(var, dtfl_type_u64(),
	    halve, DTFL_NOCTX);
	dtfl_collection *con;

	(void)ctx; (void)inner;
	con = dtfl_collection_consolidate(mapped);
	dtfl_collection_free(mapped);
	return con;
}

typedef struct halve_build {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
} halve_build;

static void
build_halve(dtfl_scope *scope, void *arg)
{
	halve_build *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *fix = dtfl_collection_iterate(base, halve_logic,
	    DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(fix, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(fix);
	dtfl_collection_free(base);
}

DTFL_TEST(iterate__halve_to_odd)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	halve_build c;
	collected out;
	uint64_t i, one = 1;
	int steps;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "halve", tc, build_halve, &c), ==, 0);

	for (i = 1; i <= 8; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, i);
		dtfl_input_session_insert(c.in, buf, 8);
	}
	dtfl_input_session_advance_to(c.in, &one);
	dtfl_input_session_flush(c.in);
	dtfl_input_session_close(c.in);

	/* Step to termination with a bound: a non-converging loop would run
	 * past the bound and fail, which catches a diverging fixpoint. */
	for (steps = 0; steps < 100000 && dtfl_worker_step(w); steps++)
		;
	assert_true(dtfl_probe_done(c.probe));
	assert_int(steps, <, 100000);

	/* Fixpoint multiset of {1..8} under halve-to-odd:
	 *   1->1 2->1 3->3 4->1 5->5 6->3 7->7 8->1
	 * so 1 appears 4x, 3 appears 2x, 5 and 7 once each. */
	assert_int64(net(&out, 1), ==, 4);
	assert_int64(net(&out, 3), ==, 2);
	assert_int64(net(&out, 5), ==, 1);
	assert_int64(net(&out, 7), ==, 1);
	assert_int64(net(&out, 2), ==, 0);
	assert_int64(net(&out, 4), ==, 0);
	assert_int64(net(&out, 6), ==, 0);
	assert_int64(net(&out, 8), ==, 0);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Fixpoint 2: transitive closure (reachability) by join + distinct.    */
/* ------------------------------------------------------------------ */

/*
 * Graph: 0->1, 1->2, 2->3, 3->1 (a cycle among 1,2,3), and 4->5 (a
 * disjoint component).  Reachable from {0}: {0,1,2,3}.  5 is not reachable.
 */
static const uint64_t edges[][2] = {
	{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 1 }, { 4, 5 },
};
#define NEDGE ((size_t)(sizeof edges / sizeof edges[0]))

typedef struct reach_build {
	dtfl_input_session *roots;	/* bare node records */
	dtfl_input_session *graph;	/* (src, dst) records */
	dtfl_probe *probe;
	collected *out;
} reach_build;

/* join_map result: reachable node joined with (src -> dst): emit dst. */
static void
emit_dst(void *ctx, const void *key, size_t klen, const void *v1, size_t v1len,
    const void *v2, size_t v2len, dtfl_join_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)v1; (void)v1len; (void)v2len;
	dtfl_join_emit_record(out, v2, v2len);
}

typedef struct reach_ctx {
	dtfl_collection *roots;	/* entered inside the subscope */
	dtfl_collection *graph;	/* entered inside the subscope */
} reach_ctx;

static dtfl_collection *
reach_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	reach_ctx *rc = vctx;
	dtfl_collection *g = dtfl_collection_enter(rc->graph, inner);
	/* var holds reachable nodes keyed by self (value is unit). */
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

static void
build_reach(dtfl_scope *scope, void *arg)
{
	reach_build *c = arg;
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	dtfl_collection *graph = dtfl_input_session_to_collection(c->graph,
	    scope);
	reach_ctx rc = { roots, graph };
	dtfl_collection *fix = dtfl_collection_iterate(roots, reach_logic,
	    DTFL_CTX(&rc, NULL));
	dtfl_collection *ins = dtfl_collection_inspect(fix, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(fix);
	dtfl_collection_free(graph);
	dtfl_collection_free(roots);
}

/* Feeds the roots and the graph, then steps to a bounded fixpoint. */
static void
feed_reach(dtfl_worker *w, reach_build *c, size_t index, size_t peers)
{
	uint64_t one = 1;
	unsigned char buf[16];
	size_t i;
	int steps;

	if (index == 0) {
		dtfl_put_u64(buf, 0);		/* root node 0 */
		dtfl_input_session_insert(c->roots, buf, 8);
	}
	for (i = index; i < NEDGE; i += peers) {
		dtfl_put_u64(buf, edges[i][0]);
		dtfl_put_u64(buf + 8, edges[i][1]);
		dtfl_input_session_insert(c->graph, buf, 16);
	}
	dtfl_input_session_advance_to(c->roots, &one);
	dtfl_input_session_advance_to(c->graph, &one);
	dtfl_input_session_flush(c->roots);
	dtfl_input_session_flush(c->graph);
	dtfl_input_session_close(c->roots);
	dtfl_input_session_close(c->graph);
	for (steps = 0; steps < 100000 && dtfl_worker_step(w); steps++)
		;
}

DTFL_TEST(iterate__transitive_closure)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	reach_build c;
	collected out;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.roots = dtfl_input_session_new(dtfl_type_u64(), r);
	c.graph = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "reach", tc, build_reach, &c), ==, 0);
	feed_reach(w, &c, 0, 1);

	assert_true(dtfl_probe_done(c.probe));
	/* Reachable from 0: {0,1,2,3}; 4 and 5 not reachable. */
	assert_int64(net(&out, 0), ==, 1);
	assert_int64(net(&out, 1), ==, 1);
	assert_int64(net(&out, 2), ==, 1);
	assert_int64(net(&out, 3), ==, 1);
	assert_int64(net(&out, 4), ==, 0);
	assert_int64(net(&out, 5), ==, 0);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Fixpoint 3: reachability on three Process workers.                   */
/* ------------------------------------------------------------------ */

typedef struct mw_ctx {
	dtfl_time_class *tc;
	collected out[3];
} mw_ctx;

static void
mw_worker(dtfl_worker *w, void *arg)
{
	mw_ctx *e = arg;
	size_t idx = dtfl_worker_index(w);
	size_t peers = dtfl_worker_peers(w);
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	reach_build c;

	memset(&e->out[idx], 0, sizeof e->out[idx]);
	c.roots = dtfl_input_session_new(dtfl_type_u64(), r);
	c.graph = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	c.out = &e->out[idx];
	dtfl_worker_dataflow(w, "mwreach", e->tc, build_reach, &c);
	feed_reach(w, &c, idx, peers);
	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_diff_free(r);
}

DTFL_TEST(iterate__multiworker)
{
	dtfl_config *cfg = dtfl_config_new();
	mw_ctx e;
	size_t wi;
	int64_t got;
	uint64_t node;

	(void)params; (void)data;
	memset(&e, 0, sizeof e);
	e.tc = dtfl_time_u64();
	dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS);
	dtfl_config_set_threads(cfg, 3);
	assert_int(dtfl_execute(cfg, mw_worker, &e, NULL), ==, 0);

	/* The summed output across workers is the same closure {0,1,2,3}:
	 * distinct exchanges by key, so each node is produced on one worker. */
	for (node = 0; node <= 3; node++) {
		got = 0;
		for (wi = 0; wi < 3; wi++)
			got += net(&e.out[wi], node);
		assert_int64(got, ==, 1);
	}
	for (node = 4; node <= 5; node++) {
		got = 0;
		for (wi = 0; wi < 3; wi++)
			got += net(&e.out[wi], node);
		assert_int64(got, ==, 0);
	}

	dtfl_time_class_free(e.tc);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(iterate__halve_to_odd),
	DTFL_T(iterate__transitive_closure),
	DTFL_T(iterate__multiworker),
	DTFL_T_END
};

DTFL_TEST_MAIN("iterate", tests)
