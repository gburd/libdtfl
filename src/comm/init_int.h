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

/* Joins the network threads; returned for Cluster configurations. */
typedef struct __dtfl_comms_guard __dtfl_comms_guard;

/*
 * Builds `*nbuilders` allocator builders (one per local worker) for cfg.
 * On success returns 0, stores a newly allocated builders array and, for
 * clusters, a network guard (else NULL).  On failure returns DTFL_EIO or
 * DTFL_EINVAL with error detail.
 */
int __dtfl_comm_initialize(const dtfl_config *cfg,
    __dtfl_allocator_builder ***builders, size_t *nbuilders,
    __dtfl_comms_guard **guard);

/* Waits for network threads to exit, then frees the guard. */
void __dtfl_comms_guard_join(__dtfl_comms_guard *guard);

#endif /* DTFL_INIT_INT_H */
