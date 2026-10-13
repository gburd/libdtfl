/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/reduce.h: the reduce family of differential dataflow operators.
 *
 * reduce groups a collection of (key, value) updates by key and applies a
 * user reduction to the accumulated multiset of values at each key, emitting
 * output (value, diff) pairs.  It is incremental: re-evaluating a key at a
 * new time retracts the previously produced output and asserts the new one,
 * so the collection it defines is always the function of its input at every
 * time.  count sums the diffs per key; distinct maps any positive
 * accumulation to one; threshold applies a user predicate to the accumulated
 * diff of each key.  These are all special cases of reduce.
 *
 * A reduce input is a collection whose record is a key followed by a value
 * (key || val), split by the key type's length (dtfl/record.h D1).  count,
 * distinct and threshold take a plain collection whose record is the key.
 *
 * Ownership: these methods consume their input collection handle the way the
 * other collection methods do (the caller still frees it) and return a new
 * handle the caller frees with dtfl_collection_free or dtfl_arranged_free.
 */
#ifndef DTFL_REDUCE_H
#define DTFL_REDUCE_H

#include "dtfl/arrange.h"
#include "dtfl/base.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The output sink a reduce logic writes through.  The logic emits each
 * output (value, diff) pair: the value bytes of the output value type and a
 * diff value of the output diff class.  reduce pairs each with the key and
 * the time at which the output holds.
 */
typedef struct dtfl_reduce_emit dtfl_reduce_emit;
DTFL_API void dtfl_reduce_emit_pair(dtfl_reduce_emit *e, const void *val,
    size_t vlen, const void *diff);

/*
 * A reduce logic: given the key and the accumulated input multiset at a time
 * (an array of (value, diff) pairs, sorted by the value type's order), it
 * emits output (value, diff) pairs through the emit handle.  The input array
 * is non-empty and `n` is its length; `vals[i]` points to value i's bytes,
 * `vlens[i]` is its length, and `diffs` is the packed diff column (the input
 * collection's diff width per entry).  The output diff is of `out_diff` (the
 * class passed to dtfl_collection_reduce), which must be Abelian.
 */
typedef void (*dtfl_reduce_fn)(void *ctx, const void *key, size_t klen,
    size_t n, const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out);

/*
 * reduce / reduce_named: group `c` (records are key || val) by key, apply
 * `logic` to the accumulated values of each key, and emit output records
 * key || out_val with differences of class `out_diff` (which must be
 * Abelian).  `key_type` splits each record into key and value, and
 * `val_type` describes the value side (so the library can order and index
 * values); `out_val` describes the emitted value.  Returns a new collection
 * the caller frees.
 */
DTFL_API dtfl_collection *dtfl_collection_reduce(dtfl_collection *c,
    const dtfl_type *key_type, const dtfl_type *val_type,
    const dtfl_type *out_val, const dtfl_diff *out_diff, dtfl_reduce_fn logic,
    dtfl_ctx ctx);
DTFL_API dtfl_collection *dtfl_collection_reduce_named(dtfl_collection *c,
    const char *name, const dtfl_type *key_type, const dtfl_type *val_type,
    const dtfl_type *out_val, const dtfl_diff *out_diff, dtfl_reduce_fn logic,
    dtfl_ctx ctx);

/* ------------------------------------------------------------------ */
/* Reduce directly over an arrangement (arrangement.rs reduce_core,       */
/* reduce_abelian).                                                       */
/* ------------------------------------------------------------------ */

/*
 * A reduce-core logic: like dtfl_reduce_fn, but it also sees the tentative
 * accumulated output and emits the full output change rather than the new
 * output alone.  `key` is the current key; the input multiset is the `n`
 * (value, diff) entries (`in_vals[i]`/`in_vlens[i]` are value i's bytes and
 * length, `in_diffs` is the packed input-diff column); the tentative output
 * multiset is the `m` (value, diff) entries (`out_vals`/`out_vlens`/
 * `out_diffs`, packed with the output diff width).  The logic emits output
 * (value, diff) pairs through `out`; those pairs are the change to apply to
 * the output, so a reduce_core logic that wants to keep the prior output
 * must retract it itself.  dtfl_arranged_reduce_abelian wraps this to
 * retract the prior output automatically (see below).
 */
typedef void (*dtfl_reduce_core_fn)(void *ctx, const void *key, size_t klen,
    size_t n, const void *const *in_vals, const size_t *in_vlens,
    const void *in_diffs, size_t m, const void *const *out_vals,
    const size_t *out_vlens, const void *out_diffs, dtfl_reduce_emit *out);

/*
 * dtfl_arranged_reduce_core (Arranged::reduce_core): reduce an arrangement
 * `a` (key -> value) directly, producing an output arrangement
 * (key -> out_val, out_diff differences).  `name` labels the operator;
 * `logic` computes the output change per key as described for
 * dtfl_reduce_core_fn.  `out_diff` need not be Abelian.  Returns a handle
 * the caller frees with dtfl_arranged_free; the caller still owns `a`.
 */
DTFL_API dtfl_arranged *dtfl_arranged_reduce_core(dtfl_arranged *a,
    const char *name, const dtfl_type *out_val, const dtfl_diff *out_diff,
    dtfl_reduce_core_fn logic, dtfl_ctx ctx);

/*
 * dtfl_arranged_reduce_abelian (Arranged::reduce_abelian): as reduce_core,
 * but it wraps `logic` to retract the prior output (negate and append the
 * tentative output) and consolidate, so `logic` need only produce the new
 * output for each key.  `out_diff` must be Abelian.  Returns a handle the
 * caller frees with dtfl_arranged_free; the caller still owns `a`.
 */
DTFL_API dtfl_arranged *dtfl_arranged_reduce_abelian(dtfl_arranged *a,
    const char *name, const dtfl_type *out_val, const dtfl_diff *out_diff,
    dtfl_reduce_core_fn logic, dtfl_ctx ctx);

/*
 * count: for each distinct key in `c` (the whole record is the key), emit
 * the record key || count, where count is the summed input diff rendered as
 * a value of the input diff class, with an output difference of `out_diff`
 * (which must be Abelian with a `one`, e.g. dtfl_diff_i64).  Returns a new
 * collection the caller frees.
 */
DTFL_API dtfl_collection *dtfl_collection_count(dtfl_collection *c,
    const dtfl_diff *out_diff);

/*
 * distinct: for each key whose accumulated diff is non-zero, emit the key
 * with difference one of `out_diff` (Abelian with `one`).  Equivalent to a
 * threshold that maps any non-zero accumulation to one.  Returns a new
 * collection the caller frees.
 */
DTFL_API dtfl_collection *dtfl_collection_distinct(dtfl_collection *c,
    const dtfl_diff *out_diff);

/*
 * A threshold predicate: given the key and its accumulated input diff
 * (the input collection's diff class), it writes an output diff value of
 * `out_diff` through `out` and returns true to emit it, or false to emit
 * nothing.  Called with the accumulation before and after each change; the
 * operator takes the difference, so the predicate need not be incremental.
 */
typedef bool (*dtfl_threshold_fn)(void *ctx, const void *key, size_t klen,
    const void *acc_diff, void *out);

/*
 * threshold / threshold_named: transform the multiplicity of each key.  For
 * each key, `thresh` maps the accumulated input diff to an output diff of
 * `out_diff` (Abelian); the operator emits the change in that output.
 * Returns a new collection the caller frees.
 */
DTFL_API dtfl_collection *dtfl_collection_threshold(dtfl_collection *c,
    const dtfl_diff *out_diff, dtfl_threshold_fn thresh, dtfl_ctx ctx);
DTFL_API dtfl_collection *dtfl_collection_threshold_named(dtfl_collection *c,
    const char *name, const dtfl_diff *out_diff, dtfl_threshold_fn thresh,
    dtfl_ctx ctx);

/* ------------------------------------------------------------------ */
/* Total-order specializations (count.rs, threshold.rs)                 */
/* ------------------------------------------------------------------ */

/*
 * count_total, distinct_total and threshold_total are optimizations of
 * count, distinct and threshold for a totally ordered time (count.rs
 * CountTotal, threshold.rs ThresholdTotal).  They read the input trace
 * before the current batch to recover each key's accumulated difference and
 * emit the change in the counted or thresholded value directly, without the
 * general reduce engine.  The collection's time class must be totally
 * ordered (dtfl_time_is_total); otherwise they panic.  Each returns a new
 * collection the caller frees.
 *
 * count_total emits the record key || count, where count is the summed input
 * difference rendered as a value of the input difference class, with an
 * output difference of one of `out_diff` (Abelian with a one).
 * distinct_total emits each key whose accumulation is non-zero with a
 * difference of one.  threshold_total maps a key's accumulated input
 * difference to an output difference of `out_diff` through `thresh`, which
 * must be Abelian, and emits the change.
 */
DTFL_API dtfl_collection *dtfl_collection_count_total(dtfl_collection *c,
    const dtfl_diff *out_diff);
DTFL_API dtfl_collection *dtfl_collection_distinct_total(dtfl_collection *c,
    const dtfl_diff *out_diff);
DTFL_API dtfl_collection *dtfl_collection_threshold_total(dtfl_collection *c,
    const dtfl_diff *out_diff, dtfl_threshold_fn thresh, dtfl_ctx ctx);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_REDUCE_H */
