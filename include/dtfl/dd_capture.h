/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/dd_capture.h: capture and replay of differential collections, the
 * CDC V2 protocol (differential-dataflow/src/capture.rs).
 *
 * A differential collection is a stream of (data, time, diff) updates.
 * The CDC V2 protocol records facts about the collection that once true
 * stay true: the exact changes at each time (Updates), and the exact
 * number of distinct updates within a time interval (Progress).  A
 * replay reconstructs the original multiset from the message stream even
 * when that stream duplicates and reorders messages, which is the point
 * of the protocol: the storage between capture and replay may do either.
 *
 * This port provides the single-threaded pieces of capture.rs:
 *
 *   - dtfl_cdc_log: a recorded stream of CDC V2 messages (Updates and
 *     Progress), as produced by the sink and consumed by the replay
 *     iterator.  A log can be captured from a collection or built by
 *     hand, and its messages read back in any order.
 *
 *   - dtfl_collection_capture: the sink (capture.rs sink::build).  It
 *     observes a consolidated collection, recording each update as an
 *     Updates message and each advance of the input frontier as a
 *     Progress message with the exact per-time update counts.  As
 *     upstream requires, the collection must already be consolidated:
 *     the sink does not consolidate on the stream's behalf, so a record
 *     seen is taken to be a true, irrevocable change.
 *
 *   - dtfl_cdc_iter: the deduplicating, reordering replay iterator
 *     (capture.rs iterator::Iter).  Messages are pushed in any order and
 *     with arbitrary duplication; the iterator yields each update once,
 *     in frontier-ordered batches, holding a memory footprint
 *     proportional to the mismatch between received updates and progress.
 *
 * All three carry the record type, time class and diff class of the
 * collection, so an update is (record bytes, time words, diff bytes),
 * laid out the way the collection's inspect callback presents one.
 *
 * Not ported here: the data-parallel source::build dataflow, which
 * reconstructs the stream across workers by running the same dedup and
 * reorder logic as dtfl_cdc_iter inside four exchange-connected timely
 * operators (MESSAGES, UPDATES, PROGRESS, FEEDBACK).  The single-worker
 * iterator is the reference the distributed version implements; it
 * covers the round-trip-under-duplication-and-reordering guarantee.
 */
#ifndef DTFL_DD_CAPTURE_H
#define DTFL_DD_CAPTURE_H

#include "dtfl/base.h"
#include "dtfl/collection.h"
#include "dtfl/time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The kind of a CDC V2 message. */
typedef enum dtfl_cdc_kind {
	DTFL_CDC_UPDATES = 0,	/* a batch of (data, time, diff) updates */
	DTFL_CDC_PROGRESS = 1	/* a count statement over a time interval */
} dtfl_cdc_kind;

/*
 * A recorded stream of CDC V2 messages (capture.rs Message).  Carries
 * the record type, time class and diff class so updates and times can be
 * sized and compared.  Not thread safe; owned by its creating thread.
 */
typedef struct dtfl_cdc_log dtfl_cdc_log;

/*
 * A new, empty message log for a collection of records `rec`, times of
 * class `tc` and diffs of class `diff`.  Retains the three descriptors.
 * Free with dtfl_cdc_log_free.
 */
DTFL_API dtfl_cdc_log *dtfl_cdc_log_new(const dtfl_type *rec,
    const dtfl_time_class *tc, const dtfl_diff *diff);

/* Frees a log and everything in it. */
DTFL_API void dtfl_cdc_log_free(dtfl_cdc_log *log);

/* The number of messages recorded in the log. */
DTFL_API size_t dtfl_cdc_log_len(const dtfl_cdc_log *log);

/* The kind of message `i` (0 <= i < dtfl_cdc_log_len). */
DTFL_API dtfl_cdc_kind dtfl_cdc_log_kind(const dtfl_cdc_log *log, size_t i);

/*
 * Appends an Updates message holding one (record, time, diff) triple.
 * `rec`/`reclen` is the record, `time` the time words (tc's depth), and
 * `diff` the diff bytes (diff class width).  Each call records a one-
 * triple batch; this is what the capturer emits and what a hand-built
 * log uses to seed updates.
 */
DTFL_API void dtfl_cdc_log_push_update(dtfl_cdc_log *log, const void *rec,
    size_t reclen, const uint64_t *time, const void *diff);

/*
 * Appends a Progress message: a statement that over the interval
 * (lower, upper] the given per-time counts are exact and final.  `lower`
 * and `upper` are antichains of `nlower`/`nupper` time points (each tc's
 * depth words); `counts` is `ncounts` (time, count) pairs, with the
 * times packed depth words each and the counts a parallel array.
 */
DTFL_API void dtfl_cdc_log_push_progress(dtfl_cdc_log *log,
    const uint64_t *lower, size_t nlower, const uint64_t *upper, size_t nupper,
    const uint64_t *count_times, const size_t *counts, size_t ncounts);

/*
 * Copies message `i` of `src` onto the end of `dst` (same descriptors).
 * Used to duplicate and reorder a captured log before replay, which is
 * exactly the abuse the protocol tolerates.
 */
DTFL_API void dtfl_cdc_log_append(dtfl_cdc_log *dst, const dtfl_cdc_log *src,
    size_t i);

/*
 * The sink (capture.rs sink::build): capture `c` into `log`.  Returns a
 * new collection, identical to `c`, whose updates and frontier advances
 * are recorded into `log` as the dataflow runs; attach it to a probe and
 * step the worker to drive the capture.  `c` must already be
 * consolidated (the sink does not consolidate), so pass the output of
 * dtfl_collection_consolidate.  `log` must outlive the dataflow.  The
 * caller frees the returned collection with dtfl_collection_free.
 */
DTFL_API dtfl_collection *dtfl_collection_capture(dtfl_collection *c,
    dtfl_cdc_log *log);

/*
 * The replay iterator (capture.rs iterator::Iter): a deduplicating,
 * reordering reader of CDC V2 messages.  Push messages in any order and
 * with arbitrary duplication, then drain completed batches.  Carries the
 * same descriptors as the log it reads.  Free with dtfl_cdc_iter_free.
 */
typedef struct dtfl_cdc_iter dtfl_cdc_iter;

DTFL_API dtfl_cdc_iter *dtfl_cdc_iter_new(const dtfl_type *rec,
    const dtfl_time_class *tc, const dtfl_diff *diff);

DTFL_API void dtfl_cdc_iter_free(dtfl_cdc_iter *it);

/*
 * Pushes message `i` of `log` into the iterator.  Duplicates are
 * discarded internally; order does not matter.
 */
DTFL_API void dtfl_cdc_iter_push(dtfl_cdc_iter *it, const dtfl_cdc_log *log,
    size_t i);

/*
 * Receives one reconstructed batch: the updates newly resolved by the
 * last pushes and the frontier through which they are now final.  `fn`
 * is called once per update of the batch with its (record, time, diff);
 * `frontier`/`nfrontier` is the batch's lower bound (an antichain, tc's
 * depth words each).  Returns true if a batch was produced (fn may be
 * called zero times for a frontier-only advance), false if no new batch
 * is ready; call repeatedly until it returns false.
 */
typedef void (*dtfl_cdc_update_fn)(void *ctx, const void *rec, size_t reclen,
    const uint64_t *time, const void *diff);
DTFL_API bool dtfl_cdc_iter_next(dtfl_cdc_iter *it, dtfl_cdc_update_fn fn,
    void *ctx, const uint64_t **frontier, size_t *nfrontier);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_DD_CAPTURE_H */
