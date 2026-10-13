/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/ir.rs, interactive/src/lib.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/ir.c: the DDIR value data model and the scalar Term
 * interpreter.
 *
 * Value is an algebraic data type over a single scalar Int, with Tuple,
 * Variant and List constructors; dd_value_cmp implements the structural
 * ordering that Rust derives for Value (variant tag then field by field),
 * which gives min, join-key matching and distinct.  dd_eval walks a Term
 * against an environment stack of Values, mirroring ir::eval: Var indexes
 * the environment absolutely, Bound counts from the innermost pushed
 * binder, and Case/Fold push and pop their binders around sub-evaluation.
 */
#include "ddir_int.h"

#include "../base/base_int.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* dd_vec                                                              */
/* ------------------------------------------------------------------ */

void
dd_vec_push(dd_vec *v, void *p)
{
	if (v->len == v->cap) {
		size_t ncap = v->cap ? v->cap * 2 : 4;
		v->data = __dtfl_realloc(v->data, ncap * sizeof(void *));
		v->cap = ncap;
	}
	v->data[v->len++] = p;
}

void
dd_vec_free(dd_vec *v)
{
	__dtfl_free(v->data);
	v->data = NULL;
	v->len = 0;
	v->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Value                                                               */
/* ------------------------------------------------------------------ */

dd_value *
dd_value_int(int64_t n)
{
	dd_value *v = __dtfl_alloc(sizeof *v);

	v->kind = DD_V_INT;
	v->u.int_v = n;
	return v;
}

dd_value *
dd_value_unit(void)
{
	return dd_value_tuple(NULL, 0);
}

dd_value *
dd_value_tuple(dd_value **items, size_t len)
{
	dd_value *v = __dtfl_alloc(sizeof *v);

	v->kind = DD_V_TUPLE;
	v->u.seq.items = items;
	v->u.seq.len = len;
	return v;
}

dd_value *
dd_value_list(dd_value **items, size_t len)
{
	dd_value *v = __dtfl_alloc(sizeof *v);

	v->kind = DD_V_LIST;
	v->u.seq.items = items;
	v->u.seq.len = len;
	return v;
}

dd_value *
dd_value_variant(uint32_t tag, dd_value *payload)
{
	dd_value *v = __dtfl_alloc(sizeof *v);

	v->kind = DD_V_VARIANT;
	v->u.variant.tag = tag;
	v->u.variant.payload = payload;
	return v;
}

dd_value *
dd_value_clone(const dd_value *v)
{
	switch (v->kind) {
	case DD_V_INT:
		return dd_value_int(v->u.int_v);
	case DD_V_TUPLE:
	case DD_V_LIST: {
		size_t n = v->u.seq.len, i;
		dd_value **items = n ? __dtfl_alloc(n * sizeof(dd_value *)) : NULL;

		for (i = 0; i < n; i++)
			items[i] = dd_value_clone(v->u.seq.items[i]);
		return v->kind == DD_V_TUPLE ? dd_value_tuple(items, n)
					     : dd_value_list(items, n);
	}
	case DD_V_VARIANT:
		return dd_value_variant(v->u.variant.tag,
		    dd_value_clone(v->u.variant.payload));
	}
	__dtfl_panic("dd_value_clone: bad kind");
}

void
dd_value_free(dd_value *v)
{
	if (v == NULL)
		return;
	switch (v->kind) {
	case DD_V_INT:
		break;
	case DD_V_TUPLE:
	case DD_V_LIST: {
		size_t i;

		for (i = 0; i < v->u.seq.len; i++)
			dd_value_free(v->u.seq.items[i]);
		__dtfl_free(v->u.seq.items);
		break;
	}
	case DD_V_VARIANT:
		dd_value_free(v->u.variant.payload);
		break;
	}
	__dtfl_free(v);
}

/*
 * The derived Ord on Value: the enum discriminant orders first
 * (Int < Tuple < Variant < List), then within each the fields compare
 * lexicographically.  Variant compares (tag, payload).  This is what gives
 * join-key equality, min and distinct their meaning.
 */
int
dd_value_cmp(const dd_value *a, const dd_value *b)
{
	if (a->kind != b->kind)
		return a->kind < b->kind ? -1 : 1;
	switch (a->kind) {
	case DD_V_INT:
		if (a->u.int_v != b->u.int_v)
			return a->u.int_v < b->u.int_v ? -1 : 1;
		return 0;
	case DD_V_TUPLE:
	case DD_V_LIST: {
		size_t na = a->u.seq.len, nb = b->u.seq.len;
		size_t n = na < nb ? na : nb, i;

		for (i = 0; i < n; i++) {
			int c = dd_value_cmp(a->u.seq.items[i], b->u.seq.items[i]);

			if (c != 0)
				return c;
		}
		if (na != nb)
			return na < nb ? -1 : 1;
		return 0;
	}
	case DD_V_VARIANT:
		if (a->u.variant.tag != b->u.variant.tag)
			return a->u.variant.tag < b->u.variant.tag ? -1 : 1;
		return dd_value_cmp(a->u.variant.payload, b->u.variant.payload);
	}
	__dtfl_panic("dd_value_cmp: bad kind");
}

bool
dd_value_truthy(const dd_value *v)
{
	return v->kind == DD_V_INT && v->u.int_v != 0;
}

int64_t
dd_value_as_int(const dd_value *v)
{
	if (v->kind != DD_V_INT)
		__dtfl_panic("dd_value_as_int: expected Int");
	return v->u.int_v;
}

/* ------------------------------------------------------------------ */
/* splitmix64 (lib.rs hash_u64)                                        */
/* ------------------------------------------------------------------ */

uint64_t
dd_hash_u64(uint64_t index)
{
	uint64_t x = index * UINT64_C(0x9e3779b97f4a7c15);

	x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
	return x ^ (x >> 31);
}

/* ------------------------------------------------------------------ */
/* Term constructors / clone / free                                    */
/* ------------------------------------------------------------------ */

dd_term *
dd_term_new(dd_term_kind kind)
{
	dd_term *t = __dtfl_calloc(1, sizeof *t);

	t->kind = kind;
	return t;
}

static dd_vec
dd_term_vec_clone(const dd_vec *v)
{
	dd_vec out = { 0 };
	size_t i;

	for (i = 0; i < v->len; i++)
		dd_vec_push(&out, dd_term_clone(v->data[i]));
	return out;
}

static void
dd_term_vec_free(dd_vec *v)
{
	size_t i;

	for (i = 0; i < v->len; i++)
		dd_term_free(v->data[i]);
	dd_vec_free(v);
}

dd_term *
dd_term_clone(const dd_term *t)
{
	dd_term *o = dd_term_new(t->kind);

	switch (t->kind) {
	case DD_T_VAR:
		o->u.var = t->u.var;
		break;
	case DD_T_BOUND:
		o->u.bound = t->u.bound;
		break;
	case DD_T_INT:
		o->u.int_lit = t->u.int_lit;
		break;
	case DD_T_TUPLE:
	case DD_T_HASH: {
		const dd_vec *src = t->kind == DD_T_TUPLE ? &t->u.seq : &t->u.hash;
		dd_vec *dst = t->kind == DD_T_TUPLE ? &o->u.seq : &o->u.hash;

		*dst = dd_term_vec_clone(src);
		break;
	}
	case DD_T_LIST:
		o->u.seq = dd_term_vec_clone(&t->u.seq);
		break;
	case DD_T_SPREAD:
		o->u.spread = dd_term_clone(t->u.spread);
		break;
	case DD_T_PROJ:
		o->u.proj.base = dd_term_clone(t->u.proj.base);
		o->u.proj.index = t->u.proj.index;
		break;
	case DD_T_INJECT:
		o->u.inject.tag = dd_term_clone(t->u.inject.tag);
		o->u.inject.payload = dd_term_clone(t->u.inject.payload);
		break;
	case DD_T_CASE:
		o->u.case_.scrutinee = dd_term_clone(t->u.case_.scrutinee);
		o->u.case_.arms = dd_term_vec_clone(&t->u.case_.arms);
		o->u.case_.dflt = t->u.case_.dflt
		    ? dd_term_clone(t->u.case_.dflt) : NULL;
		break;
	case DD_T_FOLD:
		o->u.fold.list = dd_term_clone(t->u.fold.list);
		o->u.fold.init = dd_term_clone(t->u.fold.init);
		o->u.fold.step = dd_term_clone(t->u.fold.step);
		break;
	case DD_T_IF:
		o->u.if_.cond = dd_term_clone(t->u.if_.cond);
		o->u.if_.then_ = dd_term_clone(t->u.if_.then_);
		o->u.if_.els = dd_term_clone(t->u.if_.els);
		break;
	case DD_T_UNARY:
		o->u.unary.op = t->u.unary.op;
		o->u.unary.tag = t->u.unary.tag;
		o->u.unary.arg = dd_term_clone(t->u.unary.arg);
		break;
	case DD_T_BINARY:
		o->u.binary.op = t->u.binary.op;
		o->u.binary.l = dd_term_clone(t->u.binary.l);
		o->u.binary.r = dd_term_clone(t->u.binary.r);
		break;
	}
	return o;
}

void
dd_term_free(dd_term *t)
{
	if (t == NULL)
		return;
	switch (t->kind) {
	case DD_T_VAR:
	case DD_T_BOUND:
	case DD_T_INT:
		break;
	case DD_T_TUPLE:
	case DD_T_LIST:
		dd_term_vec_free(&t->u.seq);
		break;
	case DD_T_HASH:
		dd_term_vec_free(&t->u.hash);
		break;
	case DD_T_SPREAD:
		dd_term_free(t->u.spread);
		break;
	case DD_T_PROJ:
		dd_term_free(t->u.proj.base);
		break;
	case DD_T_INJECT:
		dd_term_free(t->u.inject.tag);
		dd_term_free(t->u.inject.payload);
		break;
	case DD_T_CASE:
		dd_term_free(t->u.case_.scrutinee);
		dd_term_vec_free(&t->u.case_.arms);
		dd_term_free(t->u.case_.dflt);
		break;
	case DD_T_FOLD:
		dd_term_free(t->u.fold.list);
		dd_term_free(t->u.fold.init);
		dd_term_free(t->u.fold.step);
		break;
	case DD_T_IF:
		dd_term_free(t->u.if_.cond);
		dd_term_free(t->u.if_.then_);
		dd_term_free(t->u.if_.els);
		break;
	case DD_T_UNARY:
		dd_term_free(t->u.unary.arg);
		break;
	case DD_T_BINARY:
		dd_term_free(t->u.binary.l);
		dd_term_free(t->u.binary.r);
		break;
	}
	__dtfl_free(t);
}

dd_projection
dd_projection_clone(const dd_projection *p)
{
	dd_projection out;

	out.key = dd_term_clone(p->key);
	out.val = dd_term_clone(p->val);
	return out;
}

void
dd_projection_free_fields(dd_projection *p)
{
	dd_term_free(p->key);
	dd_term_free(p->val);
	p->key = NULL;
	p->val = NULL;
}

dd_linear_op
dd_linear_op_clone(const dd_linear_op *op)
{
	dd_linear_op out;

	out.kind = op->kind;
	out.proj.key = NULL;
	out.proj.val = NULL;
	out.term = NULL;
	switch (op->kind) {
	case DD_LIN_PROJECT:
		out.proj = dd_projection_clone(&op->proj);
		break;
	case DD_LIN_FILTER:
	case DD_LIN_ENTER_AT:
	case DD_LIN_FLATMAP:
		out.term = dd_term_clone(op->term);
		break;
	case DD_LIN_NEGATE:
	case DD_LIN_LIFT_ITER:
		break;
	}
	return out;
}

void
dd_linear_op_free_fields(dd_linear_op *op)
{
	if (op->kind == DD_LIN_PROJECT)
		dd_projection_free_fields(&op->proj);
	else
		dd_term_free(op->term);
	op->term = NULL;
}

/* ------------------------------------------------------------------ */
/* The Term interpreter: dd_eval (ir::eval)                            */
/* ------------------------------------------------------------------ */

/* Push a value onto the environment stack. */
static void
env_push(dd_value ***env, size_t *len, size_t *cap, dd_value *v)
{
	if (*len == *cap) {
		size_t ncap = *cap ? *cap * 2 : 8;

		*env = __dtfl_realloc(*env, ncap * sizeof(dd_value *));
		*cap = ncap;
	}
	(*env)[(*len)++] = v;
}

/*
 * Resolve a "place" term (a Var/Bound/Proj chain) to a borrowed value in
 * the environment, without cloning.  Returns NULL for terms that build a
 * fresh value.  Mirrors ir::eval_ref.
 */
static const dd_value *
eval_ref(const dd_term *t, dd_value **env, size_t len)
{
	switch (t->kind) {
	case DD_T_VAR:
		return t->u.var < len ? env[t->u.var] : NULL;
	case DD_T_BOUND:
		if (t->u.bound + 1 > len)
			return NULL;
		return env[len - 1 - t->u.bound];
	case DD_T_PROJ: {
		const dd_value *place = eval_ref(t->u.proj.base, env, len);

		if (place == NULL)
			return NULL;
		if ((place->kind == DD_V_TUPLE || place->kind == DD_V_LIST) &&
		    t->u.proj.index < place->u.seq.len)
			return place->u.seq.items[t->u.proj.index];
		return NULL;
	}
	default:
		return NULL;
	}
}

static dd_value *eval_unary(dd_unop op, uint32_t tag, dd_value *v);
static dd_value *eval_binary(dd_binop op, dd_value *l, dd_value *r);

/* Build a tuple/list element sequence, splicing any Spread children. */
static void
build_seq(const dd_vec *fields, dd_value ***env, size_t *len, size_t *cap,
    dd_value ***out_items, size_t *out_len)
{
	dd_vec out = { 0 };
	size_t i;

	for (i = 0; i < fields->len; i++) {
		const dd_term *f = fields->data[i];

		if (f->kind == DD_T_SPREAD) {
			dd_value *inner = dd_eval(f->u.spread, env, len, cap);
			size_t j;

			if (inner->kind != DD_V_TUPLE && inner->kind != DD_V_LIST)
				__dtfl_panic("Spread of non-aggregate value");
			for (j = 0; j < inner->u.seq.len; j++)
				dd_vec_push(&out,
				    dd_value_clone(inner->u.seq.items[j]));
			dd_value_free(inner);
		} else {
			dd_vec_push(&out, dd_eval(f, env, len, cap));
		}
	}
	*out_items = (dd_value **)out.data;
	*out_len = out.len;
}

dd_value *
dd_eval(const dd_term *term, dd_value ***env, size_t *len, size_t *cap)
{
	switch (term->kind) {
	case DD_T_VAR:
		return dd_value_clone((*env)[term->u.var]);
	case DD_T_BOUND:
		return dd_value_clone((*env)[*len - 1 - term->u.bound]);
	case DD_T_INT:
		return dd_value_int(term->u.int_lit);
	case DD_T_TUPLE:
	case DD_T_LIST: {
		dd_value **items;
		size_t n;

		build_seq(&term->u.seq, env, len, cap, &items, &n);
		return term->kind == DD_T_TUPLE ? dd_value_tuple(items, n)
						: dd_value_list(items, n);
	}
	case DD_T_SPREAD:
		__dtfl_panic("Spread is only valid as a direct child of "
		    "Tuple/List");
	case DD_T_PROJ: {
		const dd_value *place = eval_ref(term->u.proj.base, *env, *len);
		dd_value *sel;

		if (place != NULL) {
			if (place->kind != DD_V_TUPLE && place->kind != DD_V_LIST)
				__dtfl_panic("Proj on non-aggregate value");
			if (term->u.proj.index >= place->u.seq.len)
				__dtfl_panic("Proj index out of bounds");
			return dd_value_clone(place->u.seq.items[term->u.proj.index]);
		}
		sel = dd_eval(term->u.proj.base, env, len, cap);
		if (sel->kind != DD_V_TUPLE && sel->kind != DD_V_LIST)
			__dtfl_panic("Proj on non-aggregate value");
		if (term->u.proj.index >= sel->u.seq.len)
			__dtfl_panic("Proj index out of bounds");
		{
			dd_value *r = sel->u.seq.items[term->u.proj.index];

			sel->u.seq.items[term->u.proj.index] = NULL;
			dd_value_free(sel);
			return r;
		}
	}
	case DD_T_INJECT: {
		dd_value *tagv = dd_eval(term->u.inject.tag, env, len, cap);
		uint32_t tag = (uint32_t)dd_value_as_int(tagv);
		dd_value *payload = dd_eval(term->u.inject.payload, env, len, cap);

		dd_value_free(tagv);
		return dd_value_variant(tag, payload);
	}
	case DD_T_CASE: {
		dd_value *scrut = dd_eval(term->u.case_.scrutinee, env, len, cap);
		uint32_t tag;
		dd_value *payload;
		dd_value *r;

		if (scrut->kind != DD_V_VARIANT)
			__dtfl_panic("Case scrutinee is not a Variant");
		tag = scrut->u.variant.tag;
		if (tag < term->u.case_.arms.len) {
			/* Push the payload as Bound(0). */
			payload = scrut->u.variant.payload;
			scrut->u.variant.payload = NULL;
			dd_value_free(scrut);
			env_push(env, len, cap, payload);
			r = dd_eval(term->u.case_.arms.data[tag], env, len, cap);
			dd_value_free((*env)[--(*len)]);
			return r;
		}
		dd_value_free(scrut);
		if (term->u.case_.dflt != NULL)
			return dd_eval(term->u.case_.dflt, env, len, cap);
		__dtfl_panic("Case: no arm for tag and no default");
	}
	case DD_T_FOLD: {
		dd_value *list = dd_eval(term->u.fold.list, env, len, cap);
		dd_value *acc;
		size_t i;

		if (list->kind != DD_V_LIST)
			__dtfl_panic("Fold on non-list value");
		acc = dd_eval(term->u.fold.init, env, len, cap);
		for (i = 0; i < list->u.seq.len; i++) {
			/* step: elem = Bound(0), acc = Bound(1). */
			dd_value *elem = dd_value_clone(list->u.seq.items[i]);
			dd_value *next;

			env_push(env, len, cap, acc);
			env_push(env, len, cap, elem);
			next = dd_eval(term->u.fold.step, env, len, cap);
			dd_value_free((*env)[--(*len)]); /* elem */
			dd_value_free((*env)[--(*len)]); /* acc */
			acc = next;
		}
		dd_value_free(list);
		return acc;
	}
	case DD_T_IF: {
		dd_value *cond = dd_eval(term->u.if_.cond, env, len, cap);
		bool t = dd_value_truthy(cond);

		dd_value_free(cond);
		return dd_eval(t ? term->u.if_.then_ : term->u.if_.els,
		    env, len, cap);
	}
	case DD_T_HASH: {
		dd_value *boundv = dd_eval(term->u.hash.data[0], env, len, cap);
		int64_t bound = dd_value_as_int(boundv);
		uint64_t acc = UINT64_C(0xcbf29ce484222325);
		int64_t h;
		size_t i;

		dd_value_free(boundv);
		for (i = 1; i < term->u.hash.len; i++) {
			dd_value *a = dd_eval(term->u.hash.data[i], env, len, cap);

			acc ^= (uint64_t)dd_value_as_int(a);
			acc = dd_hash_u64(acc);
			dd_value_free(a);
		}
		h = (int64_t)(acc >> 1); /* non-negative */
		return dd_value_int(bound > 0 ? h % bound : h);
	}
	case DD_T_UNARY:
		return eval_unary(term->u.unary.op, term->u.unary.tag,
		    dd_eval(term->u.unary.arg, env, len, cap));
	case DD_T_BINARY: {
		dd_binop op = term->u.binary.op;

		/* Short-circuit the logical operators. */
		if (op == DD_BIN_AND) {
			dd_value *l = dd_eval(term->u.binary.l, env, len, cap);
			bool lt = dd_value_truthy(l);
			bool res;
			dd_value *r;

			dd_value_free(l);
			if (!lt)
				return dd_value_int(0);
			r = dd_eval(term->u.binary.r, env, len, cap);
			res = dd_value_truthy(r);
			dd_value_free(r);
			return dd_value_int(res);
		}
		if (op == DD_BIN_OR) {
			dd_value *l = dd_eval(term->u.binary.l, env, len, cap);
			bool lt = dd_value_truthy(l);
			bool res;
			dd_value *r;

			dd_value_free(l);
			if (lt)
				return dd_value_int(1);
			r = dd_eval(term->u.binary.r, env, len, cap);
			res = dd_value_truthy(r);
			dd_value_free(r);
			return dd_value_int(res);
		}
		return eval_binary(op,
		    dd_eval(term->u.binary.l, env, len, cap),
		    dd_eval(term->u.binary.r, env, len, cap));
	}
	}
	__dtfl_panic("dd_eval: bad term kind");
}

static dd_value *
eval_unary(dd_unop op, uint32_t tag, dd_value *v)
{
	dd_value *r;

	switch (op) {
	case DD_UN_NEG:
		r = dd_value_int(-dd_value_as_int(v));
		break;
	case DD_UN_NOT:
		r = dd_value_int(!dd_value_truthy(v));
		break;
	case DD_UN_ISTAG:
		r = dd_value_int(v->kind == DD_V_VARIANT &&
		    v->u.variant.tag == tag);
		break;
	case DD_UN_LEN:
		if (v->kind != DD_V_TUPLE && v->kind != DD_V_LIST)
			__dtfl_panic("Len on non-aggregate value");
		r = dd_value_int((int64_t)v->u.seq.len);
		break;
	default:
		__dtfl_panic("eval_unary: bad op");
	}
	dd_value_free(v);
	return r;
}

static dd_value *
eval_binary(dd_binop op, dd_value *l, dd_value *r)
{
	dd_value *out;
	int c;

	switch (op) {
	case DD_BIN_ADD:
		out = dd_value_int(dd_value_as_int(l) + dd_value_as_int(r));
		break;
	case DD_BIN_SUB:
		out = dd_value_int(dd_value_as_int(l) - dd_value_as_int(r));
		break;
	case DD_BIN_MUL:
		out = dd_value_int(dd_value_as_int(l) * dd_value_as_int(r));
		break;
	case DD_BIN_EQ:
		out = dd_value_int(dd_value_cmp(l, r) == 0);
		break;
	case DD_BIN_NE:
		out = dd_value_int(dd_value_cmp(l, r) != 0);
		break;
	case DD_BIN_LT:
		c = dd_value_cmp(l, r);
		out = dd_value_int(c < 0);
		break;
	case DD_BIN_LE:
		c = dd_value_cmp(l, r);
		out = dd_value_int(c <= 0);
		break;
	case DD_BIN_GT:
		c = dd_value_cmp(l, r);
		out = dd_value_int(c > 0);
		break;
	case DD_BIN_GE:
		c = dd_value_cmp(l, r);
		out = dd_value_int(c >= 0);
		break;
	default:
		__dtfl_panic("eval_binary: logical ops short-circuit");
	}
	dd_value_free(l);
	dd_value_free(r);
	return out;
}

/* ------------------------------------------------------------------ */
/* Public value API (ddir.h): thin shims over the internal model.      */
/* ------------------------------------------------------------------ */

dtfl_ddir_value *
dtfl_ddir_value_int(int64_t n)
{
	return dd_value_int(n);
}

dtfl_ddir_value *
dtfl_ddir_value_unit(void)
{
	return dd_value_unit();
}

dtfl_ddir_value *
dtfl_ddir_value_tuple(dtfl_ddir_value **items, size_t len)
{
	dd_value **copy = len ? __dtfl_alloc(len * sizeof(dd_value *)) : NULL;
	size_t i;

	for (i = 0; i < len; i++)
		copy[i] = items[i];
	return dd_value_tuple(copy, len);
}

dtfl_ddir_value *
dtfl_ddir_value_list(dtfl_ddir_value **items, size_t len)
{
	dd_value **copy = len ? __dtfl_alloc(len * sizeof(dd_value *)) : NULL;
	size_t i;

	for (i = 0; i < len; i++)
		copy[i] = items[i];
	return dd_value_list(copy, len);
}

dtfl_ddir_value *
dtfl_ddir_value_variant(uint32_t tag, dtfl_ddir_value *payload)
{
	return dd_value_variant(tag, payload);
}

dtfl_ddir_value *
dtfl_ddir_value_clone(const dtfl_ddir_value *v)
{
	return dd_value_clone(v);
}

void
dtfl_ddir_value_free(dtfl_ddir_value *v)
{
	dd_value_free(v);
}

int
dtfl_ddir_value_cmp(const dtfl_ddir_value *a, const dtfl_ddir_value *b)
{
	return dd_value_cmp(a, b);
}
