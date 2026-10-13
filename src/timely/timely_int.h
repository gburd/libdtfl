/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/timely_int.h: the dataflow layer internals.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/channels/mod.rs
 *   timely/src/dataflow/channels/pushers/mod.rs
 *   timely/src/dataflow/channels/pullers/mod.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A channel carries dtfl_message values: a time of the scope's class, a
 * refcounted dtfl_batch of records (the container, PLAN D2), and the
 * from/seq fields upstream's Message<T, C> stamps.  Because records are
 * already bytes, a message serializes with a memcpy of its words and the
 * batch encoding (comm_int.h), so there is no bincode.
 *
 * Pushers and pullers wrap the comm layer's __dtfl_push / __dtfl_pull:
 * a Tee fans a message out by sharing the (refcounted) batch, a Counter
 * records record counts into the operator's produced / consumed change
 * batches, an Exchange routes each record by the pact's route function,
 * and a Progress pusher validates a capability before a session sends.
 * Everything here belongs to one worker thread (PLAN section 5).
 */
#ifndef DTFL_TIMELY_INT_H
#define DTFL_TIMELY_INT_H

#include "dtfl/timely.h"
#include "comm/comm_int.h"
#include "progress/progress_int.h"
#include "progress/operate_int.h"

typedef struct __dtfl_worker __dtfl_worker;

/* ------------------------------------------------------------------ */
/* Message (channels/mod.rs)                                           */
/* ------------------------------------------------------------------ */

/*
 * A timestamped batch travelling on a channel.  `time` is depth words of
 * the scope's class; `data` is a refcounted batch; `from` and `seq` are
 * stamped by the LogPusher (pact.c).  Owned by whoever holds it: a
 * message read through dtfl_op_input_next() belongs to the input handle
 * (handles.c), one on a channel to the channel.
 */
struct dtfl_message {
	uint64_t time[DTFL_TIME_MAX];
	__dtfl_batch *data;
	size_t from;
	size_t seq;
	unsigned depth;	/* words in time */
};

dtfl_message *__dtfl_message_new(const dtfl_time_class *tc, const uint64_t *time,
    __dtfl_batch *data);
void __dtfl_message_free(dtfl_message *m);

/*
 * The static message class for data channels.  The time depth is encoded
 * in each message, so one class serves every channel and every worker
 * passes the same pointer (the process allocator keys a channel by it).
 */
const __dtfl_msg_class *__dtfl_message_class(void);

/* ------------------------------------------------------------------ */
/* Tee (pushers/tee.rs)                                                */
/* ------------------------------------------------------------------ */

/*
 * The shared pusher list behind a stream.  A Tee pusher pushes into it;
 * TeeHelper adds pushers to it.  Reference counted: the stream holds one
 * reference and the Tee pusher another.  Clone and free go through the
 * static message class.
 */
typedef struct __dtfl_tee __dtfl_tee;

__dtfl_tee *__dtfl_tee_new(void);
__dtfl_tee *__dtfl_tee_retain(__dtfl_tee *t);
void __dtfl_tee_release(__dtfl_tee *t);
/* Adds a pusher (takes ownership); the Tee frees it when released. */
void __dtfl_tee_add_pusher(__dtfl_tee *t, __dtfl_push *p);
/* A Push into the shared list; retains `t`. */
__dtfl_push *__dtfl_tee_pusher(__dtfl_tee *t);

/* ------------------------------------------------------------------ */
/* Counters (pushers/counter.rs, pullers/counter.rs)                   */
/* ------------------------------------------------------------------ */

/*
 * A pushing Counter: updates `produced` (time -> +record_count) on each
 * data push, forwards to `inner` (takes ownership), and only forwards a
 * flush when `produced` is dirty.  `produced` belongs to the operator
 * and must outlive the pusher.
 */
__dtfl_push *__dtfl_push_counter_new(__dtfl_push *inner,
    __dtfl_change_batch *produced);
/* Repoints the produced batch after the builder relocates it. */
void __dtfl_push_counter_set_produced(__dtfl_push *p,
    __dtfl_change_batch *produced);

/*
 * A pulling Counter: wraps a puller (takes ownership) and defers each
 * message's consumed update (time -> +record_count) until the message is
 * released, as upstream's ConsumedGuard records on drop.
 */
typedef struct __dtfl_pull_counter __dtfl_pull_counter;

__dtfl_pull_counter *__dtfl_pull_counter_new(__dtfl_pull *inner,
    __dtfl_change_batch *consumed);
/* Repoints the consumed batch after the builder relocates it. */
void __dtfl_pull_counter_set_consumed(__dtfl_pull_counter *c,
    __dtfl_change_batch *consumed);
void __dtfl_pull_counter_free(__dtfl_pull_counter *c);
/*
 * Pulls the next message, recording the previous message's consumption
 * first (the guard runs when the next message is pulled or the handle is
 * released).  Returns a borrow owned by the counter, or NULL.
 */
dtfl_message *__dtfl_pull_counter_next(__dtfl_pull_counter *c);
/* Records the held message's consumption and drops it (end of logic). */
void __dtfl_pull_counter_finish(__dtfl_pull_counter *c);

/* ------------------------------------------------------------------ */
/* Exchange (pushers/exchange.rs)                                      */
/* ------------------------------------------------------------------ */

/*
 * An Exchange pusher: routes each record of a message by `route` (its
 * hash) modulo the number of pushers, buffering per peer and flushing a
 * buffer when the time changes or on a final flush.  Takes ownership of
 * the `n` pushers array (allocated with __dtfl_alloc) and every pusher.
 * `tc` and `type` are retained.  The record type supplies the default
 * hash when `route` is NULL.
 */
__dtfl_push *__dtfl_push_exchange_new(const dtfl_time_class *tc,
    const dtfl_type *type, __dtfl_push **pushers, size_t n,
    uint64_t (*route)(const void *, size_t, void *), void *ctx);

/* ------------------------------------------------------------------ */
/* Progress output pusher (pushers/progress.rs)                        */
/* ------------------------------------------------------------------ */

/*
 * The output chain: a Progress pusher over a push Counter over a Tee.
 * __dtfl_output bundles the Progress validation state (the internal
 * capability batch and the output port) with the counter+tee pusher.
 * dtfl_session (handles.c) opens a buffer at a capability's time and
 * flushes batches through this chain.
 */
struct dtfl_output {
	__dtfl_push *pushee;		/* Counter over Tee; owned */
	__dtfl_change_batch *internal;	/* capability batch, not owned */
	size_t port;
	const dtfl_time_class *tc;	/* not retained */
	const dtfl_type *type;		/* record type, not retained */
};

/* True if `cap` is valid for this output (progress.rs valid). */
bool __dtfl_output_valid(const dtfl_output *o, const dtfl_cap *cap);
/* Ships `data` (consumed) at `time` through the chain, if non-empty. */
void __dtfl_output_give(dtfl_output *o, const uint64_t *time, __dtfl_batch *data);
/* Flushes the chain (ProgressSession drop). */
void __dtfl_output_done(dtfl_output *o);

/*
 * Reserves `len` bytes for the next record of an open session and
 * returns where to write them (session_give's uninitialized form).  The
 * record is committed when the pointer is written; the returned pointer
 * stays valid until the next give/reserve or the session closes.
 */
void *__dtfl_session_reserve(dtfl_session *s, size_t len);

/*
 * Mark this session's records as owning pointers (the batch-pointer
 * streams): every buffer the session flushes runs `drop` on each record
 * when it frees and `retain` on each record of a copy, so a batch sent
 * downstream owns a reference whether or not a consumer reads it.
 */
void __dtfl_session_set_batch_ptr_hooks(dtfl_session *s,
    void (*drop)(void *rec), void (*retain)(void *rec));

/* ------------------------------------------------------------------ */
/* Pact (channels/pact.rs)                                             */
/* ------------------------------------------------------------------ */

/*
 * Pipeline / ExchangeCore connect: allocates the channel for `type`
 * records with times of `tc`, and returns the pusher (into which the
 * source stream pushes) and the pull counter the operator reads.  The
 * identifier and path identify the channel to the worker.  The message
 * class is owned by the returned pusher chain and freed with it.
 */
void __dtfl_pact_connect(dtfl_scope *s, const dtfl_pact *pact, size_t identifier,
    const size_t *path, size_t path_len, const dtfl_time_class *tc,
    const dtfl_type *type, __dtfl_push **pusher, __dtfl_pull **puller);

/* ------------------------------------------------------------------ */
/* Stream (stream.rs)                                                  */
/* ------------------------------------------------------------------ */

/*
 * A stream handle: the source location of its producing operator, the
 * Tee the producer pushes into, the record type, and the containing
 * scope.  Streams are owned by the scope (allocated in a pool) and need
 * not be freed by the user (PLAN D6, see timely.h).
 */
struct dtfl_stream {
	__dtfl_location source;
	__dtfl_tee *ports;	/* retained */
	dtfl_scope *scope;
	const dtfl_type *type;	/* retained */
};

dtfl_stream *__dtfl_stream_new(dtfl_scope *s, __dtfl_location source,
    __dtfl_tee *ports, const dtfl_type *type);
void __dtfl_stream_free(dtfl_stream *st);
/* Stream::connect_to: records the edge and adds `pusher` to the Tee. */
void __dtfl_stream_connect_to(dtfl_stream *st, __dtfl_location target,
    __dtfl_push *pusher, size_t identifier);

/*
 * The scope pools the streams it creates so a build callback need not
 * free them.  __dtfl_scope_track_stream registers one for release when
 * the dataflow is dropped.  Defined in builder.c (the pool lives with
 * the operator resources).
 */

/* ------------------------------------------------------------------ */
/* Capability (capability.rs)                                          */
/* ------------------------------------------------------------------ */

/*
 * A capability at `time` for output `port`.  It holds a +1 at `time` in
 * the operator's internal change batch (progress_int.h) for its whole
 * life; clone adds another, drop removes it, downgrade moves it.  The
 * batch belongs to the operator and must outlive every capability.
 */
struct dtfl_cap {
	const dtfl_time_class *tc;	/* not retained */
	uint64_t time[DTFL_TIME_MAX];
	__dtfl_change_batch *internal;	/* not owned */
	size_t port;
};

dtfl_cap *__dtfl_cap_new(const dtfl_time_class *tc, const uint64_t *time,
    __dtfl_change_batch *internal, size_t port);

/* ------------------------------------------------------------------ */
/* Raw operator (builder_raw.rs)                                       */
/* ------------------------------------------------------------------ */

/*
 * The raw logic a built operator runs on each schedule.  It is handed
 * the operator's shared progress and returns true to report the operator
 * incomplete (build_reschedule).  The rc builder wraps the user logic in
 * one of these to drain frontiers and publish progress (builder.c).
 */
typedef bool (*__dtfl_raw_logic_fn)(void *ctx, __dtfl_shared_progress *sp);

/*
 * builder_raw OperatorCore as an Operate.  Takes ownership of `summary`
 * (one PortConnectivity per input, over `outputs` ports), the shared
 * progress, and the logic context (freed with `ctx_free`).  `name` and
 * `path` are copied.  initialize activates the operator once, reserves
 * `peers` capabilities at the minimum time on every output, and returns
 * the summary and shared progress.
 */
__dtfl_operate *__dtfl_op_core_new(__dtfl_worker *w, const char *name,
    const size_t *path, size_t path_len, const dtfl_time_class *tc, size_t peers,
    size_t inputs, size_t outputs, const __dtfl_frontier_interest *notify,
    __dtfl_connectivity *summary, __dtfl_raw_logic_fn logic, void *ctx,
    void (*ctx_free)(void *));

/* ------------------------------------------------------------------ */
/* Operator handle (handles.rs)                                        */
/* ------------------------------------------------------------------ */

/*
 * The handle passed to user logic, valid only during the call.  It
 * exposes the inputs (pull counters and per-input summaries), the
 * outputs (progress pushers), the per-input frontiers, and the initial
 * capabilities on the first call.  Everything it points to is owned by
 * the operator core (builder.c) and lives as long as the operator.
 */
struct dtfl_op {
	dtfl_scope *scope;
	size_t inputs, outputs;
	const dtfl_time_class *tc;
	__dtfl_pull_counter **inputs_pull;	/* per input */
	__dtfl_connectivity *summaries;		/* .ports[input]: summaries to each output */
	__dtfl_mutable_antichain *frontier;	/* per input */
	__dtfl_change_batch **internals;	/* per output: capability batch */
	dtfl_output *outputs_arr;		/* per output */
	dtfl_cap **initial_caps;		/* per output; taken on first call */
	uint64_t cur_time[DTFL_TIME_MAX];	/* time of the current input message */
	size_t cur_input;			/* input the current message came from */
	bool have_cur;
	bool first_call;
	__dtfl_worker *worker;
	size_t *address;
	size_t address_len;
};

#endif /* DTFL_TIMELY_INT_H */
