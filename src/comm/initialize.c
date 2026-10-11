/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/initialize.c: building the allocators a configuration asks
 * for.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/initialize.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * This is Config::try_build.  initialize_from and WorkerGuards spawn
 * the worker threads; in libdtfl that is the timely layer's job
 * (dtfl_execute), so they are not here.  The Config enum and its
 * command-line parsing are in config.c.
 */
#include "comm/init_int.h"
#include "comm/sim_int.h"

int
__dtfl_comm_initialize(const dtfl_config *cfg,
    __dtfl_allocator_builder ***builders, size_t *nbuilders,
    __dtfl_comms_guard **guard)
{
	size_t threads = dtfl_config_threads(cfg);
	__dtfl_allocator_builder **b;

	*builders = NULL;
	*nbuilders = 0;
	*guard = NULL;
	switch (dtfl_config_mode(cfg)) {
	case DTFL_COMM_THREAD:
		b = __dtfl_alloc(sizeof *b);
		b[0] = __dtfl_thread_builder();
		threads = 1;
		break;
	case DTFL_COMM_PROCESS:
		b = __dtfl_calloc(threads, sizeof *b);
		__dtfl_process_builders(threads, b);
		break;
	case DTFL_COMM_SIM:
		b = __dtfl_calloc(threads, sizeof *b);
		__dtfl_sim_builders(threads, dtfl_config_seed(cfg), b);
		break;
	case DTFL_COMM_PROCESS_BINARY:
	case DTFL_COMM_CLUSTER:
	default:
		return __dtfl_err(DTFL_ENOTSUP, "communication mode %d is not "
		    "implemented yet", (int)dtfl_config_mode(cfg));
	}
	*builders = b;
	*nbuilders = threads;
	return 0;
}

void
__dtfl_comms_guard_join(__dtfl_comms_guard *guard)
{
	/* No mode builds network threads yet, so there is never a guard. */
	DTFL_ASSERT(guard == NULL);
}
