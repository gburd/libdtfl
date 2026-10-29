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
 * batches are read.  The batch transforms below apply the same maps
 * eagerly to one batch, returning a fresh owned __dtfl_batch_ord; the
 * enter operator uses them on the stream.  The shared trace stays lazy:
 * an entered arrangement's agent carries a __dtfl_trace_enter chain that
 * maps frontiers and the times its cursor lists read.
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

/*
 * enter_at.rs `prior`: maps an inner time back to an outer time for the
 * compaction frontiers an entered trace communicates to the trace it wraps.
 */
typedef void (*__dtfl_enter_prior_fn)(void *ctx, const uint64_t *inner_time,
    uint64_t *outer_time);

/*
 * TraceEnter (enter.rs) and TraceEnter in enter_at.rs: one wrapper layer
 * presenting a trace whose times are in `outer` as a trace in `inner`.
 * Upstream nests the wrapper types (TraceEnter<TraceEnter<Tr, T1>, T2>);
 * here `next` is the layer the outer trace is itself read through, NULL
 * when it is the shared spine, so a chain maps spine times outward-in.
 * Immutable once built and reference counted: the agents that carry it,
 * their clones, the cursor lists they hand out and the enter operator all
 * share it.  Worker-local, like the agent.  It owns ctx (ctx_free on the
 * last release) and references to next and both classes.
 *
 * logic == NULL is enter.rs: update times map with to_inner and
 * compaction frontiers with to_outer.  Otherwise enter_at.rs: update times
 * come from logic(key, val, time) and compaction frontiers map with prior.
 * In both, batch descriptions and the get_*_compaction views map with
 * to_inner, and batches_through maps its upper with to_outer.
 */
typedef struct __dtfl_trace_enter {
	__dtfl_arc ref;
	struct __dtfl_trace_enter *next;	/* retained; NULL at the spine */
	const dtfl_time_class *outer;		/* retained */
	const dtfl_time_class *inner;		/* retained */
	__dtfl_enter_at_fn logic;		/* NULL: to_inner */
	__dtfl_enter_prior_fn prior;		/* used when logic != NULL */
	void *ctx;
	void (*ctx_free)(void *);
} __dtfl_trace_enter;

/* Takes ownership of the caller's reference on `next` and of ctx. */
__dtfl_trace_enter *__dtfl_trace_enter_new(__dtfl_trace_enter *next,
    const dtfl_time_class *outer, const dtfl_time_class *inner,
    __dtfl_enter_at_fn logic, __dtfl_enter_prior_fn prior, void *ctx,
    void (*ctx_free)(void *));
__dtfl_trace_enter *__dtfl_trace_enter_retain(__dtfl_trace_enter *e);
void __dtfl_trace_enter_release(__dtfl_trace_enter *e);

/*
 * BatchCursorEnter::map_times through the chain: the spine time `t` of
 * the update at (key, val) as a time of e->inner, written to `out`.
 */
void __dtfl_trace_enter_time(const __dtfl_trace_enter *e, const void *key,
    size_t klen, const void *val, size_t vlen, const uint64_t *t,
    uint64_t *out);
/* A spine-class antichain lifted to e->inner with to_inner; inits *out. */
void __dtfl_trace_enter_frontier_in(const __dtfl_trace_enter *e,
    const __dtfl_antichain *spine, __dtfl_antichain *out);
/*
 * n times of e->inner (flat, depth words each) mapped to the spine class;
 * inits *out.  compaction selects prior over to_outer on enter_at layers
 * (set_*_compaction); batches_through passes false.
 */
void __dtfl_trace_enter_frontier_out(const __dtfl_trace_enter *e,
    const uint64_t *f, size_t n, bool compaction, __dtfl_antichain *out);
/*
 * BatchEnter::make_from for one layer, eagerly: `src` is a batch of
 * e->outer; the result is a new owned batch of e->inner.
 */
__dtfl_batch_ord *__dtfl_trace_enter_batch(const __dtfl_trace_enter *e,
    const __dtfl_batch_ord *src);
/*
 * Replace a cursor list's spine batches by entered copies through its
 * chain and drop the chain, for readers that walk cl->batches with their
 * own cursors (the int_proxy tactics) instead of through map_times.  Call
 * it before the list is navigated.  A list with no chain is unchanged.
 */
void __dtfl_cursor_list_materialize(__dtfl_cursor_list *cl);

#endif /* DTFL_WRAP_INT_H */
