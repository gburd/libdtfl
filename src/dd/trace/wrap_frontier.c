/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/wrappers/frontier.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/wrap_frontier.c: the frontiered-batch transform.
 *
 * Upstream TraceFrontier / BatchFrontier wrap a trace so every exposed time
 * is first advanced by `since` and then suppressed if it is in advance of
 * `until`, presenting a deterministic view on [since, until).  Upstream does
 * this lazily in the cursor's map_times (frontier.rs BatchCursorFrontier);
 * libdtfl's one container is a flat byte arena, so the transform is applied
 * eagerly, producing a new batch whose update times are advanced to `since`
 * and whose updates at or beyond `until` are dropped.  An empty `until`
 * (end of time) performs only the advance.  The result is reconsolidated by
 * the builder, since advancing can collapse distinct times onto one.
 */
#include "dd/trace/trace_int.h"
#include "dd/wrap_int.h"

/* Per-value state while re-emitting advanced, suppressed update times. */
typedef struct frontier_ctx {
	__dtfl_builder_ord *bld;
	const void *key;
	size_t klen;
	const void *val;
	size_t vlen;
	const uint64_t *since;
	size_t ns;
	const uint64_t *until;
	size_t nu;
	const dtfl_time_class *tc;
	unsigned depth;
} frontier_ctx;

/*
 * map_times logic: advance the time to `since`, then push it unless `until`
 * is less_equal the advanced time (frontier.rs map_times).
 */
static void
frontier_emit(const uint64_t *time, const void *diff, void *vctx)
{
	frontier_ctx *c = vctx;
	uint64_t tmp[DTFL_TIME_MAX];

	__dtfl_time_copy(c->tc, tmp, time);
	__dtfl_time_advance_by(c->tc, tmp, c->since, c->ns);
	if (c->nu != 0 && __dtfl_frontier_less_equal(c->tc, c->until, c->nu, tmp))
		return;
	__dtfl_builder_ord_push(c->bld, c->key, c->klen, c->val, c->vlen, tmp,
	    diff);
}

__dtfl_batch_ord *
__dtfl_batch_frontier(const __dtfl_batch_ord *src, const uint64_t *since,
    size_t ns, const uint64_t *until, size_t nu)
{
	__dtfl_cursor cur;
	__dtfl_builder_ord bld;
	__dtfl_description desc;
	__dtfl_antichain lower, upper, since_ac;
	const dtfl_time_class *tc = src->tc;
	unsigned depth = tc->depth;

	__dtfl_builder_ord_init(&bld, src->storage.keyed, src->kt, src->vt, tc,
	    src->dc, 0, 0, 0);
	__dtfl_cursor_init(&cur, src);
	while (__dtfl_cursor_key_valid(&cur, src)) {
		size_t klen;
		const void *key = __dtfl_cursor_key(&cur, src, &klen);

		while (__dtfl_cursor_val_valid(&cur, src)) {
			size_t vlen;
			const void *val = __dtfl_cursor_val(&cur, src, &vlen);
			frontier_ctx c = { &bld, key, klen, val, vlen, since,
			    ns, until, nu, tc, depth };

			__dtfl_cursor_map_times(&cur, src, frontier_emit, &c);
			__dtfl_cursor_step_val(&cur, src);
		}
		__dtfl_cursor_step_key(&cur, src);
	}

	/* Description: lower/upper unchanged; since joined with the batch's. */
	__dtfl_antichain_init(&lower, tc);
	__dtfl_antichain_copy(&lower, __dtfl_batch_ord_lower(src));
	__dtfl_antichain_init(&upper, tc);
	__dtfl_antichain_copy(&upper, __dtfl_batch_ord_upper(src));
	__dtfl_antichain_init(&since_ac, tc);
	{
		const __dtfl_antichain *bs =
		    __dtfl_description_since(&src->description);
		size_t i;

		for (i = 0; i < __dtfl_antichain_len(bs); i++)
			__dtfl_antichain_insert(&since_ac,
			    __dtfl_antichain_at(bs, i));
		for (i = 0; i < ns; i++)
			__dtfl_antichain_insert(&since_ac,
			    since + (size_t)depth * i);
	}
	__dtfl_description_init(&desc, &lower, &upper, &since_ac);
	return __dtfl_builder_ord_done(&bld, &desc);
}
