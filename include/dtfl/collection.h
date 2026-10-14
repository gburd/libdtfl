/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/collection.h: the differential dataflow programming model.
 *
 * A dtfl_collection is a timely dataflow stream whose records are
 * differential updates: a (record, time, diff) triple.  The record is a
 * byte string described by a dtfl_type (dtfl/record.h); the time is the
 * logical time the change takes effect, a vector of the scope's
 * dtfl_time_class coordinates; the diff is a value of the collection's
 * dtfl_diff class saying how the multiplicity of the record changes.
 *
 * The collection methods (map, filter, flat_map, concat, negate,
 * consolidate, inspect, enter, leave, probe, explode, delay) build new
 * collections from old ones, exactly as differential dataflow's
 * Collection does.  They reuse timely's operators underneath.
 *
 * A dtfl_input_session feeds a collection: it buffers (record, diff)
 * changes at the session's current epoch, and advance_to and flush drive
 * the timely frontier so operators can make progress.
 *
 * Ownership: a dtfl_collection is a small handle owned by its scope;
 * free it with dtfl_collection_free (which does not touch the dataflow).
 * A dtfl_diff is reference counted and released with dtfl_diff_free.  A
 * dtfl_input_session is owned by the caller and released with
 * dtfl_input_session_close, which flushes and seals its input.
 */
#ifndef DTFL_COLLECTION_H
#define DTFL_COLLECTION_H

#include "dtfl/base.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtfl_collection dtfl_collection;
typedef struct dtfl_input_session dtfl_input_session;

/* ------------------------------------------------------------------ */
/* Diff classes (difference.rs; PLAN D4)                               */
/* ------------------------------------------------------------------ */

/*
 * A difference class: the algebra of a collection's diff type.  The
 * built-ins below are the signed (Abelian), unsigned (Monoid) and
 * present (Semigroup only) classes.  Values are stored natively; the
 * default for differential dataflow is i64.
 */
typedef struct dtfl_diff dtfl_diff;

DTFL_API dtfl_diff *dtfl_diff_i64(void);	/* the default */
DTFL_API dtfl_diff *dtfl_diff_i32(void);
DTFL_API dtfl_diff *dtfl_diff_i16(void);
DTFL_API dtfl_diff *dtfl_diff_i8(void);
DTFL_API dtfl_diff *dtfl_diff_u64(void);
DTFL_API dtfl_diff *dtfl_diff_u32(void);
DTFL_API dtfl_diff *dtfl_diff_u16(void);
DTFL_API dtfl_diff *dtfl_diff_u8(void);
DTFL_API dtfl_diff *dtfl_diff_present(void);

DTFL_API dtfl_diff *dtfl_diff_retain(const dtfl_diff *d);
DTFL_API void dtfl_diff_free(const dtfl_diff *d);
/* Width in bytes of a value of this class (0 for present). */
DTFL_API size_t dtfl_diff_width(const dtfl_diff *d);
/* True if the class is Abelian (has negate), which negate requires. */
DTFL_API bool dtfl_diff_is_abelian(const dtfl_diff *d);

/*
 * A user-defined diff class (difference.rs Semigroup and its extensions).
 * The callbacks implement the algebra of one difference type whose values
 * are `width` bytes, stored natively and unaligned (so copy with memcpy).
 * Each callback receives the ops' own `ctx` so a family of related classes
 * can share state.  A zero-width class (like Present) passes NULL value
 * pointers.
 *
 *   plus_equals	acc += x (Semigroup; required)
 *   is_zero		true if x is the additive identity (required); a
 *			semigroup need not have a zero, so this may always
 *			return false (differential uses it only to drop
 *			cancelled updates)
 *   cmp			Rust's Ord on values: <0, 0, >0 (required)
 *   multiply		acc *= x in place, Multiply<Self> (NULL = absent);
 *			providing it lets join multiply this diff with itself
 *   negate		x = -x, Abelian (NULL = not Abelian); if set, zero
 *			must be set too, and reduce_abelian, count and negate
 *			accept this class
 *   zero		write the zero value to out, Monoid (NULL = Semigroup
 *			only)
 *
 * `type` describes a value of this class when it becomes a record value
 * (e.g. in explode or count); NULL means the class carries no record
 * value (the unit type).  `ctx` is passed to every callback and freed by
 * `ctx_free` when the last reference to the class is released; either may
 * be NULL.
 */
typedef struct dtfl_diff_ops {
	const char *name;
	size_t width;
	void (*plus_equals)(void *ctx, void *acc, const void *x);
	bool (*is_zero)(void *ctx, const void *x);
	int (*cmp)(void *ctx, const void *a, const void *b);
	void (*multiply)(void *ctx, void *acc, const void *x);
	void (*negate)(void *ctx, void *x);
	void (*zero)(void *ctx, void *out);
	const dtfl_type *type;
	void *ctx;
	void (*ctx_free)(void *ctx);
} dtfl_diff_ops;

/*
 * A new diff class from a user-defined algebra (difference.rs).  Copies
 * *ops and retains ops->type if set.  plus_equals, is_zero and cmp are
 * required; negate requires zero (an Abelian class is a Monoid).  Panics
 * through the panic hook if a required callback is missing or negate is
 * set without zero.  Release the class with dtfl_diff_free.
 */
DTFL_API dtfl_diff *dtfl_diff_new(const dtfl_diff_ops *ops);

/* ------------------------------------------------------------------ */
/* Input sessions (input.rs)                                           */
/* ------------------------------------------------------------------ */

/*
 * A new input session over records of `type` with differences of class
 * `diff`.  The session retains both.  Its epoch starts at the minimum
 * time of the scope it is later added to.  Returns NULL on allocation
 * failure through the panic hook (it cannot happen for other reasons).
 */
DTFL_API dtfl_input_session *dtfl_input_session_new(const dtfl_type *type,
    const dtfl_diff *diff);

/*
 * Adds the session's input to `scope` and returns the collection it
 * feeds (InputSession::to_collection).  The scope's time class must be
 * totally ordered.  May be called once per session.
 */
DTFL_API dtfl_collection *dtfl_input_session_to_collection(
    dtfl_input_session *s, dtfl_scope *scope);

/*
 * Views a stream of differential update records as a collection
 * (differential's AsCollection trait).  `st` must carry records in the
 * update layout the collection operators produce: the data record bytes
 * followed by the scope's time words and the diff value.  `rec_type` is
 * the data record type (not the update type), and `diff` is its diff
 * class; this call takes the caller's `diff` reference.  The time-word
 * depth is taken from the stream's scope.  Returns a collection the
 * caller frees with dtfl_collection_free; it does not copy the stream.
 *
 * This is the bridge from a raw source (for example an unordered input
 * emitting update records at partial-order times) into the differential
 * layer, where dtfl_input_session_to_collection cannot be used because
 * the time class is not totally ordered.
 */
DTFL_API dtfl_collection *dtfl_stream_as_collection(dtfl_stream *st,
    const dtfl_type *rec_type, dtfl_diff *diff);

/* Adds `change` to the weight of `record` at the current epoch. */
DTFL_API void dtfl_input_session_update(dtfl_input_session *s,
    const void *record, size_t len, const void *change);
/*
 * Adds `change` at a future time `at` (>= the current epoch).
 * update_at panics if `at` is before the epoch.
 */
DTFL_API void dtfl_input_session_update_at(dtfl_input_session *s,
    const void *record, size_t len, const uint64_t *at, const void *change);
/* insert adds one copy; remove retracts one; both need an Abelian class. */
DTFL_API void dtfl_input_session_insert(dtfl_input_session *s,
    const void *record, size_t len);
DTFL_API void dtfl_input_session_remove(dtfl_input_session *s,
    const void *record, size_t len);

/*
 * Advances the epoch to `t` (>= the current epoch); panics otherwise.
 * This does not inform timely dataflow until the next flush.
 */
DTFL_API void dtfl_input_session_advance_to(dtfl_input_session *s,
    const uint64_t *t);
/* The session's current epoch. */
DTFL_API const uint64_t *dtfl_input_session_time(const dtfl_input_session *s);
/* Flushes buffered updates into the input and advances its frontier. */
DTFL_API void dtfl_input_session_flush(dtfl_input_session *s);
/* Flushes, seals the input and frees the session (upstream Drop). */
DTFL_API void dtfl_input_session_close(dtfl_input_session *s);

/* ------------------------------------------------------------------ */
/* Collections (collection.rs)                                         */
/* ------------------------------------------------------------------ */

/*
 * The record and diff a collection carries, and the stream and scope
 * behind it.  dtfl_collection_free releases the handle; the underlying
 * dataflow is owned by the worker.
 */
DTFL_API const dtfl_type *dtfl_collection_type(const dtfl_collection *c);
DTFL_API const dtfl_diff *dtfl_collection_diff(const dtfl_collection *c);
DTFL_API dtfl_scope *dtfl_collection_scope(const dtfl_collection *c);
DTFL_API void dtfl_collection_free(dtfl_collection *c);

/*
 * A collection record sink.  A map or flat_map callback writes each
 * output record (the record only, not its time or diff) through it; the
 * operator attaches the input update's time and diff.  An explode
 * callback writes a record then a factor value of its factor class.
 */
typedef struct dtfl_collection_emit dtfl_collection_emit;
DTFL_API void dtfl_collection_emit_record(dtfl_collection_emit *e,
    const void *rec, size_t len);
/*
 * explode: emit (record, factor).  `factor` is a value of the factor
 * diff class passed to dtfl_collection_explode.
 */
DTFL_API void dtfl_collection_emit_pair(dtfl_collection_emit *e,
    const void *rec, size_t len, const void *factor);

/* A record callback writes zero or more output records through an emit. */
typedef void (*dtfl_collection_map_fn)(void *ctx, const void *rec, size_t len,
    dtfl_collection_emit *out);
typedef bool (*dtfl_collection_pred_fn)(void *ctx, const void *rec, size_t len);
/* An inspect callback sees each (record, time, diff) update. */
typedef void (*dtfl_collection_inspect_fn)(void *ctx, const void *rec,
    size_t len, const uint64_t *time, const void *diff);

/*
 * map / flat_map: transform the record of each update, keeping its time
 * and diff.  map emits exactly one record of `out_type` per input;
 * flat_map emits zero or more.  filter keeps updates whose record
 * satisfies the predicate.
 */
DTFL_API dtfl_collection *dtfl_collection_map(dtfl_collection *c,
    const dtfl_type *out_type, dtfl_collection_map_fn fn, dtfl_ctx ctx);
DTFL_API dtfl_collection *dtfl_collection_flat_map(dtfl_collection *c,
    const dtfl_type *out_type, dtfl_collection_map_fn fn, dtfl_ctx ctx);
DTFL_API dtfl_collection *dtfl_collection_filter(dtfl_collection *c,
    dtfl_collection_pred_fn fn, dtfl_ctx ctx);

/*
 * negate: flips the sign of every diff.  The collection's class must be
 * Abelian (dtfl_diff_is_abelian); otherwise it panics.
 */
DTFL_API dtfl_collection *dtfl_collection_negate(dtfl_collection *c);

/* concat / concatenate: accumulate the updates of several collections. */
DTFL_API dtfl_collection *dtfl_collection_concat(dtfl_collection *a,
    dtfl_collection *b);
DTFL_API dtfl_collection *dtfl_collection_concatenate(dtfl_scope *scope,
    dtfl_collection *const *sources, size_t n);

/*
 * consolidate: groups updates by (record, time), sums their diffs and
 * drops those that sum to zero, exchanging records across workers so
 * each (record, time) is accumulated on one worker.  The result holds
 * at most one update per (record, time).
 */
DTFL_API dtfl_collection *dtfl_collection_consolidate(dtfl_collection *c);

/* inspect: calls `fn` for each update, for debugging or extraction. */
DTFL_API dtfl_collection *dtfl_collection_inspect(dtfl_collection *c,
    dtfl_collection_inspect_fn fn, dtfl_ctx ctx);

/*
 * explode: replaces each record with records of `out_type` and
 * multiplies the diff by a second diff, moving aggregatable data into
 * the difference.  `fn` emits (record, factor) pairs through the emit
 * handle, where each factor is a value of `factor_diff`; the result's
 * class is the product class of `factor_diff` and the input's class.
 * Returns NULL and reports DTFL_EALGEBRA if the multiply is undefined
 * for the pair.
 */
typedef void (*dtfl_collection_explode_fn)(void *ctx, const void *rec,
    size_t len, dtfl_collection_emit *out);
DTFL_API dtfl_collection *dtfl_collection_explode(dtfl_collection *c,
    const dtfl_type *out_type, const dtfl_diff *factor_diff,
    dtfl_collection_explode_fn fn, dtfl_ctx ctx);

/*
 * delay: re-times each update by `fn`, which must only advance the time
 * and must be monotone.  The timely capability advances with the same
 * logic.
 */
typedef void (*dtfl_collection_delay_fn)(void *ctx, const uint64_t *time,
    uint64_t *new_time);
DTFL_API dtfl_collection *dtfl_collection_delay(dtfl_collection *c,
    dtfl_collection_delay_fn fn, dtfl_ctx ctx);

/*
 * enter / leave: move a collection across a scope boundary, projecting
 * the embedded time with the scope refinement (to_inner / to_outer).
 * `child` must directly refine the collection's scope; leave returns to
 * the parent scope.
 */
DTFL_API dtfl_collection *dtfl_collection_enter(dtfl_collection *c,
    dtfl_scope *child);
DTFL_API dtfl_collection *dtfl_collection_leave(dtfl_collection *c);

/*
 * enter_at: like enter, but each record appears in the child scope at a
 * per-record initial inner time rather than the minimum.  The child must
 * directly refine the collection's scope (a Product<outer, inner>).  For
 * each update `logic` writes the initial inner time (`inner` words,
 * matching the child's inner coordinate depth) into `inner_time_out`,
 * which the operator pairs with the refined outer time, as
 * Collection::enter_at pairs Product::new(time, initial(data)).
 */
typedef void (*dtfl_collection_enter_at_fn)(void *ctx, const void *rec,
    size_t len, uint64_t *inner_time_out);
DTFL_API dtfl_collection *dtfl_collection_enter_at(dtfl_collection *c,
    dtfl_scope *child, dtfl_collection_enter_at_fn logic, dtfl_ctx ctx);

/*
 * probe / probe_with: attach a timely probe to the collection's output,
 * to detect when it has stabilized.  probe allocates a probe and stores
 * it through `probe`; probe_with attaches an existing one.
 */
DTFL_API dtfl_collection *dtfl_collection_probe(dtfl_collection *c,
    dtfl_probe **probe);
DTFL_API dtfl_collection *dtfl_collection_probe_with(dtfl_collection *c,
    dtfl_probe *probe);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_COLLECTION_H */
