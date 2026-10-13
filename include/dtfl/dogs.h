/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/dogs.h: delta queries and worst-case-optimal joins (dogsdogsdogs).
 *
 * A delta query maintains a multi-way equijoin incrementally by keeping,
 * for each relation, an index of its tuples, and for each change to a
 * relation extending the changed prefix with the matching tuples of the
 * other relations.  dtfl_collection_index wraps a (key, value) collection
 * into the three traces an extender needs: a count trace (how many
 * extensions a relation offers a prefix), a propose trace (the extensions
 * themselves) and a validate trace (whether a proposed extension exists).
 *
 * dtfl_collection_index_extend_using builds an extender from an index and
 * a key selector that reads the join key out of a prefix; the key selector
 * is what makes one index serve several positions in a query (forward and
 * reverse edges, say).  dtfl_collection_extend runs the generic join: given
 * a prefix stream and a set of extenders, it counts each extender's offer,
 * routes each prefix to the extender that offers the fewest (the
 * worst-case-optimal choice), proposes from it, and validates against the
 * rest, concatenating the per-extender results.  With a single extender it
 * is a plain propose.
 *
 * These dataflows run in an AltNeu scope (dtfl/time.h dtfl_time_altneu):
 * each relation is entered at its alt time, and a relation that appears
 * after the delta stream in the join order is delayed to its neu time, so
 * every pair of concurrent updates interacts exactly once (the delta-query
 * correctness condition).  dtfl_collection_delay_neu re-times a collection
 * from alt to neu for that purpose.  dtfl_collection_differentiate and
 * dtfl_collection_integrate (calculus.rs) move a collection into and out of
 * its instantaneous-change form, which is what makes the delta-flow
 * region's accumulated state almost always empty.
 *
 * The propose, validate and count operators are also exposed directly, for
 * hand-written delta queries that wire the relations themselves rather than
 * through the generic join.
 *
 * Ownership: a dtfl_collection_index and a dtfl_prefix_extender are handles
 * the caller owns and frees with their _free functions.  Freeing an index
 * releases its share of the three traces.  An extender holds a reference to
 * its index, so an index may be freed once its extenders are built.
 */
#ifndef DTFL_DOGS_H
#define DTFL_DOGS_H

#include "dtfl/arrange.h"
#include "dtfl/base.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtfl_collection_index dtfl_collection_index;
typedef struct dtfl_prefix_extender dtfl_prefix_extender;

/* ------------------------------------------------------------------ */
/* AltNeu helpers (altneu.rs)                                          */
/* ------------------------------------------------------------------ */

/*
 * delay a collection in an AltNeu scope from its alt time to its neu time
 * (upstream `.delay(|t| AltNeu::neu(t.time))`).  Every update keeps its
 * base time but moves from alt (2t) to neu (2t + 1), so a relation joined
 * after the delta stream sees the delta stream's current state (the delta
 * query correctness condition).  The collection's scope must be an AltNeu
 * scope; otherwise it panics.
 */
DTFL_API dtfl_collection *dtfl_collection_delay_neu(dtfl_collection *c);

/* ------------------------------------------------------------------ */
/* The differential calculus (calculus.rs)                             */
/* ------------------------------------------------------------------ */

/*
 * differentiate (calculus.rs Differentiate): enter the AltNeu scope `child`
 * and present `c` as its instantaneous changes.  Each update (data, alt(t),
 * diff) is emitted together with (data, neu(t), -diff), so the accumulated
 * collection is the change at the moment it occurs and empty thereafter.
 * `c`'s diff class must be Abelian; `child` must be an AltNeu scope that
 * directly refines `c`'s scope.  Returns a collection in `child`.
 */
DTFL_API dtfl_collection *dtfl_collection_differentiate(dtfl_collection *c,
    dtfl_scope *child);

/*
 * integrate (calculus.rs Integrate): collect the instantaneous changes of
 * an AltNeu collection back into a collection in the parent scope.  Each
 * neu update is discarded and each alt update has its wrapper stripped,
 * reconstructing the collection.  `c`'s scope must be an AltNeu scope.
 */
DTFL_API dtfl_collection *dtfl_collection_integrate(dtfl_collection *c);

/* ------------------------------------------------------------------ */
/* The collection index (lib.rs CollectionIndex)                       */
/* ------------------------------------------------------------------ */

/*
 * Indexes a collection of key||val records (CollectionIndex::index).  `kt`
 * splits each record into its key and value; `vt` describes the value.
 * Builds the count, propose and validate traces the extenders read.  The
 * collection's diff class must be the signed default (isize), as upstream's
 * CollectionIndex requires.  Returns a handle the caller frees with
 * dtfl_collection_index_free.
 */
DTFL_API dtfl_collection_index *dtfl_collection_index_new(dtfl_collection *c,
    const dtfl_type *kt, const dtfl_type *vt);

/* Releases the index handle (and its share of the three traces). */
DTFL_API void dtfl_collection_index_free(dtfl_collection_index *idx);

/*
 * A key selector: reads the join key out of a prefix record and writes its
 * bytes to `key`, storing the length in *klen.  `key` has room for at least
 * `cap` bytes (the key type's maximum width).  The selected key must be a
 * value of the index's key type.
 */
typedef void (*dtfl_key_selector_fn)(void *ctx, const void *prefix,
    size_t plen, void *key, size_t cap, size_t *klen);

/*
 * Builds an extender from an index and a key selector (extend_using).  The
 * extender reads the index at the key the selector picks from each prefix.
 * `prefix_type` is the record type of the prefixes the extender will see;
 * it is retained.  Returns a handle the caller frees with
 * dtfl_prefix_extender_free.  The extender holds its own reference to the
 * index's traces, so the index may be freed afterwards.
 */
DTFL_API dtfl_prefix_extender *dtfl_collection_index_extend_using(
    const dtfl_collection_index *idx, const dtfl_type *prefix_type,
    dtfl_key_selector_fn key, dtfl_ctx ctx);

/* Releases an extender handle. */
DTFL_API void dtfl_prefix_extender_free(dtfl_prefix_extender *pe);

/* ------------------------------------------------------------------ */
/* The generic join driver (lib.rs ProposeExtensionMethod::extend)     */
/* ------------------------------------------------------------------ */

/*
 * Extends each prefix in `prefixes` with the values the extenders agree
 * on (ProposeExtensionMethod::extend), producing records prefix||extension
 * of `out_type`.  With one extender this is a plain propose.  With several,
 * each prefix is routed to the extender that offers it the fewest
 * extensions, proposed from there, then validated against every other
 * extender; the per-extender results are concatenated.  The extenders all
 * read the same prefix type (the type passed to extend_using) and extend
 * with the same value type.  The caller still owns `prefixes` and the
 * extenders.
 */
DTFL_API dtfl_collection *dtfl_collection_extend(dtfl_collection *prefixes,
    dtfl_prefix_extender *const *extenders, size_t n,
    const dtfl_type *out_type);

/* ------------------------------------------------------------------ */
/* The operators (operators/{count,propose,validate}.rs)               */
/* ------------------------------------------------------------------ */

/*
 * propose (operators/propose.rs): for each prefix, extend it with every
 * value the arrangement holds under the prefix's key, scaling the prefix
 * diff by the matched count.  `prefixes` carries prefix records; `arr` is a
 * key -> value arrangement; `key` selects the key from a prefix.  The
 * output records are prefix||value of `out_type`.  Reads the arrangement at
 * times less or equal to each update's own (the delta-query probe).
 */
DTFL_API dtfl_collection *dtfl_dogs_propose(dtfl_collection *prefixes,
    dtfl_arranged *arr, const dtfl_type *out_type, dtfl_key_selector_fn key,
    dtfl_ctx ctx);

/*
 * propose_distinct (operators/propose.rs): like dtfl_dogs_propose but does
 * not scale the prefix diff by the match count.
 */
DTFL_API dtfl_collection *dtfl_dogs_propose_distinct(dtfl_collection *prefixes,
    dtfl_arranged *arr, const dtfl_type *out_type, dtfl_key_selector_fn key,
    dtfl_ctx ctx);

/*
 * validate (operators/validate.rs): keep each prefix||value extension whose
 * (key(prefix), value) pair is present in the arrangement `arr` (a
 * (key, value) -> unit arrangement, arranged by self).  `key` selects the
 * key from the prefix part; the value part is the extension.  The output
 * records are the retained prefix||value of `out_type`.
 */
DTFL_API dtfl_collection *dtfl_dogs_validate(dtfl_collection *extensions,
    dtfl_arranged *arr, const dtfl_type *out_type,
    const dtfl_type *prefix_type, const dtfl_type *val_type,
    dtfl_key_selector_fn key, dtfl_ctx ctx);

/*
 * count (operators/count.rs): annotate each prefix triple (prefix, count,
 * index) with the smaller of its current count and the number of extensions
 * the arrangement offers under the prefix's key; when the arrangement's
 * count is smaller the triple's index is overwritten with `index`.  Used by
 * the generic join to route each prefix to its least-offering extender.
 * `prefixes` carries prefix||count||index records (count and index are u64
 * values following the prefix); `arr` is a key -> value arrangement; `key`
 * selects the key from the prefix part.  The output type equals the input
 * type.
 */
DTFL_API dtfl_collection *dtfl_dogs_count(dtfl_collection *prefixes,
    dtfl_arranged *arr, const dtfl_type *prefix_type,
    dtfl_key_selector_fn key, dtfl_ctx ctx, uint64_t index);

/* ------------------------------------------------------------------ */
/* half_join (operators/half_join.rs)                                  */
/* ------------------------------------------------------------------ */

/*
 * A half_join output closure: for a matched (key, val1, val2) it writes the
 * output record bytes to `out_rec` (up to `cap`, storing the length through
 * `out_len`).  The operator pairs the record with the joined time (the
 * lattice join of the update's carried time1 and the matched trace time2)
 * and the product of the two diffs, and ships it at the update's initial
 * time.  `key`, `val1` and `val2` are the key, streamed value and matched
 * value byte slices.
 */
typedef void (*dtfl_half_join_output_fn)(void *ctx, const void *key,
    size_t klen, const void *val1, size_t v1len, const void *val2,
    size_t v2len, void *out_rec, size_t cap, size_t *out_len);

/*
 * half_join (operators/half_join.rs): a binary equijoin that responds only
 * to updates on its first input, used to build delta joins over partially
 * ordered times.  Each streamed update is a record key||val1||time1 carried
 * at an initial time less or equal to time1; for every (key, val2) in `arr`
 * at a trace time2 that `comparison` accepts against the initial time, it
 * emits output_func(key, val1, val2) at the joined time (time1 join time2),
 * carried at the initial time, with the product of the diffs.  The output
 * record is out_record||joined_time of a type whose record part is
 * `out_type` (the operator appends the joined time words).
 *
 * `strict` selects the total-order comparison: true compares time2 strictly
 * less than the initial time (ignoring concurrent updates), false compares
 * less or equal (seeing concurrent updates).  Under an AltNeu scope these
 * are the two sides that make a pair of concurrent updates interact exactly
 * once.  `kt` and `v1t` split the streamed record into its key and value;
 * the carried time1 is the scope's time words that follow them.  The caller
 * still owns `arr`.
 */
DTFL_API dtfl_collection *dtfl_dogs_half_join(dtfl_collection *stream,
    dtfl_arranged *arr, const dtfl_type *kt, const dtfl_type *v1t,
    const dtfl_type *out_type, bool strict,
    dtfl_half_join_output_fn output_func, dtfl_ctx ctx);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_DOGS_H */
