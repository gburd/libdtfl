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
 * scope-tree walk renders items in order; an iterating scope (one with
 * vars) becomes a nested iterative subscope, a plain scope a region.
 */
#include "ddir_int.h"
#include "backend_int.h"

#include "../base/base_int.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/reduce.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

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
/* Scalar operator callbacks (dd_eval over decoded (key, val) rows)    */
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

/* Project: (key, val) -> (new_key, new_val) via the projection terms. */
void
project_fn(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const dd_projection *proj = ctx;
	dd_value *k, *v, *env[2], *nk, *nv;
	uint8_t *buf;
	size_t blen;

	decode_pair(rec, len, &k, &v);
	env[0] = k;
	env[1] = v;
	nk = eval1(proj->key, env, 2);
	nv = eval1(proj->val, env, 2);
	buf = encode_pair(nk, nv, &blen);
	dtfl_collection_emit_record(out, buf, blen);
	__dtfl_free(buf);
	dd_value_free(nk);
	dd_value_free(nv);
	dd_value_free(k);
	dd_value_free(v);
}

/* Filter: keep rows where the term is truthy. */
bool
filter_fn(void *ctx, const void *rec, size_t len)
{
	const dd_term *term = ctx;
	dd_value *k, *v, *env[2], *r;
	bool keep;

	decode_pair(rec, len, &k, &v);
	env[0] = k;
	env[1] = v;
	r = eval1(term, env, 2);
	keep = dd_value_truthy(r);
	dd_value_free(r);
	dd_value_free(k);
	dd_value_free(v);
	return keep;
}

/* FlatMap: explode a List-valued term into one row per element,
 * value tuple(pos, element), keyed as the input. */
void
flatmap_fn(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const dd_term *term = ctx;
	dd_value *k, *v, *env[2], *list;
	size_t i;

	decode_pair(rec, len, &k, &v);
	env[0] = k;
	env[1] = v;
	list = eval1(term, env, 2);
	if (list->kind != DD_V_LIST)
		__dtfl_panic("flatmap: expected a List");
	for (i = 0; i < list->u.seq.len; i++) {
		dd_value **items = __dtfl_alloc(2 * sizeof(dd_value *));
		dd_value *pair_val, *out_k;
		uint8_t *buf;
		size_t blen;

		items[0] = dd_value_int((int64_t)i);
		items[1] = dd_value_clone(list->u.seq.items[i]);
		pair_val = dd_value_tuple(items, 2);
		out_k = dd_value_clone(k);
		buf = encode_pair(out_k, pair_val, &blen);
		dtfl_collection_emit_record(out, buf, blen);
		__dtfl_free(buf);
		dd_value_free(out_k);
		dd_value_free(pair_val);
	}
	dd_value_free(list);
	dd_value_free(k);
	dd_value_free(v);
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
	list = dd_value_list((dd_value **)items.data, items.len);
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

static void
eval_build_fn(dtfl_scope *scope, void *arg)
{
	eval_build *eb = arg;
	size_t ne = eb->program->root.exports.len;
	dtfl_collection **input_cols;
	dtfl_collection **exports;
	size_t i;

	input_cols = eb->n_inputs
	    ? __dtfl_alloc(eb->n_inputs * sizeof(dtfl_collection *)) : NULL;
	for (i = 0; i < eb->n_inputs; i++)
		input_cols[i] = dtfl_input_session_to_collection(
		    eb->sessions[i], scope);
	exports = ne ? __dtfl_alloc(ne * sizeof(dtfl_collection *)) : NULL;
	eb->err = dd_render_program(eb->program, scope, input_cols,
	    eb->n_inputs, exports);
	if (eb->err == 0) {
		for (i = 0; i < ne; i++) {
			dtfl_collection *ins = dtfl_collection_inspect(
			    exports[i], eval_sink,
			    DTFL_CTX(&eb->accs[i], NULL));
			dtfl_collection *probed = dtfl_collection_probe_with(ins,
			    eb->probe);

			dtfl_collection_free(probed);
			dtfl_collection_free(ins);
			dtfl_collection_free(exports[i]);
		}
	} else {
		for (i = 0; i < ne; i++)
			if (exports[i] != NULL)
				dtfl_collection_free(exports[i]);
	}
	for (i = 0; i < eb->n_inputs; i++)
		dtfl_collection_free(input_cols[i]);
	__dtfl_free(input_cols);
	__dtfl_free(exports);
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
