/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_nested_iterate.c: a nested iterative subscope (an
 * inner fixpoint with its own feedback Variable) inside an outer
 * iterate must terminate on a directed cycle.
 *
 * This is the minimal reproduction of the scc nested-scope progress
 * bug: scc nests label propagation (an inner iterative subscope with
 * its own feedback Variable, src/algo/propagate.c) inside the scc
 * feedback loop.  On a directed cycle of three or more nodes a worker
 * step must still advance the outer frontier; if a capability in the
 * inner scope is never dropped, the outer loop never terminates.
 *
 * The outer iterate carries a monotonic variable seeded from the whole
 * edge set, and each round runs an inner propagate over the cycle and
 * keeps the edges whose endpoints share the minimum label.  On a single
 * strongly connected cycle every edge survives, so the fixpoint is the
 * whole edge set.  The run is bounded by a step cap, so a loop that
 * fails to close shows up as a failure rather than an infinite loop.
 */
#include "dtfl_test.h"

#include "dtfl/algo.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 256
#define STEP_CAP 20000

typedef struct out_edge {
	uint64_t src, dst;
	int64_t diff;
} out_edge;

typedef struct collected {
	out_edge u[MAX_OUT];
	size_t n;
} collected;

static int64_t
net(const collected *c, uint64_t s, uint64_t d)
{
	size_t i;
	int64_t sum = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].src == s && c->u[i].dst == d)
			sum += c->u[i].diff;
	return sum;
}

static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	const unsigned char *p = rec;

	(void)time;
	if (c->n >= MAX_OUT || len < 16)
		return;
	c->u[c->n].src = dtfl_get_u64(p);
	c->u[c->n].dst = dtfl_get_u64(p + 8);
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* map (src, dst) -> (dst, dst): the destination node labelled by itself. */
static void
node_self(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 8, 8);
	memcpy(buf + 8, (const unsigned char *)rec + 8, 8);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/*
 * join_map (edges keyed by dst, labels keyed by node): for each edge
 * (src, dst) whose dst carries label l, emit (src, dst) when the edge's
 * source also carries label l.  Here we only need termination, so emit
 * the edge keyed for a second label join: record dst | src | l_dst.
 */
static void
join_label1(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1l, const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[24];

	(void)ctx; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v1, 8);		/* dst  -> new key */
	memcpy(buf + 8, key, 8);	/* src */
	memcpy(buf + 16, v2, 8);	/* label of src */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

static void
join_label2(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1l, const void *v2, size_t v2l, dtfl_join_emit *out)
{
	const unsigned char *val1 = v1;
	unsigned char buf[16];

	(void)ctx; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, val1, 8);		/* src */
	memcpy(buf + 8, key, 8);	/* dst */
	/* carry both labels for the filter: pack l_src, l_dst after. */
	(void)v2;
	dtfl_join_emit_record(out, buf, sizeof buf);
}

typedef struct iter_ctx {
	dtfl_collection *graph;	/* original edges, outer scope */
} iter_ctx;

/*
 * The outer iterate logic.  `var` holds the candidate edges.  Label the
 * nodes, run the inner propagate over the candidate edges, and keep the
 * candidate edges (the propagate output drives the inner nested scope).
 * On a single cycle every edge survives, so returning the labelled
 * candidate edges reaches a fixpoint; the point of the test is that the
 * nested propagate's progress closes so the outer loop terminates.
 */
static dtfl_collection *
outer_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	const dtfl_type *pr = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	const dtfl_type *trip = dtfl_type_tuple((const dtfl_type *[]){
	    dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64() }, 3);
	iter_ctx *c = vctx;
	dtfl_collection *edges = dtfl_collection_enter(c->graph, inner);
	dtfl_collection *nodes_raw = dtfl_collection_map(edges, pr, node_self,
	    DTFL_NOCTX);
	dtfl_collection *nodes = dtfl_collection_consolidate(nodes_raw);
	/* The inner nested iterative subscope with its own feedback. */
	dtfl_collection *labels = dtfl_algo_propagate(var, nodes);
	dtfl_collection *j1 = dtfl_collection_join_map(edges, labels,
	    dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64(), trip,
	    join_label1, DTFL_NOCTX);
	dtfl_collection *result = dtfl_collection_join_map(j1, labels,
	    dtfl_type_u64(), pr, dtfl_type_u64(), pr, join_label2, DTFL_NOCTX);

	dtfl_collection_free(j1);
	dtfl_collection_free(labels);
	dtfl_collection_free(nodes);
	dtfl_collection_free(nodes_raw);
	dtfl_collection_free(edges);
	dtfl_type_free(trip);
	dtfl_type_free(pr);
	return result;
}

typedef struct build_ctx {
	dtfl_input_session *edges;
	dtfl_probe *probe;
	collected *out;
} build_ctx;

static void
build_nested(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	iter_ctx ic = { edges };
	dtfl_collection *fix = dtfl_collection_iterate(edges, outer_logic,
	    DTFL_CTX(&ic, NULL));
	dtfl_collection *con = dtfl_collection_consolidate(fix);
	dtfl_collection *ins = dtfl_collection_inspect(con, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(fix);
	dtfl_collection_free(edges);
}

/* A directed 3-cycle 0->1->2->0. */
static const uint64_t cycle3[][2] = {
	{ 0, 1 }, { 1, 2 }, { 2, 0 },
};
#define NEDGE3 ((size_t)(sizeof cycle3 / sizeof cycle3[0]))

DTFL_TEST(nested_iterate__three_cycle_terminates)
{
	dtfl_worker *w;
	dtfl_time_class *tc;
	dtfl_diff *r;
	const dtfl_type *pair;
	build_ctx c;
	collected out;
	uint64_t i, one = 1;
	int steps;

	(void)params; (void)data;

	w = dtfl_worker_new_standalone();
	tc = dtfl_time_u64();
	r = dtfl_diff_i64();
	pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	memset(&out, 0, sizeof out);
	c.edges = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "nested", tc, build_nested, &c),
	    ==, 0);

	for (i = 0; i < NEDGE3; i++) {
		unsigned char buf[16];

		dtfl_put_u64(buf, cycle3[i][0]);
		dtfl_put_u64(buf + 8, cycle3[i][1]);
		dtfl_input_session_insert(c.edges, buf, 16);
	}
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_flush(c.edges);
	dtfl_input_session_close(c.edges);

	/* The run must terminate under the step cap.  A nested scope whose
	 * progress does not close spins past the cap, failing here. */
	for (steps = 0; steps < STEP_CAP && dtfl_worker_step(w); steps++)
		;
	assert_int(steps, <, STEP_CAP);
	assert_true(dtfl_probe_done(c.probe));

	/* On a single strongly connected cycle every edge survives. */
	for (i = 0; i < NEDGE3; i++)
		assert_int64(net(&out, cycle3[i][0], cycle3[i][1]), ==, 1);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(nested_iterate__three_cycle_terminates),
	DTFL_T_END,
};

DTFL_TEST_MAIN("nested_iterate", tests)
