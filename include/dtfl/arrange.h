/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/arrange.h: arrangements, the shared indexed form of a collection.
 *
 * An arrangement turns a dtfl_collection into a maintained, worker-local
 * index: a shared, reference-counted trace of (key, value, time, diff)
 * updates, plus a stream of the batches that build it.  Several differential
 * operators (join, reduce, count) read an arrangement instead of a
 * collection, so the cost of indexing a collection, communication,
 * computation and memory, is paid once and shared.
 *
 * dtfl_collection_arrange indexes a collection: it exchanges updates by
 * record, batches each frontier interval through the merge batcher into a
 * spine, and shares the spine through a trace agent.  dtfl_arranged_as_collection
 * turns an arrangement back into a collection by reading its batches.
 * dtfl_arranged_trace extracts a shareable trace handle (a cloned agent),
 * which dtfl_trace_import brings into another dataflow on the same worker,
 * producing a second arrangement over the same backing data; this is how
 * one index serves several downstream operators (PLAN section 5 rule 4: a
 * trace is worker-local and imports only into dataflows on its own worker).
 *
 * Ownership: dtfl_arranged and dtfl_trace are handles the caller owns and
 * frees with dtfl_arranged_free and dtfl_trace_free.  Freeing a trace handle
 * releases its compaction hold, which lets the shared trace compact further;
 * a handle that is never freed holds compaction at its frontier forever.
 */
#ifndef DTFL_ARRANGE_H
#define DTFL_ARRANGE_H

#include "dtfl/base.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/timely.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtfl_arranged dtfl_arranged;
typedef struct dtfl_trace dtfl_trace;

/*
 * Arranges a collection into a shared trace (arrange_named).  The records
 * are indexed by the whole record as key; downstream operators read the
 * arrangement's batches or its trace.  `name` labels the operator.  The
 * collection's diff class must be at least a Semigroup, which every class
 * satisfies.  Returns a handle the caller frees with dtfl_arranged_free.
 */
DTFL_API dtfl_arranged *dtfl_collection_arrange(dtfl_collection *c,
    const char *name);

/*
 * Arranges a stream of keyed upserts into a shared trace
 * (arrange_from_upsert).  Each record of `upserts` is a key followed by an
 * option(value) (one tag byte, 0 for None and 1 for Some, then the value),
 * and the update's embedded time is the upsert time; the diff is ignored.
 * The collection holds, for each key, the most recent value by time, and a
 * None uninstalls the key.  The operator computes the retractions itself,
 * so the result trace indexes key -> value with isize diffs.  Ties in time
 * take the greatest option(value) (None < Some).  The time class must be
 * totally ordered.  Returns a handle the caller frees with
 * dtfl_arranged_free.
 */
DTFL_API dtfl_arranged *dtfl_arrange_from_upsert(dtfl_collection *upserts,
    const dtfl_type *key_type, const dtfl_type *val_type, const char *name);

/*
 * Reads an arrangement back as a collection (as_collection).  Each stored
 * (key, value) becomes zero or more output records of `out_type`: `fn`
 * writes them from the key and value bytes through the emit handle, and the
 * operator pairs each with every (time, diff) of that (key, value).  When
 * `fn` is NULL the output record is the stored record itself (the key,
 * since arrange indexes the whole record) and `out_type` must be the
 * collection's record type.  The output diff class is the arrangement's.
 */
typedef struct dtfl_arranged_emit dtfl_arranged_emit;
DTFL_API void dtfl_arranged_emit_record(dtfl_arranged_emit *e, const void *rec,
    size_t len);
typedef void (*dtfl_arranged_kv_fn)(void *ctx, const void *key, size_t klen,
    const void *val, size_t vlen, dtfl_arranged_emit *out);
DTFL_API dtfl_collection *dtfl_arranged_as_collection(dtfl_arranged *a,
    const dtfl_type *out_type, dtfl_arranged_kv_fn fn, dtfl_ctx ctx);

/*
 * Extracts a shareable trace handle from an arrangement (the Arranged.trace
 * field).  The handle is a new reader of the shared trace; it may outlive
 * the arrangement and be imported into other dataflows on the same worker.
 * The caller frees it with dtfl_trace_free, which releases its compaction
 * hold.
 */
DTFL_API dtfl_trace *dtfl_arranged_trace(const dtfl_arranged *a);

/*
 * Imports a trace into a dataflow scope (TraceAgent::import_named),
 * producing an arrangement over the same backing data.  The scope must
 * belong to the same worker that created the trace.  `name` labels the
 * source operator.  Returns a handle the caller frees with
 * dtfl_arranged_free; the trace handle remains the caller's.
 */
DTFL_API dtfl_arranged *dtfl_trace_import(dtfl_trace *t, dtfl_scope *scope,
    const char *name);
DTFL_API void dtfl_trace_free(dtfl_trace *t);

/*
 * enter / leave: carry an arrangement across a scope boundary.  enter brings
 * it into `child`, a scope that directly refines the arrangement's scope,
 * lifting every time with to_inner; leave returns to the parent.
 * enter_region / leave_region apply to a region (a subscope with the same
 * time class), where no time transform is needed.  Each returns a new handle
 * the caller frees.
 */
DTFL_API dtfl_arranged *dtfl_arranged_enter(dtfl_arranged *a,
    dtfl_scope *child);
DTFL_API dtfl_arranged *dtfl_arranged_leave(dtfl_arranged *a);
DTFL_API dtfl_arranged *dtfl_arranged_enter_region(dtfl_arranged *a,
    dtfl_scope *region);
DTFL_API dtfl_arranged *dtfl_arranged_leave_region(dtfl_arranged *a);

/* The record and diff an arrangement carries. */
DTFL_API const dtfl_type *dtfl_arranged_type(const dtfl_arranged *a);
DTFL_API const dtfl_diff *dtfl_arranged_diff(const dtfl_arranged *a);
DTFL_API dtfl_scope *dtfl_arranged_scope(const dtfl_arranged *a);

/* Releases the handle; the shared trace lives on until its last reader. */
DTFL_API void dtfl_arranged_free(dtfl_arranged *a);

/* ------------------------------------------------------------------ */
/* Join (join.rs; collection.rs join family)                           */
/* ------------------------------------------------------------------ */

/*
 * A join result sink.  The result closure of a join writes each output
 * record (the record bytes only) through it; the operator attaches the
 * joined time (time1 join time2) and the multiplied diff (diff1 * diff2)
 * to every record it emits.
 */
typedef struct dtfl_join_emit dtfl_join_emit;
DTFL_API void dtfl_join_emit_record(dtfl_join_emit *e, const void *rec,
    size_t len);

/*
 * The result closure of a join: for a key present in both inputs, it is
 * called with the key and a matched pair of values, and emits zero or more
 * output records of the join's out_type through the sink.  val1 and val2
 * are the stored value bytes; key is the shared key bytes.
 */
typedef void (*dtfl_join_result_fn)(void *ctx, const void *key, size_t klen,
    const void *val1, size_t v1len, const void *val2, size_t v2len,
    dtfl_join_emit *out);

/*
 * dtfl_arranged_join_core (Arranged::join_core): joins two arranged traces
 * sharing a key type.  For every pair of matched values the result closure
 * emits output records of `out_type`, each carried at the joined time with
 * the product of the two diffs; the output is consolidated.  The two diff
 * classes must have a declared Multiply (join.rs); otherwise this reports
 * DTFL_EALGEBRA and returns NULL.  The caller still owns a1 and a2.
 */
DTFL_API dtfl_collection *dtfl_arranged_join_core(dtfl_arranged *a1,
    dtfl_arranged *a2, const dtfl_type *out_type, dtfl_join_result_fn result,
    dtfl_ctx ctx);

/*
 * dtfl_collection_join_map (Collection::join_map): arranges both inputs by
 * key and joins them, applying `result` to each matched (key, val1, val2).
 * c1 holds key||val1 records, c2 holds key||val2; `key_type` splits both.
 * `val1_type` and `val2_type` describe the stored values.  The output
 * records are of `out_type`, with product diffs; it may report
 * DTFL_EALGEBRA (NULL) if the diff classes do not multiply.
 */
DTFL_API dtfl_collection *dtfl_collection_join_map(dtfl_collection *c1,
    dtfl_collection *c2, const dtfl_type *key_type, const dtfl_type *val1_type,
    const dtfl_type *val2_type, const dtfl_type *out_type,
    dtfl_join_result_fn result, dtfl_ctx ctx);

/*
 * dtfl_collection_join (Collection::join): matches (key, val1) and (key,
 * val2) by key, yielding records key||val1||val2.  `out_type` describes
 * that concatenation.  Equivalent to join_map with the identity-concat
 * closure.
 */
DTFL_API dtfl_collection *dtfl_collection_join(dtfl_collection *c1,
    dtfl_collection *c2, const dtfl_type *key_type, const dtfl_type *val1_type,
    const dtfl_type *val2_type, const dtfl_type *out_type);

/*
 * dtfl_collection_semijoin (Collection::semijoin): matches (key, val) in
 * `c` against the bare key records of `keys` by key, keeping the former
 * with multiplicities multiplied.  `keys` is arranged by self (key ->
 * unit).  The output records are key||val of `out_type`.
 */
DTFL_API dtfl_collection *dtfl_collection_semijoin(dtfl_collection *c,
    dtfl_collection *keys, const dtfl_type *key_type, const dtfl_type *val_type,
    const dtfl_type *out_type);

/*
 * dtfl_collection_antijoin (Collection::antijoin): subtracts the semijoin
 * with `keys` from `c`, discarding records whose key is present in `keys`.
 * `c`'s diff class must be Abelian.  The output records are key||val of
 * `out_type`.
 */
DTFL_API dtfl_collection *dtfl_collection_antijoin(dtfl_collection *c,
    dtfl_collection *keys, const dtfl_type *key_type, const dtfl_type *val_type,
    const dtfl_type *out_type);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_ARRANGE_H */
