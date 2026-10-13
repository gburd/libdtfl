/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dogs3-v0.25.1
 *   dogsdogsdogs/src/operators/lookup_map.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dogs/lookup_map.c: the lookup_map operator, the core of propose,
 * validate and count.
 *
 * For each prefix in the first input, lookup_map selects a key, seeks it in
 * the arranged trace of the second input, and for every value under that
 * key accumulates the matched diffs at trace times less or equal to the
 * prefix's own time.  When the accumulation is non-zero it applies the
 * output function and ships (output record, prefix time, output diff).
 *
 * The time comparison is the lattice less_equal of the scope's time class.
 * In a delta query that class is AltNeu, whose less_equal is the total
 * order on the encoded 2t + neu word, so a prefix at an alt time sees only
 * the prior state of a neu-delayed relation and a prefix at a neu time sees
 * that relation's current state as well.  This is the condition that makes
 * each pair of concurrent updates in a delta query interact exactly once.
 *
 * Prefixes arrive on a per-key exchange so matching keys reach the worker
 * holding the trace.  Requests are stashed per capability and processed
 * once the trace frontier has passed the capability's time, i.e. the trace
 * holds every update at times the prefix might match.  A fresh cursor is
 * cut for each capability processed, as upstream re-borrows trace.cursor().
 */
#include "dogs/dogs_int.h"
#include "dd/common_int.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

#include <string.h>

/* A stashed prefix update: record bytes, embedded time, prefix diff. */
typedef struct lm_prefix {
	unsigned char *rec;	/* owned: record bytes */
	size_t rlen;
	uint64_t time[DTFL_TIME_MAX];
	unsigned char *diff;	/* owned: prefix diff bytes (dw), or NULL */
} lm_prefix;

/* A capability and the prefixes stashed under it. */
typedef struct lm_stash {
	dtfl_cap *cap;		/* retained */
	__dtfl_vec prefixes;	/* lm_prefix, FIFO */
} lm_stash;

typedef struct lookup_ctx {
	__dtfl_trace_agent reader;	/* cloned reader of the arrangement */
	const dtfl_time_class *tc;
	unsigned depth;
	const __dtfl_diff_class *dc;	/* prefix diff class */
	size_t dw;
	const __dtfl_diff_class *mdc;	/* matched (trace) diff class */
	size_t mdw;
	const dtfl_type *out_type;	/* retained */
	const __dtfl_diff_class *odc;	/* output diff class */
	size_t odw;
	dtfl_key_selector_fn key;
	void *key_ctx;
	void (*key_free)(void *);
	__dtfl_lookup_output_fn output_func;
	void *out_ctx;
	void (*out_free)(void *);
	__dtfl_vec stash;		/* lm_stash */
} lookup_ctx;

/* The per-key exchange route: hash of the selected key. */
typedef struct lm_route {
	dtfl_key_selector_fn key;
	void *ctx;
	const dtfl_type *kt;		/* trace key type, for the hash */
	unsigned depth;
	size_t dw;
} lm_route;

static uint64_t
lm_route_fn(const void *rec, size_t len, void *vctx)
{
	lm_route *r = vctx;
	size_t rlen = __dtfl_update_rec_len(r->depth, r->dw, len);
	unsigned char key[DTFL_DOGS_KEY_MAX];
	size_t klen = 0;

	r->key(r->ctx, rec, rlen, key, sizeof key, &klen);
	return dtfl_type_hash(r->kt, key, klen);
}

/* Finds the stash slot for `t`, or SIZE_MAX. */
static size_t
find_stash(lookup_ctx *c, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < c->stash.len; i++) {
		lm_stash *s = DTFL_VEC_AT(&c->stash, lm_stash, i);

		if (__dtfl_time_eq(c->tc, dtfl_cap_time(s->cap), t))
			return i;
	}
	return SIZE_MAX;
}

/* A matched-diff accumulator the map_times callback drives. */
typedef struct lm_acc {
	lookup_ctx *c;
	const uint64_t *time;	/* the prefix time (match bound) */
	unsigned char count[DTFL_COMMON_DIFF_MAX];
} lm_acc;

/*
 * Sum the matched diff into count for each trace time less or equal to the
 * prefix time, under the scope's lattice order (AltNeu's total order).
 */
static void
lm_map_times(const uint64_t *t, const void *d, void *vctx)
{
	lm_acc *a = vctx;

	if (dtfl_time_less_equal(a->c->tc, t, a->time))
		a->c->mdc->plus_equals(a->c->mdc, a->count, d);
}

/* The comparison that sorts prefixes by their selected key. */
static int
lm_prefix_cmp(lookup_ctx *c, const dtfl_type *kt, const lm_prefix *a,
    const lm_prefix *b)
{
	unsigned char ka[DTFL_DOGS_KEY_MAX], kb[DTFL_DOGS_KEY_MAX];
	size_t kal = 0, kbl = 0;

	c->key(c->key_ctx, a->rec, a->rlen, ka, sizeof ka, &kal);
	c->key(c->key_ctx, b->rec, b->rlen, kb, sizeof kb, &kbl);
	return dtfl_type_cmp(kt, ka, kal, kb, kbl);
}

/* Insertion sort of the prefix vector by selected key (small per key). */
static void
lm_sort_prefixes(lookup_ctx *c, const dtfl_type *kt, __dtfl_vec *v)
{
	size_t i, j;

	for (i = 1; i < v->len; i++) {
		lm_prefix key = *DTFL_VEC_AT(v, lm_prefix, i);

		j = i;
		while (j > 0 && lm_prefix_cmp(c, kt,
		    DTFL_VEC_AT(v, lm_prefix, j - 1), &key) > 0) {
			*DTFL_VEC_AT(v, lm_prefix, j) =
			    *DTFL_VEC_AT(v, lm_prefix, j - 1);
			j--;
		}
		*DTFL_VEC_AT(v, lm_prefix, j) = key;
	}
}

static void
lm_prefix_free(lm_prefix *p)
{
	if (p->rec != NULL)
		__dtfl_free(p->rec);
	if (p->diff != NULL)
		__dtfl_free(p->diff);
}

/* Process one stash slot once its time is complete in the trace. */
static void
lm_process(dtfl_op *op, lookup_ctx *c, lm_stash *s)
{
	dtfl_session *sess = dtfl_op_session(op, 0, s->cap);
	const dtfl_type *kt = c->reader.kt;
	__dtfl_cursor_list cursor;
	size_t i;

	/* Sort requests by selected key for in-order cursor traversal. */
	lm_sort_prefixes(c, kt, &s->prefixes);

	__dtfl_trace_agent_cursor(&c->reader, &cursor);

	for (i = 0; i < s->prefixes.len; i++) {
		lm_prefix *p = DTFL_VEC_AT(&s->prefixes, lm_prefix, i);
		unsigned char key[DTFL_DOGS_KEY_MAX];
		size_t klen = 0;

		c->key(c->key_ctx, p->rec, p->rlen, key, sizeof key, &klen);
		__dtfl_cursor_list_seek_key(&cursor, key, klen);
		if (__dtfl_cursor_list_key_valid(&cursor)) {
			size_t ck;
			const void *ckey = __dtfl_cursor_list_key(&cursor, &ck);

			if (dtfl_type_cmp(kt, ckey, ck, key, klen) != 0)
				goto cleared;
			while (__dtfl_cursor_list_val_valid(&cursor)) {
				size_t vlen;
				const void *val =
				    __dtfl_cursor_list_val(&cursor, &vlen);
				lm_acc acc;
				unsigned char orec[DTFL_DOGS_KEY_MAX];
				unsigned char odiff[DTFL_COMMON_DIFF_MAX];
				size_t olen = 0;

				acc.c = c;
				acc.time = p->time;
				c->mdc->zero(c->mdc, acc.count);
				__dtfl_cursor_list_map_times(&cursor,
				    lm_map_times, &acc);
				if (!c->mdc->is_zero(c->mdc, acc.count)) {
					bool keep = c->output_func(c->out_ctx,
					    p->rec, p->rlen, p->diff, val, vlen,
					    acc.count, orec, sizeof orec, &olen,
					    odiff);

					if (keep && !c->odc->is_zero(c->odc,
					    odiff)) {
						size_t tw = (size_t)c->depth *
						    sizeof(uint64_t);
						unsigned char *dst =
						    __dtfl_session_reserve(sess,
						    olen + tw + c->odw);

						if (olen != 0)
							memcpy(dst, orec, olen);
						memcpy(dst + olen, p->time, tw);
						if (c->odw != 0)
							memcpy(dst + olen + tw,
							    odiff, c->odw);
					}
				}
				__dtfl_cursor_list_step_val(&cursor);
			}
			__dtfl_cursor_list_rewind_vals(&cursor);
		}
cleared:
		/* Mark this prefix processed by zeroing its diff. */
		if (c->dw != 0)
			c->dc->zero(c->dc, p->diff);
	}

	__dtfl_cursor_list_free(&cursor);
	dtfl_session_close(sess);
}

static bool
lookup_logic(dtfl_op *op, void *vctx)
{
	lookup_ctx *c = vctx;
	dtfl_message *m;
	size_t fn, i;
	const uint64_t *frontier2;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* Drain input 1, stashing prefix requests by capability. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		size_t idx = find_stash(c, dtfl_cap_time(cap));
		lm_stash *s;
		size_t k = dtfl_message_len(m);

		if (idx == SIZE_MAX) {
			lm_stash ns;

			ns.cap = cap;
			__dtfl_vec_init(&ns.prefixes, sizeof(lm_prefix));
			__dtfl_vec_push(&c->stash, &ns);
			s = DTFL_VEC_AT(&c->stash, lm_stash, c->stash.len - 1);
		} else {
			dtfl_cap_drop(cap);
			s = DTFL_VEC_AT(&c->stash, lm_stash, idx);
		}
		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw,
			    len);
			const uint64_t *et = __dtfl_update_time(rec, c->depth,
			    c->dw, len);
			const void *diff = c->dw != 0 ?
			    __dtfl_update_diff(rec, c->dw, len) : NULL;
			lm_prefix p;

			p.rec = rlen != 0 ? __dtfl_alloc(rlen) : NULL;
			if (rlen != 0)
				memcpy(p.rec, rec, rlen);
			p.rlen = rlen;
			memcpy(p.time, et, (size_t)c->depth * sizeof(uint64_t));
			if (c->dw != 0) {
				p.diff = __dtfl_alloc(c->dw);
				memcpy(p.diff, diff, c->dw);
			} else {
				p.diff = NULL;
			}
			__dtfl_vec_push(&s->prefixes, &p);
		}
	}

	/* Drain input 2 (arrangement batches): observe only, drive scheduling. */
	while ((m = dtfl_op_input_next(op, 1)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 1, 0);

		dtfl_cap_drop(cap);
	}

	frontier2 = dtfl_op_frontier(op, 1, &fn);

	/* Process stash slots whose time the trace frontier has passed. */
	for (i = 0; i < c->stash.len; i++) {
		lm_stash *s = DTFL_VEC_AT(&c->stash, lm_stash, i);

		if (!__dtfl_frontier_less_equal(c->tc, frontier2, fn,
		    dtfl_cap_time(s->cap)))
			lm_process(op, c, s);
	}

	/* Drop fully processed prefixes (zero diff) and empty slots. */
	for (i = 0; i < c->stash.len;) {
		lm_stash *s = DTFL_VEC_AT(&c->stash, lm_stash, i);
		size_t j, w = 0;

		for (j = 0; j < s->prefixes.len; j++) {
			lm_prefix *p = DTFL_VEC_AT(&s->prefixes, lm_prefix, j);

			if (c->dw != 0 && c->dc->is_zero(c->dc, p->diff)) {
				lm_prefix_free(p);
			} else {
				if (w != j)
					*DTFL_VEC_AT(&s->prefixes, lm_prefix,
					    w) = *p;
				w++;
			}
		}
		s->prefixes.len = w;
		if (s->prefixes.len == 0) {
			dtfl_cap_drop(s->cap);
			__dtfl_vec_free(&s->prefixes);
			__dtfl_vec_remove(&c->stash, i);
		} else {
			i++;
		}
	}

	/*
	 * Logical compaction of the trace: the merge frontier is input1's
	 * frontier joined with every stashed capability time.  Compacting
	 * only to there keeps the AltNeu times distinct enough for the
	 * less_equal comparison above (lookup_map.rs: do not collapse unequal
	 * times to the frontier).
	 */
	{
		size_t f1n;
		const uint64_t *f1 = dtfl_op_frontier(op, 0, &f1n);
		__dtfl_antichain frontier;
		size_t j;

		__dtfl_antichain_init(&frontier, c->tc);
		for (j = 0; j < f1n; j++)
			__dtfl_antichain_insert(&frontier,
			    f1 + (size_t)c->depth * j);
		for (j = 0; j < c->stash.len; j++) {
			lm_stash *s = DTFL_VEC_AT(&c->stash, lm_stash, j);

			__dtfl_antichain_insert(&frontier,
			    dtfl_cap_time(s->cap));
		}
		__dtfl_trace_agent_set_logical(&c->reader,
		    (const uint64_t *)frontier.elems.data,
		    __dtfl_antichain_len(&frontier));
		__dtfl_antichain_free(&frontier);
	}
	return false;
}

static void
lookup_free(void *vctx)
{
	lookup_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->stash.len; i++) {
		lm_stash *s = DTFL_VEC_AT(&c->stash, lm_stash, i);
		size_t j;

		for (j = 0; j < s->prefixes.len; j++)
			lm_prefix_free(DTFL_VEC_AT(&s->prefixes, lm_prefix, j));
		__dtfl_vec_free(&s->prefixes);
		dtfl_cap_drop(s->cap);
	}
	__dtfl_vec_free(&c->stash);
	__dtfl_trace_agent_free(&c->reader);
	if (c->key_free != NULL)
		c->key_free(c->key_ctx);
	if (c->out_free != NULL)
		c->out_free(c->out_ctx);
	dtfl_type_free(c->out_type);
	__dtfl_free(c);
}

dtfl_collection *
__dtfl_dogs_lookup_map(dtfl_collection *prefixes, dtfl_arranged *arr,
    const dtfl_type *out_type, const __dtfl_diff_class *odc,
    dtfl_key_selector_fn key, void *key_ctx, void (*key_free)(void *),
    __dtfl_lookup_output_fn output_func, void *out_ctx,
    void (*out_free)(void *))
{
	const __dtfl_trace_agent *g = __dtfl_arranged_agent(arr);
	dtfl_scope *scope = dtfl_stream_scope(prefixes->stream);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	unsigned depth = prefixes->depth;
	lookup_ctx *c = __dtfl_alloc(sizeof *c);
	lm_route *route = __dtfl_alloc(sizeof *route);
	dtfl_diff *out_diff;
	const dtfl_type *ut;
	dtfl_op_builder *b;
	dtfl_stream *st;

	memset(c, 0, sizeof *c);
	__dtfl_trace_agent_clone(g, &c->reader);
	/* Do not block physical merging for this operator. */
	{
		uint64_t minimum[DTFL_TIME_MAX] = { 0 };

		__dtfl_trace_agent_set_physical(&c->reader, minimum, 0);
	}
	c->tc = tc;
	c->depth = depth;
	c->dc = prefixes->diff->cls;
	c->dw = c->dc->width;
	c->mdc = g->dc;
	c->mdw = c->mdc->width;
	c->out_type = dtfl_type_retain(out_type);
	c->odc = odc;
	c->odw = odc->width;
	c->key = key;
	c->key_ctx = key_ctx;
	c->key_free = key_free;
	c->output_func = output_func;
	c->out_ctx = out_ctx;
	c->out_free = out_free;
	__dtfl_vec_init(&c->stash, sizeof(lm_stash));

	route->key = key;
	route->ctx = key_ctx;
	route->kt = g->kt;
	route->depth = depth;
	route->dw = c->dw;

	out_diff = __dtfl_diff_wrap(odc);
	ut = __dtfl_update_type(out_type, depth, odc->width);
	b = dtfl_op_builder_new(scope, "LookupMap");
	dtfl_op_builder_input(b, prefixes->stream,
	    dtfl_pact_exchange(lm_route_fn, route));
	dtfl_op_builder_input(b, __dtfl_arranged_stream(arr),
	    dtfl_pact_pipeline());
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
	dtfl_op_builder_set_notify(b, 1, DTFL_NOTIFY_ALWAYS);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, lookup_logic, c, lookup_free);
	dtfl_type_free(ut);
	/* The route ctx is owned by the exchange pact; freed with the op. */
	return __dtfl_as_collection(st, out_type, out_diff, depth);
}
