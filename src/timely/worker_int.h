/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/worker_int.h: activations and the worker.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/scheduling/activate.rs
 *   timely/src/worker.rs
 *   timely/src/execute.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A worker owns an allocator, the activation queue, the logging
 * registry, its dataflows, and the channel-to-path map.  All of it
 * belongs to the worker's thread (PLAN section 5, rule 1).  The only
 * cross-thread entry is the sync activation queue, which other threads
 * reach through dtfl_sync_activator (mutex-protected list plus unpark).
 */
#ifndef DTFL_WORKER_INT_H
#define DTFL_WORKER_INT_H

#include <pthread.h>

#include "base/log_int.h"
#include "comm/comm_int.h"
#include "dtfl/config.h"
#include "dtfl/timely.h"
#include "progress/operate_int.h"

/* ------------------------------------------------------------------ */
/* Activations (activate.rs)                                           */
/* ------------------------------------------------------------------ */

/*
 * Paths are sequences of size_t.  Activations keeps (offset, length)
 * bounds into a slice buffer exactly as upstream: activate() appends;
 * advance() drains sync and timer activations, then sorts and dedups
 * the bounds and compacts the slices; for_extensions() visits each
 * distinct next element of active paths extending a prefix.
 */
typedef struct __dtfl_sync_activations __dtfl_sync_activations;

typedef struct __dtfl_activations {
	size_t clean;
	__dtfl_vec bounds;	/* struct { size_t off, len; } */
	__dtfl_vec slices;	/* size_t */
	__dtfl_vec buffer;	/* size_t */
	__dtfl_sync_activations *sync;	/* shared with other threads */
	/* Timer: when has_timer, activate_after queues (deadline_ns, path). */
	bool has_timer;
	uint64_t timer_start_ns;
	__dtfl_vec timers;	/* binary min-heap of struct timer entries */
	/* Clock: real monotonic unless the simulator installs one. */
	uint64_t (*clock)(void *ctx);
	void *clock_ctx;
} __dtfl_activations;

void __dtfl_activations_init(__dtfl_activations *a, bool has_timer,
    uint64_t (*clock)(void *), void *clock_ctx);
void __dtfl_activations_free(__dtfl_activations *a);
void __dtfl_activations_activate(__dtfl_activations *a, const size_t *path,
    size_t len);
void __dtfl_activations_activate_after(__dtfl_activations *a,
    const size_t *path, size_t len, uint64_t delay_ns);
void __dtfl_activations_advance(__dtfl_activations *a);
void __dtfl_activations_for_extensions(const __dtfl_activations *a,
    const size_t *path, size_t len, void (*action)(void *ctx, size_t ext),
    void *ctx);
/*
 * empty_for: 0 if anything is active (or there is no timer), the time
 * until the next timer otherwise, UINT64_MAX if nothing is pending.
 */
uint64_t __dtfl_activations_empty_for(const __dtfl_activations *a);
/* Elapsed nanoseconds on the activation clock since the worker started. */
uint64_t __dtfl_activations_elapsed(const __dtfl_activations *a);

/*
 * Thread-safe activation queue (SyncActivations).  Reference counted,
 * shared between the worker and any number of dtfl_sync_activator
 * handles; push() may be called from any thread and unparks the worker.
 */
__dtfl_sync_activations *__dtfl_sync_activations_new(__dtfl_parker *parker);
__dtfl_sync_activations *__dtfl_sync_activations_retain(__dtfl_sync_activations *s);
void __dtfl_sync_activations_release(__dtfl_sync_activations *s);
/* Returns DTFL_ESTATE if the worker has gone away. */
int __dtfl_sync_activations_push(__dtfl_sync_activations *s,
    const size_t *path, size_t len);
/* Worker side: marks the queue closed (SyncActivationError afterwards). */
void __dtfl_sync_activations_close(__dtfl_sync_activations *s);

/* ------------------------------------------------------------------ */
/* Worker (worker.rs)                                                  */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_dataflow __dtfl_dataflow;	/* worker.rs Wrapper */

struct __dtfl_worker {
	__dtfl_allocator *allocator;
	__dtfl_activations activations;
	__dtfl_log_registry *log;
	dtfl_progress_mode progress_mode;
	size_t identifiers;		/* next channel/operator identifier */
	size_t dataflow_counter;	/* next dataflow index */
	__dtfl_vec dataflows;		/* __dtfl_dataflow *, sorted by index */
	__dtfl_vec paths;		/* channel id -> path; sparse, see worker.c */
	__dtfl_vec temp_channel_ids;	/* size_t; channels of the dataflow being built */
	__dtfl_vec active_dataflows;	/* size_t scratch */
	pthread_t owner;		/* debug owner-thread check */
	int stepping;			/* reentrancy guard (PLAN section 5, rule 6) */
};

typedef struct __dtfl_worker __dtfl_worker;

/*
 * Creates a worker over a built allocator (takes ownership).  `timer`
 * enables activate_after.  The clock (NULL = real monotonic) is used for
 * activations and log timestamps.
 */
__dtfl_worker *__dtfl_worker_new(__dtfl_allocator *a, dtfl_progress_mode mode,
    bool timer, uint64_t (*clock)(void *), void *clock_ctx);
void __dtfl_worker_free(__dtfl_worker *w);

/*
 * Worker::step_or_park.  timeout_ns: 0 never parks; UINT64_MAX may park
 * indefinitely.  Returns true if dataflows remain.
 */
bool __dtfl_worker_step_or_park(__dtfl_worker *w, uint64_t timeout_ns);

size_t __dtfl_worker_new_identifier(__dtfl_worker *w);
size_t __dtfl_worker_peek_identifier(const __dtfl_worker *w);

/* Worker::allocate / pipeline / broadcast: records channel -> path. */
void __dtfl_worker_allocate(__dtfl_worker *w, size_t identifier,
    const size_t *path, size_t len, const __dtfl_msg_class *mc,
    __dtfl_push **pushers, __dtfl_pull **puller);
void __dtfl_worker_pipeline(__dtfl_worker *w, size_t identifier,
    const size_t *path, size_t len, const __dtfl_msg_class *mc,
    __dtfl_push **pusher, __dtfl_pull **puller);
void __dtfl_worker_broadcast(__dtfl_worker *w, size_t identifier,
    const size_t *path, size_t len, const __dtfl_msg_class *mc,
    __dtfl_push **pusher, __dtfl_pull **puller);

/* The named logger, or NULL; a new reference. */
__dtfl_logger *__dtfl_worker_logger(__dtfl_worker *w, const char *name);

/*
 * Timely event logging (timely_log.c).  Each is a no-op when no
 * "timely" logger is registered.  Events are encoded as bincode
 * TimelyEvent values (PLAN D9).
 */
void __dtfl_worker_log_operates(__dtfl_worker *w, size_t id, const size_t *addr,
    size_t addr_len, const char *name);
void __dtfl_worker_log_shutdown(__dtfl_worker *w, size_t id);
void __dtfl_worker_log_schedule(__dtfl_worker *w, size_t id, bool start);
void __dtfl_worker_log_push_progress(__dtfl_worker *w, size_t op_id);
void __dtfl_worker_log_comm_channel(__dtfl_worker *w, size_t id, bool progress);
void __dtfl_worker_log_channel(__dtfl_worker *w, size_t id, const size_t *scope,
    size_t scope_len, size_t src_node, size_t src_port, size_t tgt_node,
    size_t tgt_port, const char *typ);
void __dtfl_worker_log_park(__dtfl_worker *w, bool park, uint64_t timeout_ns);
void __dtfl_worker_log_messages(__dtfl_worker *w, bool is_send, size_t channel,
    size_t source, size_t target, size_t seq, int64_t record_count);
void __dtfl_worker_log_text(__dtfl_worker *w, const char *text);

/*
 * Communication event logging (src/comm/comm_log.c).  Each is a no-op
 * when no "communication" logger is registered.  Events are encoded as
 * bincode CommunicationEvent values (PLAN D9).  The network loops log
 * through their own per-thread loggers instead (__dtfl_comm_log_*,
 * comm/init_int.h); these forms log to a worker's registry.
 */
struct __dtfl_message_header;
void __dtfl_worker_log_comm_message(__dtfl_worker *w, bool is_send,
    const struct __dtfl_message_header *h);
void __dtfl_worker_log_comm_state(__dtfl_worker *w, bool send, size_t process,
    size_t remote, bool start);
void __dtfl_worker_log_comm_setup(__dtfl_worker *w, bool sender, size_t process,
    bool has_remote, size_t remote);

/*
 * Differential event logging (src/dd/dd_log.c).  Each is a no-op when no
 * "differential/arrange" logger is registered.  Events are encoded as
 * bincode DifferentialEvent values (PLAN D9).  The trace layer emits
 * through the logger it holds (__dtfl_dd_log_*, dd/trace/trace_int.h);
 * these worker forms look the logger up per call.
 */
void __dtfl_worker_log_dd_batch(__dtfl_worker *w, size_t op, size_t length);
void __dtfl_worker_log_dd_merge(__dtfl_worker *w, size_t op, size_t scale,
    size_t length1, size_t length2, bool has_complete, size_t complete);
void __dtfl_worker_log_dd_drop(__dtfl_worker *w, size_t op, size_t length);
void __dtfl_worker_log_dd_merge_shortfall(__dtfl_worker *w, size_t op,
    size_t scale, size_t shortfall);
void __dtfl_worker_log_dd_trace_share(__dtfl_worker *w, size_t op,
    int64_t diff);
void __dtfl_worker_log_dd_batcher(__dtfl_worker *w, size_t op,
    int64_t records_diff, int64_t size_diff, int64_t capacity_diff,
    int64_t allocations_diff);

/*
 * Installs a built top-level operator as dataflow `index` (worker.rs
 * dataflow_core after func returns): logs OperatesEvent, initializes,
 * wraps, records the channels allocated since the last install.
 * `resources` (may be NULL) is released with `resources_free` when the
 * dataflow is dropped, after its operators (upstream drops resources
 * last).
 */
void __dtfl_worker_install_dataflow(__dtfl_worker *w, size_t index,
    size_t identifier, __dtfl_operate *op, void *resources,
    void (*resources_free)(void *));
size_t __dtfl_worker_allocate_dataflow_index(__dtfl_worker *w);
void __dtfl_worker_drop_dataflow(__dtfl_worker *w, size_t index);

/* Debug-build owner check (PLAN section 5, rule 1). */
#ifdef DTFL_DEBUG
#define DTFL_WORKER_CHECK(w)						\
	do {								\
		if (!pthread_equal((w)->owner, pthread_self()))		\
			__dtfl_panic("worker used from a thread that "	\
			    "does not own it");				\
	} while (0)
#else
#define DTFL_WORKER_CHECK(w) ((void)0)
#endif

/*
 * Public activators (timely.h).  An Activator holds the worker and an
 * owned copy of the operator's address; dtfl_activator_activate marks
 * that path active.  A SyncActivator holds a reference to the worker's
 * SyncActivations queue, so it may activate from another thread.
 */
dtfl_activator *__dtfl_activator_new(__dtfl_worker *w, const size_t *path,
    size_t len);
dtfl_sync_activator *__dtfl_sync_activator_new(__dtfl_worker *w,
    const size_t *path, size_t len);

#endif /* DTFL_WORKER_INT_H */
