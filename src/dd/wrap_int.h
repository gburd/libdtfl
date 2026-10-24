/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/dd/wrap_int.h: library-internal trace-wrapper batch transforms.
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1:
 *   differential-dataflow/src/trace/wrappers/mod.rs
 *   differential-dataflow/src/trace/wrappers/frontier.rs
 *   differential-dataflow/src/trace/wrappers/enter.rs
 *   differential-dataflow/src/trace/wrappers/enter_at.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * Upstream wraps a trace lazily, mapping update times inside the cursor as
 * batches are read; libdtfl's one flat container (PLAN D2) applies the same
 * maps eagerly, producing a transformed batch.  Each transform takes a
 * reference on `src` and returns a fresh owned __dtfl_batch_ord.  The
 * arrange operator's enter/leave and import paths use these to carry an
 * arrangement across a scope boundary or to restrict it to [since, until).
 */
#ifndef DTFL_WRAP_INT_H
#define DTFL_WRAP_INT_H

#include "dd/trace/trace_int.h"

/*
 * BatchFrontier (frontier.rs): advance each update time to `since`, then
 * drop updates whose advanced time is in advance of `until`.  since/until
 * are flat runs of src's time words (ns / nu elements; nu == 0 is the empty
 * frontier, end of time, which suppresses nothing).
 */
__dtfl_batch_ord *__dtfl_batch_frontier(const __dtfl_batch_ord *src,
    const uint64_t *since, size_t ns, const uint64_t *until, size_t nu);

/*
 * BatchEnter (enter.rs): map every update time and the batch's description
 * from the outer class to the inner class with to_inner (Refines).  The
 * result is a batch over `inner`.
 */
__dtfl_batch_ord *__dtfl_batch_enter(const __dtfl_batch_ord *src,
    const dtfl_time_class *inner);

/*
 * BatchEnterAt (enter_at.rs): like enter, but each update's inner time comes
 * from `logic(key, val, time)` instead of to_inner.  `logic` receives the
 * key and value bytes and the outer time words and writes the inner time.
 */
typedef void (*__dtfl_enter_at_fn)(void *ctx, const void *key, size_t klen,
    const void *val, size_t vlen, const uint64_t *outer_time,
    uint64_t *inner_time);
__dtfl_batch_ord *__dtfl_batch_enter_at(const __dtfl_batch_ord *src,
    const dtfl_time_class *inner, __dtfl_enter_at_fn logic, void *ctx);

#endif /* DTFL_WRAP_INT_H */
