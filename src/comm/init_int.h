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
#include "base/log_int.h"
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
 * dtfl_execute_worker's communication (ADR-0013): a builder for worker
 * `index` of `peers`, one per process, whose network I/O is driven
 * inline over `t` (borrowed).  *net is the driver; after building the
 * allocator on this thread call __dtfl_inline_net_attach_built(*net),
 * and after destroying it __dtfl_inline_net_finish and _free.  With
 * peers == 1 the builder is a Thread builder and *net is NULL.
 */
struct __dtfl_inline_net;
__dtfl_allocator_builder *__dtfl_inline_builder(const dtfl_config *cfg,
    size_t index, size_t peers, const dtfl_transport *t,
    struct __dtfl_inline_net **net);
void __dtfl_inline_net_attach_built(struct __dtfl_inline_net *net);
/*
 * After the allocator is destroyed: writes what is left and an end of
 * stream to every peer, then reads every peer to its end of stream.
 * Panics like recv_loop on a peer that closes early or sends after its
 * end.
 */
void __dtfl_inline_net_finish(struct __dtfl_inline_net *n);
/* Frees the driver; no wake reaches the transport after this. */
void __dtfl_inline_net_free(struct __dtfl_inline_net *n);

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
 * the guard, and returns 0.  `log_hook` (may be NULL) is Hooks.log_fn:
 * each network thread calls it once, on itself, for its own
 * "communication" logger.  The guard takes the hook by value and runs
 * its ctx_free after joining the network threads.
 */
struct __dtfl_comm_log_hook;
int __dtfl_tcp_initialize(__dtfl_allocator_builder **inner, size_t threads,
    size_t my_index, size_t processes, int *fds, __dtfl_spill_fn *spill,
    const struct __dtfl_comm_log_hook *log_hook,
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

/*
 * CommunicationSetup (communication/src/logging.rs): which network
 * thread a logger is for.
 */
typedef struct __dtfl_comm_setup {
	bool sender;		/* a send thread (else the receive thread) */
	size_t process;		/* this process */
	size_t remote;		/* the remote process */
} __dtfl_comm_setup;

/*
 * Hooks.log_fn: returns a new logger for one network thread (owned by
 * and used only on that thread, which releases it when its loop ends),
 * or NULL for none.  Called on the network thread itself.
 */
typedef struct __dtfl_comm_log_hook {
	__dtfl_logger *(*log_fn)(void *ctx, const __dtfl_comm_setup *setup);
	void *ctx;
	void (*ctx_free)(void *);	/* may be NULL; run at guard join */
} __dtfl_comm_log_hook;

/*
 * The log_fn installed from a "communication" config subscription: ctx
 * is a __dtfl_calloc'd copy of the subscription (__dtfl_log_sub *, freed
 * with __dtfl_free); the logger decodes for its subscriber, reporting
 * this process's index as the batch worker.
 */
__dtfl_logger *__dtfl_comm_subscriber_log_fn(void *ctx,
    const __dtfl_comm_setup *setup);

/* CommunicationEvent encoders (src/comm/comm_log.c), logging to `l`. */
struct __dtfl_message_header;
void __dtfl_comm_log_message(__dtfl_logger *l, bool is_send,
    const struct __dtfl_message_header *h);
void __dtfl_comm_log_state(__dtfl_logger *l, bool send, size_t process,
    size_t remote, bool start);
void __dtfl_comm_log_setup(__dtfl_logger *l, bool sender, size_t process,
    bool has_remote, size_t remote);

/*
 * A standalone logger decoding `stream` records for subscriber fn with
 * shared ctx (not freed), reporting `worker` as the batch's worker
 * index (src/timely/log.c).  Owned by the calling thread.
 */
__dtfl_logger *__dtfl_log_subscriber_logger(const char *stream, size_t worker,
    dtfl_log_subscriber_fn fn, void *ctx);

#endif /* DTFL_INIT_INT_H */
