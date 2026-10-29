/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/wrappers/enter.rs
 *   differential-dataflow/src/trace/wrappers/enter_at.rs (TraceEnter)
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/wrap_enter.c: TraceEnter and BatchEnter, presenting a trace
 * in a nested scope.
 *
 * Upstream TraceEnter / BatchEnter extend every timestamp with the inner
 * scope's coordinates set to their default (enter.rs BatchCursorEnter::
 * map_times calls TInner::to_inner), and map compaction frontiers back with
 * to_outer so holds reach the shared parent trace.  enter_at.rs is the same
 * wrapper with the update times from a user logic(key, val, time) and the
 * compaction frontiers mapped with a user `prior`.
 *
 * Two halves.  __dtfl_batch_enter is BatchEnter::make_from applied eagerly
 * to one batch, for the batches on an entered arrangement's stream (the
 * flat container has no lazy cursor wrapper, PLAN D2).  __dtfl_trace_enter
 * is TraceEnter: the entered arrangement's agent carries it, and the agent
 * maps frontiers through it and hands out cursor lists that lift each time
 * as map_times reads it, so the shared trace's batches are never copied.
 */
#include "dd/wrap_int.h"
#include "progress/time_int.h"

typedef struct enter_ctx {
	__dtfl_builder_ord *bld;
	const void *key;
	size_t klen;
	const void *val;
	size_t vlen;
	const dtfl_time_class *outer;
	const dtfl_time_class *inner;
} enter_ctx;

static void
enter_emit(const uint64_t *time, const void *diff, void *vctx)
{
	enter_ctx *c = vctx;
	uint64_t inner[DTFL_TIME_MAX];

	__dtfl_time_to_inner(c->inner, c->outer, time, inner);
	__dtfl_builder_ord_push(c->bld, c->key, c->klen, c->val, c->vlen, inner,
	    diff);
}

/* Lifts an antichain of outer times to inner times with to_inner. */
static void
enter_antichain(const __dtfl_antichain *src, const dtfl_time_class *outer,
    const dtfl_time_class *inner, __dtfl_antichain *out)
{
	uint64_t t[DTFL_TIME_MAX];
	size_t i;

	__dtfl_antichain_init(out, inner);
	for (i = 0; i < __dtfl_antichain_len(src); i++) {
		__dtfl_time_to_inner(inner, outer, __dtfl_antichain_at(src, i),
		    t);
		__dtfl_antichain_insert(out, t);
	}
}

__dtfl_batch_ord *
__dtfl_batch_enter(const __dtfl_batch_ord *src, const dtfl_time_class *inner)
{
	__dtfl_cursor cur;
	__dtfl_builder_ord bld;
	__dtfl_description desc;
	__dtfl_antichain lower, upper, since;
	const dtfl_time_class *outer = src->tc;

	__dtfl_builder_ord_init(&bld, src->storage.keyed, src->kt, src->vt,
	    inner, src->dc, 0, 0, 0);
	__dtfl_cursor_init(&cur, src);
	while (__dtfl_cursor_key_valid(&cur, src)) {
		size_t klen;
		const void *key = __dtfl_cursor_key(&cur, src, &klen);

		while (__dtfl_cursor_val_valid(&cur, src)) {
			size_t vlen;
			const void *val = __dtfl_cursor_val(&cur, src, &vlen);
			enter_ctx c = { &bld, key, klen, val, vlen, outer,
			    inner };

			__dtfl_cursor_map_times(&cur, src, enter_emit, &c);
			__dtfl_cursor_step_val(&cur, src);
		}
		__dtfl_cursor_step_key(&cur, src);
	}

	enter_antichain(__dtfl_batch_ord_lower(src), outer, inner, &lower);
	enter_antichain(__dtfl_batch_ord_upper(src), outer, inner, &upper);
	enter_antichain(__dtfl_description_since(&src->description), outer,
	    inner, &since);
	__dtfl_description_init(&desc, &lower, &upper, &since);
	return __dtfl_builder_ord_done(&bld, &desc);
}

/* ------------------------------------------------------------------ */
/* TraceEnter (enter.rs, enter_at.rs): the trace-level wrapper.        */
/* ------------------------------------------------------------------ */

__dtfl_trace_enter *
__dtfl_trace_enter_new(__dtfl_trace_enter *next, const dtfl_time_class *outer,
    const dtfl_time_class *inner, __dtfl_enter_at_fn logic,
    __dtfl_enter_prior_fn prior, void *ctx, void (*ctx_free)(void *))
{
	__dtfl_trace_enter *e = __dtfl_alloc(sizeof *e);

	DTFL_ASSERT(logic == NULL || prior != NULL);
	__dtfl_arc_init(&e->ref);
	e->next = next;
	e->outer = dtfl_time_class_retain(outer);
	e->inner = dtfl_time_class_retain(inner);
	e->logic = logic;
	e->prior = prior;
	e->ctx = ctx;
	e->ctx_free = ctx_free;
	return e;
}

__dtfl_trace_enter *
__dtfl_trace_enter_retain(__dtfl_trace_enter *e)
{
	if (e != NULL)
		__dtfl_arc_retain(&e->ref);
	return e;
}

void
__dtfl_trace_enter_release(__dtfl_trace_enter *e)
{
	if (e == NULL || !__dtfl_arc_release(&e->ref))
		return;
	__dtfl_trace_enter_release(e->next);
	if (e->ctx_free != NULL)
		e->ctx_free(e->ctx);
	dtfl_time_class_free(e->outer);
	dtfl_time_class_free(e->inner);
	__dtfl_free(e);
}

/*
 * BatchCursorEnter::map_times: enter.rs presents TInner::to_inner(time),
 * enter_at.rs presents logic(key, val, time).  The wrapped trace's time
 * comes first from the layers below.
 */
void
__dtfl_trace_enter_time(const __dtfl_trace_enter *e, const void *key,
    size_t klen, const void *val, size_t vlen, const uint64_t *t,
    uint64_t *out)
{
	uint64_t mid[DTFL_TIME_MAX];

	if (e->next != NULL) {
		__dtfl_trace_enter_time(e->next, key, klen, val, vlen, t, mid);
		t = mid;
	}
	if (e->logic != NULL)
		e->logic(e->ctx, key, klen, val, vlen, t, out);
	else
		__dtfl_time_to_inner(e->inner, e->outer, t, out);
}

/* get_logical_compaction / BatchEnter::make_from: map with to_inner. */
void
__dtfl_trace_enter_frontier_in(const __dtfl_trace_enter *e,
    const __dtfl_antichain *spine, __dtfl_antichain *out)
{
	__dtfl_antichain mid;

	if (e->next == NULL) {
		enter_antichain(spine, e->outer, e->inner, out);
		return;
	}
	__dtfl_trace_enter_frontier_in(e->next, spine, &mid);
	enter_antichain(&mid, e->outer, e->inner, out);
	__dtfl_antichain_free(&mid);
}

/*
 * set_*_compaction and batches_through: stash1 collects to_outer(time)
 * (prior(time) for an enter_at layer's compaction), which the wrapped
 * trace maps further.
 */
void
__dtfl_trace_enter_frontier_out(const __dtfl_trace_enter *e,
    const uint64_t *f, size_t n, bool compaction, __dtfl_antichain *out)
{
	__dtfl_antichain stash1;
	uint64_t t[DTFL_TIME_MAX];
	size_t i;

	__dtfl_antichain_init(&stash1, e->outer);
	for (i = 0; i < n; i++) {
		const uint64_t *time = f + (size_t)e->inner->depth * i;

		if (compaction && e->logic != NULL)
			e->prior(e->ctx, time, t);
		else
			__dtfl_time_to_outer(e->inner, e->outer, time, t);
		__dtfl_antichain_insert(&stash1, t);
	}
	if (e->next == NULL) {
		*out = stash1;
		return;
	}
	__dtfl_trace_enter_frontier_out(e->next, __dtfl_ac_base(&stash1),
	    __dtfl_antichain_len(&stash1), compaction, out);
	__dtfl_antichain_free(&stash1);
}

__dtfl_batch_ord *
__dtfl_trace_enter_batch(const __dtfl_trace_enter *e,
    const __dtfl_batch_ord *src)
{
	if (e->logic != NULL)
		return __dtfl_batch_enter_at(src, e->inner, e->logic, e->ctx);
	return __dtfl_batch_enter(src, e->inner);
}

/* A spine batch through every layer, innermost first. */
static __dtfl_batch_ord *
enter_chain_batch(const __dtfl_trace_enter *e, const __dtfl_batch_ord *src)
{
	__dtfl_batch_ord *mid, *out;

	if (e->next == NULL)
		return __dtfl_trace_enter_batch(e, src);
	mid = enter_chain_batch(e->next, src);
	out = __dtfl_trace_enter_batch(e, mid);
	__dtfl_batch_ord_release(mid);
	return out;
}

void
__dtfl_cursor_list_materialize(__dtfl_cursor_list *cl)
{
	__dtfl_trace_enter *e = cl->enter;
	__dtfl_batch_ord **batches;
	const dtfl_type *kt = cl->kt, *vt = cl->vt;
	size_t i, n = cl->n;

	if (e == NULL)
		return;
	batches = __dtfl_alloc(DTFL_MAX(n, 1u) * sizeof *batches);
	for (i = 0; i < n; i++)
		batches[i] = enter_chain_batch(e, cl->batches[i]);
	__dtfl_cursor_list_free(cl);	/* releases e's list reference */
	__dtfl_cursor_list_init(cl, batches, n, kt, vt);
	__dtfl_free(batches);
}
