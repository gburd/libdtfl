/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_worker.c: the P3 exit check (PLAN section 10).
 *
 * A hand-built two-operator dataflow, using only the raw Operate and
 * Schedule interfaces (no P4 dataflow layer): a source operator holding
 * a capability that it downgrades through times 0..ROUNDS-1 and then
 * drops, and a sink with one input connected to it.  Data never flows;
 * progress does.  The sink records the frontier it observes through
 * its shared progress.  On every worker, the sink's frontier must pass
 * each round's time, and finally become empty, and the dataflow must
 * complete.  Run on the Thread, Process and Sim allocators.
 */
#include "dtfl_test.h"
#include "timely/scope_int.h"

#include <stdlib.h>

#define ROUNDS 5

/* ---- the source: one output, a capability it advances ---- */

typedef struct src_op {
	__dtfl_operate operate;
	__dtfl_schedule sched;
	__dtfl_shared_progress *sp;	/* owned until initialize */
	const dtfl_time_class *tc;
	uint64_t now;			/* current capability time */
	uint64_t rounds;		/* advance through times 0..rounds-1 */
	size_t peers;
	bool released;
	__dtfl_worker *w;
} src_op;

static const __dtfl_frontier_interest no_interest[1] = { DTFL_INTEREST_NEVER };

static bool op_local(const __dtfl_operate *op) { (void)op; return true; }
static size_t src_inputs(const __dtfl_operate *op) { (void)op; return 0; }
static size_t src_outputs(const __dtfl_operate *op) { (void)op; return 1; }
static const __dtfl_frontier_interest *
src_notify(const __dtfl_operate *op) { (void)op; return no_interest; }

/*
 * Advances the capability by one time unit each schedule until `rounds`,
 * then releases it.  Returns true (incomplete) while it still holds the
 * capability, false once released, as the Schedule contract requires.
 *
 * Each worker advances on its own schedule, so with more than one worker
 * the per-worker frontiers would diverge and a straggler would announce
 * a capability already retired cluster-wide (which the subgraph's
 * debug-build progress check rejects, correctly).  Multi-worker runs
 * therefore use rounds == 1: hold time 0, then drop it.  Multi-round
 * progression is exercised by the single-worker Thread and Sim runs.
 */
static bool
src_schedule(__dtfl_schedule *s)
{
	src_op *o = (src_op *)((char *)s - offsetof(src_op, sched));
	uint64_t next;

	if (o->released)
		return false;
	if (o->now + 1 < o->rounds) {
		next = o->now + 1;
		__dtfl_cb_update(&o->sp->internals[0], &next, 1);
		__dtfl_cb_update(&o->sp->internals[0], &o->now, -1);
		o->now = next;
		__dtfl_activations_activate(&o->w->activations, s->path, s->path_len);
		return true;
	}
	__dtfl_cb_update(&o->sp->internals[0], &o->now, -1);
	o->released = true;
	return false;
}

static void
src_destroy(__dtfl_schedule *s)
{
	src_op *o = (src_op *)((char *)s - offsetof(src_op, sched));

	__dtfl_free(s->name);
	__dtfl_free(s->path);
	__dtfl_free(o);
}

static const __dtfl_schedule_ops src_sched_ops = { src_schedule, src_destroy };

static __dtfl_schedule *
src_initialize(__dtfl_operate *op, __dtfl_connectivity *conn,
    __dtfl_shared_progress **progress)
{
	src_op *o = (src_op *)op;
	uint64_t zero = 0;

	__dtfl_connectivity_init(conn, o->tc, 0);
	/* Every worker's source starts with one capability at time 0. */
	__dtfl_cb_update(&o->sp->internals[0], &zero, 1);
	*progress = o->sp;
	__dtfl_activations_activate(&o->w->activations, o->sched.path,
	    o->sched.path_len);
	return &o->sched;
}

static void
src_operate_destroy(__dtfl_operate *op)
{
	src_op *o = (src_op *)op;

	__dtfl_shared_progress_free(o->sp);
	src_destroy(&o->sched);
}

static const __dtfl_operate_ops src_ops = {
	op_local, src_inputs, src_outputs, src_notify, src_initialize,
	src_operate_destroy
};

/* ---- the sink: one input, records its frontier ---- */

typedef struct sink_op {
	__dtfl_operate operate;
	__dtfl_schedule sched;
	__dtfl_shared_progress *sp;
	const dtfl_time_class *tc;
	__dtfl_mutable_antichain frontier;
	uint64_t rounds;		/* expected final time */
	uint64_t *passed;		/* shared result: highest round passed + 1 */
	bool *emptied;
} sink_op;

static const __dtfl_frontier_interest always[1] = { DTFL_INTEREST_ALWAYS };

static size_t sink_inputs(const __dtfl_operate *op) { (void)op; return 1; }
static size_t sink_outputs(const __dtfl_operate *op) { (void)op; return 0; }
static const __dtfl_frontier_interest *
sink_notify(const __dtfl_operate *op) { (void)op; return always; }

static bool
sink_schedule(__dtfl_schedule *s)
{
	sink_op *o = (sink_op *)((char *)s - offsetof(sink_op, sched));
	uint64_t t;

	__dtfl_mantichain_update_batch(&o->frontier, &o->sp->frontiers[0]);
	__dtfl_cb_clear(&o->frontier.changes);
	if (__dtfl_mantichain_is_empty(&o->frontier)) {
		*o->emptied = true;
		*o->passed = o->rounds;
		return false;
	}
	t = __dtfl_mantichain_frontier(&o->frontier)[0];
	if (t > *o->passed)
		*o->passed = t;
	return true;
}

static void
sink_destroy(__dtfl_schedule *s)
{
	sink_op *o = (sink_op *)((char *)s - offsetof(sink_op, sched));

	__dtfl_mantichain_free(&o->frontier);
	__dtfl_free(s->name);
	__dtfl_free(s->path);
	__dtfl_free(o);
}

static const __dtfl_schedule_ops sink_sched_ops = { sink_schedule, sink_destroy };

static __dtfl_schedule *
sink_initialize(__dtfl_operate *op, __dtfl_connectivity *conn,
    __dtfl_shared_progress **progress)
{
	sink_op *o = (sink_op *)op;

	__dtfl_connectivity_init(conn, o->tc, 1);
	*progress = o->sp;
	return &o->sched;
}

static void
sink_operate_destroy(__dtfl_operate *op)
{
	sink_op *o = (sink_op *)op;

	__dtfl_shared_progress_free(o->sp);
	sink_destroy(&o->sched);
}

static const __dtfl_operate_ops sink_ops = {
	op_local, sink_inputs, sink_outputs, sink_notify, sink_initialize,
	sink_operate_destroy
};

/* ---- building the dataflow ---- */

typedef struct result {
	uint64_t passed;
	bool emptied;
	size_t index, peers;
} result;

#define MAX_WORKERS 8

typedef struct run_ctx {
	dtfl_time_class *tc;
	uint64_t rounds;
	result r[MAX_WORKERS];
} run_ctx;

static void
schedule_init(__dtfl_schedule *s, const char *name, dtfl_scope *scope,
    __dtfl_slot *slot)
{
	s->name = __dtfl_strdup(name);
	s->path = __dtfl_scope_slot_addr(scope, slot, &s->path_len);
}

static void
build(dtfl_scope *scope, void *arg)
{
	run_ctx *rc = arg;
	__dtfl_worker *w = scope->worker;
	result *r = &rc->r[w->allocator->index];
	__dtfl_slot s1 = __dtfl_scope_reserve(scope);
	__dtfl_slot s2 = __dtfl_scope_reserve(scope);
	src_op *src = __dtfl_calloc(1, sizeof *src);
	sink_op *sink = __dtfl_calloc(1, sizeof *sink);
	__dtfl_location from = { s1.index, 0, true }, to = { s2.index, 0, false };

	src->operate.ops = &src_ops;
	src->sched.ops = &src_sched_ops;
	src->tc = dtfl_scope_time_class(scope);
	src->sp = __dtfl_shared_progress_new(src->tc, 0, 1);
	src->rounds = rc->rounds;
	src->w = w;
	schedule_init(&src->sched, "Source", scope, &s1);

	sink->operate.ops = &sink_ops;
	sink->sched.ops = &sink_sched_ops;
	sink->tc = dtfl_scope_time_class(scope);
	sink->sp = __dtfl_shared_progress_new(sink->tc, 1, 0);
	__dtfl_mantichain_init(&sink->frontier, sink->tc);
	sink->rounds = rc->rounds;
	sink->passed = &r->passed;
	sink->emptied = &r->emptied;
	schedule_init(&sink->sched, "Sink", scope, &s2);

	__dtfl_scope_add_edge(scope, from, to);
	__dtfl_scope_install(scope, &s1, &src->operate);
	__dtfl_scope_install(scope, &s2, &sink->operate);
}

static void
worker_main(dtfl_worker *pub, void *arg)
{
	run_ctx *rc = arg;
	result *r = &rc->r[dtfl_worker_index(pub)];

	r->index = dtfl_worker_index(pub);
	r->peers = dtfl_worker_peers(pub);
	assert_int(dtfl_worker_dataflow(pub, "progress", rc->tc, build, rc), ==, 0);
}

static void
run_mode(dtfl_comm_mode mode, size_t threads, uint64_t seed, uint64_t rounds)
{
	dtfl_config *cfg = dtfl_config_new();
	run_ctx rc;
	size_t i;

	memset(&rc, 0, sizeof rc);
	rc.tc = dtfl_time_u64();
	rc.rounds = rounds;
	assert_int(dtfl_config_set_mode(cfg, mode), ==, 0);
	if (mode != DTFL_COMM_THREAD)
		assert_int(dtfl_config_set_threads(cfg, threads), ==, 0);
	dtfl_config_set_seed(cfg, seed);
	assert_int(dtfl_execute(cfg, worker_main, &rc, NULL), ==, 0);
	for (i = 0; i < threads; i++) {
		assert_size(rc.r[i].index, ==, i);
		assert_size(rc.r[i].peers, ==, threads);
		assert_true(rc.r[i].emptied);
		assert_uint64(rc.r[i].passed, ==, rounds);
	}
	dtfl_time_class_free(rc.tc);
	dtfl_config_free(cfg);
}

/*
 * One worker: the source advances through all ROUNDS times, so this
 * exercises multi-step frontier progression.
 */
DTFL_TEST(progress_on_thread)
{
	(void)params; (void)data;
	run_mode(DTFL_COMM_THREAD, 1, 0, ROUNDS);
	return MUNIT_OK;
}

/*
 * Four worker threads: each holds time 0 and drops it (rounds == 1), so
 * every worker does identical work and their frontiers stay in step.
 * This exercises cross-thread progress exchange and distributed
 * termination.
 */
DTFL_TEST(progress_on_process)
{
	(void)params; (void)data;
	run_mode(DTFL_COMM_PROCESS, 4, 0, 1);
	return MUNIT_OK;
}

/*
 * The simulator: one worker advancing through all ROUNDS over twenty
 * seeds (multi-step progression), then three workers at rounds == 1
 * (seeded cross-worker exchange and termination).
 */
DTFL_TEST(progress_on_sim)
{
	uint64_t seed;

	(void)params; (void)data;
	for (seed = 1; seed <= 20; seed++)
		run_mode(DTFL_COMM_SIM, 1, seed, ROUNDS);
	for (seed = 1; seed <= 20; seed++)
		run_mode(DTFL_COMM_SIM, 3, seed, 1);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(progress_on_thread),
	DTFL_T(progress_on_process),
	DTFL_T(progress_on_sim),
	DTFL_T_END
};

DTFL_TEST_MAIN("worker", tests)
