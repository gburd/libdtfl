/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_sim.h: run a multi-worker property under a drawn
 * execution (P16.7).
 *
 *   pbt_exec x;
 *   pbt_exec_draw(tc, &x, 2, 4);       mode, workers, seed, faults
 *   PBT_ASSERT(pbt_exec_run(tc, &x, worker_fn, ctx) == 0);
 *
 * The draw picks the simulator (DTFL_COMM_SIM) or real threads
 * (DTFL_COMM_PROCESS), the worker count, the simulator's seed, the
 * progress mode, and the simulator's faults (sim_int.h).  Everything is
 * drawn whatever the mode, so a reproduce blob means the same case
 * under any override, and shrinking moves toward the simulator with no
 * faults and seed 0.
 *
 * DTFL_PBT_COMM=sim or =process overrides the drawn mode (check-sim
 * sets sim), and DTFL_PBT_FAULTS=on or =off whether the faults are
 * enabled (the fault values are still drawn).  Under the simulator a library panic, including a
 * simulation that exceeds its step cap, ends the case as a failure
 * with the panic message in its note, so hegel can shrink it; under
 * real threads a panic aborts as usual.  A case that panicked leaks the
 * simulated workers.
 *
 * Under the simulator every worker function runs to completion before
 * any other worker steps (src/timely/execute.c), so a worker function
 * must not wait for progress that needs another worker.  Feed, close
 * the inputs and return; dtfl_execute steps the dataflows to the end.
 */
#ifndef DTFL_PBT_SIM_H
#define DTFL_PBT_SIM_H

#include "pbt.h"

#include "comm/sim_int.h"
#include "dtfl/config.h"
#include "dtfl/timely.h"

/* A livelock fails a case after this many scheduler turns. */
#define PBT_SIM_MAX_STEPS UINT64_C(2000000)

typedef struct pbt_exec {
	dtfl_comm_mode mode;
	size_t threads;
	uint64_t seed;
	dtfl_progress_mode progress;
	__dtfl_sim_faults faults;
} pbt_exec;

static void
pbt_exec_draw(pbt_tc *tc, pbt_exec *x, size_t min_threads, size_t max_threads)
{
	const char *force = getenv("DTFL_PBT_COMM");
	const char *faults = getenv("DTFL_PBT_FAULTS");
	bool sim = pbt_int(tc, 0, 1) == 0, on;

	memset(x, 0, sizeof *x);
	x->threads = (size_t)pbt_int(tc, (int64_t)min_threads,
	    (int64_t)max_threads);
	x->seed = (uint64_t)pbt_int(tc, 0, INT64_MAX);
	x->progress = pbt_bool(tc) ? DTFL_PROGRESS_EAGER : DTFL_PROGRESS_DEMAND;
	on = pbt_bool(tc);
	if (faults != NULL && strcmp(faults, "on") == 0)
		on = true;
	else if (faults != NULL && strcmp(faults, "off") == 0)
		on = false;
	if (on) {
		x->faults.stall_permille = (uint32_t)pbt_int(tc, 0, 500);
		x->faults.stall_max = (uint32_t)pbt_int(tc, 1, 64);
		x->faults.delay_max = (uint32_t)pbt_int(tc, 0, 32);
	}
	x->faults.max_steps = PBT_SIM_MAX_STEPS;
	if (force != NULL && strcmp(force, "sim") == 0)
		sim = true;
	else if (force != NULL && strcmp(force, "process") == 0)
		sim = false;
	x->mode = sim ? DTFL_COMM_SIM : DTFL_COMM_PROCESS;
	pbt_note(tc, "exec: %s workers=%zu seed=%llu progress=%s "
	    "stall=%u/1000 stall_max=%u delay_max=%u", sim ? "sim" : "process",
	    x->threads, (unsigned long long)x->seed,
	    x->progress == DTFL_PROGRESS_EAGER ? "eager" : "demand",
	    x->faults.stall_permille, x->faults.stall_max,
	    x->faults.delay_max);
}

#if defined(DTFL_HAVE_HEGEL)

/* The case a simulator panic ends; the simulator runs on this thread. */
static pbt_tc *pbt_exec_tc;

static void
pbt_exec_panic(const char *msg, void *ctx)
{
	pbt_tc *tc = pbt_exec_tc;

	(void)ctx;
	if (tc != NULL) {
		pbt_exec_tc = NULL;
		dtfl_set_panic_hook(NULL, NULL);
		pbt_note(tc, "panic: %s", msg);
		pbt_unwind(tc, HEGEL_STATUS_INTERESTING, "simulation panic");
	}
	fprintf(stderr, "libdtfl panic: %s\n", msg);
	abort();
}

#endif

static void
pbt_exec_free_config(void *cfg)
{
	dtfl_config_free(cfg);
}

/* Runs fn on every worker of x; returns dtfl_execute's result. */
static int
pbt_exec_run(pbt_tc *tc, const pbt_exec *x, dtfl_worker_fn fn, void *ctx)
{
	dtfl_config *cfg = dtfl_config_new();
	int rc;

	pbt_defer(tc, pbt_exec_free_config, cfg);
	if (dtfl_config_set_mode(cfg, x->mode) != 0 ||
	    dtfl_config_set_threads(cfg, x->threads) != 0)
		return -1;
	dtfl_config_set_seed(cfg, x->seed);
	dtfl_config_set_progress_mode(cfg, x->progress);
	if (x->mode != DTFL_COMM_SIM)
		return dtfl_execute(cfg, fn, ctx, NULL);
	__dtfl_config_set_sim_faults(cfg, &x->faults);
#if defined(DTFL_HAVE_HEGEL)
	pbt_exec_tc = tc;
	dtfl_set_panic_hook(pbt_exec_panic, NULL);
#endif
	rc = dtfl_execute(cfg, fn, ctx, NULL);
#if defined(DTFL_HAVE_HEGEL)
	pbt_exec_tc = NULL;
	dtfl_set_panic_hook(NULL, NULL);
#endif
	return rc;
}

#endif /* DTFL_PBT_SIM_H */
