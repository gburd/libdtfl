/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/iterate.h: the iterate operator and the Variable feedback.
 *
 * iterate applies a differential dataflow fragment repeatedly until the
 * collection it produces stops changing (a fixed point), or until the
 * loop coordinate's range is exhausted.  It builds an iterative subscope
 * whose time is dtfl_time_product(outer, u64): one extra u64 coordinate
 * counts the iteration.  The source collection is entered into the
 * subscope, a Variable is seeded from it, the user logic transforms the
 * Variable's collection into the next iterate, the Variable is set to
 * that result (which feeds back with the loop coordinate advanced by one
 * and the source negated so only the delta circulates), and the result
 * leaves the subscope.
 *
 * A dtfl_variable is a recursively defined collection: create one from an
 * initial collection or empty, use its collection to define a result, and
 * dtfl_variable_set binds the Variable to that result.  set consumes the
 * Variable.  Variables let you write mutual recursion that iterate alone
 * cannot: two or more collections that evolve together in one subscope.
 *
 * iterate does not insert consolidate for you; a logic that can produce
 * cancelling differences must consolidate (or end in reduce, count or
 * distinct) or the loop may circulate forever.  The diff class of the
 * collection iterate is called on must be Abelian (it negates the
 * source); dtfl_variable_new needs only a Semigroup, dtfl_variable_new_from
 * needs an Abelian class (it negates its source), as upstream does.
 *
 * Ownership: dtfl_collection_iterate returns a new collection in the
 * outer scope that the caller frees with dtfl_collection_free.  A
 * dtfl_variable is freed by dtfl_variable_set (which consumes it); the
 * collection a Variable exposes is a handle the caller frees.
 */
#ifndef DTFL_ITERATE_H
#define DTFL_ITERATE_H

#include "dtfl/base.h"
#include "dtfl/collection.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtfl_variable dtfl_variable;

/*
 * The iterate logic.  Called once with the iterative subscope and the
 * collection of the Variable seeded from the entered source.  It must
 * return a new collection in that subscope (the next iterate), built from
 * the Variable's collection with the collection methods.  The returned
 * collection is set as the Variable and then leaves the subscope.  The
 * caller does not free the returned collection; iterate consumes it.
 */
typedef dtfl_collection *(*dtfl_iterate_fn)(void *ctx, dtfl_scope *inner,
    dtfl_collection *var);

/*
 * iterate: apply `logic` to `c` until a fixed point (Collection::iterate).
 * `c`'s diff class must be Abelian.  Returns a new collection in `c`'s
 * scope the caller frees with dtfl_collection_free.
 */
DTFL_API dtfl_collection *dtfl_collection_iterate(dtfl_collection *c,
    dtfl_iterate_fn logic, dtfl_ctx ctx);

/*
 * Variable::new: a new empty Variable in `scope` (an iterative subscope)
 * and its collection, which starts empty and is updated each iteration by
 * the logic the Variable is later set to.  The step summary advances the
 * loop coordinate by one each iteration; this is the Variable differential
 * dataflow builds for a subscope.  `type` and `diff` describe the records;
 * the diff class need only be a Semigroup.  Stores the collection through
 * `*collection` (a handle the caller frees) and returns the Variable.
 */
DTFL_API dtfl_variable *dtfl_variable_new(dtfl_scope *scope,
    const dtfl_type *type, const dtfl_diff *diff, dtfl_collection **collection);

/*
 * Variable::new_from: a new Variable seeded from `source`, and its
 * collection, which is `source` concatenated with the feedback.  The
 * Variable retains the negated source, so set binds `x = logic(x + source)
 * - source`, a fixed point with `x + source = logic(x + source)`.
 * `source`'s diff class must be Abelian (it is negated).  The caller still
 * owns `source`.  Stores the collection through `*collection` and returns
 * the Variable.
 */
DTFL_API dtfl_variable *dtfl_variable_new_from(dtfl_collection *source,
    dtfl_collection **collection);

/*
 * Variable::set: bind the Variable to `result`, feeding it back with the
 * loop coordinate advanced and any negated source concatenated.  Consumes
 * the Variable (it is freed) and `result` (the caller does not free it).
 */
DTFL_API void dtfl_variable_set(dtfl_variable *v, dtfl_collection *result);

/*
 * Variable::set with an explicit concat: bind the Variable to the
 * concatenation of `result` and `addend`, then feed back.  A convenience
 * for the common pattern of concatenating one more collection into the
 * result before closing the loop.  Consumes the Variable, `result` and
 * `addend`.
 */
DTFL_API void dtfl_variable_set_concat(dtfl_variable *v,
    dtfl_collection *result, dtfl_collection *addend);

/* ------------------------------------------------------------------ */
/* Dynamic scopes (dynamic/mod.rs, dynamic/pointstamp.rs)              */
/* ------------------------------------------------------------------ */

/*
 * enter_dynamic / leave_dynamic: move a collection in and out of a
 * dynamically created iterative scope whose inner time is a PointStamp of
 * `level` coordinates (dynamic/mod.rs).  The scope's time class must be a
 * PointStamp or a Product whose inner class is a PointStamp
 * (dtfl_time_pointstamp).  enter_dynamic is the identity (the stream is
 * already in the scope).  leave_dynamic strips all but the first
 * `level - 1` PointStamp coordinates of every update's time and of its
 * capability, which lets an update leave one dynamic iteration level.
 * Each returns a new collection the caller frees with
 * dtfl_collection_free.
 */
DTFL_API dtfl_collection *dtfl_collection_enter_dynamic(dtfl_collection *c,
    size_t level);
DTFL_API dtfl_collection *dtfl_collection_leave_dynamic(dtfl_collection *c,
    size_t level);

/*
 * feedback_summary: build the path summary a dynamic feedback operator
 * uses at PointStamp coordinate `level` (dynamic/mod.rs feedback_summary).
 * `tc` is the scope's PointStamp (or Product-with-PointStamp) time class;
 * `step` is the summary applied at that coordinate (a single word, the
 * increment for the inner coordinate kind).  The result is written into
 * `out` (dtfl_summary_depth(tc) words): retain None, identity actions for
 * the first level - 1 coordinates, then `step` at coordinate level - 1.
 * `level` must be between 1 and the PointStamp's maximum length.
 */
DTFL_API void dtfl_feedback_summary(const dtfl_time_class *tc, size_t level,
    uint64_t step, uint64_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_ITERATE_H */
