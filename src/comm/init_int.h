/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/init_int.h: communication configuration and launch.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/initialize.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * __dtfl_comm_initialize builds one allocator builder per local worker
 * for the configured mode, sets up networking for clusters (connects
 * sockets, starts one send and one receive thread per remote process),
 * and returns a guard that joins those threads.  The timely layer
 * (src/timely/execute.c) spawns the worker threads and hands each its
 * builder.
 */
#ifndef DTFL_INIT_INT_H
#define DTFL_INIT_INT_H

#include "comm/comm_int.h"
#include "dtfl/config.h"
#include "dtfl/log.h"

/* Joins the network threads; returned for Cluster configurations. */
typedef struct __dtfl_comms_guard __dtfl_comms_guard;

/*
 * Builds `*nbuilders` allocator builders (one per local worker) for cfg.
 * On success returns 0, stores a newly allocated builders array (free it
 * with __dtfl_free after handing out or destroying every builder) and,
 * for clusters, a network guard (else NULL).  On failure returns
 * DTFL_EIO or DTFL_EINVAL with error detail, or DTFL_ENOTSUP for a mode
 * this build cannot run yet (ProcessBinary and Cluster until the
 * zero-copy allocators land).
 */
int __dtfl_comm_initialize(const dtfl_config *cfg,
    __dtfl_allocator_builder ***builders, size_t *nbuilders,
    __dtfl_comms_guard **guard);

/* Waits for network threads to exit, then frees the guard.  NULL is a no-op. */
void __dtfl_comms_guard_join(__dtfl_comms_guard *guard);

/*
 * create_sockets (networking.rs): build the full TCP mesh among
 * `processes` processes.  On success fills *fds_out, a newly allocated
 * array of `processes` fds with my_index == -1 and a connected socket
 * elsewhere (__dtfl_free it after passing to __dtfl_tcp_initialize),
 * and returns 0; else DTFL_EIO or DTFL_EINVAL with detail.
 */
int __dtfl_create_sockets(char *const *addresses, size_t processes,
    size_t my_index, int **fds_out);

/*
 * initialize_networking_from_sockets (zero_copy/initialize.rs): wrap the
 * `threads` typed Process builders in TcpBuilders and start the network
 * threads over `fds`.  `spill` (borrowed, may be NULL) is Hooks::spill:
 * the builders and send threads retain it for their queues.  Takes ownership of `inner` (the array is freed)
 * and of `fds` (closed by the guard).  On success stores the builders
 * array (threads entries; free it after handing out every builder) and
 * the guard, and returns 0.
 */
int __dtfl_tcp_initialize(__dtfl_allocator_builder **inner, size_t threads,
    size_t my_index, size_t processes, int *fds, __dtfl_spill_fn *spill,
    __dtfl_allocator_builder ***builders, __dtfl_comms_guard **guard);

/*
 * Cluster { zerocopy }: -z was given with -n > 1, so intra-process
 * peers use ProcessBinary under the TCP allocator.  Not public yet; the
 * cluster allocator is its only reader.
 */
bool __dtfl_config_zerocopy(const dtfl_config *cfg);

/*
 * The spill factory set by dtfl_config_set_spill_file (borrowed), or
 * NULL.  Allocators that create MergeQueues retain it.
 */
__dtfl_spill_fn *__dtfl_config_spill(const dtfl_config *cfg);

/*
 * Log subscriptions recorded on a config (dtfl_config_log_subscribe),
 * installed on every worker as it starts.  The ctx is shared across
 * worker threads; ctx_free is called once, when the config is freed.
 */
typedef struct __dtfl_log_sub {
	const char *stream;
	dtfl_log_subscriber_fn fn;
	void *ctx;
	void (*ctx_free)(void *);
} __dtfl_log_sub;

size_t __dtfl_config_log_subs(const dtfl_config *cfg,
    const __dtfl_log_sub **subs);

#endif /* DTFL_INIT_INT_H */
