/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/arrange/arrangement.rs
 *   differential-dataflow/src/collection.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/arrangement.c: the arrange operator and the Arranged type.
 *
 * arrange_core reacts to an advancing input frontier by producing the
 * sequence of batches whose [lower, upper) intervals tile the collection's
 * updates, indexing each by its record and committing it to a shared trace
 * through a TraceAgent writer.  The operator stashes updates through a
 * chunker into a merge batcher, retains a capability for the lower envelope
 * of buffered updates, and on each frontier advance seals one batch per
 * held capability not in advance of the new frontier (arrangement.rs
 * arrange_core).  The output stream carries one record per batch: a retained
 * __dtfl_batch_ord pointer on a Pipeline edge (DTFL_BATCH_PTR), which
 * as_collection and the P8 join/reduce operators read.
 *
 * Arranged is the collection's indexed form: the batch stream plus the
 * shared trace reader.  as_collection reads the batch stream and emits the
 * stored (key, value) updates as a collection (arrangement.rs
 * flat_map_batches); enter/leave carry the arrangement across a scope
 * boundary; the trace handle imports the shared trace into other dataflows
 * on the same worker (agent.rs import_named).  join_core and reduce_* are
 * declared on Arranged in arrange_int.h and defined in P8; this file does
 * not implement them.
 */
#include "dd/arrange_int.h"
#include "dd/wrap_int.h"
#include "dtfl/arrange.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

/*
 * An arrangement handle: the batch-pointer output stream and the shared
 * trace reader (a TraceAgent).  Owns the agent; the stream is the scope's.
 * keyed means the trace indexes the whole record as key with a unit value
 * (arrange_by_self), which is what dtfl_collection_arrange builds.
 */
struct dtfl_arranged {
	dtfl_stream *stream;		/* batch-pointer records; scope-owned */
	__dtfl_trace_agent agent;	/* shared trace reader */
	const dtfl_type *type;		/* record type (retained) */
	dtfl_diff *diff;		/* retained */
	unsigned depth;			/* time words */
	bool owns_agent;		/* free releases the agent */
};

/* A standalone trace handle: a cloned agent that can outlive the arrangement. */
struct dtfl_trace {
	__dtfl_trace_agent agent;
	const dtfl_type *type;		/* retained */
	dtfl_diff *diff;		/* retained */
	unsigned depth;
	bool keyed;
};

/* ------------------------------------------------------------------ */
/* The arrange operator (arrange_core).                                */
/* ------------------------------------------------------------------ */

typedef struct arrange_ctx {
	__dtfl_merge_batcher batcher;
	__dtfl_chunker chunker;
	__dtfl_trace_agent reader;	/* the shared reader (also in Arranged) */
	__dtfl_trace_writer writer;
	__dtfl_vec caps;		/* dtfl_cap *, lower envelope of batcher */
	__dtfl_antichain prev_frontier;
	const dtfl_type *rt;		/* record (whole) type; retained */
	const dtfl_type *kt;		/* key type for the builder split */
	const dtfl_type *vt;		/* value type (unit for key batches) */
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
	unsigned depth;
	size_t dw;
	bool keyed;
	void *route;			/* arrange_route_ctx * (by-key); owned */
} arrange_ctx;

/* Finds the capability for time `t` among the held caps, or SIZE_MAX. */
static size_t
find_cap(const arrange_ctx *c, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < c->caps.len; i++)
		if (__dtfl_words_cmp(dtfl_cap_time(*DTFL_VEC_AT(&c->caps,
		    dtfl_cap *, i)), t, c->depth) == 0)
			return i;
	return SIZE_MAX;
}

/* Builds one batch from the batcher sealed at `upper` and its description. */
static __dtfl_batch_ord *
seal_batch(arrange_ctx *c, const __dtfl_antichain *upper)
{
	__dtfl_batch **chain;
	size_t nchain;
	__dtfl_description desc;
	__dtfl_batch_ord *batch;

	__dtfl_merge_batcher_seal(&c->batcher, upper, &chain, &nchain, &desc);
	batch = __dtfl_builder_ord_seal(c->keyed, c->kt, c->vt,
	    c->tc, c->dc, chain, nchain, &desc);
	{
		size_t i;

		for (i = 0; i < nchain; i++)
			__dtfl_batch_release(chain[i]);
		if (chain != NULL)
			__dtfl_free(chain);
	}
	return batch;
}

static bool
arrange_logic(dtfl_op *op, void *vctx)
{
	arrange_ctx *c = vctx;
	dtfl_message *m;
	size_t fn, i, j;
	const uint64_t *frontier;
	bool any_behind;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* Stash received updates; retain a capability per message. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t k = dtfl_message_len(m);
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		__dtfl_batch *chunk;

		if (find_cap(c, dtfl_cap_time(cap)) == SIZE_MAX)
			__dtfl_vec_push(&c->caps, &cap);
		else
			dtfl_cap_drop(cap);
		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw,
			    len);
			uint64_t etbuf[DTFL_TIME_MAX];
			const uint64_t *et = __dtfl_update_time_copy(rec,
			    c->depth, c->dw, len, etbuf);
			const void *diff = c->dw != 0 ?
			    __dtfl_update_diff(rec, c->dw, len) : NULL;

			__dtfl_chunker_push(&c->chunker, rec, rlen, et, diff);
		}
		while ((chunk = __dtfl_chunker_extract(&c->chunker)) != NULL)
			__dtfl_merge_batcher_push(&c->batcher, chunk);
	}

	/* The input frontier as an antichain. */
	frontier = dtfl_op_frontier(op, 0, &fn);

	/* Strict progress only: skip when the frontier is unchanged. */
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
			return c->caps.len != 0;
	}

	/* Flush any partial chunk the chunker still holds into the batcher. */
	{
		__dtfl_batch *chunk;

		while ((chunk = __dtfl_chunker_finish(&c->chunker)) != NULL)
			__dtfl_merge_batcher_push(&c->batcher, chunk);
	}

	/* Any held capability not in advance of the input frontier? */
	any_behind = false;
	for (i = 0; i < c->caps.len; i++) {
		dtfl_cap *cap = *DTFL_VEC_AT(&c->caps, dtfl_cap *, i);

		if (!__dtfl_frontier_less_equal(c->tc, frontier, fn,
		    dtfl_cap_time(cap))) {
			any_behind = true;
			break;
		}
	}

	if (any_behind) {
		__dtfl_antichain upper;

		__dtfl_antichain_init(&upper, c->tc);
		/* Retire capabilities one at a time (arrange_core). */
		for (i = 0; i < c->caps.len; i++) {
			dtfl_cap *cap = *DTFL_VEC_AT(&c->caps, dtfl_cap *, i);
			__dtfl_batch_ord *batch;
			dtfl_session *s;

			if (__dtfl_frontier_less_equal(c->tc, frontier, fn,
			    dtfl_cap_time(cap)))
				continue;

			/* upper = frontier join the later capabilities. */
			__dtfl_antichain_clear(&upper);
			for (j = 0; j < fn; j++)
				__dtfl_antichain_insert(&upper,
				    frontier + (size_t)c->depth * j);
			for (j = i + 1; j < c->caps.len; j++)
				__dtfl_antichain_insert(&upper,
				    dtfl_cap_time(*DTFL_VEC_AT(&c->caps,
				    dtfl_cap *, j)));

			batch = seal_batch(c, &upper);
			/*
			 * The writer takes this reference into the trace (and
			 * retains its own for each listener queue).  The stream
			 * record is a borrow of the trace-owned batch, valid while
			 * the trace holds it; the consumer does not release it.
			 */
			__dtfl_trace_writer_insert(&c->writer, batch,
			    dtfl_cap_time(cap));
			s = dtfl_op_session(op, 0, cap);
			{
				unsigned char *dst = __dtfl_session_reserve(s,
				    DTFL_BATCH_PTR_SIZE);

				memcpy(dst, &batch, sizeof batch);
			}
			dtfl_session_close(s);
		}

		/* Downgrade caps to the batcher's lower update frontier. */
		{
			const __dtfl_antichain *bf =
			    __dtfl_merge_batcher_frontier(&c->batcher);
			__dtfl_vec newcaps;
			size_t ci;

			__dtfl_vec_init(&newcaps, sizeof(dtfl_cap *));
			for (j = 0; j < __dtfl_antichain_len(bf); j++) {
				const uint64_t *t = __dtfl_antichain_at(bf, j);
				dtfl_cap *found = NULL;

				for (ci = 0; ci < c->caps.len; ci++) {
					dtfl_cap *cap = *DTFL_VEC_AT(&c->caps,
					    dtfl_cap *, ci);

					if (__dtfl_frontier_less_equal(c->tc,
					    dtfl_cap_time(cap), 1, t)) {
						found = cap;
						break;
					}
				}
				if (found == NULL)
					__dtfl_panic("arrange: failed to find "
					    "capability");
				{
					dtfl_cap *nc = dtfl_cap_delayed(found, t);

					__dtfl_vec_push(&newcaps, &nc);
				}
			}
			/* Drop the old capabilities and adopt the new set. */
			for (ci = 0; ci < c->caps.len; ci++)
				dtfl_cap_drop(*DTFL_VEC_AT(&c->caps,
				    dtfl_cap *, ci));
			__dtfl_vec_free(&c->caps);
			c->caps = newcaps;
		}
		__dtfl_antichain_free(&upper);
	} else {
		/*
		 * No capability is behind the frontier: announce progress
		 * without data.  Seal the batcher to advance its lower bound
		 * (discarding the readied updates) and seal the writer to the
		 * frontier (an empty batch, not sent downstream).
		 */
		__dtfl_antichain front;
		__dtfl_batch **chain;
		size_t nchain, bi;
		__dtfl_description desc;

		__dtfl_antichain_init(&front, c->tc);
		for (i = 0; i < fn; i++)
			__dtfl_antichain_insert(&front,
			    frontier + (size_t)c->depth * i);
		__dtfl_merge_batcher_seal(&c->batcher, &front, &chain, &nchain,
		    &desc);
		for (bi = 0; bi < nchain; bi++)
			__dtfl_batch_release(chain[bi]);
		if (chain != NULL)
			__dtfl_free(chain);
		__dtfl_description_free(&desc);
		__dtfl_trace_writer_seal(&c->writer, &front);
		__dtfl_antichain_free(&front);
	}

	/* Record the new input frontier. */
	__dtfl_antichain_clear(&c->prev_frontier);
	for (i = 0; i < fn; i++)
		__dtfl_antichain_insert(&c->prev_frontier,
		    frontier + (size_t)c->depth * i);

	return c->caps.len != 0;
}

static void
arrange_free(void *vctx)
{
	arrange_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->caps.len; i++)
		dtfl_cap_drop(*DTFL_VEC_AT(&c->caps, dtfl_cap *, i));
	__dtfl_vec_free(&c->caps);
	__dtfl_antichain_free(&c->prev_frontier);
	/*
	 * The writer seals to the empty frontier on free (writer.rs Drop),
	 * closing the trace.  The operator's own reader (ac->reader) is a
	 * referee like any other and is released here; the Arranged keeps its
	 * own cloned reader, so the shared trace lives until that is freed too.
	 */
	__dtfl_trace_writer_free(&c->writer);
	__dtfl_trace_agent_free(&c->reader);
	__dtfl_merge_batcher_free(&c->batcher);
	__dtfl_chunker_free(&c->chunker);
	dtfl_type_free(c->rt);
	if (c->route != NULL)
		__dtfl_free(c->route);
	__dtfl_free(c);
}

/* ------------------------------------------------------------------ */
/* dtfl_collection_arrange / arrange_by_key.                           */
/* ------------------------------------------------------------------ */

/*
 * Routes a key||val update record by its key prefix, so matching keys
 * from the two join inputs land on the same worker (ExchangePact by key).
 * The context carries the key type (to split) and the record's time/diff
 * widths (to bound the key span).  A key batch (whole record is the key)
 * uses the record's own hash and never installs this.
 */
typedef struct arrange_route_ctx {
	const dtfl_type *kt;
	unsigned depth;
	size_t dw;
} arrange_route_ctx;

static uint64_t
arrange_route_by_key(const void *rec, size_t len, void *vctx)
{
	arrange_route_ctx *rc = vctx;
	size_t rlen = __dtfl_update_rec_len(rc->depth, rc->dw, len);
	size_t klen = dtfl_type_value_len(rc->kt, rec, rlen);

	if (klen == SIZE_MAX || klen > rlen)
		klen = rlen;
	return dtfl_type_hash(rc->kt, rec, klen);
}

/*
 * arrange_core: index a collection into a shared trace.  `keyed` true
 * stores the whole record as the key with a unit value (arrange_by_self,
 * what dtfl_collection_arrange builds and routes by the whole record);
 * `keyed` false splits each key||val record with `kt`, storing the key and
 * value in a value batch (arrange_by_key) and routing by the key prefix so
 * join matches land together.  `kt`/`vt` are retained by the arrangement.
 */
static dtfl_arranged *
arrange_core(dtfl_collection *c, bool keyed, const dtfl_type *kt,
    const dtfl_type *vt, const char *name)
{
	dtfl_scope *scope = dtfl_stream_scope(c->stream);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	unsigned depth = c->depth;
	size_t dw = c->diff->cls->width;
	const __dtfl_diff_class *dc = c->diff->cls;
	arrange_ctx *ac = __dtfl_alloc(sizeof *ac);
	dtfl_arranged *a = __dtfl_alloc(sizeof *a);
	__dtfl_trace *trace;
	const dtfl_type *bptr = __dtfl_batch_ptr_type();
	dtfl_pact pact;
	dtfl_op_builder *b;

	trace = __dtfl_trace_new(keyed, kt, vt, tc, dc);

	ac->rt = dtfl_type_retain(c->type);
	ac->kt = kt;
	ac->vt = vt;
	ac->tc = tc;
	ac->dc = dc;
	ac->depth = depth;
	ac->dw = dw;
	ac->keyed = keyed;
	ac->route = NULL;
	__dtfl_merge_batcher_init(&ac->batcher, c->type, tc, dc);
	__dtfl_chunker_init(&ac->chunker, c->type, depth, dc);
	__dtfl_vec_init(&ac->caps, sizeof(dtfl_cap *));
	__dtfl_antichain_init(&ac->prev_frontier, tc);
	{
		uint64_t minimum[DTFL_TIME_MAX] = { 0 };

		__dtfl_antichain_insert(&ac->prev_frontier, minimum);
	}

	/* TraceAgent::new gives the reader (shared) and the writer. */
	__dtfl_trace_agent_new(trace, keyed, kt, vt, tc, dc, &ac->reader,
	    &ac->writer);
	/* The Arranged keeps a cloned reader; the operator keeps ac->reader. */
	__dtfl_trace_agent_clone(&ac->reader, &a->agent);

	b = dtfl_op_builder_new(scope, name != NULL ? name : "Arrange");
	if (keyed) {
		pact = dtfl_pact_exchange(NULL, NULL);
	} else {
		arrange_route_ctx *rc = __dtfl_alloc(sizeof *rc);

		rc->kt = kt;
		rc->depth = depth;
		rc->dw = dw;
		ac->route = rc;
		pact = dtfl_pact_exchange(arrange_route_by_key, rc);
	}
	dtfl_op_builder_input(b, c->stream, pact);
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
	a->stream = dtfl_op_builder_output(b, bptr);
	dtfl_op_builder_build(b, arrange_logic, ac, arrange_free);

	a->type = dtfl_type_retain(keyed ? c->type : kt);
	a->diff = dtfl_diff_retain(c->diff);
	a->depth = depth;
	a->owns_agent = true;
	return a;
}

dtfl_arranged *
dtfl_collection_arrange(dtfl_collection *c, const char *name)
{
	return arrange_core(c, true, c->type, dtfl_type_unit(), name);
}

/*
 * __dtfl_collection_arrange_by_key: index a collection of key||val records
 * into a value trace split by `kt`, routing by key.  Used by join to
 * arrange both inputs by their shared key (collection.rs arrange_by_key).
 */
dtfl_arranged *
__dtfl_collection_arrange_by_key(dtfl_collection *c, const dtfl_type *kt,
    const dtfl_type *vt, const char *name)
{
	return arrange_core(c, false, kt, vt, name);
}

/*
 * dtfl_collection_arrange_by_key: the public forwarder to the internal
 * arrange_by_key (collection.rs arrange_by_key).  A thin wrapper so
 * examples and downstream code can build the shared index that
 * dtfl_arranged_join_core and dtfl_arranged_reduce_core read.
 */
dtfl_arranged *
dtfl_collection_arrange_by_key(dtfl_collection *c, const dtfl_type *key,
    const dtfl_type *val, const char *name)
{
	return __dtfl_collection_arrange_by_key(c, key, val, name);
}

/* ------------------------------------------------------------------ */
/* as_collection (flat_map_batches).                                   */
/* ------------------------------------------------------------------ */

typedef struct ascoll_ctx {
	dtfl_arranged_kv_fn fn;
	void *user;
	void (*user_free)(void *);
	const dtfl_type *out_type;	/* retained */
	unsigned depth;
	size_t dw;
} ascoll_ctx;

/*
 * The record sink an as_collection fn writes through.  It buffers the
 * emitted records (as a __dtfl_batch of records with no time or diff);
 * the operator then pairs each buffered record with every (time, diff) of
 * the current (key, value).
 */
struct dtfl_arranged_emit {
	__dtfl_batch *recs;	/* record bytes only (tw == 0, dw == 0) */
};

void
dtfl_arranged_emit_record(dtfl_arranged_emit *e, const void *rec, size_t len)
{
	__dtfl_batch_push(e->recs, rec, len, NULL, NULL);
}

/* Emits one (record, time, diff) update onto the session. */
static void
emit_update(dtfl_session *s, const void *rec, size_t rlen, const uint64_t *time,
    unsigned depth, const void *diff, size_t dw)
{
	size_t tw = (size_t)depth * sizeof(uint64_t);
	unsigned char *dst = __dtfl_session_reserve(s, rlen + tw + dw);

	if (rlen != 0)
		memcpy(dst, rec, rlen);
	if (tw != 0)
		memcpy(dst + rlen, time, tw);
	if (dw != 0)
		memcpy(dst + rlen + tw, diff, dw);
}

/*
 * Per-(key, value) context while walking a batch cursor.  When fn is NULL
 * `rec`/`rlen` is the key (identity).  When fn is set, `recs` holds the
 * records fn emitted for this (key, value); each is paired with every time.
 */
typedef struct ascoll_walk {
	ascoll_ctx *c;
	dtfl_session *s;
	const void *rec;
	size_t rlen;
	__dtfl_batch *recs;	/* fn path: buffered output records */
} ascoll_walk;

static void
ascoll_on_time(const uint64_t *time, const void *diff, void *vctx)
{
	ascoll_walk *w = vctx;

	if (w->recs == NULL) {
		emit_update(w->s, w->rec, w->rlen, time, w->c->depth, diff,
		    w->c->dw);
	} else {
		size_t j, n = __dtfl_batch_len(w->recs);

		for (j = 0; j < n; j++) {
			size_t rlen;
			const void *rec = __dtfl_batch_rec(w->recs, j, &rlen);

			emit_update(w->s, rec, rlen, time, w->c->depth, diff,
			    w->c->dw);
		}
	}
}

static bool
ascoll_logic(dtfl_op *op, void *vctx)
{
	ascoll_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *recp = dtfl_message_record(m, i, &len);
			__dtfl_batch_ord *batch;
			__dtfl_cursor cur;

			memcpy(&batch, recp, sizeof batch);
			__dtfl_cursor_init(&cur, batch);
			while (__dtfl_cursor_key_valid(&cur, batch)) {
				size_t klen;
				const void *key = __dtfl_cursor_key(&cur, batch,
				    &klen);

				while (__dtfl_cursor_val_valid(&cur, batch)) {
					size_t vlen;
					const void *val = __dtfl_cursor_val(&cur,
					    batch, &vlen);
					ascoll_walk w;

					w.c = c;
					w.s = s;
					w.rec = key;
					w.rlen = klen;
					w.recs = NULL;
					if (c->fn != NULL) {
						dtfl_arranged_emit e;

						e.recs = __dtfl_batch_new(0, 0);
						c->fn(c->user, key, klen, val,
						    vlen, &e);
						w.recs = e.recs;
					}
					__dtfl_cursor_map_times(&cur, batch,
					    ascoll_on_time, &w);
					if (w.recs != NULL)
						__dtfl_batch_release(w.recs);
					__dtfl_cursor_step_val(&cur, batch);
				}
				__dtfl_cursor_step_key(&cur, batch);
			}
			/*
			 * The batch pointer record is a borrow of a batch owned by
			 * the trace (or a listener queue for an import); it is not
			 * released here.
			 */
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
ascoll_free(void *vctx)
{
	ascoll_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	dtfl_type_free(c->out_type);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_arranged_as_collection(dtfl_arranged *a, const dtfl_type *out_type,
    dtfl_arranged_kv_fn fn, dtfl_ctx ctx)
{
	ascoll_ctx *c = __dtfl_alloc(sizeof *c);
	const dtfl_type *ut = __dtfl_update_type(out_type, a->depth,
	    a->diff->cls->width);
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(a->stream),
	    "AsCollection");
	dtfl_stream *st;

	c->fn = fn;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	c->out_type = dtfl_type_retain(out_type);
	c->depth = a->depth;
	c->dw = a->diff->cls->width;
	dtfl_op_builder_input(b, a->stream, dtfl_pact_pipeline());
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, ascoll_logic, c, ascoll_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, out_type, dtfl_diff_retain(a->diff),
	    a->depth);
}

/* ------------------------------------------------------------------ */
/* enter / leave / region.                                             */
/* ------------------------------------------------------------------ */

/* The batch-pointer transform an enter/leave operator applies per record. */
typedef enum cross_kind {
	CROSS_REGION = 0,	/* same time class: pass the pointer through */
	CROSS_ENTER,		/* to_inner: __dtfl_batch_enter */
	CROSS_LEAVE		/* to_outer: handled as region here (same words) */
} cross_kind;

typedef struct cross_ctx {
	cross_kind kind;
	const dtfl_time_class *inner;	/* CROSS_ENTER target class */
	__dtfl_vec owned;		/* fresh entered batches, freed at teardown */
} cross_ctx;

static bool
cross_batch_logic(dtfl_op *op, void *vctx)
{
	cross_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *recp = dtfl_message_record(m, i, &len);
			__dtfl_batch_ord *in, *out;
			unsigned char *dst;

			memcpy(&in, recp, sizeof in);
			/*
			 * The incoming record is a borrow (owned by the source
			 * trace).  A region passes the same borrow through; an
			 * enter builds a fresh batch with lifted times, which this
			 * operator owns and frees at teardown, keeping the emitted
			 * record a borrow like every other batch-pointer record.
			 */
			if (c->kind == CROSS_ENTER) {
				out = __dtfl_batch_enter(in, c->inner);
				__dtfl_vec_push(&c->owned, &out);
			} else {
				out = in;
			}
			dst = __dtfl_session_reserve(s, DTFL_BATCH_PTR_SIZE);
			memcpy(dst, &out, sizeof out);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
cross_free(void *vctx)
{
	cross_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->owned.len; i++)
		__dtfl_batch_ord_release(*DTFL_VEC_AT(&c->owned,
		    __dtfl_batch_ord *, i));
	__dtfl_vec_free(&c->owned);
	__dtfl_free(c);
}

static dtfl_arranged *
cross_impl(dtfl_arranged *a, dtfl_stream *crossed, dtfl_scope *to_scope,
    cross_kind kind, const char *name)
{
	const dtfl_time_class *to = dtfl_scope_time_class(to_scope);
	cross_ctx *cc = __dtfl_alloc(sizeof *cc);
	dtfl_arranged *out = __dtfl_alloc(sizeof *out);
	dtfl_op_builder *b;

	cc->kind = kind;
	cc->inner = to;
	__dtfl_vec_init(&cc->owned, sizeof(__dtfl_batch_ord *));
	b = dtfl_op_builder_new(to_scope, name);
	dtfl_op_builder_input(b, crossed, dtfl_pact_pipeline());
	out->stream = dtfl_op_builder_output(b, __dtfl_batch_ptr_type());
	dtfl_op_builder_build(b, cross_batch_logic, cc, cross_free);

	__dtfl_trace_agent_clone(&a->agent, &out->agent);
	out->type = dtfl_type_retain(a->type);
	out->diff = dtfl_diff_retain(a->diff);
	out->depth = dtfl_time_depth(to);
	out->owns_agent = true;
	return out;
}

dtfl_arranged *
dtfl_arranged_enter_region(dtfl_arranged *a, dtfl_scope *region)
{
	dtfl_stream *inner = dtfl_stream_enter(a->stream, region);

	return cross_impl(a, inner, region, CROSS_REGION, "Enter");
}

dtfl_arranged *
dtfl_arranged_leave_region(dtfl_arranged *a)
{
	dtfl_scope *outer = dtfl_scope_parent(dtfl_stream_scope(a->stream));
	dtfl_stream *out = dtfl_stream_leave(a->stream);

	return cross_impl(a, out, outer, CROSS_REGION, "Leave");
}

dtfl_arranged *
dtfl_arranged_enter(dtfl_arranged *a, dtfl_scope *child)
{
	dtfl_stream *inner = dtfl_stream_enter(a->stream, child);

	return cross_impl(a, inner, child, CROSS_ENTER, "Enter");
}

dtfl_arranged *
dtfl_arranged_leave(dtfl_arranged *a)
{
	dtfl_scope *outer = dtfl_scope_parent(dtfl_stream_scope(a->stream));
	dtfl_stream *out = dtfl_stream_leave(a->stream);

	/*
	 * leave returns to the parent's time class.  The batches entered the
	 * child by appending inner coordinates with to_inner; leaving drops
	 * them with to_outer.  Since arrange_by times are at the child's
	 * minimum inner coordinate, the words at the parent are the leading
	 * outer words, which the parent reads directly.  No per-batch rewrite
	 * is needed for the regions and single-level product scopes P7
	 * exercises; a general to_outer batch rewrite lands with the P8 join
	 * and reduce consumers that leave non-trivial inner times.
	 */
	return cross_impl(a, out, outer, CROSS_LEAVE, "Leave");
}

/* ------------------------------------------------------------------ */
/* Trace handle and import.                                            */
/* ------------------------------------------------------------------ */

dtfl_trace *
dtfl_arranged_trace(const dtfl_arranged *a)
{
	dtfl_trace *t = __dtfl_alloc(sizeof *t);

	__dtfl_trace_agent_clone(&a->agent, &t->agent);
	t->type = dtfl_type_retain(a->type);
	t->diff = dtfl_diff_retain(a->diff);
	t->depth = a->depth;
	t->keyed = a->agent.keyed;
	return t;
}

/*
 * A second independent handle onto the same shared trace: clones the agent
 * (so the clone carries its own compaction holds) and retains the type and
 * diff.  Used where one trace must be shared by several owners, such as a
 * CollectionIndex handing a copy to each extender it builds.
 */
dtfl_trace *
__dtfl_trace_clone(const dtfl_trace *src)
{
	dtfl_trace *t = __dtfl_alloc(sizeof *t);

	__dtfl_trace_agent_clone(&src->agent, &t->agent);
	t->type = dtfl_type_retain(src->type);
	t->diff = dtfl_diff_retain(src->diff);
	t->depth = src->depth;
	t->keyed = src->keyed;
	return t;
}

void
dtfl_trace_free(dtfl_trace *t)
{
	if (t == NULL)
		return;
	__dtfl_trace_agent_free(&t->agent);
	dtfl_type_free(t->type);
	dtfl_diff_free(t->diff);
	__dtfl_free(t);
}

/* The import source operator: drains its replay queue, emitting batches. */
typedef struct import_ctx {
	__dtfl_replay_queue *queue;	/* retained (strong reference) */
	dtfl_cap *cap;			/* persistent output capability */
	bool activator_set;
	__dtfl_vec owned;		/* emitted batches, freed at teardown */
	const dtfl_time_class *tc;
	unsigned depth;
} import_ctx;

static bool
import_logic(dtfl_op *op, void *vctx)
{
	import_ctx *c = vctx;
	size_t i;

	/*
	 * Register this source's activator on the listener queue the first
	 * time we run, so the writer wakes us when it pushes new batches
	 * (agent.rs new_listener installs scope.activator_for(info.address)).
	 */
	if (!c->activator_set) {
		c->queue->activator = dtfl_op_activator(op);
		c->activator_set = true;
	}

	/* Grab the initial capability once; it persists across reschedules. */
	if (c->cap == NULL)
		c->cap = dtfl_op_initial_cap(op, 0);
	if (c->cap == NULL)
		return false;	/* capability already dropped: done */

	/*
	 * Drain the replay queue: a Frontier instruction downgrades our
	 * capability; an empty frontier (end of time) drops it, after which
	 * we are done.  A Batch instruction emits the batch under a
	 * capability delayed to its hint time (agent.rs import_core).
	 */
	for (i = 0; i < c->queue->instrs.len; i++) {
		__dtfl_replay_instr *in = DTFL_VEC_AT(&c->queue->instrs,
		    __dtfl_replay_instr, i);

		if (in->kind == DTFL_REPLAY_FRONTIER) {
			if (__dtfl_antichain_len(&in->frontier) == 0) {
				/* End of time: drop the capability. */
				if (c->cap != NULL) {
					dtfl_cap_drop(c->cap);
					c->cap = NULL;
				}
			} else if (c->cap != NULL) {
				dtfl_cap_downgrade(c->cap,
				    __dtfl_antichain_at(&in->frontier, 0));
			}
		} else if (c->cap != NULL && in->has_hint &&
		    !__dtfl_batch_ord_is_empty(in->batch)) {
			dtfl_cap *d = dtfl_cap_delayed(c->cap, in->hint_time);
			dtfl_session *s = dtfl_op_session(op, 0, d);
			__dtfl_batch_ord *b = __dtfl_batch_ord_retain(in->batch);
			unsigned char *dst = __dtfl_session_reserve(s,
			    DTFL_BATCH_PTR_SIZE);

			/*
			 * The emitted record is a borrow; this operator owns the
			 * batch until teardown, outliving any later consumer read,
			 * so the batch survives the queue drain below.
			 */
			__dtfl_vec_push(&c->owned, &b);
			memcpy(dst, &b, sizeof b);
			dtfl_session_close(s);
			dtfl_cap_drop(d);
		}
	}
	/* Consume the instructions (release their held references). */
	for (i = 0; i < c->queue->instrs.len; i++) {
		__dtfl_replay_instr *in = DTFL_VEC_AT(&c->queue->instrs,
		    __dtfl_replay_instr, i);

		if (in->kind == DTFL_REPLAY_BATCH)
			__dtfl_batch_ord_release(in->batch);
		else
			__dtfl_antichain_free(&in->frontier);
	}
	__dtfl_vec_clear(&c->queue->instrs);
	/*
	 * Return false (idle) once the queue is drained: there is no more work
	 * to do until the writer pushes again, at which point it activates us.
	 * Returning true unconditionally would spin the worker forever while
	 * we hold a capability.  We keep the capability (so the output frontier
	 * does not race ahead of batches still to come) until the writer's
	 * end-of-time seal drops it above.
	 */
	return false;
}

static void
import_free(void *vctx)
{
	import_ctx *c = vctx;
	size_t i;

	if (c->cap != NULL)
		dtfl_cap_drop(c->cap);
	for (i = 0; i < c->owned.len; i++)
		__dtfl_batch_ord_release(*DTFL_VEC_AT(&c->owned,
		    __dtfl_batch_ord *, i));
	__dtfl_vec_free(&c->owned);
	/* Mark the queue dropped so the writer prunes it, then release. */
	c->queue->dropped = true;
	__dtfl_replay_queue_release(c->queue);
	__dtfl_free(c);
}

dtfl_arranged *
dtfl_trace_import(dtfl_trace *t, dtfl_scope *scope, const char *name)
{
	dtfl_arranged *a = __dtfl_alloc(sizeof *a);
	import_ctx *c = __dtfl_alloc(sizeof *c);
	dtfl_op_builder *b = dtfl_op_builder_new(scope,
	    name != NULL ? name : "ArrangedSource");
	dtfl_activator *act;

	/* The import clones the agent, as import_core does (self.clone()). */
	__dtfl_trace_agent_clone(&t->agent, &a->agent);

	/* A source operator: no inputs, one batch-pointer output. */
	a->stream = dtfl_op_builder_output(b, __dtfl_batch_ptr_type());
	c->cap = NULL;
	c->activator_set = false;
	__dtfl_vec_init(&c->owned, sizeof(__dtfl_batch_ord *));
	c->tc = t->agent.tc;
	c->depth = t->depth;
	/*
	 * Attach a listener seeded with the current trace batches.  The
	 * operator's activator reschedules it when the writer pushes more.
	 */
	dtfl_op_builder_build(b, import_logic, c, import_free);
	act = NULL;		/* activator obtained inside build if needed */
	c->queue = __dtfl_trace_agent_new_listener(&a->agent, act);

	a->type = dtfl_type_retain(t->type);
	a->diff = dtfl_diff_retain(t->diff);
	a->depth = t->depth;
	a->owns_agent = true;
	return a;
}

/* ------------------------------------------------------------------ */
/* Accessors and free.                                                 */
/* ------------------------------------------------------------------ */

const dtfl_type *
dtfl_arranged_type(const dtfl_arranged *a)
{
	return a->type;
}

const dtfl_diff *
dtfl_arranged_diff(const dtfl_arranged *a)
{
	return a->diff;
}

dtfl_scope *
dtfl_arranged_scope(const dtfl_arranged *a)
{
	return dtfl_stream_scope(a->stream);
}

dtfl_stream *
__dtfl_arranged_stream(const dtfl_arranged *a)
{
	return a->stream;
}

__dtfl_trace_agent *
__dtfl_arranged_agent(const dtfl_arranged *a)
{
	return (__dtfl_trace_agent *)&a->agent;
}

unsigned
__dtfl_arranged_depth(const dtfl_arranged *a)
{
	return a->depth;
}

void
dtfl_arranged_free(dtfl_arranged *a)
{
	if (a == NULL)
		return;
	if (a->owns_agent)
		__dtfl_trace_agent_free(&a->agent);
	dtfl_type_free(a->type);
	dtfl_diff_free(a->diff);
	__dtfl_free(a);
}

/* ------------------------------------------------------------------ */
/* Internal construction, shared with reduce (arrange_int.h).          */
/* ------------------------------------------------------------------ */

dtfl_arranged *
__dtfl_arranged_from_agent(dtfl_stream *stream, __dtfl_trace_agent *agent,
    const dtfl_type *type, dtfl_diff *diff, unsigned depth, bool keyed)
{
	dtfl_arranged *a = __dtfl_alloc(sizeof *a);

	(void)keyed;	/* agent->keyed already records this */
	a->stream = stream;
	a->agent = *agent;		/* move the agent in */
	a->type = dtfl_type_retain(type);
	a->diff = diff;			/* ownership transferred by caller */
	a->depth = depth;
	a->owns_agent = true;
	return a;
}

const __dtfl_diff_class *
__dtfl_arranged_diff_class(const dtfl_arranged *a)
{
	return a->diff->cls;
}

bool
__dtfl_arranged_keyed(const dtfl_arranged *a)
{
	return a->agent.keyed;
}
