/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/backend/vec.rs, interactive/src/backend/mod.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/backend_vec.c: the vec rendering substrate and the scope-tree
 * walk.
 *
 * A collection element is a (key, val) pair of DDIR Values, serialized to
 * a self-describing byte string whose dtfl_type cmp reproduces the derived
 * Ord on Value (so join-key matching, min and distinct are structural).
 * The leaf operators (linear, join, reduce, arrange, concat, inspect) are
 * built from the collection/arrange/reduce/iterate C API with the
 * tree-walking Term interpreter (dd_eval) as the scalar engine.  The
 * scope-tree walk renders items in order; the whole program runs in one
 * iterative dynamic scope whose time is Product<u64, PointStamp<u64>>
 * (dynamic/mod.rs), a nested { } rendering as a region at the next
 * PointStamp coordinate, exactly as backend/vec.rs does.
 *
 * The Linear chain is one operator over the raw (record, time, diff)
 * updates: it runs the op sequence per update and emits each result at
 * time join(t_in, t_delta), where t_delta is the minimum except for
 * EnterAt (which delays the PointStamp coordinate at the op's level) and
 * the value gain of LiftIter (which folds that coordinate into the value).
 */
#include "ddir_int.h"
#include "backend_int.h"

#include "../base/base_int.h"

#include "dd/collection_int.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/reduce.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

/* The word offset of PointStamp coordinate 0 inside a time class: the outer
 * depth for a Product<outer, PointStamp>, or 0 for a bare PointStamp. */
static unsigned
dd_pointstamp_word(const dtfl_time_class *tc)
{
	if (tc->shape == DTFL_SHAPE_PRODUCT && tc->b != NULL &&
	    tc->b->shape == DTFL_SHAPE_POINTSTAMP)
		return tc->a->depth;
	if (tc->shape == DTFL_SHAPE_POINTSTAMP)
		return 0;
	__dtfl_panic("dd_render_linear: not a PointStamp scope");
}

/* ------------------------------------------------------------------ */
/* Value serialization                                                 */
/* ------------------------------------------------------------------ */

/* Tag bytes, chosen so a plain byte compare is NOT relied on; the type's
 * cmp decodes and uses dd_value_cmp. */
enum { ENC_INT = 0, ENC_TUPLE = 1, ENC_VARIANT = 2, ENC_LIST = 3 };

static void
enc_byte(uint8_t **buf, size_t *len, size_t *cap, uint8_t b)
{
	if (*len == *cap) {
		*cap = *cap ? *cap * 2 : 32;
		*buf = __dtfl_realloc(*buf, *cap);
	}
	(*buf)[(*len)++] = b;
}

static void
enc_u32(uint8_t **buf, size_t *len, size_t *cap, uint32_t v)
{
	int i;

	for (i = 0; i < 4; i++)
		enc_byte(buf, len, cap, (uint8_t)(v >> (8 * i)));
}

static void
enc_i64(uint8_t **buf, size_t *len, size_t *cap, int64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		enc_byte(buf, len, cap, (uint8_t)((uint64_t)v >> (8 * i)));
}

void
dd_value_encode(const dd_value *v, uint8_t **buf, size_t *len, size_t *cap)
{
	size_t i;

	switch (v->kind) {
	case DD_V_INT:
		enc_byte(buf, len, cap, ENC_INT);
		enc_i64(buf, len, cap, v->u.int_v);
		break;
	case DD_V_TUPLE:
	case DD_V_LIST:
		enc_byte(buf, len, cap,
		    v->kind == DD_V_TUPLE ? ENC_TUPLE : ENC_LIST);
		enc_u32(buf, len, cap, (uint32_t)v->u.seq.len);
		for (i = 0; i < v->u.seq.len; i++)
			dd_value_encode(v->u.seq.items[i], buf, len, cap);
		break;
	case DD_V_VARIANT:
		enc_byte(buf, len, cap, ENC_VARIANT);
		enc_u32(buf, len, cap, v->u.variant.tag);
		dd_value_encode(v->u.variant.payload, buf, len, cap);
		break;
	}
}

static uint32_t
dec_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t
dec_i64(const uint8_t *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return (int64_t)v;
}

dd_value *
dd_value_decode(const uint8_t **p, const uint8_t *end)
{
	uint8_t tag;

	DTFL_ASSERT(*p < end);
	tag = *(*p)++;
	switch (tag) {
	case ENC_INT: {
		int64_t n;

		DTFL_ASSERT(*p + 8 <= end);
		n = dec_i64(*p);
		*p += 8;
		return dd_value_int(n);
	}
	case ENC_TUPLE:
	case ENC_LIST: {
		uint32_t n, i;
		dd_value **items;

		DTFL_ASSERT(*p + 4 <= end);
		n = dec_u32(*p);
		*p += 4;
		items = n ? __dtfl_alloc(n * sizeof(dd_value *)) : NULL;
		for (i = 0; i < n; i++)
			items[i] = dd_value_decode(p, end);
		return tag == ENC_TUPLE ? dd_value_tuple(items, n)
					: dd_value_list(items, n);
	}
	case ENC_VARIANT: {
		uint32_t t;
		dd_value *payload;

		DTFL_ASSERT(*p + 4 <= end);
		t = dec_u32(*p);
		*p += 4;
		payload = dd_value_decode(p, end);
		return dd_value_variant(t, payload);
	}
	default:
		__dtfl_panic("dd_value_decode: bad tag %u", (unsigned)tag);
	}
}

/* The byte length of one serialized value at p (SIZE_MAX if malformed). */
static size_t
value_len(const dtfl_type *t, const void *p, size_t avail)
{
	const uint8_t *base = p;
	const uint8_t *q = base;
	const uint8_t *end = base + avail;

	(void)t;
	/* Walk the structure without allocating. */
	{
		/* Iterative stack of remaining sub-values to skip. */
		size_t pending = 1;

		while (pending > 0) {
			uint8_t tag;

			if (q >= end)
				return SIZE_MAX;
			tag = *q++;
			pending--;
			switch (tag) {
			case ENC_INT:
				if (q + 8 > end)
					return SIZE_MAX;
				q += 8;
				break;
			case ENC_TUPLE:
			case ENC_LIST: {
				uint32_t n;

				if (q + 4 > end)
					return SIZE_MAX;
				n = dec_u32(q);
				q += 4;
				pending += n;
				break;
			}
			case ENC_VARIANT:
				if (q + 4 > end)
					return SIZE_MAX;
				q += 4;
				pending += 1;
				break;
			default:
				return SIZE_MAX;
			}
		}
	}
	return (size_t)(q - base);
}

/* Three-way compare: decode both and use the structural dd_value_cmp. */
static int
value_cmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	const uint8_t *pa = a, *pb = b;
	dd_value *va = dd_value_decode(&pa, pa + alen);
	dd_value *vb = dd_value_decode(&pb, pb + blen);
	int c;

	(void)t;
	c = dd_value_cmp(va, vb);
	dd_value_free(va);
	dd_value_free(vb);
	return c;
}

/* The pair (key, val): two serialized values back to back.  Ordered key
 * first then val, which value_cmp over the whole span gives for free since
 * the key is a complete self-describing prefix. */
static size_t
pair_len(const dtfl_type *t, const void *p, size_t avail)
{
	size_t k = value_len(t, p, avail);
	size_t v;

	if (k == SIZE_MAX || k > avail)
		return SIZE_MAX;
	v = value_len(t, (const uint8_t *)p + k, avail - k);
	if (v == SIZE_MAX)
		return SIZE_MAX;
	return k + v;
}

static int
pair_cmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	const uint8_t *pa = a, *pb = b;
	dd_value *ak, *av, *bk, *bv;
	int c;

	(void)t;
	ak = dd_value_decode(&pa, (const uint8_t *)a + alen);
	av = dd_value_decode(&pa, (const uint8_t *)a + alen);
	bk = dd_value_decode(&pb, (const uint8_t *)b + blen);
	bv = dd_value_decode(&pb, (const uint8_t *)b + blen);
	c = dd_value_cmp(ak, bk);
	if (c == 0)
		c = dd_value_cmp(av, bv);
	dd_value_free(ak);
	dd_value_free(av);
	dd_value_free(bk);
	dd_value_free(bv);
	return c;
}

/* Static type descriptors (no ctx; the fns are stateless).  const so they
 * land in read-only data, not the writable-globals set. */
static const dtfl_type dd_value_type_desc = {
	"ddir_value", 0, value_len, value_cmp, NULL, NULL, NULL, NULL, NULL
};
static const dtfl_type dd_pair_type_desc = {
	"ddir_pair", 0, pair_len, pair_cmp, NULL, NULL, NULL, NULL, NULL
};

const dtfl_type *
dd_value_type(void)
{
	return &dd_value_type_desc;
}

const dtfl_type *
dd_pair_type(void)
{
	return &dd_pair_type_desc;
}

/* Encode a (key, val) pair record into a fresh buffer; caller frees. */
static uint8_t *
encode_pair(const dd_value *k, const dd_value *v, size_t *out_len)
{
	uint8_t *buf = NULL;
	size_t len = 0, cap = 0;

	dd_value_encode(k, &buf, &len, &cap);
	dd_value_encode(v, &buf, &len, &cap);
	*out_len = len;
	return buf;
}

/* Split a pair record into owned (key, val) values. */
static void
decode_pair(const void *rec, size_t len, dd_value **k, dd_value **v)
{
	const uint8_t *p = rec;
	const uint8_t *end = p + len;

	*k = dd_value_decode(&p, end);
	*v = dd_value_decode(&p, end);
}

/* ------------------------------------------------------------------ */
/* The faithful Linear operator (backend/vec.rs render_linear)         */
/* ------------------------------------------------------------------ */

/* Evaluate `term` against env {key, val} (and possibly more). */
static dd_value *
eval1(const dd_term *term, dd_value **env0, size_t n)
{
	dd_value **env = __dtfl_alloc((n ? n : 1) * sizeof(dd_value *));
	size_t len = n, cap = n ? n : 1, i;
	dd_value *r;

	for (i = 0; i < n; i++)
		env[i] = env0[i];
	r = dd_eval(term, &env, &len, &cap);
	__dtfl_free(env);
	return r;
}

/* append_iter: extend a Tuple with Int(iter), or wrap any other value as
 * (value, iter) (backend/vec.rs append_iter). */
static dd_value *
append_iter(dd_value *val, int64_t iter)
{
	dd_value **items;
	size_t n;

	if (val->kind == DD_V_TUPLE) {
		n = val->u.seq.len;
		items = __dtfl_alloc((n + 1) * sizeof(dd_value *));
		if (n > 0)
			memcpy(items, val->u.seq.items,
			    n * sizeof(dd_value *));
		items[n] = dd_value_int(iter);
		/* reuse val's children; free the shell only. */
		__dtfl_free(val->u.seq.items);
		__dtfl_free(val);
		return dd_value_tuple(items, n + 1);
	}
	items = __dtfl_alloc(2 * sizeof(dd_value *));
	items[0] = val;
	items[1] = dd_value_int(iter);
	return dd_value_tuple(items, 2);
}

/* One intermediate (key, val, t_delta, diff) during the op chain. */
typedef struct lin_row {
	dd_value *k;
	dd_value *v;
	uint64_t tdelta[DTFL_TIME_MAX]; /* time delta words, minimum = all 0 */
	int64_t d;
} lin_row;

typedef struct lin_vec {
	lin_row *data;
	size_t len, cap;
} lin_vec;

static void
lin_push(lin_vec *v, lin_row r)
{
	if (v->len == v->cap) {
		v->cap = v->cap ? v->cap * 2 : 4;
		v->data = __dtfl_realloc(v->data, v->cap * sizeof(lin_row));
	}
	v->data[v->len++] = r;
}

/* The Linear operator context: the op chain and the scope depth (level). */
typedef struct linear_ctx {
	const dtfl_time_class *tc; /* retained */
	unsigned depth;		   /* time words per update */
	size_t dw;		   /* diff width (always 8: i64) */
	unsigned ps_word;	   /* word offset of PointStamp coord 0 */
	size_t level;		   /* the op's scope depth */
	const dd_linear_op *const *ops;
	size_t n_ops;
} linear_ctx;

/* Apply the op chain to one input row at embedded time `t_in`.  Returns the
 * resulting rows in `out` (fresh, caller frees k/v). */
static void
linear_apply(const linear_ctx *c, const dd_value *k0, const dd_value *v0,
    const uint64_t *t_in, lin_vec *out)
{
	lin_vec cur = { 0 };
	size_t oi, i;
	int64_t iter_at_level = 0;
	lin_row seed;

	/* iter_at_level = t_in.inner.get(level - 1), or 0.  PointStamp coord
	 * (level - 1) sits at word ps_word + (level - 1). */
	if (c->level >= 1)
		iter_at_level =
		    (int64_t)t_in[c->ps_word + (c->level - 1)];

	seed.k = dd_value_clone(k0);
	seed.v = dd_value_clone(v0);
	memset(seed.tdelta, 0, sizeof seed.tdelta);
	seed.d = 1;
	lin_push(&cur, seed);

	for (oi = 0; oi < c->n_ops; oi++) {
		const dd_linear_op *op = c->ops[oi];
		lin_vec next = { 0 };

		for (i = 0; i < cur.len; i++) {
			lin_row row = cur.data[i];
			dd_value *env[2];

			env[0] = row.k;
			env[1] = row.v;
			switch (op->kind) {
			case DD_LIN_PROJECT: {
				lin_row nr = row;

				nr.k = eval1(op->proj.key, env, 2);
				nr.v = eval1(op->proj.val, env, 2);
				dd_value_free(row.k);
				dd_value_free(row.v);
				lin_push(&next, nr);
				break;
			}
			case DD_LIN_FILTER: {
				dd_value *r = eval1(op->term, env, 2);
				bool keep = dd_value_truthy(r);

				dd_value_free(r);
				if (keep) {
					lin_push(&next, row);
				} else {
					dd_value_free(row.k);
					dd_value_free(row.v);
				}
				break;
			}
			case DD_LIN_NEGATE: {
				lin_row nr = row;

				nr.d = -row.d;
				lin_push(&next, nr);
				break;
			}
			case DD_LIN_ENTER_AT: {
				dd_value *raw = eval1(op->term, env, 2);
				int64_t rv = dd_value_as_int(raw);
				uint64_t urv = (uint64_t)rv;
				uint64_t delay;
				unsigned lz, used;
				lin_row nr = row;

				dd_value_free(raw);
				/* 256 * (64 - leading_zeros(raw)). */
				lz = urv == 0 ? 64u
				    : (unsigned)__builtin_clzll(urv);
				used = 64u - lz;
				delay = (uint64_t)256 * used;
				/* t_delta = Product(0, PointStamp(coords)) where
				 * coord (level - 1) = delay (saturating). */
				memset(nr.tdelta, 0, sizeof nr.tdelta);
				{
					size_t coord = c->level >= 1
					    ? c->level - 1 : 0;
					nr.tdelta[c->ps_word + coord] = delay;
				}
				lin_push(&next, nr);
				break;
			}
			case DD_LIN_LIFT_ITER: {
				lin_row nr = row;

				nr.v = append_iter(row.v, iter_at_level);
				lin_push(&next, nr);
				break;
			}
			case DD_LIN_FLATMAP: {
				dd_value *list = eval1(op->term, env, 2);
				size_t j;

				if (list->kind != DD_V_LIST)
					__dtfl_panic("flatmap: expected a "
					    "List");
				for (j = 0; j < list->u.seq.len; j++) {
					dd_value **it =
					    __dtfl_alloc(2 * sizeof(dd_value *));
					lin_row nr = row;

					it[0] = dd_value_int((int64_t)j);
					it[1] = dd_value_clone(
					    list->u.seq.items[j]);
					nr.k = dd_value_clone(row.k);
					nr.v = dd_value_tuple(it, 2);
					lin_push(&next, nr);
				}
				dd_value_free(list);
				dd_value_free(row.k);
				dd_value_free(row.v);
				break;
			}
			}
		}
		__dtfl_free(cur.data);
		cur = next;
	}
	for (i = 0; i < cur.len; i++)
		lin_push(out, cur.data[i]);
	__dtfl_free(cur.data);
}

static bool
linear_logic(dtfl_op *op, void *vctx)
{
	linear_ctx *c = vctx;
	dtfl_message *m;
	size_t tw = (size_t)c->depth * sizeof(uint64_t);

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			uint64_t etbuf[DTFL_TIME_MAX];
			const uint64_t *et = __dtfl_update_time_copy(rec,
			    c->depth, c->dw, len, etbuf);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw,
			    len);
			int64_t d_in;
			dd_value *k, *v;
			lin_vec rows = { 0 };
			size_t r;

			memcpy(&d_in, __dtfl_update_diff(rec, c->dw, len),
			    sizeof d_in);
			decode_pair(rec, rlen, &k, &v);
			linear_apply(c, k, v, et, &rows);
			dd_value_free(k);
			dd_value_free(v);
			for (r = 0; r < rows.len; r++) {
				lin_row *lr = &rows.data[r];
				uint64_t out_time[DTFL_TIME_MAX];
				uint8_t *buf;
				size_t blen;
				unsigned char *dst;
				int64_t d_out = d_in * lr->d;

				/* out_time = join(t_in, t_delta).  The embedded
				 * time rides in the record; it needs no
				 * capability of its own unless EnterAt advanced it
				 * past the message capability (t_delta nonzero),
				 * when a delayed capability covers it. */
				dtfl_time_join(c->tc, et, lr->tdelta, out_time);
				buf = encode_pair(lr->k, lr->v, &blen);
				if (dtfl_time_cmp(c->tc, out_time, et) == 0) {
					dst = __dtfl_session_reserve(s,
					    blen + tw + c->dw);
					memcpy(dst, buf, blen);
					memcpy(dst + blen, out_time, tw);
					memcpy(dst + blen + tw, &d_out, c->dw);
				} else {
					dtfl_cap *dc =
					    dtfl_op_input_cap_delayed(op, 0,
					    0, out_time);
					dtfl_session *ds =
					    dtfl_op_session(op, 0, dc);

					dst = __dtfl_session_reserve(ds,
					    blen + tw + c->dw);
					memcpy(dst, buf, blen);
					memcpy(dst + blen, out_time, tw);
					memcpy(dst + blen + tw, &d_out, c->dw);
					dtfl_session_close(ds);
					dtfl_cap_drop(dc);
				}
				__dtfl_free(buf);
				dd_value_free(lr->k);
				dd_value_free(lr->v);
			}
			__dtfl_free(rows.data);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
linear_free(void *vctx)
{
	linear_ctx *c = vctx;

	dtfl_time_class_free(c->tc);
	__dtfl_free(c);
}

/* Render a Linear chain `ops` over `in` at scope depth `level`. */
dtfl_collection *
dd_render_linear(dtfl_collection *in, const dd_vec *ops, size_t level)
{
	dtfl_scope *scope = dtfl_stream_scope(in->stream);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	unsigned depth = in->depth;
	linear_ctx *c = __dtfl_alloc(sizeof *c);
	const dtfl_type *ut;
	dtfl_op_builder *b;
	dtfl_stream *st;

	/* Product<u64, PointStamp<u64>> has the u64 outer at word 0, so the
	 * PointStamp coordinates start at the outer depth. */
	c->tc = dtfl_time_class_retain(tc);
	c->depth = depth;
	c->dw = sizeof(int64_t);
	c->ps_word = dd_pointstamp_word(tc);
	c->level = level;
	c->ops = (const dd_linear_op *const *)ops->data;
	c->n_ops = ops->len;
	ut = __dtfl_update_type(dd_pair_type(), depth, c->dw);
	b = dtfl_op_builder_new(scope, "Linear");
	dtfl_op_builder_input(b, in->stream, dtfl_pact_pipeline());
	/* Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, linear_logic, c, linear_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, dd_pair_type(),
	    dtfl_diff_retain(in->diff), depth);
}

/* ------------------------------------------------------------------ */
/* Join and reduce callbacks                                           */
/* ------------------------------------------------------------------ */

/* Join result: env {key, left_val, right_val} -> projected (key, val). */
void
join_fn(void *ctx, const void *key, size_t klen, const void *v1, size_t v1len,
    const void *v2, size_t v2len, dtfl_join_emit *out)
{
	const dd_projection *proj = ctx;
	const uint8_t *pk = key, *p1 = v1, *p2 = v2;
	dd_value *env[3], *nk, *nv;
	uint8_t *buf;
	size_t blen;

	env[0] = dd_value_decode(&pk, pk + klen);
	env[1] = dd_value_decode(&p1, p1 + v1len);
	env[2] = dd_value_decode(&p2, p2 + v2len);
	nk = eval1(proj->key, env, 3);
	nv = eval1(proj->val, env, 3);
	buf = encode_pair(nk, nv, &blen);
	dtfl_join_emit_record(out, buf, blen);
	__dtfl_free(buf);
	dd_value_free(nk);
	dd_value_free(nv);
	dd_value_free(env[0]);
	dd_value_free(env[1]);
	dd_value_free(env[2]);
}

/* Reduce logics.  Values arrive sorted by the value type's order; diffs are
 * i64. */
void
reduce_min_fn(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	const uint8_t *p = vals[0];
	dd_value *min;
	uint8_t *buf = NULL;
	size_t blen = 0, cap = 0;
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)diffs;
	/* The first value is the minimum (DD hands them sorted). */
	min = dd_value_decode(&p, (const uint8_t *)vals[0] + vlens[0]);
	dd_value_encode(min, &buf, &blen, &cap);
	dtfl_reduce_emit_pair(out, buf, blen, &one);
	__dtfl_free(buf);
	dd_value_free(min);
	(void)n;
}

void
reduce_distinct_fn(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	dd_value *unit = dd_value_unit();
	uint8_t *buf = NULL;
	size_t blen = 0, cap = 0;
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)n; (void)vals; (void)vlens;
	(void)diffs;
	dd_value_encode(unit, &buf, &blen, &cap);
	dtfl_reduce_emit_pair(out, buf, blen, &one);
	__dtfl_free(buf);
	dd_value_free(unit);
}

void
reduce_count_fn(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t count = 0, one = 1;
	size_t i;
	const int64_t *d = diffs;
	dd_value **items;
	dd_value *tup;
	uint8_t *buf = NULL;
	size_t blen = 0, cap = 0;

	(void)ctx; (void)key; (void)klen; (void)vals; (void)vlens;
	for (i = 0; i < n; i++)
		count += d[i];
	if (count <= 0)
		return;
	items = __dtfl_alloc(sizeof(dd_value *));
	items[0] = dd_value_int(count);
	tup = dd_value_tuple(items, 1);
	dd_value_encode(tup, &buf, &blen, &cap);
	dtfl_reduce_emit_pair(out, buf, blen, &one);
	__dtfl_free(buf);
	dd_value_free(tup);
}

void
reduce_collect_fn(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	const int64_t *d = diffs;
	dd_vec items = { 0 };
	dd_value *list;
	uint8_t *buf = NULL;
	size_t blen = 0, cap = 0, i;
	int64_t j, one = 1;

	(void)ctx; (void)key; (void)klen;
	for (i = 0; i < n; i++) {
		const uint8_t *p = vals[i];
		dd_value *v = dd_value_decode(&p,
		    (const uint8_t *)vals[i] + vlens[i]);

		for (j = 0; j < d[i]; j++)
			dd_vec_push(&items, dd_value_clone(v));
		dd_value_free(v);
	}
	list = dd_value_list((dd_value **)(void *)items.data, items.len);
	dd_value_encode(list, &buf, &blen, &cap);
	dtfl_reduce_emit_pair(out, buf, blen, &one);
	__dtfl_free(buf);
	dd_value_free(list);
}

/* ------------------------------------------------------------------ */
/* evaluate: data-in / data-out on a single worker                     */
/* ------------------------------------------------------------------ */

static void
out_acc_add(out_acc *a, dd_value *k, dd_value *v, int64_t d)
{
	size_t i;

	for (i = 0; i < a->n; i++)
		if (dd_value_cmp(a->keys[i], k) == 0 &&
		    dd_value_cmp(a->vals[i], v) == 0) {
			a->diffs[i] += d;
			dd_value_free(k);
			dd_value_free(v);
			return;
		}
	if (a->n == a->cap) {
		a->cap = a->cap ? a->cap * 2 : 16;
		a->keys = __dtfl_realloc(a->keys, a->cap * sizeof(dd_value *));
		a->vals = __dtfl_realloc(a->vals, a->cap * sizeof(dd_value *));
		a->diffs = __dtfl_realloc(a->diffs, a->cap * sizeof(int64_t));
	}
	a->keys[a->n] = k;
	a->vals[a->n] = v;
	a->diffs[a->n] = d;
	a->n++;
}

static void
eval_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	out_acc *a = ctx;
	dd_value *k, *v;
	int64_t d;

	(void)time;
	decode_pair(rec, len, &k, &v);
	memcpy(&d, diff, sizeof d);
	out_acc_add(a, k, v, d);
}

typedef struct eval_build {
	const dtfl_ddir_program *program;
	const dtfl_ddir_row *const *inputs;
	const size_t *input_lens;
	size_t n_inputs;
	dtfl_input_session **sessions; /* one per input */
	out_acc *accs;		       /* one per export */
	dtfl_probe *probe;
	int err;
} eval_build;

/* The iterative subscope build: enter the outer (u64) input collections into
 * the Product<u64, PointStamp<u64>> scope, render the program there, and leave
 * each export back out to the outer scope (backend/vec.rs evaluate). */
typedef struct iter_build {
	eval_build *eb;
	dtfl_collection *const *outer_cols; /* one per input (outer scope) */
	dtfl_collection **leaving;	    /* exports left to the outer (owned) */
	size_t n_exports;
	int err;
} iter_build;

static void
eval_iter_fn(dtfl_scope *inner, void *arg)
{
	iter_build *ib = arg;
	eval_build *eb = ib->eb;
	size_t ne = eb->program->root.exports.len;
	dtfl_collection **entered;
	dtfl_collection **exports;
	size_t i;

	entered = eb->n_inputs
	    ? __dtfl_alloc(eb->n_inputs * sizeof(dtfl_collection *)) : NULL;
	for (i = 0; i < eb->n_inputs; i++)
		entered[i] = dtfl_collection_enter(
		    (dtfl_collection *)ib->outer_cols[i], inner);
	exports = ne ? __dtfl_alloc(ne * sizeof(dtfl_collection *)) : NULL;
	ib->err = dd_render_program(eb->program, inner, entered, eb->n_inputs,
	    exports);
	ib->n_exports = ne;
	ib->leaving = ne ? __dtfl_alloc(ne * sizeof(dtfl_collection *)) : NULL;
	if (ib->err == 0) {
		for (i = 0; i < ne; i++) {
			ib->leaving[i] = dtfl_collection_leave(exports[i]);
			dtfl_collection_free(exports[i]);
		}
	} else {
		for (i = 0; i < ne; i++) {
			if (exports[i] != NULL)
				dtfl_collection_free(exports[i]);
			ib->leaving[i] = NULL;
		}
	}
	for (i = 0; i < eb->n_inputs; i++)
		dtfl_collection_free(entered[i]);
	__dtfl_free(entered);
	__dtfl_free(exports);
}

static void
eval_build_fn(dtfl_scope *scope, void *arg)
{
	eval_build *eb = arg;
	size_t ne = eb->program->root.exports.len;
	dtfl_collection **input_cols;
	dtfl_time_class *inner_tc = dtfl_ddir_time_class();
	iter_build ib;
	size_t i;

	input_cols = eb->n_inputs
	    ? __dtfl_alloc(eb->n_inputs * sizeof(dtfl_collection *)) : NULL;
	for (i = 0; i < eb->n_inputs; i++)
		input_cols[i] = dtfl_input_session_to_collection(
		    eb->sessions[i], scope);

	memset(&ib, 0, sizeof ib);
	ib.eb = eb;
	ib.outer_cols = input_cols;
	/* The one iterative dynamic scope: Product<u64, PointStamp<u64>>. */
	dtfl_scope_scoped(scope, "iterate", inner_tc, eval_iter_fn, &ib);
	eb->err = ib.err;
	if (eb->err == 0) {
		for (i = 0; i < ne; i++) {
			dtfl_collection *ins = dtfl_collection_inspect(
			    ib.leaving[i], eval_sink,
			    DTFL_CTX(&eb->accs[i], NULL));
			dtfl_collection *probed = dtfl_collection_probe_with(ins,
			    eb->probe);

			dtfl_collection_free(probed);
			dtfl_collection_free(ins);
			dtfl_collection_free(ib.leaving[i]);
		}
	} else {
		for (i = 0; i < ne; i++)
			if (ib.leaving != NULL && ib.leaving[i] != NULL)
				dtfl_collection_free(ib.leaving[i]);
	}
	__dtfl_free(ib.leaving);
	for (i = 0; i < eb->n_inputs; i++)
		dtfl_collection_free(input_cols[i]);
	__dtfl_free(input_cols);
	dtfl_time_class_free(inner_tc);
}

/*
 * Evaluate `program` on explicit inputs on a single in-process worker.
 * Writes an out_acc per export into `accs` (caller-provided, zeroed).
 * Returns 0 or a DTFL_E* code.
 */
int
dd_evaluate(const dtfl_ddir_program *program, const dtfl_ddir_row *const *inputs,
    const size_t *input_lens, size_t n_inputs, out_acc *accs)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	eval_build eb;
	size_t i, j;
	uint64_t one = 1;
	int steps, rc;

	memset(&eb, 0, sizeof eb);
	eb.program = program;
	eb.inputs = inputs;
	eb.input_lens = input_lens;
	eb.n_inputs = n_inputs;
	eb.accs = accs;
	eb.probe = dtfl_probe_new();
	eb.sessions = n_inputs
	    ? __dtfl_alloc(n_inputs * sizeof(dtfl_input_session *)) : NULL;
	for (i = 0; i < n_inputs; i++)
		eb.sessions[i] = dtfl_input_session_new(dd_pair_type(), i64);

	rc = dtfl_worker_dataflow(w, "ddir", tc, eval_build_fn, &eb);
	if (rc != 0 || eb.err != 0) {
		for (i = 0; i < n_inputs; i++)
			dtfl_input_session_close(eb.sessions[i]);
		__dtfl_free(eb.sessions);
		dtfl_probe_free(eb.probe);
		dtfl_time_class_free(tc);
		dtfl_diff_free(i64);
		dtfl_worker_free(w);
		return rc != 0 ? rc : eb.err;
	}

	/* Feed each input's rows at time 0, then seal. */
	for (i = 0; i < n_inputs; i++) {
		for (j = 0; j < input_lens[i]; j++) {
			const dtfl_ddir_row *row = &inputs[i][j];
			uint8_t *buf;
			size_t blen;

			buf = encode_pair(row->key, row->val, &blen);
			dtfl_input_session_insert(eb.sessions[i], buf, blen);
			__dtfl_free(buf);
		}
		dtfl_input_session_advance_to(eb.sessions[i], &one);
		dtfl_input_session_flush(eb.sessions[i]);
		dtfl_input_session_close(eb.sessions[i]);
	}
	__dtfl_free(eb.sessions);

	/* Step to quiescence under a bound (a diverging loop is a bug). */
	for (steps = 0; steps < 1000000 && dtfl_worker_step(w); steps++)
		;
	DTFL_ASSERT(steps < 1000000);

	dtfl_probe_free(eb.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(i64);
	dtfl_worker_free(w);
	return 0;
}
