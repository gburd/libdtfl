/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/execute.c: starting workers.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/execute.rs
 *   communication/src/initialize.rs (initialize_from, WorkerGuards)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * dtfl_execute builds one allocator builder per local worker
 * (__dtfl_comm_initialize), starts a thread per worker (except for a
 * single Thread worker, which runs on the caller), and on each runs the
 * user function, then steps until no dataflows remain, as execute_from
 * does.  The simulation mode runs all workers on the calling thread,
 * round-robin by a seeded schedule, until every worker's function has
 * returned and every dataflow has completed.
 */
#include "timely/scope_int.h"
#include "base/os_thread.h"
#include "comm/init_int.h"
#include "comm/sim_int.h"

#include <stdio.h>

struct dtfl_guards {
	size_t n;
	__dtfl_thread *threads;
	struct worker_start *starts;
	__dtfl_comms_guard *net;
	int status;
};

typedef struct worker_start {
	__dtfl_allocator_builder *builder;
	dtfl_progress_mode mode;
	dtfl_worker_fn fn;
	void *ctx;
	size_t index;
	const dtfl_config *cfg;	/* borrowed; for log subscriptions */
} worker_start;

/*
 * One worker's whole life: build the allocator on this thread (the
 * builder is Send, the allocator is not), run the function, then step
 * until the dataflows are done.
 */
static void
run_worker(worker_start *ws)
{
	dtfl_worker pub;
	__dtfl_allocator *a = __dtfl_allocator_build(ws->builder);

	pub.w = __dtfl_worker_new(a, ws->mode, true, NULL, NULL);
	pub.standalone = false;
	if (ws->cfg != NULL) {
		__dtfl_worker_install_log_subs(pub.w, ws->cfg);
		pub.w->idle_merge_effort =
		    (size_t)dtfl_config_idle_merge_effort(ws->cfg);
	}
	ws->fn(&pub, ws->ctx);
	while (pub.w->dataflows.len > 0)
		(void)__dtfl_worker_step_or_park(pub.w, UINT64_MAX);
	__dtfl_worker_free(pub.w);
}

static void *
worker_thread(void *arg)
{
	run_worker(arg);
	return NULL;
}

int
dtfl_execute(const dtfl_config *cfg, dtfl_worker_fn fn, void *ctx,
    dtfl_guards **guards_out)
{
	__dtfl_allocator_builder **builders = NULL;
	size_t n = 0, i;
	__dtfl_comms_guard *net = NULL;
	dtfl_guards *g;
	int rc;

	if (cfg == NULL || fn == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_execute: NULL config or function");
	if (dtfl_config_mode(cfg) == DTFL_COMM_SIM)
		return __dtfl_sim_execute(cfg, fn, ctx);
	rc = __dtfl_comm_initialize(cfg, &builders, &n, &net);
	if (rc != DTFL_OK)
		return rc;
	g = __dtfl_calloc(1, sizeof *g);
	g->n = n;
	g->net = net;
	g->threads = __dtfl_calloc(DTFL_MAX(n, 1), sizeof *g->threads);
	g->starts = __dtfl_calloc(DTFL_MAX(n, 1), sizeof *g->starts);
	for (i = 0; i < n; i++) {
		g->starts[i].builder = builders[i];
		g->starts[i].mode = dtfl_config_progress_mode(cfg);
		g->starts[i].fn = fn;
		g->starts[i].ctx = ctx;
		g->starts[i].index = i;
		g->starts[i].cfg = cfg;
	}
	__dtfl_free(builders);
	if (n == 1 && dtfl_config_mode(cfg) == DTFL_COMM_THREAD && guards_out == NULL) {
		/* A single Thread worker runs on the calling thread. */
		run_worker(&g->starts[0]);
		g->n = 0;
	} else {
		for (i = 0; i < n; i++) {
			char name[48];

			snprintf(name, sizeof name, "timely:work-%zu", i);
			rc = __dtfl_thread_spawn(name, worker_thread, &g->starts[i],
			    &g->threads[i]);
			if (rc != DTFL_OK)
				__dtfl_panic("cannot start worker thread %zu: %s", i,
				    dtfl_last_error_detail());
		}
	}
	if (guards_out != NULL) {
		*guards_out = g;
		return DTFL_OK;
	}
	return dtfl_guards_join(g);
}

/* WorkerGuards::join: waits for every worker and the network threads. */
int
dtfl_guards_join(dtfl_guards *g)
{
	size_t i;
	int status;

	if (g == NULL)
		return DTFL_OK;
	for (i = 0; i < g->n; i++)
		(void)__dtfl_thread_join(g->threads[i]);
	__dtfl_comms_guard_join(g->net);
	status = g->status;
	__dtfl_free(g->threads);
	__dtfl_free(g->starts);
	__dtfl_free(g);
	return status;
}

/* execute_directly: one Thread worker on the calling thread. */
int
dtfl_execute_directly(dtfl_worker_fn fn, void *ctx)
{
	dtfl_config *cfg = dtfl_config_new();
	int rc;

	rc = dtfl_execute(cfg, fn, ctx, NULL);
	dtfl_config_free(cfg);
	return rc;
}

/* A worker the caller steps by hand (the threadless example). */
dtfl_worker *
dtfl_worker_new_standalone(void)
{
	dtfl_worker *pub = __dtfl_calloc(1, sizeof *pub);
	__dtfl_allocator_builder *b = __dtfl_thread_builder();

	pub->w = __dtfl_worker_new(__dtfl_allocator_build(b), DTFL_PROGRESS_DEMAND,
	    true, NULL, NULL);
	pub->standalone = true;
	return pub;
}

void
dtfl_worker_free(dtfl_worker *pub)
{
	if (pub == NULL)
		return;
	DTFL_ASSERT(pub->standalone);
	__dtfl_worker_free(pub->w);
	__dtfl_free(pub);
}

size_t
dtfl_worker_index(const dtfl_worker *pub)
{
	return pub->w->allocator->index;
}

size_t
dtfl_worker_peers(const dtfl_worker *pub)
{
	return pub->w->allocator->peers;
}

bool
dtfl_worker_step(dtfl_worker *pub)
{
	return __dtfl_worker_step_or_park(pub->w, 0);
}

bool
dtfl_worker_step_or_park(dtfl_worker *pub, uint64_t timeout_ns)
{
	return __dtfl_worker_step_or_park(pub->w, timeout_ns);
}

void
dtfl_worker_step_while(dtfl_worker *pub, bool (*cond)(void *), void *ctx)
{
	while (cond(ctx))
		(void)__dtfl_worker_step_or_park(pub->w, 0);
}

bool
dtfl_worker_has_dataflows(const dtfl_worker *pub)
{
	return pub->w->dataflows.len > 0;
}

void
dtfl_worker_drop_dataflow(dtfl_worker *pub, size_t index)
{
	__dtfl_worker_drop_dataflow(pub->w, index);
}

size_t
dtfl_worker_next_dataflow_index(const dtfl_worker *pub)
{
	return pub->w->dataflow_counter;
}

int
dtfl_worker_dataflow(dtfl_worker *pub, const char *name,
    const dtfl_time_class *tc, dtfl_build_fn build, void *ctx)
{
	return __dtfl_worker_dataflow(pub->w, pub, name, tc, build, ctx);
}

/*
 * The simulation runtime (PLAN D8): every worker on the calling thread.
 * Each round picks a live worker by the seeded generator and either
 * runs the rest of its user function (once, at the start) or takes one
 * step.  A worker is done when its function has returned and it has no
 * dataflows.  Time is virtual: the sim allocator's clock drives both
 * activations and log timestamps, so a seed fully determines the run.
 */
typedef struct sim_worker {
	dtfl_worker pub;
	bool started;
	bool done;
} sim_worker;

static uint64_t
sim_clock_fn(void *sim)
{
	return __dtfl_sim_clock(sim);
}

int
__dtfl_sim_execute(const dtfl_config *cfg, dtfl_worker_fn fn, void *ctx)
{
	size_t n = dtfl_config_threads(cfg), i, live;
	__dtfl_allocator_builder **b = __dtfl_calloc(DTFL_MAX(n, 1), sizeof *b);
	sim_worker *ws = __dtfl_calloc(DTFL_MAX(n, 1), sizeof *ws);
	void *sim = NULL;
	uint64_t steps = 0;

	if (n == 0)
		n = 1;
	__dtfl_sim_builders(n, dtfl_config_seed(cfg), b);
	for (i = 0; i < n; i++) {
		__dtfl_allocator *a = __dtfl_allocator_build(b[i]);

		sim = __dtfl_sim_of(a);
		ws[i].pub.w = __dtfl_worker_new(a, dtfl_config_progress_mode(cfg), true,
		    sim_clock_fn, sim);
		__dtfl_worker_install_log_subs(ws[i].pub.w, cfg);
		ws[i].pub.w->idle_merge_effort =
		    (size_t)dtfl_config_idle_merge_effort(cfg);
	}
	__dtfl_free(b);
	/*
	 * User functions run first, in worker order: they build dataflows,
	 * and dataflows must be built in the same order on every worker.  A
	 * function that steps its worker (as most do) interleaves with the
	 * others through the steps it takes on its own worker only, which is
	 * the documented limit of the simulator: cross-worker progress inside
	 * one user function requires that function to return.
	 */
	for (i = 0; i < n; i++) {
		fn(&ws[i].pub, ctx);
		ws[i].started = true;
	}
	do {
		live = 0;
		for (i = 0; i < n; i++)
			if (!ws[i].done)
				live++;
		if (live == 0)
			break;
		i = (size_t)(__dtfl_sim_rand(sim) % n);
		if (ws[i].done)
			continue;
		if (!__dtfl_worker_step_or_park(ws[i].pub.w, 0))
			ws[i].done = true;
		__dtfl_sim_advance(sim, 1000);
		if (++steps > UINT64_C(100000000))
			__dtfl_panic("simulation made no progress after %llu steps",
			    (unsigned long long)steps);
	} while (live > 0);
	for (i = 0; i < n; i++)
		__dtfl_worker_free(ws[i].pub.w);
	__dtfl_free(ws);
	return DTFL_OK;
}
