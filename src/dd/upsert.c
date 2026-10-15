/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/arrange/upsert.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/upsert.c: arrange a stream of keyed upserts into a trace.
 *
 * An upsert is a (key, Option<value>, time): the value a key holds is the
 * most recent one by time, and a None uninstalls the key.  Upserts carry no
 * diffs, so the operator must compute the retractions itself: on each input
 * frontier advance it processes the upserts whose time is not in advance of
 * the sealing upper, groups them by key, reads the key's prior value from the
 * trace it maintains, and emits (-prev, +next) differences only where the
 * value changes.  Ties in time take the greatest Option<value> (None < Some),
 * which the option descriptor's cmp gives directly (upsert.rs 222,262).
 * Upserts require a totally ordered time.
 *
 * The input record is key||option(value); the embedded differential time is
 * the upsert time, and the diff is ignored.  The output trace is a value
 * trace key -> value with isize diffs, published through a TraceAgent exactly
 * as the arrange operator does, so as_collection, join and reduce read it the
 * same way.
 *
 * Limit: the time-ordered pending set is a vector sorted on each frontier
 * advance rather than a binary heap (upsert.rs uses a BinaryHeap); the
 * behavior is identical and the heap is a throughput optimization, deferred
 * with the pbt_upsert heap property.
 */
#include "dd/arrange_int.h"
#include "dtfl/arrange.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

/* An arranged handle, shared with arrangement.c through dtfl/arrange.h. */
struct dtfl_arranged {
	dtfl_stream *stream;
	__dtfl_trace_agent agent;
	const dtfl_type *type;
	dtfl_diff *diff;
	unsigned depth;
	bool owns_agent;
};

/* One buffered upsert: time words, key bytes, option(value) bytes. */
typedef struct pending {
	uint64_t time[DTFL_TIME_MAX];
	__dtfl_vec key;		/* bytes */
	__dtfl_vec optval;	/* bytes: 1 tag + value */
} pending;

typedef struct upsert_ctx {
	__dtfl_trace_agent reader;	/* internal trace reader (upsert.rs) */
	__dtfl_trace_writer writer;
	__dtfl_vec caps;		/* dtfl_cap *, lower envelope */
	__dtfl_vec queue;		/* pending, time-ordered on seal */
	__dtfl_antichain prev_frontier;
	const dtfl_type *kt;		/* key type (not retained) */
	const dtfl_type *vt;		/* value type (not retained) */
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;	/* isize */
	unsigned depth;
	size_t dw;			/* diff width (8 for isize) */
} upsert_ctx;

static void
pending_free(pending *p)
{
	__dtfl_vec_free(&p->key);
	__dtfl_vec_free(&p->optval);
}

/* ------------------------------------------------------------------ */
/* Reading the prior value of a key from the maintained trace.         */
/* ------------------------------------------------------------------ */

/* Sums the diffs the cursor sees for the current value. */
typedef struct count_ctx {
	int64_t count;
} count_ctx;

static void
count_times(const uint64_t *time, const void *diff, void *vctx)
{
	count_ctx *c = vctx;
	int64_t d;

	(void)time;
	memcpy(&d, diff, sizeof d);
	c->count += d;
}

/*
 * The value currently installed for `key` in the trace, or NULL.  Each key
 * has at most one value with a net diff of 1 (upsert.rs asserts count is 0
 * or 1); that value, if present, is written into *out (bytes) and returned.
 */
static const void *
prior_value(upsert_ctx *c, __dtfl_cursor_list *cl, const void *key,
    size_t klen, __dtfl_vec *out)
{
	const void *found = NULL;
	size_t flen = 0;

	__dtfl_cursor_list_rewind_keys(cl);
	__dtfl_cursor_list_seek_key(cl, key, klen);
	if (__dtfl_cursor_list_key_valid(cl)) {
		size_t kl;
		const void *k = __dtfl_cursor_list_key(cl, &kl);

		if (dtfl_type_cmp(c->kt, k, kl, key, klen) == 0) {
			while (__dtfl_cursor_list_val_valid(cl)) {
				size_t vl;
				const void *v = __dtfl_cursor_list_val(cl, &vl);
				count_ctx cc = { 0 };

				__dtfl_cursor_list_map_times(cl, count_times,
				    &cc);
				if (cc.count == 1) {
					found = v;
					flen = vl;
				}
				__dtfl_cursor_list_step_val(cl);
			}
		}
	}
	if (found == NULL)
		return NULL;
	__dtfl_vec_clear(out);
	__dtfl_vec_push_n(out, found, flen);
	return out->data;
}

/* ------------------------------------------------------------------ */
/* Processing one sealing interval.                                    */
/* ------------------------------------------------------------------ */

/* Sorts the queue by (time, key, option<value>) ascending (upsert.rs sort). */
static int
pending_cmp(upsert_ctx *c, const pending *a, const pending *b)
{
	int r = __dtfl_words_cmp(a->time, b->time, c->depth);

	if (r != 0)
		return r;
	r = dtfl_type_cmp(c->kt, a->key.data, a->key.len, b->key.data,
	    b->key.len);
	if (r != 0)
		return r;
	/* option<value> order: None < Some, then by value (upsert.rs tie). */
	return dtfl_type_cmp(dtfl_type_option(c->vt), a->optval.data,
	    a->optval.len, b->optval.data, b->optval.len);
}

/* Insertion sort of the queue indices in place (queue is small per seal). */
static void
sort_queue(upsert_ctx *c)
{
	size_t i, j, n = c->queue.len;
	pending *q = (pending *)(void *)c->queue.data;

	for (i = 1; i < n; i++) {
		pending tmp = q[i];

		j = i;
		while (j > 0 && pending_cmp(c, &q[j - 1], &tmp) > 0) {
			q[j] = q[j - 1];
			j--;
		}
		q[j] = tmp;
	}
}

/*
 * Seal and send one batch for the capability at index `idx`.  Builds the
 * upper (the input frontier joined with the times of later capabilities),
 * removes the pending upserts not in advance of that upper, groups them by
 * key, and for each key reads the prior installed value from the maintained
 * trace and emits -prev / +next differences only where the value changes.
 * In a tie on time the greatest option<value> wins (upsert.rs 222,262).
 */
static void
process_interval(upsert_ctx *c, dtfl_op *op, dtfl_cap *cap,
    const uint64_t *frontier, size_t fn, size_t idx, __dtfl_antichain *upper)
{
	__dtfl_cursor_list cl;
	__dtfl_builder_ord bld;
	__dtfl_description desc;
	__dtfl_antichain lower, up, since;
	__dtfl_batch_ord *batch;
	__dtfl_vec taken;		/* pending taken for this interval */
	__dtfl_vec prevbuf;		/* scratch for prior_value */
	size_t i, j, keep;
	const int64_t one = 1, minus = -1;

	/* upper = frontier join the times of subsequent capabilities. */
	__dtfl_antichain_clear(upper);
	for (j = 0; j < fn; j++)
		__dtfl_antichain_insert(upper, frontier + (size_t)c->depth * j);
	for (j = idx + 1; j < c->caps.len; j++)
		__dtfl_antichain_insert(upper,
		    dtfl_cap_time(*DTFL_VEC_AT(&c->caps, dtfl_cap *, j)));

	/* Take the upserts whose time is not in advance of `upper`. */
	__dtfl_vec_init(&taken, sizeof(pending));
	keep = 0;
	for (i = 0; i < c->queue.len; i++) {
		pending *p = DTFL_VEC_AT(&c->queue, pending, i);

		if (!__dtfl_antichain_less_equal(upper, p->time))
			__dtfl_vec_push(&taken, p);
		else
			*DTFL_VEC_AT(&c->queue, pending, keep++) = *p;
	}
	c->queue.len = keep;

	/* A cursor over the current trace, to read prior values. */
	__dtfl_trace_agent_cursor(&c->reader, &cl);

	__dtfl_builder_ord_init(&bld, false, c->kt, c->vt, c->tc, c->dc, 0, 0,
	    0);
	__dtfl_vec_init(&prevbuf, 1);

	/* Sort taken by (key, then time, then option<value>). */
	{
		pending *t = (pending *)(void *)taken.data;
		size_t n = taken.len, a, b;

		for (a = 1; a < n; a++) {
			pending tmp = t[a];
			int rk;

			b = a;
			while (b > 0) {
				rk = dtfl_type_cmp(c->kt, t[b - 1].key.data,
				    t[b - 1].key.len, tmp.key.data,
				    tmp.key.len);
				if (rk == 0)
					rk = __dtfl_words_cmp(t[b - 1].time,
					    tmp.time, c->depth);
				if (rk <= 0)
					break;
				t[b] = t[b - 1];
				b--;
			}
			t[b] = tmp;
		}
	}

	/* Process key by key. */
	for (i = 0; i < taken.len;) {
		pending *p = DTFL_VEC_AT(&taken, pending, i);
		const void *prev;
		size_t prev_len = 0;
		bool have_prev;
		size_t g;

		/* Prior value installed for this key. */
		prev = prior_value(c, &cl, p->key.data, p->key.len, &prevbuf);
		have_prev = prev != NULL;
		if (have_prev)
			prev_len = prevbuf.len;

		/* Walk this key's upserts in time order; dedup equal times. */
		g = i;
		while (g < taken.len &&
		    dtfl_type_cmp(c->kt, DTFL_VEC_AT(&taken, pending, g)->key.data,
			DTFL_VEC_AT(&taken, pending, g)->key.len, p->key.data,
			p->key.len) == 0) {
			pending *u = DTFL_VEC_AT(&taken, pending, g);
			const unsigned char *ov = u->optval.data;
			bool some = u->optval.len > 0 && ov[0] == 1;
			size_t nextlen = some ? u->optval.len - 1 : 0;
			const void *next = some ? ov + 1 : NULL;
			size_t gg = g + 1;

			/* Dedup upserts at the same time: last (greatest) wins. */
			while (gg < taken.len) {
				pending *w = DTFL_VEC_AT(&taken, pending, gg);

				if (dtfl_type_cmp(c->kt, w->key.data,
				    w->key.len, p->key.data, p->key.len) != 0)
					break;
				if (__dtfl_words_cmp(w->time, u->time,
				    c->depth) != 0)
					break;
				u = w;	/* sorted ascending: last is greatest */
				ov = u->optval.data;
				some = u->optval.len > 0 && ov[0] == 1;
				nextlen = some ? u->optval.len - 1 : 0;
				next = some ? ov + 1 : NULL;
				gg++;
			}

			/* Emit only if the value changes. */
			if (!(have_prev == some &&
			    (!some || (prev_len == nextlen &&
			     memcmp(prev, next, nextlen) == 0)))) {
				if (have_prev)
					__dtfl_builder_ord_push(&bld,
					    p->key.data, p->key.len, prevbuf.data,
					    prev_len, u->time, &minus);
				if (some)
					__dtfl_builder_ord_push(&bld,
					    p->key.data, p->key.len, next,
					    nextlen, u->time, &one);
				/* The new prior value is `next`. */
				__dtfl_vec_clear(&prevbuf);
				if (some)
					__dtfl_vec_push_n(&prevbuf, next, nextlen);
				have_prev = some;
				prev = some ? prevbuf.data : NULL;
				prev_len = some ? nextlen : 0;
			}
			g = gg;
		}
		i = g;
	}

	__dtfl_cursor_list_free(&cl);

	/* Description: [prev_frontier, upper), since = minimum. */
	__dtfl_antichain_init(&lower, c->tc);
	__dtfl_antichain_copy(&lower, &c->prev_frontier);
	__dtfl_antichain_init(&up, c->tc);
	__dtfl_antichain_copy(&up, upper);
	__dtfl_antichain_init(&since, c->tc);
	{
		uint64_t minimum[DTFL_TIME_MAX] = { 0 };

		__dtfl_antichain_insert(&since, minimum);
	}
	__dtfl_description_init(&desc, &lower, &up, &since);
	batch = __dtfl_builder_ord_done(&bld, &desc);

	/* prev_frontier advances to upper for the next interval. */
	__dtfl_antichain_clear(&c->prev_frontier);
	for (j = 0; j < __dtfl_antichain_len(upper); j++)
		__dtfl_antichain_insert(&c->prev_frontier,
		    __dtfl_antichain_at(upper, j));

	/*
	 * writer.insert(batch.clone()) then output.give(batch): the writer
	 * takes one reference into the trace and the stream record owns a
	 * second (released by the consumer when it is done cursoring).
	 */
	__dtfl_trace_writer_insert(&c->writer, batch, dtfl_cap_time(cap));
	__dtfl_batch_ord_retain(batch);
	{
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		unsigned char *dst;

		__dtfl_session_set_batch_ptr(s);
		dst = __dtfl_session_reserve(s, DTFL_BATCH_PTR_SIZE);
		memcpy(dst, &batch, sizeof batch);
		dtfl_session_close(s);
	}

	__dtfl_vec_free(&prevbuf);
	for (i = 0; i < taken.len; i++)
		pending_free(DTFL_VEC_AT(&taken, pending, i));
	__dtfl_vec_free(&taken);
}

static bool
upsert_logic(dtfl_op *op, void *vctx)
{
	upsert_ctx *c = vctx;
	dtfl_message *m;
	size_t fn, i, j;
	const uint64_t *frontier;
	bool any_behind;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* Stash upserts; retain a capability per message. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t k = dtfl_message_len(m);
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		bool have = false;

		for (i = 0; i < c->caps.len; i++)
			if (__dtfl_words_cmp(dtfl_cap_time(*DTFL_VEC_AT(&c->caps,
			    dtfl_cap *, i)), dtfl_cap_time(cap), c->depth) == 0) {
				have = true;
				break;
			}
		if (have)
			dtfl_cap_drop(cap);
		else
			__dtfl_vec_push(&c->caps, &cap);

		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw,
			    len);
			uint64_t etbuf[DTFL_TIME_MAX];
			const uint64_t *et = __dtfl_update_time_copy(rec,
			    c->depth, c->dw, len, etbuf);
			size_t klen = dtfl_type_value_len(c->kt, rec, rlen);
			pending p;

			memcpy(p.time, et, (size_t)c->depth * sizeof(uint64_t));
			__dtfl_vec_init(&p.key, 1);
			__dtfl_vec_init(&p.optval, 1);
			__dtfl_vec_push_n(&p.key, rec, klen);
			__dtfl_vec_push_n(&p.optval,
			    (const unsigned char *)rec + klen, rlen - klen);
			__dtfl_vec_push(&c->queue, &p);
		}
	}

	frontier = dtfl_op_frontier(op, 0, &fn);

	/* Strict progress only. */
	{
		__dtfl_antichain cur;
		bool changed;

		__dtfl_antichain_init(&cur, c->tc);
		for (i = 0; i < fn; i++)
			__dtfl_antichain_insert(&cur,
			    frontier + (size_t)c->depth * i);
		changed = !__dtfl_antichain_eq(&c->prev_frontier, &cur);
		__dtfl_antichain_free(&cur);
		if (!changed)
			return c->caps.len != 0 || c->queue.len != 0;
	}

	any_behind = false;
	for (i = 0; i < c->caps.len; i++)
		if (!__dtfl_frontier_less_equal(c->tc, frontier, fn,
		    dtfl_cap_time(*DTFL_VEC_AT(&c->caps, dtfl_cap *, i)))) {
			any_behind = true;
			break;
		}

	if (any_behind) {
		__dtfl_antichain upper;

		__dtfl_antichain_init(&upper, c->tc);
		for (i = 0; i < c->caps.len; i++) {
			dtfl_cap *cap = *DTFL_VEC_AT(&c->caps, dtfl_cap *, i);

			if (__dtfl_frontier_less_equal(c->tc, frontier, fn,
			    dtfl_cap_time(cap)))
				continue;
			process_interval(c, op, cap, frontier, fn, i, &upper);
		}

		/* Downgrade caps to the earliest pending time, else drop all. */
		{
			__dtfl_vec newcaps;
			const uint64_t *earliest = NULL;

			sort_queue(c);
			if (c->queue.len > 0)
				earliest = ((pending *)(void *)c->queue.data)[0].time;
			__dtfl_vec_init(&newcaps, sizeof(dtfl_cap *));
			if (earliest != NULL) {
				dtfl_cap *found = NULL;

				for (i = 0; i < c->caps.len; i++) {
					dtfl_cap *cap = *DTFL_VEC_AT(&c->caps,
					    dtfl_cap *, i);

					if (__dtfl_frontier_less_equal(c->tc,
					    dtfl_cap_time(cap), 1, earliest)) {
						found = cap;
						break;
					}
				}
				if (found == NULL)
					__dtfl_panic("upsert: failed to find "
					    "capability");
				{
					dtfl_cap *nc = dtfl_cap_delayed(found,
					    earliest);

					__dtfl_vec_push(&newcaps, &nc);
				}
			}
			for (i = 0; i < c->caps.len; i++)
				dtfl_cap_drop(*DTFL_VEC_AT(&c->caps,
				    dtfl_cap *, i));
			__dtfl_vec_free(&c->caps);
			c->caps = newcaps;
		}
		__dtfl_antichain_free(&upper);
	} else {
		__dtfl_antichain front;

		__dtfl_antichain_init(&front, c->tc);
		for (i = 0; i < fn; i++)
			__dtfl_antichain_insert(&front,
			    frontier + (size_t)c->depth * i);
		__dtfl_trace_writer_seal(&c->writer, &front);
		__dtfl_antichain_free(&front);
	}

	/* Advance the maintained reader's compaction (upsert.rs). */
	__dtfl_antichain_clear(&c->prev_frontier);
	for (i = 0; i < fn; i++)
		__dtfl_antichain_insert(&c->prev_frontier,
		    frontier + (size_t)c->depth * i);
	__dtfl_trace_agent_set_logical(&c->reader,
	    (const uint64_t *)(void *)c->prev_frontier.elems.data,
	    __dtfl_antichain_len(&c->prev_frontier));
	__dtfl_trace_agent_set_physical(&c->reader,
	    (const uint64_t *)(void *)c->prev_frontier.elems.data,
	    __dtfl_antichain_len(&c->prev_frontier));

	(void)j;
	return c->caps.len != 0 || c->queue.len != 0;
}

/* Routes an upsert record by its key prefix (upsert.rs hashes the key). */
static uint64_t
upsert_route(const void *rec, size_t len, void *ctx)
{
	const dtfl_type *kt = ctx;
	size_t klen = dtfl_type_value_len(kt, rec, len);

	return dtfl_type_hash(kt, rec, klen);
}

static void
upsert_free(void *vctx)
{
	upsert_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->caps.len; i++)
		dtfl_cap_drop(*DTFL_VEC_AT(&c->caps, dtfl_cap *, i));
	__dtfl_vec_free(&c->caps);
	for (i = 0; i < c->queue.len; i++)
		pending_free(DTFL_VEC_AT(&c->queue, pending, i));
	__dtfl_vec_free(&c->queue);
	__dtfl_antichain_free(&c->prev_frontier);
	__dtfl_trace_agent_free(&c->reader);
	__dtfl_trace_writer_free(&c->writer);
	__dtfl_free(c);
}

/* ------------------------------------------------------------------ */
/* dtfl_arrange_from_upsert.                                           */
/* ------------------------------------------------------------------ */

dtfl_arranged *
dtfl_arrange_from_upsert(dtfl_collection *upserts, const dtfl_type *key_type,
    const dtfl_type *val_type, const char *name)
{
	dtfl_scope *scope = dtfl_collection_scope(upserts);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	const __dtfl_diff_class *dc = __dtfl_diff_builtin(DTFL_DIFF_I64);
	unsigned depth = dtfl_time_depth(tc);
	upsert_ctx *c = __dtfl_alloc(sizeof *c);
	dtfl_arranged *a = __dtfl_alloc(sizeof *a);
	__dtfl_trace *trace;
	dtfl_op_builder *b;
	dtfl_diff *outdiff = __dtfl_diff_wrap(dc);

	/* A value trace key -> value with isize diffs (upsert.rs). */
	trace = __dtfl_trace_new(false, key_type, val_type, tc, dc);

	c->kt = key_type;
	c->vt = val_type;
	c->tc = tc;
	c->dc = dc;
	c->depth = depth;
	c->dw = dc->width;
	__dtfl_vec_init(&c->caps, sizeof(dtfl_cap *));
	__dtfl_vec_init(&c->queue, sizeof(pending));
	__dtfl_antichain_init(&c->prev_frontier, tc);
	{
		uint64_t minimum[DTFL_TIME_MAX] = { 0 };

		__dtfl_antichain_insert(&c->prev_frontier, minimum);
	}

	__dtfl_trace_agent_new(trace, false, key_type, val_type, tc, dc,
	    &c->reader, &c->writer);
	/* The Arranged keeps a cloned reader; the operator keeps c->reader. */
	__dtfl_trace_agent_clone(&c->reader, &a->agent);

	b = dtfl_op_builder_new(scope, name != NULL ? name : "Upsert");
	/* Exchange upserts by key hash (upsert.rs Exchange on key). */
	dtfl_op_builder_input(b, upserts->stream,
	    dtfl_pact_exchange(upsert_route, (void *)key_type));
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
	a->stream = dtfl_op_builder_output(b, __dtfl_batch_ptr_type());
	dtfl_op_builder_build(b, upsert_logic, c, upsert_free);

	a->type = dtfl_type_retain(key_type);
	a->diff = outdiff;
	a->depth = depth;
	a->owns_agent = true;
	return a;
}
