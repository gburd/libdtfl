/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/ddir/backend_int.h: the DDIR vec backend internals.
 *
 * The scope-tree walk renders each item into a dtfl_collection; the leaf
 * operators (linear/join/reduce/concat/inspect) are built from the
 * collection C API with dd_eval as the scalar engine.  A collection
 * element is a (key, val) pair of serialized Values (dd_pair_type).
 */
#ifndef DTFL_DDIR_BACKEND_INT_H
#define DTFL_DDIR_BACKEND_INT_H

#include "ddir_int.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/reduce.h"
#include "dtfl/timely.h"

/* Render the program's root scope into `scope`, binding `inputs` (one per
 * positional input) and returning one output collection per export (in
 * order) through `exports` (caller frees each).  Returns 0 or a DTFL_E*. */
int dd_render_program(const dtfl_ddir_program *p, dtfl_scope *scope,
    dtfl_collection *const *inputs, size_t n_inputs, dtfl_collection **exports);

/* Accumulated output rows for one export: consolidated net multiplicity. */
typedef struct out_acc {
	dd_value **keys; /* parallel arrays, owned */
	dd_value **vals;
	int64_t *diffs;
	size_t n, cap;
} out_acc;

/* Row for an input to dd_evaluate: a (key, val) pair with multiplicity +1. */
typedef struct dtfl_ddir_row dtfl_ddir_row;

/* Evaluate `program` on explicit inputs on a single in-process worker.
 * `accs` is a caller-provided array (one zeroed out_acc per export) filled
 * with each export's consolidated final contents.  Returns 0 or a
 * DTFL_E* code. */
int dd_evaluate(const dtfl_ddir_program *program,
    const dtfl_ddir_row *const *inputs, const size_t *input_lens,
    size_t n_inputs, out_acc *accs);

/* Render a Linear chain `ops` over `in` at scope depth `level`, as the single
 * flat_map of backend/vec.rs render_linear: project/filter/negate rewrite the
 * records, enter_at delays the PointStamp coordinate at `level`, and lift_iter
 * folds that coordinate into the value.  Returns a new collection; frees `in`
 * is the caller's job. */
dtfl_collection *dd_render_linear(dtfl_collection *in, const dd_vec *ops,
    size_t level);
void join_fn(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out);
void reduce_min_fn(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out);
void reduce_distinct_fn(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out);
void reduce_count_fn(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out);
void reduce_collect_fn(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out);

#endif /* DTFL_DDIR_BACKEND_INT_H */
