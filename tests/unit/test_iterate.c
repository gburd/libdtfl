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

#include <stdlib.h>
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

/* ------------------------------------------------------------------ */
/* Fixpoint 4: frontier propagation, retracted across epochs.          */
/* ------------------------------------------------------------------ */

/*
 * The frontier computation of differential's examples/progress.rs, cut
 * down to three locations (S1 = 1, T2 = 2, S2 = 3): T2 -> S2 adds one,
 * S2 -> T2 and S1 -> T2 add nothing.  Epoch 1 seeds S1@0, epoch 2 swaps
 * it for T2@0, epoch 3 retracts it.  The reduce inside the loop keeps
 * the least time per location, under Product<u64, u64>; its output
 * trace merges batches whose times advance_by reorders, which once
 * corrupted UpdsBuilder::seal: at (3,0) reduce retracted S2@1 twice,
 * then alternated +-1 at (3,k) forever.
 */
#define FR_MAX 512
#define FR_NLOC 4

typedef struct fr_upd {
	uint64_t loc, t, outer, inner;
	int64_t diff;
} fr_upd;

typedef struct fr_log {
	fr_upd u[FR_MAX];
	size_t n;
} fr_log;

static const uint64_t fr_trans[][3] = {	/* (from, to, summary) */
	{ 2, 3, 1 }, { 3, 2, 0 }, { 1, 2, 0 },
};
#define FR_NTRANS ((size_t)(sizeof fr_trans / sizeof fr_trans[0]))

static void
fr_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	fr_log *l = ctx;
	const unsigned char *p = rec;

	if (l->n >= FR_MAX || len < 16)
		return;
	l->u[l->n].loc = dtfl_get_u64(p);
	l->u[l->n].t = dtfl_get_u64(p + 8);
	l->u[l->n].outer = time[0];
	l->u[l->n].inner = time[1];
	memcpy(&l->u[l->n].diff, diff, sizeof(int64_t));
	l->n++;
}

static void
fr_sink_outer(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	uint64_t t2[2] = { time[0], 0 };

	fr_sink(ctx, rec, len, t2, diff);
}

/* join_map: (from, (to, summ)) x (from, t) -> (to, t + summ). */
static void
fr_step(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	const unsigned char *d = v1;
	unsigned char buf[16];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	dtfl_put_u64(buf, dtfl_get_u64(d));
	dtfl_put_u64(buf + 8, dtfl_get_u64(d + 8) + dtfl_get_u64(v2));
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* reduce: the lower envelope of the (totally ordered) times. */
static void
fr_min(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;
	size_t i, m = 0;

	(void)ctx; (void)key; (void)klen; (void)vlens; (void)diffs;
	for (i = 1; i < n; i++)
		if (dtfl_get_u64(vals[i]) < dtfl_get_u64(vals[m]))
			m = i;
	if (n != 0)
		dtfl_reduce_emit_pair(out, vals[m], 8, &one);
}

typedef struct fr_build {
	dtfl_input_session *trans, *times;
	dtfl_probe *probe;
	fr_log inner, result;
	dtfl_collection *otrans, *otimes;	/* outer scope, for logic */
} fr_build;

static dtfl_collection *
fr_logic(void *vctx, dtfl_scope *inner, dtfl_collection *reach)
{
	fr_build *b = vctx;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *pair = dtfl_type_pair(u64, u64);
	dtfl_collection *tr = dtfl_collection_enter(b->otrans, inner);
	dtfl_collection *j = dtfl_collection_join_map(tr, reach, u64, pair,
	    u64, pair, fr_step, DTFL_NOCTX);
	dtfl_collection *seed = dtfl_collection_enter(b->otimes, inner);
	dtfl_collection *all = dtfl_collection_concat(j, seed);
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *red = dtfl_collection_reduce(all, u64, u64, u64, i64,
	    fr_min, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(red, fr_sink,
	    DTFL_CTX(&b->inner, NULL));

	dtfl_diff_free(i64);
	dtfl_collection_free(red);
	dtfl_collection_free(all);
	dtfl_collection_free(seed);
	dtfl_collection_free(j);
	dtfl_collection_free(tr);
	dtfl_type_free(pair);
	return ins;
}

static void
build_fr(dtfl_scope *scope, void *arg)
{
	fr_build *b = arg;
	dtfl_collection *fix, *ins, *probed;

	b->otrans = dtfl_input_session_to_collection(b->trans, scope);
	b->otimes = dtfl_input_session_to_collection(b->times, scope);
	fix = dtfl_collection_iterate(b->otimes, fr_logic, DTFL_CTX(b, NULL));
	ins = dtfl_collection_inspect(fix, fr_sink_outer,
	    DTFL_CTX(&b->result, NULL));
	probed = dtfl_collection_probe_with(ins, b->probe);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(fix);
	dtfl_collection_free(b->otimes);
	dtfl_collection_free(b->otrans);
}

/* Net of (loc, t) over updates at outer <= e and inner <= k. */
static int64_t
fr_net(const fr_log *l, uint64_t loc, uint64_t t, uint64_t e, uint64_t k)
{
	int64_t sum = 0;
	size_t i;

	for (i = 0; i < l->n; i++)
		if (l->u[i].loc == loc && l->u[i].t == t &&
		    l->u[i].outer <= e && l->u[i].inner <= k)
			sum += l->u[i].diff;
	return sum;
}

/* From scratch: least time reaching each location from `seed`. */
static void
fr_fixpoint(uint64_t seed_loc, bool have_seed, uint64_t *best)
{
	bool changed = true;
	size_t i;

	for (i = 0; i < FR_NLOC; i++)
		best[i] = UINT64_MAX;
	if (!have_seed)
		return;
	best[seed_loc] = 0;
	while (changed) {
		changed = false;
		for (i = 0; i < FR_NTRANS; i++) {
			uint64_t f = fr_trans[i][0], to = fr_trans[i][1];

			if (best[f] != UINT64_MAX &&
			    best[f] + fr_trans[i][2] < best[to]) {
				best[to] = best[f] + fr_trans[i][2];
				changed = true;
			}
		}
	}
}

static void
fr_time(dtfl_input_session *s, uint64_t loc, bool insert)
{
	unsigned char buf[16];

	dtfl_put_u64(buf, loc);
	dtfl_put_u64(buf + 8, 0);
	if (insert)
		dtfl_input_session_insert(s, buf, sizeof buf);
	else
		dtfl_input_session_remove(s, buf, sizeof buf);
}

static int
fr_advance(dtfl_worker *w, fr_build *b, uint64_t t)
{
	int steps = 0;

	dtfl_input_session_advance_to(b->trans, &t);
	dtfl_input_session_flush(b->trans);
	dtfl_input_session_advance_to(b->times, &t);
	dtfl_input_session_flush(b->times);
	/* Stop on log overflow too: the broken loop never settles. */
	while (dtfl_probe_less_than(b->probe, &t) && steps < 20000 &&
	    b->inner.n < FR_MAX) {
		dtfl_worker_step(w);
		steps++;
	}
	return steps;
}

DTFL_TEST(iterate__frontier_retraction_product_time)
{
	/* The seed in force during epoch e (1-based); 0 for none. */
	static const uint64_t seed_at[4] = { 0, 1, 2, 0 };
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	const dtfl_type *trip = dtfl_type_pair(dtfl_type_u64(), pair);
	fr_build *b = calloc(1, sizeof *b);
	uint64_t e, k, loc, t, kmax = 0, best[FR_NLOC];
	size_t i;

	(void)params; (void)data;
	b->trans = dtfl_input_session_new(trip, r);
	b->times = dtfl_input_session_new(pair, r);
	b->probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "frontier", tc, build_fr, b), ==,
	    0);
	for (i = 0; i < FR_NTRANS; i++) {
		unsigned char buf[24];

		dtfl_put_u64(buf, fr_trans[i][0]);
		dtfl_put_u64(buf + 8, fr_trans[i][1]);
		dtfl_put_u64(buf + 16, fr_trans[i][2]);
		dtfl_input_session_insert(b->trans, buf, sizeof buf);
	}
	fr_time(b->times, 1, true);
	assert_int(fr_advance(w, b, 2), <, 20000);
	fr_time(b->times, 1, false);
	fr_time(b->times, 2, true);
	assert_int(fr_advance(w, b, 3), <, 20000);
	fr_time(b->times, 2, false);
	assert_int(fr_advance(w, b, 4), <, 20000);

	for (i = 0; i < b->inner.n; i++)
		if (b->inner.u[i].inner > kmax)
			kmax = b->inner.u[i].inner;
	/* A loop that alternates forever overflows the log. */
	assert_size(b->inner.n, <, FR_MAX);
	for (e = 1; e <= 3; e++) {
		/* Every accumulation of the reduce output is 0 or 1. */
		for (k = 0; k <= kmax; k++)
			for (i = 0; i < b->inner.n; i++) {
				int64_t n = fr_net(&b->inner, b->inner.u[i].loc,
				    b->inner.u[i].t, e, k);

				assert_int64(n, >=, 0);
				assert_int64(n, <=, 1);
			}
		/* The result at epoch e is the from-scratch fixpoint. */
		fr_fixpoint(seed_at[e], seed_at[e] != 0, best);
		for (loc = 1; loc < FR_NLOC; loc++)
			for (t = 0; t <= 2; t++)
				assert_int64(fr_net(&b->result, loc, t, e,
				    UINT64_MAX), ==, best[loc] == t ? 1 : 0);
	}

	dtfl_input_session_close(b->trans);
	dtfl_input_session_close(b->times);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(b->probe);
	free(b);
	dtfl_type_free(trip);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(iterate__halve_to_odd),
	DTFL_T(iterate__transitive_closure),
	DTFL_T(iterate__multiworker),
	DTFL_T(iterate__frontier_retraction_product_time),
	DTFL_T_END
};

DTFL_TEST_MAIN("iterate", tests)
