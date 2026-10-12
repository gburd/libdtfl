/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/timely_ops.h: the timely operator library.
 *
 * Every operator of timely 0.31 (core, vec and the ones removed upstream
 * after the pin, PLAN section 2) over record streams.  Closures follow
 * PLAN D5: a function pointer and a context; contexts passed with a
 * ctx_free are owned by the operator and released when it is dropped.
 *
 * Record callbacks receive the record bytes and length.  Callbacks that
 * produce records write them through a dtfl_emit handle.
 */
#ifndef DTFL_TIMELY_OPS_H
#define DTFL_TIMELY_OPS_H

#include "dtfl/timely.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Closure context with an optional destructor (PLAN D5). */
typedef struct dtfl_ctx {
	void *ptr;
	void (*free)(void *ptr);
} dtfl_ctx;

#define DTFL_CTX(p, f) ((dtfl_ctx){ (p), (f) })
#define DTFL_NOCTX ((dtfl_ctx){ NULL, NULL })

/* A sink for records produced by a callback. */
typedef struct dtfl_emit dtfl_emit;
DTFL_API void dtfl_emit_record(dtfl_emit *e, const void *rec, size_t len);
/* Reserves a record of len bytes and returns where to write it. */
DTFL_API void *dtfl_emit_reserve(dtfl_emit *e, size_t len);

/* ------------------------------------------------------------------ */
/* Inputs (core/input.rs, core/unordered_input.rs, to_stream.rs)       */
/* ------------------------------------------------------------------ */

/*
 * input_from / new_input: an input operator in `scope` fed through the
 * handle.  The handle is created once and may feed several dataflows
 * (Handle::register).  The scope's class must be totally ordered.
 */
DTFL_API dtfl_input *dtfl_input_new(const dtfl_type *type);
DTFL_API dtfl_stream *dtfl_input_to_stream(dtfl_input *in, dtfl_scope *scope);
DTFL_API void dtfl_input_send(dtfl_input *in, const void *rec, size_t len);
DTFL_API void dtfl_input_flush(dtfl_input *in);
/* Advances to t >= time(); panics otherwise. */
DTFL_API void dtfl_input_advance_to(dtfl_input *in, const uint64_t *t);
DTFL_API const uint64_t *dtfl_input_time(const dtfl_input *in);
/* Closes the input and frees the handle (upstream Drop). */
DTFL_API void dtfl_input_close(dtfl_input *in);

/*
 * new_unordered_input: an input whose data is sent under capabilities
 * the caller manages.  *cap receives the initial capability (activates
 * the operator when downgraded or dropped).
 */
typedef struct dtfl_unordered_input dtfl_unordered_input;
DTFL_API dtfl_stream *dtfl_unordered_input_new(dtfl_scope *scope,
    const dtfl_type *type, dtfl_unordered_input **handle, dtfl_cap **cap);
DTFL_API void dtfl_unordered_input_send(dtfl_unordered_input *h,
    const dtfl_cap *cap, const void *rec, size_t len);
DTFL_API void dtfl_unordered_input_flush(dtfl_unordered_input *h);
DTFL_API void dtfl_unordered_input_free(dtfl_unordered_input *h);

/*
 * to_stream: replays the records produced by `next` (return false at
 * end) at the minimum time, a bounded chunk per activation.
 */
typedef bool (*dtfl_iter_fn)(void *ctx, dtfl_emit *out);
DTFL_API dtfl_stream *dtfl_to_stream(dtfl_scope *scope, const dtfl_type *type,
    dtfl_iter_fn next, dtfl_ctx ctx);

/* ------------------------------------------------------------------ */
/* Probes (core/probe.rs)                                              */
/* ------------------------------------------------------------------ */

DTFL_API dtfl_probe *dtfl_probe_new(void);
DTFL_API void dtfl_probe_free(dtfl_probe *p);
/* probe_with: attaches the stream's frontier to the probe; returns it. */
DTFL_API dtfl_stream *dtfl_stream_probe_with(dtfl_stream *st, dtfl_probe *p);
DTFL_API bool dtfl_probe_less_than(const dtfl_probe *p, const uint64_t *t);
DTFL_API bool dtfl_probe_less_equal(const dtfl_probe *p, const uint64_t *t);
/* True when the frontier is empty (all inputs closed and drained). */
DTFL_API bool dtfl_probe_done(const dtfl_probe *p);
DTFL_API const uint64_t *dtfl_probe_frontier(const dtfl_probe *p, size_t *n);

/* ------------------------------------------------------------------ */
/* Record operators (core and vec)                                     */
/* ------------------------------------------------------------------ */

typedef void (*dtfl_map_fn)(void *ctx, const uint64_t *time, const void *rec,
    size_t len, dtfl_emit *out);
typedef bool (*dtfl_pred_fn)(void *ctx, const void *rec, size_t len);
typedef void (*dtfl_inspect_fn)(void *ctx, const uint64_t *time,
    const void *rec, size_t len);

/* map / flat_map: `fn` emits zero or more records of `out_type` per input. */
DTFL_API dtfl_stream *dtfl_stream_flat_map(dtfl_stream *st,
    const dtfl_type *out_type, dtfl_map_fn fn, dtfl_ctx ctx);
DTFL_API dtfl_stream *dtfl_stream_filter(dtfl_stream *st, dtfl_pred_fn fn,
    dtfl_ctx ctx);
DTFL_API dtfl_stream *dtfl_stream_inspect(dtfl_stream *st, dtfl_inspect_fn fn,
    dtfl_ctx ctx);
/*
 * inspect_batch: called once per message with its time and records.
 * inspect_core: also called with the new frontier when it changes
 * (msg == NULL, frontier/nfrontier set).
 */
typedef void (*dtfl_inspect_batch_fn)(void *ctx, const uint64_t *time,
    const dtfl_message *msg);
DTFL_API dtfl_stream *dtfl_stream_inspect_batch(dtfl_stream *st,
    dtfl_inspect_batch_fn fn, dtfl_ctx ctx);
typedef void (*dtfl_inspect_core_fn)(void *ctx, const dtfl_message *msg,
    const uint64_t *frontier, size_t nfrontier);
DTFL_API dtfl_stream *dtfl_stream_inspect_core(dtfl_stream *st,
    dtfl_inspect_core_fn fn, dtfl_ctx ctx);
/* exchange: route each record by route(rec) (NULL: the type's hash). */
DTFL_API dtfl_stream *dtfl_stream_exchange(dtfl_stream *st,
    uint64_t (*route)(const void *rec, size_t len, void *ctx), void *ctx);
DTFL_API dtfl_stream *dtfl_stream_broadcast(dtfl_stream *st);
DTFL_API dtfl_stream *dtfl_stream_concat(dtfl_stream *a, dtfl_stream *b);
DTFL_API dtfl_stream *dtfl_stream_concatenate(dtfl_scope *scope,
    dtfl_stream *const *streams, size_t n);
/*
 * partition: route(rec) returns a part index < parts; outs[0..parts)
 * receive the streams.
 */
DTFL_API void dtfl_stream_partition(dtfl_stream *st, size_t parts,
    size_t (*route)(void *ctx, const void *rec, size_t len), dtfl_ctx ctx,
    dtfl_stream **outs);
/* ok_err: pred true sends the record to *ok, false to *err. */
DTFL_API void dtfl_stream_ok_err(dtfl_stream *st, dtfl_pred_fn pred,
    dtfl_ctx ctx, dtfl_stream **ok, dtfl_stream **err);
/* branch: as ok_err, with the time available. */
typedef bool (*dtfl_tpred_fn)(void *ctx, const uint64_t *time,
    const void *rec, size_t len);
DTFL_API void dtfl_stream_branch(dtfl_stream *st, dtfl_tpred_fn pred,
    dtfl_ctx ctx, dtfl_stream **yes, dtfl_stream **no);
/* branch_when: whole messages by time predicate. */
DTFL_API void dtfl_stream_branch_when(dtfl_stream *st,
    bool (*pred)(void *ctx, const uint64_t *time), dtfl_ctx ctx,
    dtfl_stream **yes, dtfl_stream **no);

/*
 * delay / delay_batch / delay_total: each record (or message) is
 * re-timed to fn(time) >= time, and released when the input frontier
 * passes the new time.
 */
typedef void (*dtfl_delay_fn)(void *ctx, const void *rec, size_t len,
    const uint64_t *time, uint64_t *new_time);
DTFL_API dtfl_stream *dtfl_stream_delay(dtfl_stream *st, dtfl_delay_fn fn,
    dtfl_ctx ctx);
DTFL_API dtfl_stream *dtfl_stream_delay_total(dtfl_stream *st, dtfl_delay_fn fn,
    dtfl_ctx ctx);
typedef void (*dtfl_delay_batch_fn)(void *ctx, const uint64_t *time,
    uint64_t *new_time);
DTFL_API dtfl_stream *dtfl_stream_delay_batch(dtfl_stream *st,
    dtfl_delay_batch_fn fn, dtfl_ctx ctx);

/*
 * accumulate / count: per time, folds all records into a value and emits
 * it when the frontier passes the time.  count emits a u64 record.
 */
typedef void (*dtfl_fold_fn)(void *ctx, void *acc, const dtfl_message *msg);
DTFL_API dtfl_stream *dtfl_stream_accumulate(dtfl_stream *st,
    const dtfl_type *acc_type, const void *init, size_t init_len,
    dtfl_fold_fn fold, dtfl_ctx ctx);
DTFL_API dtfl_stream *dtfl_stream_count(dtfl_stream *st);

/*
 * aggregate: per (time, key), folds values, then emits emit(key, agg).
 * The record's key is the first field of a dtfl_type_pair record.
 */
typedef void (*dtfl_agg_fold_fn)(void *ctx, const void *key, size_t klen,
    const void *val, size_t vlen, void *agg);
typedef void (*dtfl_agg_emit_fn)(void *ctx, const void *key, size_t klen,
    const void *agg, dtfl_emit *out);
DTFL_API dtfl_stream *dtfl_stream_aggregate(dtfl_stream *st,
    size_t agg_size, dtfl_agg_fold_fn fold, dtfl_agg_emit_fn emit,
    const dtfl_type *out_type, dtfl_ctx ctx);

/*
 * state_machine: per key (first field of a pair record), keeps state of
 * state_size bytes; step(key, val, state, out) returns true to remove
 * the key's state.
 */
typedef bool (*dtfl_state_fn)(void *ctx, const void *key, size_t klen,
    const void *val, size_t vlen, void *state, dtfl_emit *out);
DTFL_API dtfl_stream *dtfl_stream_state_machine(dtfl_stream *st,
    size_t state_size, dtfl_state_fn step, const dtfl_type *out_type,
    dtfl_ctx ctx);

/* reclock: releases `data` records at the times of `clock` messages. */
DTFL_API dtfl_stream *dtfl_stream_reclock(dtfl_stream *data,
    dtfl_stream *clock);

/*
 * Result helpers on streams of Result records: a tag byte (0 Ok, 1 Err)
 * then the value.  ok / err keep one arm and emit its unwrapped value
 * (ok_type / err_type).  map_ok / map_err transform one arm's value in
 * place and pass the other arm through, so the output is still a Result
 * record of the same stream type.  and_then maps an Ok value to a new
 * Result.  unwrap_or_else emits the Ok value unwrapped, or the fallback
 * the callback computes from the Err value (out_type).
 *
 * A value transform writes the replacement value into *out (room for
 * cap bytes; the library reserves enough) and returns its length.
 */
typedef size_t (*dtfl_val_fn)(void *ctx, const void *val, size_t len,
    void *out, size_t cap);
DTFL_API dtfl_stream *dtfl_stream_ok(dtfl_stream *st, const dtfl_type *ok_type);
DTFL_API dtfl_stream *dtfl_stream_err(dtfl_stream *st, const dtfl_type *err_type);
/*
 * map_ok / map_err: fn maps one arm's value to the replacement value;
 * the other arm passes through unchanged.  The output stream keeps the
 * input's record type (tag + value).
 */
DTFL_API dtfl_stream *dtfl_stream_map_ok(dtfl_stream *st, dtfl_val_fn fn,
    dtfl_ctx ctx);
DTFL_API dtfl_stream *dtfl_stream_map_err(dtfl_stream *st, dtfl_val_fn fn,
    dtfl_ctx ctx);
/*
 * and_then: fn maps an Ok value to a new Result.  It writes the output
 * value into *out and returns its length, setting *is_ok to the output
 * tag.  Err input records pass through unchanged.
 */
typedef size_t (*dtfl_and_then_fn)(void *ctx, const void *val, size_t len,
    void *out, size_t cap, bool *is_ok);
DTFL_API dtfl_stream *dtfl_stream_and_then(dtfl_stream *st,
    dtfl_and_then_fn fn, dtfl_ctx ctx);
/*
 * unwrap_or_else: Ok values pass unwrapped; fn turns an Err value into
 * a value of out_type.  The output stream carries out_type.
 */
DTFL_API dtfl_stream *dtfl_stream_unwrap_or_else(dtfl_stream *st,
    const dtfl_type *out_type, dtfl_val_fn fn, dtfl_ctx ctx);

/*
 * iterator_source (flow_controlled): emits records from `next` with
 * times chosen by the iterator, holding back while probe is behind
 * (input_from's flow-controlled variant).
 */
typedef bool (*dtfl_timed_iter_fn)(void *ctx, uint64_t *time, dtfl_emit *out);
DTFL_API dtfl_stream *dtfl_iterator_source(dtfl_scope *scope,
    const dtfl_type *type, dtfl_timed_iter_fn next, dtfl_probe *probe,
    dtfl_ctx ctx);

/* ------------------------------------------------------------------ */
/* Scopes and loops (enterleave.rs, feedback.rs)                       */
/* ------------------------------------------------------------------ */

/* enter: a stream of the parent scope as a stream of `child`. */
DTFL_API dtfl_stream *dtfl_stream_enter(dtfl_stream *st, dtfl_scope *child);
/* leave: a stream of a child scope as a stream of its parent. */
DTFL_API dtfl_stream *dtfl_stream_leave(dtfl_stream *st);

/*
 * feedback / loop_variable: a stream whose contents arrive from a later
 * connect_loop, with each time advanced by `summary`.
 */
typedef struct dtfl_feedback dtfl_feedback;
DTFL_API dtfl_stream *dtfl_feedback_new(dtfl_scope *scope, const dtfl_type *type,
    const uint64_t *summary, dtfl_feedback **handle);
DTFL_API void dtfl_feedback_connect(dtfl_feedback *handle, dtfl_stream *st);

/* ------------------------------------------------------------------ */
/* Capture and replay (core/capture)                                   */
/* ------------------------------------------------------------------ */

/*
 * Events: progress changes or messages.  An event pusher receives them;
 * capture_into feeds one from a stream.
 */
typedef struct dtfl_event_pusher {
	void (*progress)(void *ctx, const uint64_t *times, const int64_t *diffs,
	    size_t n);
	void (*messages)(void *ctx, const uint64_t *time, const dtfl_message *msg);
	void *ctx;
} dtfl_event_pusher;

DTFL_API void dtfl_stream_capture_into(dtfl_stream *st, dtfl_event_pusher p,
    dtfl_ctx ctx);

/*
 * Event links (EventLink): an in-memory event log.  The sync link may
 * be pushed on one thread and replayed on another.
 */
typedef struct dtfl_event_link dtfl_event_link;
DTFL_API dtfl_event_link *dtfl_event_link_new(const dtfl_time_class *tc,
    const dtfl_type *type);
DTFL_API dtfl_event_link *dtfl_event_link_new_sync(const dtfl_time_class *tc,
    const dtfl_type *type);
DTFL_API void dtfl_event_link_free(dtfl_event_link *l);
DTFL_API dtfl_event_pusher dtfl_event_link_pusher(dtfl_event_link *l);
DTFL_API dtfl_stream *dtfl_event_link_replay(dtfl_event_link *l,
    dtfl_scope *scope);

/*
 * Binary event streams (EventWriter, EventReader): u64 LE length then the
 * bincode-1 event (PLAN D9).  The writer writes to `fd`; replay reads
 * from `fd`, which may be non-blocking.
 */
typedef struct dtfl_event_writer dtfl_event_writer;
DTFL_API dtfl_event_writer *dtfl_event_writer_new(int fd,
    const dtfl_time_class *tc, const dtfl_type *type);
DTFL_API void dtfl_event_writer_free(dtfl_event_writer *w);
DTFL_API dtfl_event_pusher dtfl_event_writer_pusher(dtfl_event_writer *w);
DTFL_API dtfl_stream *dtfl_event_reader_replay(int const *fds, size_t nfds,
    dtfl_scope *scope, const dtfl_type *type);

/*
 * extract: collects captured (time, record) pairs after the computation
 * has finished, sorted by time then record.  The callback is invoked
 * once per pair.
 */
DTFL_API void dtfl_event_link_extract(dtfl_event_link *l,
    void (*each)(void *ctx, const uint64_t *time, const void *rec, size_t len),
    void *ctx);

/* ------------------------------------------------------------------ */
/* Synchronization (barrier.rs, sequence.rs)                           */
/* ------------------------------------------------------------------ */

typedef struct dtfl_barrier dtfl_barrier;
/* A barrier across all workers; must be created in the same order. */
DTFL_API dtfl_barrier *dtfl_barrier_new(dtfl_worker *w);
DTFL_API void dtfl_barrier_wait(dtfl_barrier *b);
DTFL_API void dtfl_barrier_free(dtfl_barrier *b);

/*
 * Sequencer: a totally ordered log of records proposed by any worker,
 * delivered to every worker in the same order.
 */
typedef struct dtfl_sequencer dtfl_sequencer;
DTFL_API dtfl_sequencer *dtfl_sequencer_new(dtfl_worker *w, const dtfl_type *type);
DTFL_API void dtfl_sequencer_push(dtfl_sequencer *s, const void *rec, size_t len);
/* Next record in the agreed order, or NULL; *len receives its length. */
DTFL_API const void *dtfl_sequencer_next(dtfl_sequencer *s, size_t *len);
DTFL_API void dtfl_sequencer_free(dtfl_sequencer *s);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_TIMELY_OPS_H */
