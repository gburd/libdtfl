/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dogs3-v0.25.1
 *   dogsdogsdogs/src/operators/half_join.rs
 *   dogsdogsdogs/src/operators/half_join2.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dogs/half_join.c: the half-join, a binary equijoin that responds only
 * to updates on its first input.  dtfl_dogs_half_join is the AltNeu
 * strict/non-strict form (half_join.rs); dtfl_dogs_half_join2 is the
 * general form with user comparison and frontier closures (half_join2.rs),
 * which the first is a thin wrapper of.
 *
 * Each streamed update is a record key||val1||time1 carried at an initial
 * time less or equal to time1.  For every (key, val2) in the arrangement at
 * a trace time2 that the comparison accepts against the initial time, the
 * operator emits output_func(key, val1, val2) at the joined time
 * (time1 join time2), carried at the initial time, with the product of the
 * diffs.  The time is hoisted into the output record so a later operator can
 * delay the update to it; this is how a sequence of half-joins builds a
 * delta join over a partially ordered time.
 *
 * The comparison is the total-order test on trace times against the initial
 * time: strict (time2 < initial, ignoring concurrent updates) or non-strict
 * (time2 <= initial, seeing them).  Under an AltNeu scope these are the two
 * sides that make a pair of concurrent updates interact exactly once.
 *
 * Simplification: the C substrate runs one worker, so the elaborate
 * capability-downgrade and re-stash bookkeeping of upstream (which peels
 * incomplete proposals onto delayed capabilities to keep progress precise
 * across workers) is reduced to deferring a capability's proposals until
 * the arrangement frontier has passed the capability time.  The total-order
 * comparison that decides which trace times join, which is the correctness
 * core, is reproduced exactly.
 */
#include "dogs/dogs_int.h"
#include "dd/common_int.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

#include <string.h>

/* A streamed proposal: record key||val1||time1, initial time, diff1. */
typedef struct hj_prop {
	unsigned char *rec;	/* owned: key||val1||time1 bytes */
	size_t rlen;		/* length of key||val1 (time stripped off) */
	uint64_t time1[DTFL_TIME_MAX];
	uint64_t initial[DTFL_TIME_MAX];
	unsigned char *diff;	/* owned: diff1 bytes (dw), or NULL */
} hj_prop;

typedef struct hj_stash {
	dtfl_cap *cap;		/* retained */
	__dtfl_vec props;	/* hj_prop, FIFO */
} hj_stash;

typedef struct half_join_ctx {
	__dtfl_trace_agent reader;	/* cloned reader of the arrangement */
	const dtfl_time_class *tc;
	unsigned depth;
	const dtfl_type *kt;		/* key type (splits the record) */
	const dtfl_type *v1t;		/* streamed value type */
	const __dtfl_diff_class *dc;	/* streamed diff class (R) */
	size_t dw;
	const __dtfl_diff_class *mdc;	/* matched (trace) diff class */
	size_t mdw;
	const dtfl_type *out_type;	/* retained; the record part */
	const __dtfl_diff_class *odc;	/* output diff class (R * BatchDiff) */
	size_t odw;
	bool strict;			/* comparison: < vs <= (when cmp NULL) */
	dtfl_half_join_cmp_fn cmp;	/* half_join2 comparison, or NULL */
	dtfl_half_join_frontier_fn frontier_func;  /* half_join2, or NULL */
	void *cmp_ctx;			/* shared by cmp and frontier_func */
	dtfl_half_join_output_fn output_func;
	void *user;
	void (*user_free)(void *);
	void *route;			/* hj_route *, owned */
	__dtfl_vec stash;		/* hj_stash */
} half_join_ctx;

/* The per-key exchange route: hash of the record's key prefix. */
typedef struct hj_route {
	const dtfl_type *kt;
	unsigned depth;
	size_t dw;
} hj_route;

static uint64_t
hj_route_fn(const void *rec, size_t len, void *vctx)
{
	hj_route *r = vctx;
	size_t rlen = __dtfl_update_rec_len(r->depth, r->dw, len);
	size_t klen = dtfl_type_value_len(r->kt, rec, rlen);

	if (klen == SIZE_MAX || klen > rlen)
		klen = rlen;
	return dtfl_type_hash(r->kt, rec, klen);
}

static size_t
find_stash(half_join_ctx *c, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < c->stash.len; i++) {
		hj_stash *s = DTFL_VEC_AT(&c->stash, hj_stash, i);

		if (__dtfl_time_eq(c->tc, dtfl_cap_time(s->cap), t))
			return i;
	}
	return SIZE_MAX;
}

/* The comparison: trace time2 against the initial time.  half_join2
 * supplies a callback; half_join uses the strict/non-strict total-order
 * test. */
static bool
hj_cmp(const half_join_ctx *c, const uint64_t *t2, const uint64_t *initial)
{
	if (c->cmp != NULL)
		return c->cmp(c->cmp_ctx, t2, initial);
	if (c->strict)
		return dtfl_time_less_than(c->tc, t2, initial);
	return dtfl_time_less_equal(c->tc, t2, initial);
}

/* One (joined_time, diff2) output entry, before consolidation. */
typedef struct hj_td {
	uint64_t time[DTFL_TIME_MAX];
	unsigned char diff[DTFL_COMMON_DIFF_MAX];
} hj_td;

/* The map_times accumulator: collect joined (t2 join time1, diff2) entries. */
typedef struct hj_acc {
	half_join_ctx *c;
	const uint64_t *initial;
	const uint64_t *time1;
	__dtfl_vec out;		/* hj_td */
} hj_acc;

static void
hj_map_times(const uint64_t *t2, const void *d, void *vctx)
{
	hj_acc *a = vctx;

	if (hj_cmp(a->c, t2, a->initial)) {
		hj_td e;

		dtfl_time_join(a->c->tc, t2, a->time1, e.time);
		memcpy(e.diff, d, a->c->mdw);
		__dtfl_vec_push(&a->out, &e);
	}
}

/* Consolidate the (time, diff2) buffer by joined time (sum diffs). */
static void
hj_consolidate(half_join_ctx *c, __dtfl_vec *out)
{
	size_t i, w = 0;

	/* Insertion sort by time words (buffers are small per key/value). */
	for (i = 1; i < out->len; i++) {
		hj_td key = *DTFL_VEC_AT(out, hj_td, i);
		size_t j = i;

		while (j > 0 && __dtfl_words_cmp(DTFL_VEC_AT(out, hj_td,
		    j - 1)->time, key.time, c->depth) > 0) {
			*DTFL_VEC_AT(out, hj_td, j) =
			    *DTFL_VEC_AT(out, hj_td, j - 1);
			j--;
		}
		*DTFL_VEC_AT(out, hj_td, j) = key;
	}
	/* Merge equal-time runs, dropping zero sums. */
	for (i = 0; i < out->len;) {
		hj_td acc = *DTFL_VEC_AT(out, hj_td, i);
		size_t j = i + 1;

		while (j < out->len && __dtfl_words_cmp(
		    DTFL_VEC_AT(out, hj_td, j)->time, acc.time, c->depth) == 0) {
			c->mdc->plus_equals(c->mdc, acc.diff,
			    DTFL_VEC_AT(out, hj_td, j)->diff);
			j++;
		}
		if (!c->mdc->is_zero(c->mdc, acc.diff))
			*DTFL_VEC_AT(out, hj_td, w++) = acc;
		i = j;
	}
	out->len = w;
}

static void
hj_prop_free(hj_prop *p)
{
	if (p->rec != NULL)
		__dtfl_free(p->rec);
	if (p->diff != NULL)
		__dtfl_free(p->diff);
}

/* Process one stash slot once the arrangement frontier has passed its time. */
static void
hj_process(dtfl_op *op, half_join_ctx *c, hj_stash *s)
{
	dtfl_session *sess = dtfl_op_session(op, 0, s->cap);
	const dtfl_type *kt = c->reader.kt;
	__dtfl_cursor_list cursor;
	size_t i;

	__dtfl_trace_agent_cursor(&c->reader, &cursor);

	for (i = 0; i < s->props.len; i++) {
		hj_prop *p = DTFL_VEC_AT(&s->props, hj_prop, i);
		size_t klen = dtfl_type_value_len(kt, p->rec, p->rlen);
		const void *val1;
		size_t v1len;

		if (klen == SIZE_MAX || klen > p->rlen)
			klen = p->rlen;
		val1 = p->rec + klen;
		v1len = p->rlen - klen;

		__dtfl_cursor_list_seek_key(&cursor, p->rec, klen);
		if (__dtfl_cursor_list_key_valid(&cursor)) {
			size_t ck;
			const void *ckey = __dtfl_cursor_list_key(&cursor, &ck);

			if (dtfl_type_cmp(kt, ckey, ck, p->rec, klen) != 0)
				goto cleared;
			while (__dtfl_cursor_list_val_valid(&cursor)) {
				size_t v2len;
				const void *val2 =
				    __dtfl_cursor_list_val(&cursor, &v2len);
				hj_acc acc;
				size_t j;

				acc.c = c;
				acc.initial = p->initial;
				acc.time1 = p->time1;
				__dtfl_vec_init(&acc.out, sizeof(hj_td));
				__dtfl_cursor_list_map_times(&cursor,
				    hj_map_times, &acc);
				hj_consolidate(c, &acc.out);
				for (j = 0; j < acc.out.len; j++) {
					hj_td *e = DTFL_VEC_AT(&acc.out, hj_td,
					    j);
					unsigned char orec[DTFL_DOGS_KEY_MAX];
					size_t olen = 0;
					unsigned char odiff[DTFL_COMMON_DIFF_MAX];
					const __dtfl_diff_class *oc;
					size_t tw = (size_t)c->depth *
					    sizeof(uint64_t);
					unsigned char *dst;

					c->output_func(c->user, p->rec, klen,
					    val1, v1len, val2, v2len, orec,
					    sizeof orec, &olen);
					/* diff = diff1 * diff2. */
					if (__dtfl_diff_multiply(c->dc, c->mdc,
					    p->diff, e->diff, odiff, &oc) !=
					    DTFL_OK)
						continue;
					if (c->odc->is_zero(c->odc, odiff))
						continue;
					dst = __dtfl_session_reserve(sess,
					    olen + tw + c->odw);
					if (olen != 0)
						memcpy(dst, orec, olen);
					memcpy(dst + olen, e->time, tw);
					if (c->odw != 0)
						memcpy(dst + olen + tw, odiff,
						    c->odw);
				}
				__dtfl_vec_free(&acc.out);
				__dtfl_cursor_list_step_val(&cursor);
			}
			__dtfl_cursor_list_rewind_vals(&cursor);
		}
cleared:
		if (c->dw != 0)
			c->dc->zero(c->dc, p->diff);
	}

	__dtfl_cursor_list_free(&cursor);
	dtfl_session_close(sess);
}

static bool
half_join_logic(dtfl_op *op, void *vctx)
{
	half_join_ctx *c = vctx;
	dtfl_message *m;
	size_t fn, i;
	const uint64_t *frontier2;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* Drain input 1, stashing proposals by capability. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		size_t idx = find_stash(c, dtfl_cap_time(cap));
		hj_stash *s;
		size_t k = dtfl_message_len(m);

		if (idx == SIZE_MAX) {
			hj_stash ns;

			ns.cap = cap;
			__dtfl_vec_init(&ns.props, sizeof(hj_prop));
			__dtfl_vec_push(&c->stash, &ns);
			s = DTFL_VEC_AT(&c->stash, hj_stash, c->stash.len - 1);
		} else {
			dtfl_cap_drop(cap);
			s = DTFL_VEC_AT(&c->stash, hj_stash, idx);
		}
		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t full = __dtfl_update_rec_len(c->depth, c->dw,
			    len);
			size_t tw = (size_t)c->depth * sizeof(uint64_t);
			/* The record is key||val1||time1; strip time1. */
			size_t kvlen = full - tw;
			uint64_t initbuf[DTFL_TIME_MAX];
			const uint64_t *initial = __dtfl_update_time_copy(rec,
			    c->depth, c->dw, len, initbuf);
			const void *diff = c->dw != 0 ?
			    __dtfl_update_diff(rec, c->dw, len) : NULL;
			hj_prop p;

			p.rec = kvlen != 0 ? __dtfl_alloc(kvlen) : NULL;
			if (kvlen != 0)
				memcpy(p.rec, rec, kvlen);
			p.rlen = kvlen;
			memcpy(p.time1, (const unsigned char *)rec + kvlen, tw);
			memcpy(p.initial, initial, tw);
			if (c->dw != 0) {
				p.diff = __dtfl_alloc(c->dw);
				memcpy(p.diff, diff, c->dw);
			} else {
				p.diff = NULL;
			}
			__dtfl_vec_push(&s->props, &p);
		}
	}

	/* Drain input 2 (arrangement batches): observe only. */
	while ((m = dtfl_op_input_next(op, 1)) != NULL)
		dtfl_cap_drop(dtfl_op_input_cap(op, 1, 0));

	frontier2 = dtfl_op_frontier(op, 1, &fn);

	for (i = 0; i < c->stash.len; i++) {
		hj_stash *s = DTFL_VEC_AT(&c->stash, hj_stash, i);

		if (!__dtfl_frontier_less_equal(c->tc, frontier2, fn,
		    dtfl_cap_time(s->cap)))
			hj_process(op, c, s);
	}

	/* Drop fully processed proposals (zero diff) and empty slots. */
	for (i = 0; i < c->stash.len;) {
		hj_stash *s = DTFL_VEC_AT(&c->stash, hj_stash, i);
		size_t j, w = 0;

		for (j = 0; j < s->props.len; j++) {
			hj_prop *p = DTFL_VEC_AT(&s->props, hj_prop, j);

			if (c->dw != 0 && c->dc->is_zero(c->dc, p->diff)) {
				hj_prop_free(p);
			} else {
				if (w != j)
					*DTFL_VEC_AT(&s->props, hj_prop, w) = *p;
				w++;
			}
		}
		s->props.len = w;
		if (s->props.len == 0) {
			dtfl_cap_drop(s->cap);
			__dtfl_vec_free(&s->props);
			__dtfl_vec_remove(&c->stash, i);
		} else {
			i++;
		}
	}

	/* Logical compaction: input1 frontier joined with each held cap time
	 * transformed by frontier_func (half_join2) or the cap time itself. */
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
			hj_stash *s = DTFL_VEC_AT(&c->stash, hj_stash, j);

			if (c->frontier_func != NULL) {
				c->frontier_func(c->cmp_ctx,
				    dtfl_cap_time(s->cap),
				    (dtfl_half_join_frontier *)&frontier);
			} else {
				__dtfl_antichain_insert(&frontier,
				    dtfl_cap_time(s->cap));
			}
		}
		__dtfl_trace_agent_set_logical(&c->reader,
		    (const uint64_t *)(void *)frontier.elems.data,
		    __dtfl_antichain_len(&frontier));
		__dtfl_antichain_free(&frontier);
	}
	return false;
}

void
dtfl_half_join_frontier_insert(dtfl_half_join_frontier *f,
    const uint64_t *time)
{
	__dtfl_antichain_insert((__dtfl_antichain *)f, time);
}

static void
half_join_free(void *vctx)
{
	half_join_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->stash.len; i++) {
		hj_stash *s = DTFL_VEC_AT(&c->stash, hj_stash, i);
		size_t j;

		for (j = 0; j < s->props.len; j++)
			hj_prop_free(DTFL_VEC_AT(&s->props, hj_prop, j));
		__dtfl_vec_free(&s->props);
		dtfl_cap_drop(s->cap);
	}
	__dtfl_vec_free(&c->stash);
	__dtfl_trace_agent_free(&c->reader);
	if (c->user_free != NULL)
		c->user_free(c->user);
	if (c->route != NULL)
		__dtfl_free(c->route);
	dtfl_type_free(c->out_type);
	__dtfl_free(c);
}

static dtfl_collection *
half_join_build(dtfl_collection *stream, dtfl_arranged *arr,
    const dtfl_type *kt, const dtfl_type *v1t, const dtfl_type *out_type,
    bool strict, dtfl_half_join_cmp_fn cmp,
    dtfl_half_join_frontier_fn frontier_func, void *cmp_ctx,
    dtfl_half_join_output_fn output_func, dtfl_ctx ctx)
{
	const __dtfl_trace_agent *g = __dtfl_arranged_agent(arr);
	dtfl_scope *scope = dtfl_stream_scope(stream->stream);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	unsigned depth = stream->depth;
	const __dtfl_diff_class *dc = stream->diff->cls;
	const __dtfl_diff_class *mdc = g->dc;
	const __dtfl_diff_class *odc = __dtfl_diff_multiply_class(dc, mdc);
	half_join_ctx *c;
	hj_route *route;
	dtfl_diff *out_diff;
	const dtfl_type *ut;
	dtfl_op_builder *b;
	dtfl_stream *st;

	if (odc == NULL) {
		(void)__dtfl_err(DTFL_EALGEBRA,
		    "half_join: no multiply for %s * %s", dc->name, mdc->name);
		return NULL;
	}

	c = __dtfl_alloc(sizeof *c);
	route = __dtfl_alloc(sizeof *route);
	memset(c, 0, sizeof *c);
	__dtfl_trace_agent_clone(g, &c->reader);
	{
		uint64_t minimum[DTFL_TIME_MAX] = { 0 };

		__dtfl_trace_agent_set_physical(&c->reader, minimum, 0);
	}
	c->tc = tc;
	c->depth = depth;
	c->kt = kt;
	c->v1t = v1t;
	c->dc = dc;
	c->dw = dc->width;
	c->mdc = mdc;
	c->mdw = mdc->width;
	c->out_type = dtfl_type_retain(out_type);
	c->odc = odc;
	c->odw = odc->width;
	c->strict = strict;
	c->cmp = cmp;
	c->frontier_func = frontier_func;
	c->cmp_ctx = cmp_ctx;
	c->output_func = output_func;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	__dtfl_vec_init(&c->stash, sizeof(hj_stash));

	route->kt = kt;
	route->depth = depth;
	route->dw = c->dw;
	c->route = route;

	out_diff = __dtfl_diff_wrap(odc);
	ut = __dtfl_update_type(out_type, depth, odc->width);
	b = dtfl_op_builder_new(scope, "HalfJoin");
	dtfl_op_builder_input(b, stream->stream,
	    dtfl_pact_exchange(hj_route_fn, route));
	dtfl_op_builder_input(b, __dtfl_arranged_stream(arr),
	    dtfl_pact_pipeline());
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
	dtfl_op_builder_set_notify(b, 1, DTFL_NOTIFY_ALWAYS);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, half_join_logic, c, half_join_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, out_type, out_diff, depth);
}

dtfl_collection *
dtfl_dogs_half_join(dtfl_collection *stream, dtfl_arranged *arr,
    const dtfl_type *kt, const dtfl_type *v1t, const dtfl_type *out_type,
    bool strict, dtfl_half_join_output_fn output_func, dtfl_ctx ctx)
{
	return half_join_build(stream, arr, kt, v1t, out_type, strict,
	    NULL, NULL, NULL, output_func, ctx);
}

dtfl_collection *
dtfl_dogs_half_join2(dtfl_collection *stream, dtfl_arranged *arr,
    const dtfl_type *kt, const dtfl_type *v1t, const dtfl_type *out_type,
    dtfl_half_join_cmp_fn comparison,
    dtfl_half_join_frontier_fn frontier_func,
    dtfl_half_join_output_fn output_func, dtfl_ctx ctx)
{
	/* comparison and frontier_func share ctx.ptr; a NULL comparison
	 * falls back to the strict total-order test (half_join's default). */
	return half_join_build(stream, arr, kt, v1t, out_type, true,
	    comparison, frontier_func, ctx.ptr, output_func, ctx);
}
