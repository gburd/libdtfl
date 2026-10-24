/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/log.h: the structured event-logging subscriber API.
 *
 * This is the machine-readable timely and differential event logging,
 * distinct from the human-readable diagnostic sink dtfl_set_log_hook()
 * (dtfl/base.h).  A worker emits structured events on named streams:
 *   "timely"               the dataflow structure and scheduling
 *   "differential/arrange" arrangement batch, merge and trace events
 *   "communication"        message send/receive and thread state
 * These are the streams the diagnostics tools (dtfl-diag graph and
 * profile) consume to reconstruct the operator graph and timing
 * profile.  On the wire each stream is the bincode-1 fixint
 * little-endian encoding of a Rust 0.31 event enum (PLAN D9); a
 * subscriber registered here receives the events already decoded into
 * a dtfl_log_batch, so a C caller never touches the wire bytes.
 *
 * A subscriber is a function called on the worker thread with a batch
 * of events as they are flushed (once per worker step, and a final
 * empty flush when the worker shuts down).  The batch and every pointer
 * it hands out are valid only for the duration of the call.
 *
 * Register before workers start with dtfl_config_log_subscribe(), so
 * every worker installs the subscriber as it comes up, or on a single
 * worker you own with dtfl_worker_log_subscribe().  With more than one
 * worker the same ctx is shared across worker threads, exactly as the
 * ctx of dtfl_execute() is; a subscriber that writes shared state must
 * synchronize it itself.
 *
 * The "communication" stream comes from a cluster's network send and
 * receive threads (as upstream's Hooks.log_fn), so it is delivered only
 * to a dtfl_config_log_subscribe() subscription, is called on those
 * threads concurrently, and reports the process index as the worker.
 *
 * The manual page is dtfl_worker_log_subscribe(3).
 */
#ifndef DTFL_LOG_H
#define DTFL_LOG_H

#include "dtfl/base.h"
#include "dtfl/config.h"
#include "dtfl/timely.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The kind of a logged event.  The values are the union of the Rust
 * TimelyEvent, DifferentialEvent and CommunicationEvent variants at the
 * pin.  A subscriber reads an event's fields with the accessors below
 * that match its kind; reading a field that does not belong to the kind
 * returns a zero value.
 */
typedef enum dtfl_log_event_kind {
	/* timely (timely/src/logging.rs TimelyEvent) */
	DTFL_LOG_OPERATES = 0,	/* operator creation: id, addr, name */
	DTFL_LOG_CHANNELS = 1,	/* channel creation: id, scope, src, tgt, typ */
	DTFL_LOG_PUSH_PROGRESS = 2,	/* op_id */
	DTFL_LOG_MESSAGES = 3,	/* is_send, channel, source, target, seq, count */
	DTFL_LOG_SCHEDULE = 4,	/* id, start */
	DTFL_LOG_SHUTDOWN = 5,	/* id */
	DTFL_LOG_COMM_CHANNELS = 6,	/* id, progress */
	DTFL_LOG_PARK = 7,	/* park, (timeout) */
	DTFL_LOG_TEXT = 8,	/* text */

	/* differential (differential-dataflow/src/logging.rs) */
	DTFL_LOG_DD_BATCH = 100,	/* operator, length */
	DTFL_LOG_DD_MERGE = 101,	/* operator, scale, length1, length2, complete */
	DTFL_LOG_DD_DROP = 102,	/* operator, length */
	DTFL_LOG_DD_MERGE_SHORTFALL = 103,	/* operator, scale, shortfall */
	DTFL_LOG_DD_TRACE_SHARE = 104,	/* operator, diff */
	DTFL_LOG_DD_BATCHER = 105,	/* operator, four isize deltas */

	/* communication (communication/src/logging.rs) */
	DTFL_LOG_COMM_MESSAGE = 200,	/* is_send, header fields */
	DTFL_LOG_COMM_STATE = 201,	/* send, process, remote, start */
	DTFL_LOG_COMM_SETUP = 202,	/* sender, process, (remote) */

	DTFL_LOG_UNKNOWN = -1	/* a record this version does not decode */
} dtfl_log_event_kind;

/*
 * A batch of decoded events as handed to a subscriber.  Opaque: use the
 * accessors.  An event index i runs over 0 .. dtfl_log_batch_len().
 */
typedef struct dtfl_log_batch dtfl_log_batch;

/*
 * Called on the worker thread with a batch of events.  A batch with
 * length 0 is a flush notification; its time is the current log time.
 * batch is valid only for the duration of the call, as is every pointer
 * returned by an accessor on it.
 */
typedef void (*dtfl_log_subscriber_fn)(const dtfl_log_batch *batch, void *ctx);

/*
 * Registers a subscriber for a named stream on a worker you own (one
 * built with dtfl_worker_new_standalone, or the worker inside a build
 * callback).  stream is "timely", "differential/arrange" or
 * "communication".  Replaces any subscriber already on that stream.
 * ctx_free (may be NULL) releases ctx when the subscriber is removed or
 * the worker is dropped.  Returns DTFL_OK, or DTFL_ESTATE if the worker
 * has no log registry.
 */
DTFL_API int dtfl_worker_log_subscribe(dtfl_worker *w, const char *stream,
    dtfl_log_subscriber_fn fn, void *ctx, void (*ctx_free)(void *));

/*
 * Records a subscriber on a configuration, to be installed on every
 * worker as it starts (dtfl_execute, dtfl_execute_directly, the
 * simulation runtime).  ctx is shared by all workers; ctx_free (may be
 * NULL) is called once, when the configuration is freed.  Returns
 * DTFL_OK or DTFL_EINVAL.  More than one stream may be subscribed; a
 * second subscribe of the same stream replaces the first.
 */
DTFL_API int dtfl_config_log_subscribe(dtfl_config *cfg, const char *stream,
    dtfl_log_subscriber_fn fn, void *ctx, void (*ctx_free)(void *));

/* ------------------------------------------------------------------ */
/* Batch and event accessors                                           */
/* ------------------------------------------------------------------ */

/* Number of events in the batch (0 for a flush notification). */
DTFL_API size_t dtfl_log_batch_len(const dtfl_log_batch *b);
/* The batch's capability time, in nanoseconds since the worker started. */
DTFL_API uint64_t dtfl_log_batch_time(const dtfl_log_batch *b);
/* The worker index the batch came from. */
DTFL_API size_t dtfl_log_batch_worker(const dtfl_log_batch *b);
/* The stream name ("timely", ...); valid for the call. */
DTFL_API const char *dtfl_log_batch_stream(const dtfl_log_batch *b);

/* Timestamp of event i, in nanoseconds since the worker started. */
DTFL_API uint64_t dtfl_log_event_time(const dtfl_log_batch *b, size_t i);
/* Kind of event i. */
DTFL_API dtfl_log_event_kind dtfl_log_event_kind_at(const dtfl_log_batch *b,
    size_t i);

/* Operates: operator id. */
DTFL_API size_t dtfl_log_operates_id(const dtfl_log_batch *b, size_t i);
/*
 * Operates: the operator's address (nested scope path from the root).
 * Stores the length in *n and returns a pointer to *n size_t words.
 */
DTFL_API const size_t *dtfl_log_operates_addr(const dtfl_log_batch *b,
    size_t i, size_t *n);
/* Operates: the operator's name (NUL-terminated). */
DTFL_API const char *dtfl_log_operates_name(const dtfl_log_batch *b, size_t i);

/* Channels: channel id. */
DTFL_API size_t dtfl_log_channels_id(const dtfl_log_batch *b, size_t i);
/* Channels: the scope address containing the channel; length in *n. */
DTFL_API const size_t *dtfl_log_channels_scope(const dtfl_log_batch *b,
    size_t i, size_t *n);
/* Channels: source (operator index, output port). */
DTFL_API void dtfl_log_channels_source(const dtfl_log_batch *b, size_t i,
    size_t *node, size_t *port);
/* Channels: target (operator index, input port). */
DTFL_API void dtfl_log_channels_target(const dtfl_log_batch *b, size_t i,
    size_t *node, size_t *port);
/* Channels: the data type on the channel, as a string. */
DTFL_API const char *dtfl_log_channels_typ(const dtfl_log_batch *b, size_t i);

/* Schedule: the operator id and whether it is a start (vs a stop). */
DTFL_API size_t dtfl_log_schedule_id(const dtfl_log_batch *b, size_t i);
DTFL_API bool dtfl_log_schedule_start(const dtfl_log_batch *b, size_t i);

/* Shutdown / PushProgress: the operator id. */
DTFL_API size_t dtfl_log_shutdown_id(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_push_progress_op_id(const dtfl_log_batch *b, size_t i);

/* Messages (timely): the fields of a logical message send/receive. */
DTFL_API bool dtfl_log_messages_is_send(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_messages_channel(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_messages_source(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_messages_target(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_messages_seq_no(const dtfl_log_batch *b, size_t i);
DTFL_API int64_t dtfl_log_messages_record_count(const dtfl_log_batch *b,
    size_t i);

/* CommChannels: the channel id and whether it carries progress (vs data). */
DTFL_API size_t dtfl_log_comm_channels_id(const dtfl_log_batch *b, size_t i);
DTFL_API bool dtfl_log_comm_channels_progress(const dtfl_log_batch *b, size_t i);

/* Park: whether this is a park (vs unpark), and its timeout. */
DTFL_API bool dtfl_log_park_is_park(const dtfl_log_batch *b, size_t i);
/* True if the park has a timeout; stores it (ns) in *timeout_ns. */
DTFL_API bool dtfl_log_park_timeout(const dtfl_log_batch *b, size_t i,
    uint64_t *timeout_ns);

/* Text: the message. */
DTFL_API const char *dtfl_log_text(const dtfl_log_batch *b, size_t i);

/* Differential events share an operator id. */
DTFL_API size_t dtfl_log_dd_operator(const dtfl_log_batch *b, size_t i);
/* Batch / Drop: the length (order of magnitude). */
DTFL_API size_t dtfl_log_dd_length(const dtfl_log_batch *b, size_t i);
/* Merge: scale and the two input lengths. */
DTFL_API size_t dtfl_log_dd_merge_scale(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_dd_merge_length1(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_dd_merge_length2(const dtfl_log_batch *b, size_t i);
/* Merge: true if a stop (Some), with the completed length in *complete. */
DTFL_API bool dtfl_log_dd_merge_complete(const dtfl_log_batch *b, size_t i,
    size_t *complete);
/* MergeShortfall: scale and the shortfall. */
DTFL_API size_t dtfl_log_dd_shortfall_scale(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_dd_shortfall(const dtfl_log_batch *b, size_t i);
/* TraceShare: the change in share count. */
DTFL_API int64_t dtfl_log_dd_trace_share_diff(const dtfl_log_batch *b,
    size_t i);
/* Batcher: the four deltas (records, size, capacity, allocations). */
DTFL_API void dtfl_log_dd_batcher(const dtfl_log_batch *b, size_t i,
    int64_t *records_diff, int64_t *size_diff, int64_t *capacity_diff,
    int64_t *allocations_diff);

/*
 * Communication Message: whether it is a send and the message header
 * fields (channel, source, and the receiving worker range, the payload
 * length in bytes, and the sequence number).
 */
DTFL_API bool dtfl_log_comm_message_is_send(const dtfl_log_batch *b, size_t i);
DTFL_API void dtfl_log_comm_message_header(const dtfl_log_batch *b, size_t i,
    size_t *channel, size_t *source, size_t *target_lower,
    size_t *target_upper, size_t *length, size_t *seqno);
/* Communication State: a send (vs recv) thread, the process ids, start. */
DTFL_API bool dtfl_log_comm_state_send(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_comm_state_process(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_comm_state_remote(const dtfl_log_batch *b, size_t i);
DTFL_API bool dtfl_log_comm_state_start(const dtfl_log_batch *b, size_t i);
/* Communication Setup: a send thread, the process id, and the remote. */
DTFL_API bool dtfl_log_comm_setup_sender(const dtfl_log_batch *b, size_t i);
DTFL_API size_t dtfl_log_comm_setup_process(const dtfl_log_batch *b, size_t i);
/* True if the setup names a remote; stores it in *remote. */
DTFL_API bool dtfl_log_comm_setup_remote(const dtfl_log_batch *b, size_t i,
    size_t *remote);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_LOG_H */
