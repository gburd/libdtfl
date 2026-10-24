/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/wrappers/enter.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/wrap_enter.c: the enter-a-scope batch transform.
 *
 * Upstream TraceEnter / BatchEnter present a trace in a nested scope by
 * extending every timestamp with the inner scope's coordinates set to their
 * default (enter.rs BatchCursorEnter::map_times calls TInner::to_inner).
 * The compaction frontiers map back with to_outer so holds reach the shared
 * parent trace.  libdtfl's flat container applies to_inner eagerly, so this
 * builds a batch over the inner class whose update times and description are
 * the outer times lifted with __dtfl_time_to_inner (PLAN D3 Refines).
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
