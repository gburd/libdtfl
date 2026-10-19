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

/*
 * Cluster { } (initialize.rs): -n > 1 with a host list.  Build one
 * typed Process builder per local worker, resolve the mesh of sockets,
 * and wrap them in TcpBuilders with the send/recv threads started.
 * Intra-process peers stay on the Process allocator under the TCP
 * layer.
 */
static int
cluster_initialize(const dtfl_config *cfg, size_t threads,
    __dtfl_allocator_builder ***builders, size_t *nbuilders,
    __dtfl_comms_guard **guard)
{
	size_t processes = dtfl_config_processes(cfg);
	size_t my_index = dtfl_config_process(cfg);
	__dtfl_allocator_builder **inner;
	char **addresses;
	int *fds = NULL;
	size_t i;
	int rc;

	/* The host list, as a plain array for create_sockets. */
	addresses = __dtfl_calloc(processes, sizeof *addresses);
	for (i = 0; i < processes; i++)
		addresses[i] = (char *)dtfl_config_address(cfg, i);
	rc = __dtfl_create_sockets(addresses, processes, my_index, &fds);
	__dtfl_free(addresses);
	if (rc != 0)
		return rc;

	inner = __dtfl_calloc(threads, sizeof *inner);
	__dtfl_process_builders(threads, inner);
	rc = __dtfl_tcp_initialize(inner, threads, my_index, processes, fds,
	    __dtfl_config_spill(cfg), builders, guard);
	if (rc != 0)
		return rc;
	*nbuilders = threads;
	return 0;
}

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
	case DTFL_COMM_PROCESS_BINARY:
		b = __dtfl_calloc(threads, sizeof *b);
		__dtfl_process_binary_builders(threads,
		    __dtfl_config_spill(cfg), b);
		break;
	case DTFL_COMM_SIM:
		b = __dtfl_calloc(threads, sizeof *b);
		__dtfl_sim_builders(threads, dtfl_config_seed(cfg), b);
		break;
	case DTFL_COMM_CLUSTER:
		return cluster_initialize(cfg, threads, builders, nbuilders,
		    guard);
	default:
		return __dtfl_err(DTFL_ENOTSUP, "communication mode %d is not "
		    "implemented yet", (int)dtfl_config_mode(cfg));
	}
	*builders = b;
	*nbuilders = threads;
	return 0;
}
