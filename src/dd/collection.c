/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/collection.rs
 *   differential-dataflow/src/lib.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/collection.c: the Collection type and its linear methods.
 *
 * A Collection is a timely stream of (record, time, diff) updates, laid
 * out as record bytes, the scope's time words, then the diff bytes
 * (collection_int.h).  The linear methods port here: map, flat_map,
 * filter, concat, concatenate, negate, consolidate, inspect, explode,
 * delay, enter, leave, probe.  filter, concat, inspect, delay's
 * capability re-timing and probe reuse the timely operator of the same
 * name directly; map, flat_map, negate, explode, delay's embedded-time
 * re-timing and enter/leave are small unary operators here that rebuild
 * each update, since they rewrite the record, time or diff part.  The
 * arrange, join and reduce methods of collection.rs land later (PLAN
 * section 10, P7 and P8).
 *
 * Upstream Collection::consolidate is arrange-backed (collection.rs:974,
 * P7).  Here it exchanges updates by their record and sums each
 * (record, time) run with the consolidation primitive (consolidate.c),
 * holding each epoch back until the input frontier passes it, so the
 * differential model converges across workers without the trace layer.
 */
#include "dd/collection_int.h"
#include "dd/trace/trace_int.h"

#include "progress/time_int.h"
#include "timely/timely_int.h"

/* ------------------------------------------------------------------ */
/* Diff classes                                                        */
/* ------------------------------------------------------------------ */

dtfl_diff *
__dtfl_diff_wrap(const __dtfl_diff_class *cls)
{
	dtfl_diff *d = __dtfl_alloc(sizeof *d);

	__dtfl_arc_init(&d->ref);
	/* The wrapper owns one reference to the class (a no-op retain for a
	 * static built-in); dtfl_diff_free releases it. */
	d->cls = __dtfl_diff_class_retain(cls);
	return d;
}

static dtfl_diff *
diff_builtin(__dtfl_diff_id id)
{
	return __dtfl_diff_wrap(__dtfl_diff_builtin(id));
}

dtfl_diff *dtfl_diff_i64(void) { return diff_builtin(DTFL_DIFF_I64); }
dtfl_diff *dtfl_diff_i32(void) { return diff_builtin(DTFL_DIFF_I32); }
dtfl_diff *dtfl_diff_i16(void) { return diff_builtin(DTFL_DIFF_I16); }
dtfl_diff *dtfl_diff_i8(void) { return diff_builtin(DTFL_DIFF_I8); }
dtfl_diff *dtfl_diff_u64(void) { return diff_builtin(DTFL_DIFF_U64); }
dtfl_diff *dtfl_diff_u32(void) { return diff_builtin(DTFL_DIFF_U32); }
dtfl_diff *dtfl_diff_u16(void) { return diff_builtin(DTFL_DIFF_U16); }
dtfl_diff *dtfl_diff_u8(void) { return diff_builtin(DTFL_DIFF_U8); }
dtfl_diff *dtfl_diff_present(void) { return diff_builtin(DTFL_DIFF_PRESENT); }

/*
 * A user diff class forwards the internal callbacks (which take the
 * __dtfl_diff_class) to the user ops (which take the ops' ctx).  The
 * class's ctx holds the copied dtfl_diff_ops; thunk_* read the ops back
 * out of it and call the user's function with ops->ctx.
 */
static void
thunk_plus_equals(const __dtfl_diff_class *c, void *acc, const void *x)
{
	const dtfl_diff_ops *o = c->ctx;

	o->plus_equals(o->ctx, acc, x);
}

static bool
thunk_is_zero(const __dtfl_diff_class *c, const void *x)
{
	const dtfl_diff_ops *o = c->ctx;

	return o->is_zero(o->ctx, x);
}

static int
thunk_cmp(const __dtfl_diff_class *c, const void *a, const void *b)
{
	const dtfl_diff_ops *o = c->ctx;

	return o->cmp(o->ctx, a, b);
}

static void
thunk_multiply(const __dtfl_diff_class *c, void *acc, const void *x)
{
	const dtfl_diff_ops *o = c->ctx;

	o->multiply(o->ctx, acc, x);
}

static void
thunk_negate(const __dtfl_diff_class *c, void *x)
{
	const dtfl_diff_ops *o = c->ctx;

	o->negate(o->ctx, x);
}

static void
thunk_zero(const __dtfl_diff_class *c, void *out)
{
	const dtfl_diff_ops *o = c->ctx;

	o->zero(o->ctx, out);
}

/* Frees the class's ctx: the copied ops and its own user ctx. */
static void
user_ops_free(void *ctx)
{
	dtfl_diff_ops *o = ctx;

	if (o->ctx_free != NULL)
		o->ctx_free(o->ctx);
	__dtfl_free(o);
}

dtfl_diff *
dtfl_diff_new(const dtfl_diff_ops *ops)
{
	__dtfl_diff_class def = { 0 };
	dtfl_diff_ops *copy;
	const __dtfl_diff_class *cls;
	dtfl_diff *d;

	if (ops->plus_equals == NULL || ops->is_zero == NULL ||
	    ops->cmp == NULL)
		__dtfl_panic("dtfl_diff_new: %s lacks plus_equals, is_zero or "
		    "cmp", ops->name != NULL ? ops->name : "?");
	if (ops->negate != NULL && ops->zero == NULL)
		__dtfl_panic("dtfl_diff_new: %s has negate but no zero",
		    ops->name != NULL ? ops->name : "?");
	copy = __dtfl_memdup(ops, sizeof *ops);
	def.name = ops->name;
	def.width = ops->width;
	def.plus_equals = thunk_plus_equals;
	def.is_zero = thunk_is_zero;
	def.cmp = thunk_cmp;
	def.multiply = ops->multiply != NULL ? thunk_multiply : NULL;
	def.negate = ops->negate != NULL ? thunk_negate : NULL;
	def.zero = ops->zero != NULL ? thunk_zero : NULL;
	def.one = NULL;		/* a user class has no From<i8> one */
	def.type = ops->type;
	def.ctx = copy;
	def.ctx_free = user_ops_free;
	/*
	 * class_new returns the class with one reference; the wrapper takes
	 * its own with retain, so release the creation reference and leave
	 * the wrapper the sole owner.
	 */
	cls = __dtfl_diff_class_new(&def);
	d = __dtfl_diff_wrap(cls);
	__dtfl_diff_class_free(cls);
	return d;
}

dtfl_diff *
dtfl_diff_retain(const dtfl_diff *d)
{
	__dtfl_arc_retain(&((dtfl_diff *)d)->ref);
	return (dtfl_diff *)d;
}

void
dtfl_diff_free(const dtfl_diff *d)
{
	if (d == NULL)
		return;
	if (__dtfl_arc_release(&((dtfl_diff *)d)->ref)) {
		__dtfl_diff_class_free(d->cls);
		__dtfl_free((void *)d);
	}
}

size_t
dtfl_diff_width(const dtfl_diff *d)
{
	return d->cls->width;
}

bool
dtfl_diff_is_abelian(const dtfl_diff *d)
{
	return d->cls->negate != NULL;
}

/* ------------------------------------------------------------------ */
/* Update record type                                                  */
/* ------------------------------------------------------------------ */

typedef struct update_type_ctx {
	const dtfl_type *rec;	/* retained */
	unsigned depth;
	size_t dw;
	size_t tail;		/* depth * 8 + dw */
} update_type_ctx;

static size_t
update_len(const dtfl_type *t, const void *p, size_t avail)
{
	const update_type_ctx *u = t->ctx;
	size_t rlen;

	if (avail < u->tail)
		return SIZE_MAX;
	rlen = dtfl_type_value_len(u->rec, p, avail - u->tail);
	if (rlen == SIZE_MAX || rlen > avail - u->tail)
		return SIZE_MAX;
	return rlen + u->tail;
}

/* Route by the record prefix only, so exchange groups equal records. */
static uint64_t
update_hash(const dtfl_type *t, const void *p, size_t len)
{
	const update_type_ctx *u = t->ctx;
	size_t rlen = __dtfl_update_rec_len(u->depth, u->dw, len);

	return dtfl_type_hash(u->rec, p, rlen);
}

static void
update_type_free(void *vctx)
{
	update_type_ctx *u = vctx;

	dtfl_type_free(u->rec);
	__dtfl_free(u);
}

const dtfl_type *
__dtfl_update_type(const dtfl_type *rec, unsigned depth, size_t dw)
{
	update_type_ctx *u = __dtfl_alloc(sizeof *u);
	dtfl_type def;

	u->rec = dtfl_type_retain(rec);
	u->depth = depth;
	u->dw = dw;
	u->tail = (size_t)depth * sizeof(uint64_t) + dw;

	memset(&def, 0, sizeof def);
	def.name = "update";
	if (rec->fixed_size != 0) {
		def.fixed_size = rec->fixed_size + u->tail;
		def.len = NULL;
	} else {
		def.fixed_size = 0;
		def.len = update_len;
	}
	def.hash = update_hash;
	def.ctx = u;
	def.ctx_free = update_type_free;
	return dtfl_type_new(&def);
}

/* ------------------------------------------------------------------ */
/* Collection handle                                                   */
/* ------------------------------------------------------------------ */

dtfl_collection *
__dtfl_as_collection(dtfl_stream *st, const dtfl_type *rec, dtfl_diff *diff,
    unsigned depth)
{
	dtfl_collection *c = __dtfl_alloc(sizeof *c);

	c->stream = st;
	c->type = dtfl_type_retain(rec);
	c->diff = diff;		/* takes the caller's reference */
	c->depth = depth;
	return c;
}

dtfl_collection *
dtfl_stream_as_collection(dtfl_stream *st, const dtfl_type *rec_type,
    dtfl_diff *diff)
{
	const dtfl_time_class *tc = dtfl_scope_time_class(dtfl_stream_scope(st));

	return __dtfl_as_collection(st, rec_type, diff, dtfl_time_depth(tc));
}

const dtfl_type *
dtfl_collection_type(const dtfl_collection *c)
{
	return c->type;
}

const dtfl_diff *
dtfl_collection_diff(const dtfl_collection *c)
{
	return c->diff;
}

dtfl_scope *
dtfl_collection_scope(const dtfl_collection *c)
{
	return dtfl_stream_scope(c->stream);
}

dtfl_stream *
dtfl_collection_inner(const dtfl_collection *c)
{
	return c->stream;
}

void
dtfl_collection_free(dtfl_collection *c)
{
	if (c == NULL)
		return;
	dtfl_type_free(c->type);
	dtfl_diff_free(c->diff);
	__dtfl_free(c);
}

/* ------------------------------------------------------------------ */
/* Collection emit: writes record + source tail onto a real session    */
/* ------------------------------------------------------------------ */

/*
 * The sink a map/flat_map/explode callback writes through.  Each user
 * record is written to the session followed by the source update's
 * tail: the time words (projected for enter/leave) and the diff bytes.
 * explode multiplies the user's factor by the source diff into the
 * output diff bytes.
 */
struct dtfl_collection_emit {
	dtfl_session *s;	/* real output session; not owned */
	const uint64_t *time;	/* time words to append (to_depth words) */
	unsigned to_depth;
	const void *diff;	/* source diff value, or NULL (present) */
	size_t out_dw;		/* output diff width */
	/* explode only: */
	const __dtfl_diff_class *factor;
	const __dtfl_diff_class *in;	/* source diff class */
};

static void
emit_tail(dtfl_collection_emit *e, const void *rec, size_t len,
    const void *out_diff)
{
	size_t tw = (size_t)e->to_depth * sizeof(uint64_t);
	unsigned char *dst = __dtfl_session_reserve(e->s, len + tw + e->out_dw);

	/* A unit (0-byte) record has a NULL rec pointer; skip the copy. */
	if (len != 0)
		memcpy(dst, rec, len);
	if (tw != 0)
		memcpy(dst + len, e->time, tw);
	if (e->out_dw != 0)
		memcpy(dst + len + tw, out_diff, e->out_dw);
}

void
dtfl_collection_emit_record(dtfl_collection_emit *e, const void *rec,
    size_t len)
{
	emit_tail(e, rec, len, e->diff);
}

void
dtfl_collection_emit_pair(dtfl_collection_emit *e, const void *rec, size_t len,
    const void *factor)
{
	unsigned char out[64];

	/* out = factor * source_diff, in the product class. */
	if (__dtfl_diff_multiply(e->factor, e->in, factor, e->diff, out,
	    &(const __dtfl_diff_class *){ NULL }) != 0)
		__dtfl_panic("Collection::explode: multiply failed");
	emit_tail(e, rec, len, out);
}

/* ------------------------------------------------------------------ */
/* map / flat_map                                                      */
/* ------------------------------------------------------------------ */

typedef struct map_ctx {
	dtfl_collection_map_fn fn;
	void *user;
	void (*user_free)(void *);
	unsigned depth;
	size_t dw;
} map_ctx;

static bool
map_logic(dtfl_op *op, void *vctx)
{
	map_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);
			uint64_t et[DTFL_TIME_MAX];
			dtfl_collection_emit e;

			e.s = s;
			e.time = __dtfl_update_time_copy(rec, c->depth, c->dw,
			    len, et);
			e.to_depth = c->depth;
			e.diff = c->dw != 0 ?
			    __dtfl_update_diff(rec, c->dw, len) : NULL;
			e.out_dw = c->dw;
			c->fn(c->user, rec, rlen, &e);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
map_free(void *vctx)
{
	map_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

static dtfl_collection *
map_impl(dtfl_collection *c, const dtfl_type *out_type,
    dtfl_collection_map_fn fn, dtfl_ctx ctx, const char *name)
{
	map_ctx *mc = __dtfl_alloc(sizeof *mc);
	const dtfl_type *ut = __dtfl_update_type(out_type, c->depth,
	    c->diff->cls->width);
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(c->stream),
	    name);
	dtfl_stream *st;

	mc->fn = fn;
	mc->user = ctx.ptr;
	mc->user_free = ctx.free;
	mc->depth = c->depth;
	mc->dw = c->diff->cls->width;
	dtfl_op_builder_input(b, c->stream, dtfl_pact_pipeline());
	/* Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, map_logic, mc, map_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, out_type, dtfl_diff_retain(c->diff),
	    c->depth);
}

dtfl_collection *
dtfl_collection_map(dtfl_collection *c, const dtfl_type *out_type,
    dtfl_collection_map_fn fn, dtfl_ctx ctx)
{
	return map_impl(c, out_type, fn, ctx, "Map");
}

dtfl_collection *
dtfl_collection_flat_map(dtfl_collection *c, const dtfl_type *out_type,
    dtfl_collection_map_fn fn, dtfl_ctx ctx)
{
	return map_impl(c, out_type, fn, ctx, "FlatMap");
}

/* ------------------------------------------------------------------ */
/* filter                                                              */
/* ------------------------------------------------------------------ */

typedef struct filter_ctx {
	dtfl_collection_pred_fn fn;
	void *user;
	void (*user_free)(void *);
	unsigned depth;
	size_t dw;
} filter_ctx;

static bool
filter_logic(void *vctx, const void *rec, size_t len)
{
	filter_ctx *c = vctx;
	size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);

	return c->fn(c->user, rec, rlen);
}

static void
filter_free(void *vctx)
{
	filter_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_collection_filter(dtfl_collection *c, dtfl_collection_pred_fn fn,
    dtfl_ctx ctx)
{
	filter_ctx *fc = __dtfl_alloc(sizeof *fc);
	dtfl_stream *st;

	fc->fn = fn;
	fc->user = ctx.ptr;
	fc->user_free = ctx.free;
	fc->depth = c->depth;
	fc->dw = c->diff->cls->width;
	st = dtfl_stream_filter(c->stream, filter_logic, DTFL_CTX(fc, filter_free));
	return __dtfl_as_collection(st, c->type, dtfl_diff_retain(c->diff),
	    c->depth);
}

/* ------------------------------------------------------------------ */
/* negate                                                              */
/* ------------------------------------------------------------------ */

typedef struct negate_ctx {
	const __dtfl_diff_class *cls;	/* via the collection diff */
	unsigned depth;
	size_t dw;
} negate_ctx;

static bool
negate_logic(dtfl_op *op, void *vctx)
{
	negate_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			unsigned char *dst = __dtfl_session_reserve(s, len);

			memcpy(dst, rec, len);
			c->cls->negate(c->cls, dst + (len - c->dw));
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

dtfl_collection *
dtfl_collection_negate(dtfl_collection *c)
{
	negate_ctx *nc;
	dtfl_op_builder *b;
	dtfl_stream *st;
	const dtfl_type *ut;

	if (c->diff->cls->negate == NULL)
		__dtfl_panic("Collection::negate: the diff class is not Abelian");
	nc = __dtfl_alloc(sizeof *nc);
	nc->cls = c->diff->cls;
	nc->depth = c->depth;
	nc->dw = c->diff->cls->width;
	ut = __dtfl_update_type(c->type, c->depth, nc->dw);
	b = dtfl_op_builder_new(dtfl_stream_scope(c->stream), "Negate");
	dtfl_op_builder_input(b, c->stream, dtfl_pact_pipeline());
	/* Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, negate_logic, nc, __dtfl_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, c->type, dtfl_diff_retain(c->diff),
	    c->depth);
}

/* ------------------------------------------------------------------ */
/* concat / concatenate                                                */
/* ------------------------------------------------------------------ */

dtfl_collection *
dtfl_collection_concat(dtfl_collection *a, dtfl_collection *b)
{
	dtfl_stream *st = dtfl_stream_concat(a->stream, b->stream);

	return __dtfl_as_collection(st, a->type, dtfl_diff_retain(a->diff),
	    a->depth);
}

dtfl_collection *
dtfl_collection_concatenate(dtfl_scope *scope, dtfl_collection *const *sources,
    size_t n)
{
	dtfl_stream **streams = __dtfl_alloc(__dtfl_mul_size(DTFL_MAX(n, 1),
	    sizeof *streams));
	dtfl_collection *out;
	dtfl_stream *st;
	size_t i;

	for (i = 0; i < n; i++)
		streams[i] = sources[i]->stream;
	st = dtfl_stream_concatenate(scope, streams, n);
	out = __dtfl_as_collection(st, sources[0]->type,
	    dtfl_diff_retain(sources[0]->diff), sources[0]->depth);
	__dtfl_free(streams);
	return out;
}

/* ------------------------------------------------------------------ */
/* inspect                                                             */
/* ------------------------------------------------------------------ */

typedef struct inspect_ctx {
	dtfl_collection_inspect_fn fn;
	void *user;
	void (*user_free)(void *);
	unsigned depth;
	size_t dw;
} inspect_ctx;

static void
inspect_logic(void *vctx, const uint64_t *time, const void *rec, size_t len)
{
	inspect_ctx *c = vctx;
	size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);
	uint64_t buf[DTFL_TIME_MAX];
	const uint64_t *et = __dtfl_update_time_copy(rec, c->depth, c->dw,
	    len, buf);
	const void *diff = __dtfl_update_diff(rec, c->dw, len);

	(void)time;
	c->fn(c->user, rec, rlen, et, c->dw != 0 ? diff : NULL);
}

static void
inspect_free(void *vctx)
{
	inspect_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_collection_inspect(dtfl_collection *c, dtfl_collection_inspect_fn fn,
    dtfl_ctx ctx)
{
	inspect_ctx *ic = __dtfl_alloc(sizeof *ic);
	dtfl_stream *st;

	ic->fn = fn;
	ic->user = ctx.ptr;
	ic->user_free = ctx.free;
	ic->depth = c->depth;
	ic->dw = c->diff->cls->width;
	st = dtfl_stream_inspect(c->stream, inspect_logic,
	    DTFL_CTX(ic, inspect_free));
	return __dtfl_as_collection(st, c->type, dtfl_diff_retain(c->diff),
	    c->depth);
}

/* ------------------------------------------------------------------ */
/* explode                                                             */
/* ------------------------------------------------------------------ */

typedef struct explode_ctx {
	dtfl_collection_explode_fn fn;
	void *user;
	void (*user_free)(void *);
	const __dtfl_diff_class *in;	/* input diff class */
	const __dtfl_diff_class *factor;
	const __dtfl_diff_class *out;	/* product class */
	unsigned depth;
	size_t in_dw;
	size_t out_dw;
} explode_ctx;

static bool
explode_logic(dtfl_op *op, void *vctx)
{
	explode_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->in_dw,
			    len);
			uint64_t et[DTFL_TIME_MAX];
			dtfl_collection_emit e;

			e.s = s;
			e.time = __dtfl_update_time_copy(rec, c->depth, c->in_dw,
			    len, et);
			e.to_depth = c->depth;
			e.diff = __dtfl_update_diff(rec, c->in_dw, len);
			e.out_dw = c->out_dw;
			e.factor = c->factor;
			e.in = c->in;
			c->fn(c->user, rec, rlen, &e);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
explode_free(void *vctx)
{
	explode_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_diff_class_free(c->out);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_collection_explode(dtfl_collection *c, const dtfl_type *out_type,
    const dtfl_diff *factor_diff, dtfl_collection_explode_fn fn, dtfl_ctx ctx)
{
	const __dtfl_diff_class *out_cls =
	    __dtfl_diff_multiply_class(factor_diff->cls, c->diff->cls);
	explode_ctx *ec;
	dtfl_op_builder *b;
	dtfl_stream *st;
	const dtfl_type *ut;
	dtfl_diff *out_diff;

	if (out_cls == NULL) {
		if (ctx.free != NULL)
			ctx.free(ctx.ptr);
		return NULL;
	}
	ec = __dtfl_alloc(sizeof *ec);
	ec->fn = fn;
	ec->user = ctx.ptr;
	ec->user_free = ctx.free;
	ec->in = c->diff->cls;
	ec->factor = factor_diff->cls;
	ec->out = __dtfl_diff_class_retain(out_cls);
	ec->depth = c->depth;
	ec->in_dw = c->diff->cls->width;
	ec->out_dw = out_cls->width;
	out_diff = __dtfl_diff_wrap(out_cls);
	ut = __dtfl_update_type(out_type, c->depth, ec->out_dw);
	b = dtfl_op_builder_new(dtfl_stream_scope(c->stream), "Explode");
	dtfl_op_builder_input(b, c->stream, dtfl_pact_pipeline());
	/* Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, explode_logic, ec, explode_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, out_type, out_diff, c->depth);
}

/* ------------------------------------------------------------------ */
/* delay                                                               */
/* ------------------------------------------------------------------ */

/*
 * delay re-times the timely capability of each message (delay_batch)
 * and the embedded time of each update by the same monotone function.
 * The embedded re-timer is a unary operator here; the capability
 * re-timer reuses timely's delay_batch.
 */
typedef struct delay_ctx {
	dtfl_collection_delay_fn fn;
	void *user;
	void (*user_free)(void *);
	unsigned depth;
	size_t dw;
} delay_ctx;

static void
delay_batch_logic(void *vctx, const uint64_t *time, uint64_t *new_time)
{
	delay_ctx *c = vctx;

	c->fn(c->user, time, new_time);
}

static bool
delay_map_logic(dtfl_op *op, void *vctx)
{
	delay_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);
			unsigned char *dst = __dtfl_session_reserve(s, len);
			size_t tw = (size_t)c->depth * sizeof(uint64_t);
			/*
			 * The embedded time at offset rlen may be unaligned when
			 * rlen is not a multiple of 8; stage it in aligned buffers
			 * for the re-timer.
			 */
			uint64_t src_buf[DTFL_TIME_MAX];
			uint64_t dst_buf[DTFL_TIME_MAX];

			memcpy(dst, rec, len);
			if (tw != 0) {
				memcpy(src_buf,
				    (const unsigned char *)rec + rlen, tw);
				c->fn(c->user, src_buf, dst_buf);
				memcpy(dst + rlen, dst_buf, tw);
			}
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
delay_free(void *vctx)
{
	delay_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_collection_delay(dtfl_collection *c, dtfl_collection_delay_fn fn,
    dtfl_ctx ctx)
{
	delay_ctx *bc = __dtfl_alloc(sizeof *bc);
	delay_ctx *mc = __dtfl_alloc(sizeof *mc);
	const dtfl_type *ut;
	dtfl_op_builder *b;
	dtfl_stream *st;

	bc->fn = fn;
	bc->user = ctx.ptr;
	bc->user_free = NULL;	/* the embedded re-timer owns the user ctx */
	bc->depth = c->depth;
	bc->dw = c->diff->cls->width;
	*mc = *bc;
	mc->user_free = ctx.free;

	st = dtfl_stream_delay_batch(c->stream, delay_batch_logic,
	    DTFL_CTX(bc, delay_free));
	ut = __dtfl_update_type(c->type, c->depth, mc->dw);
	b = dtfl_op_builder_new(dtfl_stream_scope(st), "Delay");
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	/* Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, delay_map_logic, mc, delay_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, c->type, dtfl_diff_retain(c->diff),
	    c->depth);
}

/* ------------------------------------------------------------------ */
/* enter / leave                                                       */
/* ------------------------------------------------------------------ */

/*
 * enter / leave re-time the timely capability (dtfl_stream_enter /
 * leave) and then re-map the embedded time with the same projection, as
 * Collection::enter and leave do with the container's .enter()/.leave().
 */
typedef struct cross_ctx {
	const dtfl_time_class *from;	/* retained */
	const dtfl_time_class *to;	/* retained */
	unsigned from_depth;
	unsigned to_depth;
	size_t dw;
	bool entering;			/* to_inner vs to_outer */
	/* enter_at only: computes the initial inner time per record. */
	dtfl_collection_enter_at_fn logic;
	void *user;
	void (*user_free)(void *);
} cross_ctx;

static bool
cross_logic(dtfl_op *op, void *vctx)
{
	cross_ctx *c = vctx;
	dtfl_message *m;
	size_t to_tw = (size_t)c->to_depth * sizeof(uint64_t);
	size_t from_tw = (size_t)c->from_depth * sizeof(uint64_t);

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->from_depth, c->dw,
			    len);
			size_t out_len = rlen + to_tw + c->dw;
			unsigned char *dst = __dtfl_session_reserve(s, out_len);
			/*
			 * The embedded time sits at offset rlen, which need not
			 * be 8-aligned for a record whose length is not a
			 * multiple of 8 (a tuple of u32s, say).  Stage both the
			 * source and destination times in aligned buffers so the
			 * refinement reads and writes aligned words.
			 */
			uint64_t src_buf[DTFL_TIME_MAX];
			uint64_t dst_buf[DTFL_TIME_MAX];

			memcpy(dst, rec, rlen);
			if (from_tw != 0)
				memcpy(src_buf,
				    (const unsigned char *)rec + rlen, from_tw);
			if (c->entering)
				__dtfl_time_to_inner(c->to, c->from, src_buf,
				    dst_buf);
			else
				__dtfl_time_to_outer(c->from, c->to, src_buf,
				    dst_buf);
			/*
			 * enter_at: the outer time refined by to_inner leaves the
			 * inner coordinates at the minimum; replace them with the
			 * logic's per-record initial inner time (Product::new(time,
			 * initial(data))).
			 */
			if (c->logic != NULL)
				c->logic(c->user, rec, rlen,
				    dst_buf + c->from_depth);
			if (to_tw != 0)
				memcpy(dst + rlen, dst_buf, to_tw);
			if (c->dw != 0)
				memcpy(dst + rlen + to_tw,
				    (const unsigned char *)rec + rlen + from_tw,
				    c->dw);
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

	if (c->user_free != NULL)
		c->user_free(c->user);
	dtfl_time_class_free(c->from);
	dtfl_time_class_free(c->to);
	__dtfl_free(c);
}

static dtfl_collection *
cross_impl(dtfl_collection *c, dtfl_stream *crossed, dtfl_scope *to_scope,
    bool entering, const char *name, dtfl_collection_enter_at_fn logic,
    dtfl_ctx ctx)
{
	const dtfl_time_class *to = dtfl_scope_time_class(to_scope);
	const dtfl_time_class *from =
	    dtfl_scope_time_class(dtfl_stream_scope(c->stream));
	unsigned to_depth = dtfl_time_depth(to);
	cross_ctx *cc = __dtfl_alloc(sizeof *cc);
	const dtfl_type *ut;
	dtfl_op_builder *b;
	dtfl_stream *st;

	cc->from = dtfl_time_class_retain(from);
	cc->to = dtfl_time_class_retain(to);
	cc->from_depth = c->depth;
	cc->to_depth = to_depth;
	cc->dw = c->diff->cls->width;
	cc->entering = entering;
	cc->logic = logic;
	cc->user = ctx.ptr;
	cc->user_free = ctx.free;
	ut = __dtfl_update_type(c->type, to_depth, cc->dw);
	b = dtfl_op_builder_new(to_scope, name);
	dtfl_op_builder_input(b, crossed, dtfl_pact_pipeline());
	/* Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, cross_logic, cc, cross_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, c->type, dtfl_diff_retain(c->diff),
	    to_depth);
}

dtfl_collection *
dtfl_collection_enter(dtfl_collection *c, dtfl_scope *child)
{
	dtfl_stream *inner = dtfl_stream_enter(c->stream, child);

	return cross_impl(c, inner, child, true, "Enter", NULL, DTFL_NOCTX);
}

/*
 * enter_at (Collection::enter_at): enter the child scope, then set each
 * update's inner coordinate to the logic's per-record initial time, so
 * the record appears at Product::new(outer_time, initial(data)) in the
 * child.  The child must directly refine the collection's scope.
 */
dtfl_collection *
dtfl_collection_enter_at(dtfl_collection *c, dtfl_scope *child,
    dtfl_collection_enter_at_fn logic, dtfl_ctx ctx)
{
	dtfl_stream *inner = dtfl_stream_enter(c->stream, child);

	return cross_impl(c, inner, child, true, "EnterAt", logic, ctx);
}

dtfl_collection *
dtfl_collection_leave(dtfl_collection *c)
{
	dtfl_scope *outer_scope =
	    dtfl_scope_parent(dtfl_stream_scope(c->stream));
	dtfl_stream *outer = dtfl_stream_leave(c->stream);

	return cross_impl(c, outer, outer_scope, false, "Leave", NULL,
	    DTFL_NOCTX);
}

/* ------------------------------------------------------------------ */
/* probe                                                               */
/* ------------------------------------------------------------------ */

dtfl_collection *
dtfl_collection_probe_with(dtfl_collection *c, dtfl_probe *probe)
{
	dtfl_stream *st = dtfl_stream_probe_with(c->stream, probe);

	return __dtfl_as_collection(st, c->type, dtfl_diff_retain(c->diff),
	    c->depth);
}

dtfl_collection *
dtfl_collection_probe(dtfl_collection *c, dtfl_probe **probe)
{
	dtfl_probe *p = dtfl_probe_new();

	if (probe != NULL)
		*probe = p;
	return dtfl_collection_probe_with(c, p);
}

/* ------------------------------------------------------------------ */
/* consolidate                                                         */
/* ------------------------------------------------------------------ */

/*
 * Consolidate exchanges updates by their record, then on each worker
 * buffers every (record, time, diff) by the update's embedded
 * differential time, holding a capability delayed to that time.  When
 * the input frontier passes an embedded time, that time's run of
 * updates is consolidated (consolidate.c: one update per (record, time),
 * zeros dropped) and emitted under its capability.  Keying and releasing
 * by the embedded time is correct even when an update is placed at a
 * future time (update_at), since the capability held back for it keeps
 * the output frontier from running ahead.  This is the arrange-free form
 * the P5 model needs; the arrange-backed Collection::consolidate
 * (collection.rs:974) lands in P7.
 */
typedef struct consolidate_ctx {
	const dtfl_type *rec;		/* retained */
	const __dtfl_diff_class *cls;	/* via the output diff; retained */
	dtfl_diff *diff;		/* retained */
	const dtfl_time_class *tc;	/* scope class; not retained */
	unsigned depth;
	size_t dw;
} consolidate_ctx;

/* Per-operator state: one pending batch and capability per embedded time. */
typedef struct consolidate_state {
	consolidate_ctx *c;
	__dtfl_vec caps;	/* dtfl_cap *, one per pending time; owned */
	__dtfl_vec batches;	/* __dtfl_batch *, parallel to caps; owned */
} consolidate_state;

/*
 * Finds the pending batch for embedded time `t`, or creates one and
 * takes ownership of `src_cap`, a capability delayed to `t`.  When an
 * entry already exists the extra capability is dropped.
 */
static __dtfl_batch *
pending_for(consolidate_state *st, const uint64_t *t, dtfl_cap *src_cap)
{
	size_t i, depth = st->c->depth;
	dtfl_cap *cap;
	__dtfl_batch *b;

	for (i = 0; i < st->caps.len; i++) {
		cap = *DTFL_VEC_AT(&st->caps, dtfl_cap *, i);
		if (__dtfl_words_cmp(dtfl_cap_time(cap), t, depth) == 0) {
			dtfl_cap_drop(src_cap);
			return *DTFL_VEC_AT(&st->batches, __dtfl_batch *, i);
		}
	}
	__dtfl_vec_push(&st->caps, &src_cap);
	b = __dtfl_batch_new(depth, (unsigned)st->c->dw);
	__dtfl_vec_push(&st->batches, &b);
	return b;
}

/* Removes pending entry i; the caller has consumed its batch and cap. */
static void
pending_remove(consolidate_state *st, size_t i)
{
	__dtfl_vec_remove(&st->caps, i);
	__dtfl_vec_remove(&st->batches, i);
}

static bool
consolidate_logic(dtfl_op *op, void *vctx)
{
	consolidate_state *st = vctx;
	consolidate_ctx *c = st->c;
	dtfl_message *m;
	size_t i;

	/* No persistent initial capability: holding one would stall the
	 * output frontier at the minimum and the probe would never finish. */
	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* Buffer every update by its embedded differential time, holding a
	 * capability delayed from the current message to that time. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t k = dtfl_message_len(m);

		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);
			uint64_t etbuf[DTFL_TIME_MAX];
			const uint64_t *et = __dtfl_update_time_copy(rec,
			    c->depth, c->dw, len, etbuf);
			const void *diff = c->dw != 0 ?
			    __dtfl_update_diff(rec, c->dw, len) : NULL;
			dtfl_cap *cap = dtfl_op_input_cap_delayed(op, 0, 0, et);
			__dtfl_batch *b = pending_for(st, et, cap);

			__dtfl_batch_push(b, rec, rlen, et, diff);
		}
	}

	/* Release every buffered time the input frontier has passed. */
	i = 0;
	while (i < st->caps.len) {
		dtfl_cap *cap = *DTFL_VEC_AT(&st->caps, dtfl_cap *, i);
		const uint64_t *et = dtfl_cap_time(cap);

		if (dtfl_op_frontier_less_equal(op, 0, et)) {
			i++;
			continue;
		}
		{
			__dtfl_batch *b = *DTFL_VEC_AT(&st->batches,
			    __dtfl_batch *, i);
			dtfl_session *s = dtfl_op_session(op, 0, cap);
			size_t j, bn, tw = (size_t)c->depth * sizeof(uint64_t);

			__dtfl_consolidate_batch(b, c->rec, c->cls);
			bn = __dtfl_batch_len(b);
			for (j = 0; j < bn; j++) {
				size_t rlen;
				const void *rec = __dtfl_batch_rec(b, j, &rlen);
				unsigned char *dst = __dtfl_session_reserve(s,
				    rlen + tw + c->dw);

				/* A unit record has rlen 0 and a NULL pointer. */
				if (rlen != 0)
					memcpy(dst, rec, rlen);
				if (tw != 0)
					memcpy(dst + rlen,
					    __dtfl_batch_time(b, j), tw);
				if (c->dw != 0)
					memcpy(dst + rlen + tw,
					    __dtfl_batch_diff(b, j), c->dw);
			}
			dtfl_session_close(s);
			__dtfl_batch_release(b);
			dtfl_cap_drop(cap);
			pending_remove(st, i);
		}
	}
	return st->caps.len != 0;
}

static void
consolidate_free(void *vctx)
{
	consolidate_state *st = vctx;
	size_t i;

	for (i = 0; i < st->caps.len; i++) {
		__dtfl_batch_release(*DTFL_VEC_AT(&st->batches,
		    __dtfl_batch *, i));
		dtfl_cap_drop(*DTFL_VEC_AT(&st->caps, dtfl_cap *, i));
	}
	__dtfl_vec_free(&st->batches);
	__dtfl_vec_free(&st->caps);
	dtfl_type_free(st->c->rec);
	dtfl_diff_free(st->c->diff);
	__dtfl_free(st->c);
	__dtfl_free(st);
}

dtfl_collection *
dtfl_collection_consolidate(dtfl_collection *c)
{
	consolidate_ctx *cc = __dtfl_alloc(sizeof *cc);
	consolidate_state *st = __dtfl_alloc(sizeof *st);
	dtfl_diff *out_diff = dtfl_diff_retain(c->diff);
	const dtfl_type *ut;
	dtfl_op_builder *b;
	dtfl_stream *out;

	cc->rec = dtfl_type_retain(c->type);
	cc->cls = c->diff->cls;
	cc->diff = dtfl_diff_retain(c->diff);
	cc->tc = dtfl_scope_time_class(dtfl_stream_scope(c->stream));
	cc->depth = c->depth;
	cc->dw = c->diff->cls->width;
	st->c = cc;
	__dtfl_vec_init(&st->caps, sizeof(dtfl_cap *));
	__dtfl_vec_init(&st->batches, sizeof(__dtfl_batch *));

	ut = __dtfl_update_type(c->type, c->depth, cc->dw);
	b = dtfl_op_builder_new(dtfl_stream_scope(c->stream), "Consolidate");
	dtfl_op_builder_input(b, c->stream, dtfl_pact_exchange(NULL, NULL));
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
	out = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, consolidate_logic, st, consolidate_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(out, c->type, out_diff, c->depth);
}

/* ------------------------------------------------------------------ */
/* configure (lib.rs): the default exertion logic.                      */
/* ------------------------------------------------------------------ */

static bool
default_exert(void *ctx, const __dtfl_exert_level *batches, size_t n,
    size_t *effort)
{
	size_t non_empty = 0, i;

	for (i = 0; i < n; i++) {
		if (batches[i].count > 1) {
			*effort = *(const size_t *)ctx;
			return true;
		}
		if (batches[i].length > 0)
			non_empty += 1;
		if (non_empty > 1) {
			*effort = *(const size_t *)ctx;
			return true;
		}
	}
	return false;
}

static void
default_exert_free(void *ctx)
{
	__dtfl_free(ctx);
}

__dtfl_exert_logic
__dtfl_default_exert_logic(size_t effort)
{
	__dtfl_exert_logic l;
	size_t *e = __dtfl_alloc(sizeof *e);

	*e = effort;
	l.fn = default_exert;
	l.ctx = e;
	l.ctx_free = default_exert_free;
	return l;
}
