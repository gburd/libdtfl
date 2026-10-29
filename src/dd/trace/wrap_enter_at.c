/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/wrappers/enter_at.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/wrap_enter_at.c: the enter-with-initial-time batch transform.
 *
 * Upstream TraceEnter / BatchEnter in enter_at.rs are like enter.rs, except
 * each wrapped update is presented with a timestamp from a user `logic(key,
 * val, time)` rather than from to_inner (enter_at.rs BatchCursorEnter).  The
 * batch description is still lifted with to_inner.  libdtfl applies the logic
 * eagerly over the flat container, producing a batch over the inner class.
 * The trace-level TraceEnter of enter_at.rs, which maps compaction
 * frontiers back through the caller-supplied `prior` inverse, shares the
 * enter.rs wrapper in wrap_enter.c (__dtfl_trace_enter with a logic).
 */
#include "dd/wrap_int.h"
#include "progress/time_int.h"

typedef struct enter_at_ctx {
	__dtfl_builder_ord *bld;
	const void *key;
	size_t klen;
	const void *val;
	size_t vlen;
	__dtfl_enter_at_fn logic;
	void *ctx;
} enter_at_ctx;

static void
enter_at_emit(const uint64_t *time, const void *diff, void *vctx)
{
	enter_at_ctx *c = vctx;
	uint64_t inner[DTFL_TIME_MAX];

	c->logic(c->ctx, c->key, c->klen, c->val, c->vlen, time, inner);
	__dtfl_builder_ord_push(c->bld, c->key, c->klen, c->val, c->vlen, inner,
	    diff);
}

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
__dtfl_batch_enter_at(const __dtfl_batch_ord *src, const dtfl_time_class *inner,
    __dtfl_enter_at_fn logic, void *ctx)
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
			enter_at_ctx c = { &bld, key, klen, val, vlen, logic,
			    ctx };

			__dtfl_cursor_map_times(&cur, src, enter_at_emit, &c);
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
