/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/log_int.h: the logging registry (timely_logging).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   logging/src/lib.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A registry maps names ("timely", "differential/arrange", ...) to
 * loggers.  A logger buffers (elapsed, event) pairs in a batch and hands
 * full batches to its action; a NULL batch means "flush" and carries
 * only the current time, as upstream's `None` container.  An event is an
 * opaque byte string (the stream's own encoding, e.g. the bincode of a
 * TimelyEvent), so one logger type covers every event type upstream is
 * generic over.
 *
 * Loggers and the registry belong to one worker thread.  Times are
 * nanoseconds since the registry's start instant (upstream Duration
 * since Instant), from the worker's clock so the simulator can
 * virtualize them (PLAN D8).
 */
#ifndef DTFL_LOG_INT_H
#define DTFL_LOG_INT_H

#include "base/base_int.h"

typedef struct __dtfl_logger __dtfl_logger;
typedef struct __dtfl_log_registry __dtfl_log_registry;

/*
 * One logged event as handed to an action: its timestamp and bytes.
 */
typedef struct __dtfl_log_event {
	uint64_t time_ns;
	const unsigned char *data;
	size_t len;
} __dtfl_log_event;

/*
 * Action invoked with a batch of `n` events.  `events == NULL` (n == 0)
 * is a flush notification at time `now_ns`.  The batch memory is valid
 * only during the call.
 */
typedef void (*__dtfl_log_action)(void *ctx, uint64_t now_ns,
    const __dtfl_log_event *events, size_t n);

/* Clock used for log timestamps; returns nanoseconds. */
typedef uint64_t (*__dtfl_clock_fn)(void *ctx);

__dtfl_log_registry *__dtfl_log_registry_new(__dtfl_clock_fn clock, void *clock_ctx);
void __dtfl_log_registry_free(__dtfl_log_registry *r);

/*
 * Registry::insert: binds `name` to a new logger.  Replaces (and drops)
 * any logger already bound.  `ctx_free` (may be NULL) releases ctx when
 * the logger is dropped.
 */
void __dtfl_log_registry_insert(__dtfl_log_registry *r, const char *name,
    __dtfl_log_action action, void *ctx, void (*ctx_free)(void *));
bool __dtfl_log_registry_remove(__dtfl_log_registry *r, const char *name);
/* Registry::get: a new reference to the logger, or NULL. */
__dtfl_logger *__dtfl_log_registry_get(__dtfl_log_registry *r, const char *name);
void __dtfl_log_registry_flush(__dtfl_log_registry *r);
size_t __dtfl_log_registry_names(const __dtfl_log_registry *r,
    const char **names, size_t max);

/*
 * A logger outside any registry (Logger::new), on the monotonic clock,
 * for a thread that has no worker: the network threads.  The caller
 * holds the only reference; it belongs to the creating thread.
 */
__dtfl_logger *__dtfl_logger_new(__dtfl_log_action action, void *ctx,
    void (*ctx_free)(void *));

/* Loggers are reference counted within the worker. */
__dtfl_logger *__dtfl_logger_retain(__dtfl_logger *l);
void __dtfl_logger_release(__dtfl_logger *l);
/*
 * Logs one event of `len` bytes, stamped now.  Batches are flushed to
 * the action when they reach DTFL_BUFFER_SIZE_BYTES of event data, on
 * __dtfl_logger_flush, and when the logger is dropped.
 */
void __dtfl_logger_log(__dtfl_logger *l, const void *event, size_t len);
void __dtfl_logger_flush(__dtfl_logger *l);
uint64_t __dtfl_logger_now(const __dtfl_logger *l);

#endif /* DTFL_LOG_INT_H */
