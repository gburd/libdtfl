/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/explain/mod.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/explain.c: the explanation rewrite on the scope-tree IR.
 *
 * explain(p, shapes) transforms a program into one whose execution yields
 * per-source demand-set explanations for queries against p's first export:
 * a root { sources, query input, witness clone } with an iterative explain
 * scope { demand-set vars, forward clone on demanded rows, reverse-tracing
 * ops, demand exports }.  clone_into clones an original program's scopes
 * into an output scope, exporting a host-visible (lifted) form of every
 * value-producing site so the reverse rules can build pair tables.
 *
 * The reverse dataflow is flat inside the explain scope: a demand row
 * carries its user-iteration chain folded into the value (the Val
 * envelope), so no nesting is needed.  resolve chases import/export edges
 * to the site a reference names, and the shape-preserving lookup against
 * that site's host form injects or strips iteration coordinates as the
 * depths dictate.
 */
#include "explain_int.h"

#include "../base/base_int.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Addr                                                                */
/* ------------------------------------------------------------------ */

dd_addr
dd_addr_make(const size_t *path, size_t path_len, dd_site_kind kind,
    size_t site)
{
	dd_addr a;

	a.path = path_len ? __dtfl_alloc(path_len * sizeof(size_t)) : NULL;
	if (path_len)
		memcpy(a.path, path, path_len * sizeof(size_t));
	a.path_len = path_len;
	a.site_kind = kind;
	a.site = site;
	return a;
}

dd_addr
dd_addr_clone(const dd_addr *a)
{
	return dd_addr_make(a->path, a->path_len, a->site_kind, a->site);
}

void
dd_addr_free_fields(dd_addr *a)
{
	__dtfl_free(a->path);
	a->path = NULL;
}

int
dd_addr_cmp(const dd_addr *a, const dd_addr *b)
{
	size_t i, n = a->path_len < b->path_len ? a->path_len : b->path_len;

	for (i = 0; i < n; i++) {
		if (a->path[i] != b->path[i])
			return a->path[i] < b->path[i] ? -1 : 1;
	}
	if (a->path_len != b->path_len)
		return a->path_len < b->path_len ? -1 : 1;
	if (a->site_kind != b->site_kind)
		return a->site_kind < b->site_kind ? -1 : 1;
	if (a->site != b->site)
		return a->site < b->site ? -1 : 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Addr -> dd_ref map (sorted array)                                   */
/* ------------------------------------------------------------------ */

void
dd_addr_map_free(dd_addr_map *m)
{
	size_t i;

	for (i = 0; i < m->len; i++)
		dd_addr_free_fields(&m->data[i].key);
	__dtfl_free(m->data);
	m->data = NULL;
	m->len = m->cap = 0;
}

/* Binary search; returns the insertion index and sets *found. */
static size_t
addr_map_find(const dd_addr_map *m, const dd_addr *key, bool *found)
{
	size_t lo = 0, hi = m->len;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = dd_addr_cmp(key, &m->data[mid].key);

		if (c == 0) {
			*found = true;
			return mid;
		}
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	*found = false;
	return lo;
}

void
dd_addr_map_set(dd_addr_map *m, dd_addr key, dd_ref val)
{
	bool found;
	size_t idx = addr_map_find(m, &key, &found);

	if (found) {
		dd_addr_free_fields(&key);
		m->data[idx].val = val;
		return;
	}
	if (m->len == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 8;
		m->data = __dtfl_realloc(m->data, m->cap * sizeof(*m->data));
	}
	memmove(&m->data[idx + 1], &m->data[idx],
	    (m->len - idx) * sizeof(*m->data));
	m->data[idx].key = key;
	m->data[idx].val = val;
	m->len++;
}

const dd_ref *
dd_addr_map_get(const dd_addr_map *m, const dd_addr *key)
{
	bool found;
	size_t idx = addr_map_find(m, key, &found);

	return found ? &m->data[idx].val : NULL;
}

/* ------------------------------------------------------------------ */
/* Ref / node construction helpers                                     */
/* ------------------------------------------------------------------ */

static dd_ref
ref_local(size_t i)
{
	dd_ref r;

	r.kind = DD_REF_LOCAL;
	r.u.item = i;
	return r;
}

static dd_ref
ref_import(size_t k)
{
	dd_ref r;

	r.kind = DD_REF_IMPORT;
	r.u.import = k;
	return r;
}

static dd_ref
ref_var(size_t v)
{
	dd_ref r;

	r.kind = DD_REF_VAR;
	r.u.var = v;
	return r;
}

static dd_ref
ref_child_export(size_t item, size_t field)
{
	dd_ref r;

	r.kind = DD_REF_CHILD_EXPORT;
	r.u.child_export.item = item;
	r.u.child_export.field = field;
	return r;
}

/* Allocate a dd_node of a kind, zeroed. */
static dd_node *
node_new(dd_node_kind kind)
{
	dd_node *n = __dtfl_calloc(1, sizeof *n);

	n->kind = kind;
	return n;
}

/* Free one dd_export (name + the struct). */
static void
export_free_one(dd_export *e)
{
	__dtfl_free(e->name);
	__dtfl_free(e);
}

/* Push an item (Op or Sub) onto a scope and return its index. */
static size_t
scope_push_op(dd_scope *s, dd_node *n)
{
	dd_item *it = __dtfl_alloc(sizeof *it);

	it->kind = DD_I_OP;
	it->op = n;
	it->sub = NULL;
	dd_vec_push(&s->items, it);
	return s->items.len - 1;
}

static size_t
scope_push_sub(dd_scope *s, dd_scope *child)
{
	dd_item *it = __dtfl_alloc(sizeof *it);

	it->kind = DD_I_SUB;
	it->op = NULL;
	it->sub = child;
	dd_vec_push(&s->items, it);
	return s->items.len - 1;
}

/* Push a LiftIter Linear op over `input` onto scope `s`, return its Local. */
static dd_ref
scope_lift(dd_scope *s, dd_ref input)
{
	dd_node *n = node_new(DD_N_LINEAR);
	dd_linear_op *op = __dtfl_calloc(1, sizeof *op);

	n->input = input;
	op->kind = DD_LIN_LIFT_ITER;
	dd_vec_push(&n->ops, op);
	return ref_local(scope_push_op(s, n));
}

/* ------------------------------------------------------------------ */
/* Val row-model term builders                                         */
/* ------------------------------------------------------------------ */

dd_term *
dd_val_var(size_t slot)
{
	dd_term *t = dd_term_new(DD_T_VAR);

	t->u.var = slot;
	return t;
}

dd_term *
dd_val_f(size_t slot, size_t i)
{
	dd_term *t = dd_term_new(DD_T_PROJ);

	t->u.proj.base = dd_val_var(slot);
	t->u.proj.index = i;
	return t;
}

dd_term *
dd_val_tup(dd_term **xs, size_t n)
{
	dd_term *t = dd_term_new(DD_T_TUPLE);
	size_t i;

	/* Copies the pointers; the caller keeps ownership of the `xs` array
	 * (it may be a compound literal or a stack buffer). */
	for (i = 0; i < n; i++)
		dd_vec_push(&t->u.seq, xs[i]);
	return t;
}

/* A dd_vec of f(slot, lo..hi) terms. */
dd_vec
dd_val_fidx(size_t slot, size_t lo, size_t hi)
{
	dd_vec v = { 0 };
	size_t c;

	for (c = lo; c < hi; c++)
		dd_vec_push(&v, dd_val_f(slot, c));
	return v;
}

/* tuple(v): build a DD_T_TUPLE from a dd_vec of terms (consumes it). */
static dd_term *
tup_from_vec(dd_vec v)
{
	dd_term *t = dd_term_new(DD_T_TUPLE);

	t->u.seq = v;
	return t;
}

/* Append all of src's elements onto dst (moves pointers), frees src array. */
static void
vec_extend(dd_vec *dst, dd_vec src)
{
	size_t i;

	for (i = 0; i < src.len; i++)
		dd_vec_push(dst, src.data[i]);
	dd_vec_free(&src);
}

/* le/eq/gt/sub binary terms. */
static dd_term *
binop(dd_binop op, dd_term *l, dd_term *r)
{
	dd_term *t = dd_term_new(DD_T_BINARY);

	t->u.binary.op = op;
	t->u.binary.l = l;
	t->u.binary.r = r;
	return t;
}

static dd_term *
val_le(dd_term *a, dd_term *b)
{
	return binop(DD_BIN_LE, a, b);
}

static dd_term *
val_eq(dd_term *a, dd_term *b)
{
	return binop(DD_BIN_EQ, a, b);
}

static dd_term *
val_gt(dd_term *a, dd_term *b)
{
	return binop(DD_BIN_GT, a, b);
}

static dd_term *
val_sub(dd_term *a, dd_term *b)
{
	return binop(DD_BIN_SUB, a, b);
}

static dd_term *
val_int(int64_t n)
{
	dd_term *t = dd_term_new(DD_T_INT);

	t->u.int_lit = n;
	return t;
}

/* Reduce a dd_vec of predicate terms with AND, or NULL if empty.  Consumes
 * the vec. */
static dd_term *
and_all(dd_vec conds)
{
	dd_term *acc = NULL;
	size_t i;

	for (i = 0; i < conds.len; i++) {
		dd_term *c = conds.data[i];

		acc = acc == NULL ? c : binop(DD_BIN_AND, acc, c);
	}
	dd_vec_free(&conds);
	return acc;
}

/* A fresh projection from owned key/val terms. */
static dd_projection
make_proj(dd_term *key, dd_term *val)
{
	dd_projection p;

	p.key = key;
	p.val = val;
	return p;
}

dd_projection
dd_val_pack(size_t k, size_t v, size_t chain_len, bool has_q)
{
	dd_vec key = dd_val_fidx(0, 0, k);
	size_t tail = chain_len + (has_q ? 1 : 0);

	vec_extend(&key, dd_val_fidx(1, 0, v));
	return make_proj(tup_from_vec(key),
	    tup_from_vec(dd_val_fidx(1, v, v + tail)));
}

dd_projection
dd_val_distinct_unpack(size_t k, size_t v)
{
	return make_proj(tup_from_vec(dd_val_fidx(0, 0, k)),
	    tup_from_vec(dd_val_fidx(0, k, k + v)));
}

dd_projection
dd_val_project_kv(size_t k, size_t v)
{
	return make_proj(tup_from_vec(dd_val_fidx(0, 0, k)),
	    tup_from_vec(dd_val_fidx(1, 0, v)));
}

dd_projection
dd_val_sp_reassemble(size_t k, size_t v, size_t in_len, size_t out_len)
{
	dd_vec key = dd_val_fidx(0, 0, k);
	dd_vec val = dd_val_fidx(0, k, k + v);		/* V (from packed key) */

	vec_extend(&val, dd_val_fidx(2, 0, in_len));	/* chain_in (pair) */
	vec_extend(&val, dd_val_fidx(1, 0, out_len));	/* chain_out (dep) */
	dd_vec_push(&val, dd_val_f(1, out_len));	/* q */
	return make_proj(tup_from_vec(key), tup_from_vec(val));
}

dd_projection
dd_val_ky_join(size_t k, size_t v_in, size_t v_out, size_t in_len,
    size_t out_len, bool min)
{
	dd_vec val = dd_val_fidx(2, 0, v_in);			/* V_in */

	if (min)
		vec_extend(&val, dd_val_fidx(1, 0, v_out));	/* V_out */
	vec_extend(&val, dd_val_fidx(2, v_in, v_in + in_len));	/* chain_in */
	vec_extend(&val, dd_val_fidx(1, v_out, v_out + out_len)); /* chain_out */
	dd_vec_push(&val, dd_val_f(1, v_out + out_len));	/* q */
	return make_proj(tup_from_vec(dd_val_fidx(0, 0, k)), tup_from_vec(val));
}

dd_term *
dd_val_ky_data_eq(size_t v_in)
{
	dd_vec conds = { 0 };
	size_t i;

	for (i = 0; i < v_in; i++)
		dd_vec_push(&conds, val_eq(dd_val_f(1, i),
		    dd_val_f(1, v_in + i)));
	return and_all(conds);
}

dd_projection
dd_val_ky_drop_vout(size_t k, size_t v_in, size_t v_out, size_t in_len,
    size_t out_len)
{
	size_t after_vout = v_in + v_out;
	dd_vec val = dd_val_fidx(1, 0, v_in);

	vec_extend(&val, dd_val_fidx(1, after_vout, after_vout + in_len));
	vec_extend(&val, dd_val_fidx(1, after_vout + in_len,
	    after_vout + in_len + out_len));
	dd_vec_push(&val, dd_val_f(1, after_vout + in_len + out_len));
	return make_proj(tup_from_vec(dd_val_fidx(0, 0, k)), tup_from_vec(val));
}

/*
 * expand_value_fields: expand a user projection side over slots whose value
 * columns have the given arities.  A bare-row Spread($r) field splices only
 * row r's value columns (arities[r]); every other field passes through.
 * Returns a dd_vec of owned terms.
 */
static dd_vec
expand_value_fields(const dd_term *side, const size_t *arities, size_t n_ar)
{
	dd_vec out = { 0 };
	const dd_term *const *fields;
	size_t nf, i;
	const dd_term *single[1];

	if (side->kind == DD_T_TUPLE) {
		fields = (const dd_term *const *)side->u.seq.data;
		nf = side->u.seq.len;
	} else {
		single[0] = side;
		fields = single;
		nf = 1;
	}
	for (i = 0; i < nf; i++) {
		const dd_term *field = fields[i];

		if (field->kind == DD_T_SPREAD &&
		    field->u.spread->kind == DD_T_VAR) {
			size_t r = field->u.spread->u.var;
			size_t ar = r < n_ar ? arities[r] : 0;

			vec_extend(&out, dd_val_fidx(r, 0, ar));
		} else {
			dd_vec_push(&out, dd_term_clone(field));
		}
	}
	return out;
}

dd_projection
dd_val_lossy_pair(const dd_projection *proj, size_t k_in, size_t v_in,
    size_t in_len)
{
	size_t ar[2] = { k_in, v_in };
	dd_vec key = expand_value_fields(proj->key, ar, 2);
	dd_vec val = dd_val_fidx(0, 0, k_in);

	vec_extend(&val, dd_val_fidx(1, 0, v_in + in_len));
	return make_proj(tup_from_vec(key), tup_from_vec(val));
}

dd_projection
dd_val_lossy_reassemble(size_t k_in, size_t v_in, size_t v_out, size_t in_len,
    size_t out_len)
{
	dd_vec key = dd_val_fidx(2, 0, k_in);
	dd_vec val = dd_val_fidx(2, k_in, k_in + v_in);

	vec_extend(&val, dd_val_fidx(2, k_in + v_in, k_in + v_in + in_len));
	vec_extend(&val, dd_val_fidx(1, v_out, v_out + out_len));
	dd_vec_push(&val, dd_val_f(1, v_out + out_len));
	return make_proj(tup_from_vec(key), tup_from_vec(val));
}

dd_projection
dd_val_join_forward(const dd_projection *proj, size_t k, size_t v_l, size_t v_r,
    size_t l_len, size_t r_len)
{
	size_t ar[3] = { k, v_l, v_r };
	dd_vec key = expand_value_fields(proj->key, ar, 3);
	dd_vec val = dd_val_fidx(0, 0, k);		/* K */

	vec_extend(&val, dd_val_fidx(1, 0, v_l));	/* V_L */
	vec_extend(&val, dd_val_fidx(2, 0, v_r));	/* V_R */
	vec_extend(&val, dd_val_fidx(1, v_l, v_l + l_len)); /* chain_L */
	vec_extend(&val, dd_val_fidx(2, v_r, v_r + r_len)); /* chain_R */
	vec_extend(&val, expand_value_fields(proj->val, ar, 3)); /* V_out */
	return make_proj(tup_from_vec(key), tup_from_vec(val));
}

dd_projection
dd_val_join_combined(size_t k, size_t v_l, size_t v_r, size_t v_out,
    size_t l_len, size_t r_len, size_t out_len)
{
	size_t vl_s = k;
	size_t vr_s = vl_s + v_l;
	size_t ul_s = vr_s + v_r;
	size_t ur_s = ul_s + l_len;
	size_t vout_s = ur_s + r_len;
	dd_vec key = dd_val_fidx(2, 0, k);
	dd_vec val = dd_val_fidx(2, vl_s, vl_s + v_l);		/* V_L */

	vec_extend(&val, dd_val_fidx(2, vr_s, vr_s + v_r));	/* V_R */
	vec_extend(&val, dd_val_fidx(2, ul_s, ul_s + l_len));	/* chain_L */
	vec_extend(&val, dd_val_fidx(2, ur_s, ur_s + r_len));	/* chain_R */
	vec_extend(&val, dd_val_fidx(1, v_out, v_out + out_len)); /* chain_out */
	dd_vec_push(&val, dd_val_f(1, v_out + out_len));	/* q */
	vec_extend(&val, dd_val_fidx(1, 0, v_out));		/* V_out_dep */
	vec_extend(&val, dd_val_fidx(2, vout_s, vout_s + v_out)); /* V_out_pair */
	return make_proj(tup_from_vec(key), tup_from_vec(val));
}

/* chain_le: chain_in <= chain_out, outer-aligned, over the given slots at
 * the given field offsets.  Returns NULL if the chains share no coordinate. */
static dd_term *
chain_le_fields(size_t slot_in, size_t off_in, size_t in_len, size_t off_out,
    size_t out_len)
{
	size_t n = in_len < out_len ? in_len : out_len;
	size_t in_skip = in_len - n, out_skip = out_len - n, i;
	dd_vec conds = { 0 };

	for (i = 0; i < n; i++)
		dd_vec_push(&conds, val_le(dd_val_f(slot_in, off_in + in_skip + i),
		    dd_val_f(slot_in, off_out + out_skip + i)));
	return and_all(conds);
}

dd_term *
dd_val_join_filter(size_t v_l, size_t v_r, size_t v_out, size_t l_len,
    size_t r_len, size_t out_len)
{
	size_t ul_j = v_l + v_r;
	size_t ur_j = ul_j + l_len;
	size_t uo_j = ur_j + r_len;
	size_t q_j = uo_j + out_len;
	size_t vdep_j = q_j + 1;
	size_t vpair_j = vdep_j + v_out;
	dd_vec conds = { 0 };
	dd_term *tl, *tr, *vc;
	size_t i;

	/* time_cond(off, in_len): chain at [off..] <= chain_out at [uo_j..],
	 * outer-aligned. */
	tl = chain_le_fields(1, ul_j, l_len, uo_j, out_len);
	tr = chain_le_fields(1, ur_j, r_len, uo_j, out_len);
	for (i = 0; i < v_out; i++)
		dd_vec_push(&conds, val_eq(dd_val_f(1, vdep_j + i),
		    dd_val_f(1, vpair_j + i)));
	vc = and_all(conds);
	/* Combine the three (any may be NULL). */
	{
		dd_term *acc = NULL;
		dd_term *parts[3];
		size_t np = 0, j;

		if (tl != NULL)
			parts[np++] = tl;
		if (tr != NULL)
			parts[np++] = tr;
		if (vc != NULL)
			parts[np++] = vc;
		for (j = 0; j < np; j++)
			acc = acc == NULL ? parts[j]
			    : binop(DD_BIN_AND, acc, parts[j]);
		return acc;
	}
}

dd_projection
dd_val_join_split(bool left, size_t k, size_t v_l, size_t v_r, size_t l_len,
    size_t r_len, size_t out_len)
{
	size_t ul_j = v_l + v_r;
	size_t ur_j = ul_j + l_len;
	size_t uo_j = ur_j + r_len;
	size_t q_j = uo_j + out_len;
	dd_vec key = dd_val_fidx(0, 0, k);
	dd_vec val;

	if (left) {
		val = dd_val_fidx(1, 0, v_l);			/* V_L */
		vec_extend(&val, dd_val_fidx(1, ul_j, ul_j + l_len)); /* chain_L */
		dd_vec_push(&val, dd_val_f(1, q_j));		/* q */
	} else {
		val = dd_val_fidx(1, v_l, v_l + v_r);		/* V_R */
		vec_extend(&val, dd_val_fidx(1, ur_j, ur_j + r_len)); /* chain_R */
		dd_vec_push(&val, dd_val_f(1, q_j));		/* q */
	}
	return make_proj(tup_from_vec(key), tup_from_vec(val));
}

dd_term *
dd_val_time_le(size_t v, size_t in_len, size_t out_len)
{
	/* Joined row value: [V_pre(v) | user_in | user_out | q]; compare
	 * outer-aligned (last min(in,out)). */
	return chain_le_fields(1, v, in_len, v + in_len, out_len);
}

dd_projection
dd_val_strip(size_t k, size_t v, size_t in_len, size_t out_len, size_t keep)
{
	dd_vec key = dd_val_fidx(0, 0, k);
	dd_vec val = dd_val_fidx(1, 0, v);		/* V_pre */
	size_t drop_in = in_len - keep, i;

	for (i = 0; i < keep; i++)
		dd_vec_push(&val, dd_val_f(1, v + drop_in + i)); /* kept user_in */
	dd_vec_push(&val, dd_val_f(1, v + in_len + out_len));	/* q */
	return make_proj(tup_from_vec(key), tup_from_vec(val));
}

dd_term *
dd_val_bind_filter(size_t v)
{
	return val_gt(dd_val_f(1, v), val_int(0));
}

dd_projection
dd_val_bind_decrement(size_t k, size_t v, size_t user_len)
{
	dd_vec val = dd_val_fidx(1, 0, v);

	dd_vec_push(&val, val_sub(dd_val_f(1, v), val_int(1))); /* chain[0]-1 */
	vec_extend(&val, dd_val_fidx(1, v + 1, v + user_len));
	dd_vec_push(&val, dd_val_f(1, v + user_len));	/* q */
	return make_proj(tup_from_vec(dd_val_fidx(0, 0, k)), tup_from_vec(val));
}

/* ------------------------------------------------------------------ */
/* Sb: scope-under-construction builder                                */
/* ------------------------------------------------------------------ */

void
dd_sb_init(dd_sb *b, const char *name)
{
	memset(&b->s, 0, sizeof b->s);
	b->s.name = __dtfl_strdup(name);
	b->debug = false;
	b->boxes = NULL;
	b->n_boxes = b->cap_boxes = 0;
}

void
dd_sb_free_boxes(dd_sb *b)
{
	size_t i;

	for (i = 0; i < b->n_boxes; i++)
		__dtfl_free(b->boxes[i]);
	__dtfl_free(b->boxes);
	b->boxes = NULL;
	b->n_boxes = b->cap_boxes = 0;
}

/* Box a dd_ref into a heap handle tracked by the Sb (freed by
 * dd_sb_free_boxes). */
static dd_ref *
sb_box(dd_sb *b, dd_ref r)
{
	dd_ref *p = __dtfl_alloc(sizeof *p);

	*p = r;
	if (b->n_boxes == b->cap_boxes) {
		b->cap_boxes = b->cap_boxes ? b->cap_boxes * 2 : 16;
		b->boxes = __dtfl_realloc(b->boxes,
		    b->cap_boxes * sizeof(dd_ref *));
	}
	b->boxes[b->n_boxes++] = p;
	return p;
}

/* The dd_df Dataflow backed by the Sb scope builder.  Handles are boxed
 * dd_ref pointers. */
static void *
sb_df_project(void *ctx, void *c, dd_projection p)
{
	dd_sb *b = ctx;

	return sb_box(b, dd_sb_project(b, *(dd_ref *)c, p));
}

static void *
sb_df_filter(void *ctx, void *c, dd_term *cond)
{
	dd_sb *b = ctx;

	return sb_box(b, dd_sb_filter(b, *(dd_ref *)c, cond));
}

static void *
sb_df_join(void *ctx, void *l, void *r, dd_projection p)
{
	dd_sb *b = ctx;

	return sb_box(b, dd_sb_join(b, *(dd_ref *)l, *(dd_ref *)r, p));
}

static void *
sb_df_concat(void *ctx, void **cs, size_t n)
{
	dd_sb *b = ctx;
	dd_ref *refs = __dtfl_alloc((n ? n : 1) * sizeof(dd_ref));
	dd_ref out;
	size_t i;

	for (i = 0; i < n; i++)
		refs[i] = *(dd_ref *)cs[i];
	out = dd_sb_concat(b, refs, n);
	__dtfl_free(refs);
	return sb_box(b, out);
}

dd_df
dd_df_sb(dd_sb *b)
{
	dd_df df;

	df.ctx = b;
	df.project = sb_df_project;
	df.filter = sb_df_filter;
	df.join = sb_df_join;
	df.concat = sb_df_concat;
	return df;
}

dd_ref
dd_sb_op(dd_sb *b, dd_node *n)
{
	return ref_local(scope_push_op(&b->s, n));
}

dd_ref
dd_sb_linear(dd_sb *b, dd_ref input, dd_vec ops)
{
	dd_node *n = node_new(DD_N_LINEAR);

	n->input = input;
	n->ops = ops;
	return dd_sb_op(b, n);
}

dd_ref
dd_sb_project(dd_sb *b, dd_ref input, dd_projection p)
{
	dd_linear_op *op = __dtfl_calloc(1, sizeof *op);
	dd_vec ops = { 0 };

	op->kind = DD_LIN_PROJECT;
	op->proj = p;
	dd_vec_push(&ops, op);
	return dd_sb_linear(b, input, ops);
}

dd_ref
dd_sb_filter(dd_sb *b, dd_ref input, dd_term *cond)
{
	dd_linear_op *op = __dtfl_calloc(1, sizeof *op);
	dd_vec ops = { 0 };

	op->kind = DD_LIN_FILTER;
	op->term = cond;
	dd_vec_push(&ops, op);
	return dd_sb_linear(b, input, ops);
}

dd_ref
dd_sb_concat(dd_sb *b, dd_ref *refs, size_t n)
{
	dd_node *node;
	size_t i;

	if (n == 1)
		return refs[0];
	node = node_new(DD_N_CONCAT);
	for (i = 0; i < n; i++) {
		dd_ref *r = __dtfl_alloc(sizeof *r);

		*r = refs[i];
		dd_vec_push(&node->refs, r);
	}
	return dd_sb_op(b, node);
}

dd_ref
dd_sb_join(dd_sb *b, dd_ref l, dd_ref r, dd_projection p)
{
	dd_node *la = node_new(DD_N_ARRANGE);
	dd_ref lar;
	dd_node *ra = node_new(DD_N_ARRANGE);
	dd_ref rar;
	dd_node *jn = node_new(DD_N_JOIN);

	la->input = l;
	lar = dd_sb_op(b, la);
	ra->input = r;
	rar = dd_sb_op(b, ra);
	jn->input = lar;
	jn->right = rar;
	jn->projection = p;
	return dd_sb_op(b, jn);
}

dd_ref
dd_sb_reduce(dd_sb *b, dd_ref input, dd_reducer reducer)
{
	dd_node *a = node_new(DD_N_ARRANGE);
	dd_ref ar;
	dd_node *rd = node_new(DD_N_REDUCE);

	a->input = input;
	ar = dd_sb_op(b, a);
	rd->input = ar;
	rd->reducer = reducer;
	return dd_sb_op(b, rd);
}

dd_ref
dd_sb_variable(dd_sb *b, const char *name)
{
	dd_var *v = __dtfl_alloc(sizeof *v);

	v->name = __dtfl_strdup(name);
	dd_vec_push(&b->s.vars, v);
	return ref_var(b->s.vars.len - 1);
}

void
dd_sb_bind(dd_sb *b, dd_ref var, dd_ref value)
{
	dd_bind *bind = __dtfl_alloc(sizeof *bind);

	DTFL_ASSERT(var.kind == DD_REF_VAR);
	bind->var = var.u.var;
	bind->value = value;
	dd_vec_push(&b->s.binds, bind);
}

dd_ref
dd_sb_import(dd_sb *b, char *name, dd_source from)
{
	dd_import *imp = __dtfl_alloc(sizeof *imp);

	imp->name = name;
	imp->from = from;
	dd_vec_push(&b->s.imports, imp);
	return ref_import(b->s.imports.len - 1);
}

size_t
dd_sb_export(dd_sb *b, char *name, dd_ref value)
{
	dd_export *e = __dtfl_alloc(sizeof *e);

	e->name = name;
	e->value = value;
	dd_vec_push(&b->s.exports, e);
	return b->s.exports.len - 1;
}

void
dd_sb_debug_inspect(dd_sb *b, dd_ref input, char *label)
{
	if (b->debug) {
		dd_node *n = node_new(DD_N_INSPECT);

		n->input = input;
		n->label = label;
		dd_sb_op(b, n);
	} else {
		__dtfl_free(label);
	}
}

/* ------------------------------------------------------------------ */
/* clone_into / clone_identity                                         */
/* ------------------------------------------------------------------ */

/* Source helpers. */
static dd_source
src_parent(dd_ref r)
{
	dd_source s;

	s.kind = DD_SRC_PARENT;
	s.u.parent = r;
	return s;
}

/* map_ref: translate an orig-scope ref into the out scope (mod.rs map_ref). */
static dd_ref
map_ref(const dd_ref *r, const dd_ref *locals, const bool *locals_set,
    const size_t *subs, const bool *subs_set, const dd_ref *import_map,
    size_t var_base)
{
	switch (r->kind) {
	case DD_REF_LOCAL:
		DTFL_ASSERT(locals_set[r->u.item]);
		return locals[r->u.item];
	case DD_REF_IMPORT:
		return import_map[r->u.import];
	case DD_REF_VAR:
		return ref_var(var_base + r->u.var);
	case DD_REF_CHILD_EXPORT:
		DTFL_ASSERT(subs_set[r->u.child_export.item]);
		return ref_child_export(subs[r->u.child_export.item],
		    r->u.child_export.field);
	}
	__dtfl_panic("map_ref: bad ref kind");
}

/* Clone a node, mapping its refs with map_ref. */
typedef struct map_ctx {
	const dd_ref *locals;
	const bool *locals_set;
	const size_t *subs;
	const bool *subs_set;
	const dd_ref *import_map;
	size_t var_base;
} map_ctx;

static dd_ref
mc_map(const map_ctx *m, const dd_ref *r)
{
	return map_ref(r, m->locals, m->locals_set, m->subs, m->subs_set,
	    m->import_map, m->var_base);
}

static dd_node *
clone_node(const dd_node *node, const map_ctx *m)
{
	dd_node *o = node_new(node->kind);
	size_t i;

	switch (node->kind) {
	case DD_N_LINEAR:
		o->input = mc_map(m, &node->input);
		for (i = 0; i < node->ops.len; i++) {
			dd_linear_op *op = __dtfl_alloc(sizeof *op);

			*op = dd_linear_op_clone(node->ops.data[i]);
			dd_vec_push(&o->ops, op);
		}
		break;
	case DD_N_CONCAT:
		for (i = 0; i < node->refs.len; i++) {
			dd_ref *r = __dtfl_alloc(sizeof *r);

			*r = mc_map(m, node->refs.data[i]);
			dd_vec_push(&o->refs, r);
		}
		break;
	case DD_N_ARRANGE:
		o->input = mc_map(m, &node->input);
		break;
	case DD_N_JOIN:
		o->input = mc_map(m, &node->input);
		o->right = mc_map(m, &node->right);
		o->projection = dd_projection_clone(&node->projection);
		break;
	case DD_N_REDUCE:
		o->input = mc_map(m, &node->input);
		o->reducer = node->reducer;
		break;
	case DD_N_INSPECT:
		o->input = mc_map(m, &node->input);
		o->label = __dtfl_strdup(node->label);
		break;
	}
	return o;
}

static void clone_rec(const dd_scope *orig, dd_scope *out,
    const dd_ref *import_map, size_t n_imports, const size_t *path,
    size_t path_len, dd_addr_map *visible);

void
dd_clone_into(const dd_scope *orig, dd_scope *out, const dd_ref *import_map,
    size_t n_imports, dd_addr_map *visible)
{
	clone_rec(orig, out, import_map, n_imports, NULL, 0, visible);
}

/* A $host:N export name. */
static char *
host_name(size_t n)
{
	char buf[32];

	snprintf(buf, sizeof buf, "$host:%zu", n);
	return __dtfl_strdup(buf);
}

static void
clone_rec(const dd_scope *orig, dd_scope *out, const dd_ref *import_map,
    size_t n_imports, const size_t *path, size_t path_len, dd_addr_map *visible)
{
	size_t var_base = out->vars.len;
	dd_ref *locals;
	bool *locals_set;
	size_t *subs;
	bool *subs_set;
	size_t i;
	map_ctx m;

	(void)n_imports;
	/* Feedback variables first. */
	for (i = 0; i < orig->vars.len; i++) {
		const dd_var *v = orig->vars.data[i];
		dd_var *nv = __dtfl_alloc(sizeof *nv);

		nv->name = __dtfl_strdup(v->name);
		dd_vec_push(&out->vars, nv);
		dd_addr_map_set(visible,
		    dd_addr_make(path, path_len, DD_SITE_VAR, i),
		    ref_var(var_base + i));
	}

	locals = orig->items.len
	    ? __dtfl_calloc(orig->items.len, sizeof(dd_ref)) : NULL;
	locals_set = orig->items.len
	    ? __dtfl_calloc(orig->items.len, sizeof(bool)) : NULL;
	subs = orig->items.len
	    ? __dtfl_calloc(orig->items.len, sizeof(size_t)) : NULL;
	subs_set = orig->items.len
	    ? __dtfl_calloc(orig->items.len, sizeof(bool)) : NULL;

	m.locals = locals;
	m.locals_set = locals_set;
	m.subs = subs;
	m.subs_set = subs_set;
	m.import_map = import_map;
	m.var_base = var_base;

	for (i = 0; i < orig->items.len; i++) {
		const dd_item *item = orig->items.data[i];

		if (item->kind == DD_I_OP) {
			const dd_node *node = item->op;
			dd_node *cloned = clone_node(node, &m);
			size_t out_idx = scope_push_op(out, cloned);
			dd_ref vis;

			locals[i] = ref_local(out_idx);
			locals_set[i] = true;
			/* The host form of an Arrange is its underlying
			 * collection (reverse rules build pair tables from
			 * collections). */
			if (node->kind == DD_N_ARRANGE)
				vis = mc_map(&m, &node->input);
			else
				vis = ref_local(out_idx);
			dd_addr_map_set(visible,
			    dd_addr_make(path, path_len, DD_SITE_OP, i), vis);
		} else {
			const dd_scope *child = item->sub;
			dd_scope *child_out = __dtfl_calloc(1, sizeof *child_out);
			dd_ref *ident;
			size_t *child_path;
			dd_addr_map child_visible = { 0 };
			size_t sub_idx, j;

			child_out->name = __dtfl_strdup(child->name);
			/* Cloned imports: parent-side refs mapped into out. */
			for (j = 0; j < child->imports.len; j++) {
				const dd_import *imp = child->imports.data[j];
				dd_import *ni = __dtfl_alloc(sizeof *ni);

				ni->name = __dtfl_strdup(imp->name);
				DTFL_ASSERT(imp->from.kind == DD_SRC_PARENT);
				ni->from = src_parent(mc_map(&m,
				    &imp->from.u.parent));
				dd_vec_push(&child_out->imports, ni);
			}
			/* Inside the child, imports map to themselves. */
			ident = child->imports.len
			    ? __dtfl_alloc(child->imports.len * sizeof(dd_ref))
			    : NULL;
			for (j = 0; j < child->imports.len; j++)
				ident[j] = ref_import(j);
			child_path = __dtfl_alloc((path_len + 1) * sizeof(size_t));
			if (path_len)
				memcpy(child_path, path,
				    path_len * sizeof(size_t));
			child_path[path_len] = i;
			clone_rec(child, child_out, ident, child->imports.len,
			    child_path, path_len + 1, &child_visible);

			/* Lift each subtree site at this scope's exit and
			 * export it. */
			sub_idx = out->items.len;
			for (j = 0; j < child_visible.len; j++) {
				dd_addr_entry *e = &child_visible.data[j];
				dd_ref lift = scope_lift(child_out, e->val);
				size_t ei;

				ei = child_out->exports.len;
				{
					dd_export *ex =
					    __dtfl_alloc(sizeof *ex);

					ex->name = host_name(ei);
					ex->value = lift;
					dd_vec_push(&child_out->exports, ex);
				}
				dd_addr_map_set(visible,
				    dd_addr_clone(&e->key),
				    ref_child_export(sub_idx, ei));
			}
			dd_addr_map_free(&child_visible);
			scope_push_sub(out, child_out);
			subs[i] = sub_idx;
			subs_set[i] = true;
			__dtfl_free(ident);
			__dtfl_free(child_path);
		}
	}

	/* Loop closures, with this scope's variable indices offset. */
	for (i = 0; i < orig->binds.len; i++) {
		const dd_bind *bnd = orig->binds.data[i];
		dd_bind *nb = __dtfl_alloc(sizeof *nb);

		nb->var = var_base + bnd->var;
		nb->value = mc_map(&m, &bnd->value);
		dd_vec_push(&out->binds, nb);
	}
	/* Original exports, in order. */
	for (i = 0; i < orig->exports.len; i++) {
		const dd_export *e = orig->exports.data[i];
		dd_export *ne = __dtfl_alloc(sizeof *ne);

		ne->name = __dtfl_strdup(e->name);
		ne->value = mc_map(&m, &e->value);
		dd_vec_push(&out->exports, ne);
	}

	__dtfl_free(locals);
	__dtfl_free(locals_set);
	__dtfl_free(subs);
	__dtfl_free(subs_set);
}

dtfl_ddir_program *
dd_clone_identity(const dtfl_ddir_program *p)
{
	dtfl_ddir_program *out = __dtfl_calloc(1, sizeof *out);
	dd_ref *import_map;
	dd_addr_map visible = { 0 };
	size_t i;

	out->root.name = __dtfl_strdup(p->root.name);
	/* Copy the root imports verbatim. */
	for (i = 0; i < p->root.imports.len; i++) {
		const dd_import *imp = p->root.imports.data[i];
		dd_import *ni = __dtfl_alloc(sizeof *ni);

		ni->name = __dtfl_strdup(imp->name);
		ni->from = imp->from;
		if (imp->from.kind == DD_SRC_TRACE)
			ni->from.u.trace = __dtfl_strdup(imp->from.u.trace);
		dd_vec_push(&out->root.imports, ni);
	}
	import_map = p->root.imports.len
	    ? __dtfl_alloc(p->root.imports.len * sizeof(dd_ref)) : NULL;
	for (i = 0; i < p->root.imports.len; i++)
		import_map[i] = ref_import(i);
	dd_clone_into(&p->root, &out->root, import_map, p->root.imports.len,
	    &visible);
	dd_addr_map_free(&visible);
	__dtfl_free(import_map);
	return out;
}

/* ------------------------------------------------------------------ */
/* Target: a resolved reference (a site, or a root source)             */
/* ------------------------------------------------------------------ */

void
dd_target_free_fields(dd_target *t)
{
	if (t->kind == DD_TGT_SITE)
		dd_addr_free_fields(&t->site);
}

int
dd_target_cmp(const dd_target *a, const dd_target *b)
{
	if (a->kind != b->kind)
		return a->kind < b->kind ? -1 : 1;
	if (a->kind == DD_TGT_SITE)
		return dd_addr_cmp(&a->site, &b->site);
	if (a->source != b->source)
		return a->source < b->source ? -1 : 1;
	return 0;
}

static dd_target
tgt_site(dd_addr a)
{
	dd_target t;

	t.kind = DD_TGT_SITE;
	t.site = a;
	t.source = 0;
	return t;
}

static dd_target
tgt_source(size_t k)
{
	dd_target t;

	t.kind = DD_TGT_SOURCE;
	t.source = k;
	t.site.path = NULL;
	t.site.path_len = 0;
	return t;
}

/* ------------------------------------------------------------------ */
/* scope_at / resolve (mod.rs)                                         */
/* ------------------------------------------------------------------ */

static const dd_scope *
scope_at(const dd_scope *root, const size_t *path, size_t path_len)
{
	const dd_scope *s = root;
	size_t i;

	for (i = 0; i < path_len; i++) {
		const dd_item *it = s->items.data[path[i]];

		DTFL_ASSERT(it->kind == DD_I_SUB);
		s = it->sub;
	}
	return s;
}

/* Resolve `r` within the scope at `path` to its Target, chasing import edges
 * upward and child-export edges downward. */
static dd_target
resolve(const dd_scope *root, const size_t *path, size_t path_len,
    const dd_ref *r)
{
	switch (r->kind) {
	case DD_REF_LOCAL:
		return tgt_site(dd_addr_make(path, path_len, DD_SITE_OP,
		    r->u.item));
	case DD_REF_VAR:
		return tgt_site(dd_addr_make(path, path_len, DD_SITE_VAR,
		    r->u.var));
	case DD_REF_IMPORT:
		if (path_len == 0) {
			return tgt_source(r->u.import);
		} else {
			const dd_scope *scope = scope_at(root, path, path_len);
			const dd_import *imp = scope->imports.data[r->u.import];

			DTFL_ASSERT(imp->from.kind == DD_SRC_PARENT);
			return resolve(root, path, path_len - 1,
			    &imp->from.u.parent);
		}
	case DD_REF_CHILD_EXPORT: {
		size_t *cp = __dtfl_alloc((path_len + 1) * sizeof(size_t));
		const dd_scope *child;
		const dd_export *e;
		dd_target t;

		if (path_len)
			memcpy(cp, path, path_len * sizeof(size_t));
		cp[path_len] = r->u.child_export.item;
		child = scope_at(root, cp, path_len + 1);
		e = child->exports.data[r->u.child_export.field];
		t = resolve(root, cp, path_len + 1, &e->value);
		__dtfl_free(cp);
		return t;
	}
	}
	__dtfl_panic("resolve: bad ref kind");
}

/* ------------------------------------------------------------------ */
/* Term-arity: how a projection reshapes a flat (k, v) row             */
/* ------------------------------------------------------------------ */

static size_t
field_width(const dd_term *t, const size_t *rows, size_t n_rows)
{
	if (t->kind == DD_T_SPREAD && t->u.spread->kind == DD_T_VAR) {
		size_t r = t->u.spread->u.var;

		return r < n_rows ? rows[r] : 0;
	}
	return 1;
}

static size_t
proj_arity(const dd_term *t, const size_t *rows, size_t n_rows)
{
	size_t sum = 0, i;

	if (t->kind != DD_T_TUPLE)
		return 1;
	for (i = 0; i < t->u.seq.len; i++)
		sum += field_width(t->u.seq.data[i], rows, n_rows);
	return sum;
}

static void
apply_ops_arity(size_t *k, size_t *v, const dd_vec *ops)
{
	size_t i;

	for (i = 0; i < ops->len; i++) {
		const dd_linear_op *op = ops->data[i];

		switch (op->kind) {
		case DD_LIN_PROJECT: {
			size_t rows[2] = { *k, *v };

			*k = proj_arity(op->proj.key, rows, 2);
			*v = proj_arity(op->proj.val, rows, 2);
			break;
		}
		case DD_LIN_FILTER:
		case DD_LIN_NEGATE:
		case DD_LIN_ENTER_AT:
			break;
		case DD_LIN_LIFT_ITER:
			*v += 1;
			break;
		case DD_LIN_FLATMAP:
			*v = 2; /* value becomes tuple(pos, element) */
			break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* site_shapes: (k, v) per site, forward-propagated to a fixed point   */
/* ------------------------------------------------------------------ */

/* An Addr -> dd_shape map (sorted array). */
typedef struct shape_entry {
	dd_addr key;
	dd_shape val;
} shape_entry;

typedef struct shape_map {
	shape_entry *data;
	size_t len, cap;
} shape_map;

static void
shape_map_free(shape_map *m)
{
	size_t i;

	for (i = 0; i < m->len; i++)
		dd_addr_free_fields(&m->data[i].key);
	__dtfl_free(m->data);
	m->data = NULL;
	m->len = m->cap = 0;
}

static size_t
shape_find(const shape_map *m, const dd_addr *key, bool *found)
{
	size_t lo = 0, hi = m->len;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = dd_addr_cmp(key, &m->data[mid].key);

		if (c == 0) {
			*found = true;
			return mid;
		}
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	*found = false;
	return lo;
}

static bool
shape_has(const shape_map *m, const dd_addr *key)
{
	bool found;

	shape_find(m, key, &found);
	return found;
}

static bool
shape_get(const shape_map *m, const dd_addr *key, dd_shape *out)
{
	bool found;
	size_t idx = shape_find(m, key, &found);

	if (found)
		*out = m->data[idx].val;
	return found;
}

/* Insert if absent (ownership of key's path moves in on insert). */
static void
shape_set(shape_map *m, dd_addr key, dd_shape val)
{
	bool found;
	size_t idx = shape_find(m, &key, &found);

	if (found) {
		dd_addr_free_fields(&key);
		m->data[idx].val = val;
		return;
	}
	if (m->len == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 16;
		m->data = __dtfl_realloc(m->data, m->cap * sizeof(*m->data));
	}
	memmove(&m->data[idx + 1], &m->data[idx],
	    (m->len - idx) * sizeof(*m->data));
	m->data[idx].key = key;
	m->data[idx].val = val;
	m->len++;
}

/* Shape of a ref: a source shape or a site shape; false if unknown yet. */
static bool
shape_of_ref(const dd_scope *root, const size_t *path, size_t path_len,
    const dd_ref *r, const dd_shape *source_shapes, size_t n_src,
    const shape_map *shapes, dd_shape *out)
{
	dd_target t = resolve(root, path, path_len, r);
	bool ok;

	if (t.kind == DD_TGT_SOURCE) {
		DTFL_ASSERT(t.source < n_src);
		*out = source_shapes[t.source];
		ok = true;
	} else {
		ok = shape_get(shapes, &t.site, out);
	}
	dd_target_free_fields(&t);
	return ok;
}

static void
walk_shapes(const dd_scope *root, const dd_scope *s, const size_t *path,
    size_t path_len, const dd_shape *source_shapes, size_t n_src,
    shape_map *shapes)
{
	size_t i;

	for (i = 0; i < s->binds.len; i++) {
		const dd_bind *b = s->binds.data[i];
		dd_addr va = dd_addr_make(path, path_len, DD_SITE_VAR, b->var);
		dd_shape sh;

		if (!shape_has(shapes, &va) &&
		    shape_of_ref(root, path, path_len, &b->value, source_shapes,
		    n_src, shapes, &sh))
			shape_set(shapes, va, sh);
		else
			dd_addr_free_fields(&va);
	}
	for (i = 0; i < s->items.len; i++) {
		const dd_item *item = s->items.data[i];

		if (item->kind == DD_I_OP) {
			const dd_node *node = item->op;
			dd_addr oa = dd_addr_make(path, path_len, DD_SITE_OP, i);
			dd_shape sh;
			bool have = false;

			if (shape_has(shapes, &oa)) {
				dd_addr_free_fields(&oa);
				continue;
			}
			switch (node->kind) {
			case DD_N_LINEAR:
				have = shape_of_ref(root, path, path_len,
				    &node->input, source_shapes, n_src, shapes,
				    &sh);
				if (have)
					apply_ops_arity(&sh.k, &sh.v,
					    &node->ops);
				break;
			case DD_N_CONCAT: {
				size_t j;

				for (j = 0; j < node->refs.len && !have; j++)
					have = shape_of_ref(root, path,
					    path_len, node->refs.data[j],
					    source_shapes, n_src, shapes, &sh);
				break;
			}
			case DD_N_ARRANGE:
			case DD_N_INSPECT:
				have = shape_of_ref(root, path, path_len,
				    &node->input, source_shapes, n_src, shapes,
				    &sh);
				break;
			case DD_N_JOIN: {
				dd_shape ls, rs;

				if (shape_of_ref(root, path, path_len,
				    &node->input, source_shapes, n_src, shapes,
				    &ls) &&
				    shape_of_ref(root, path, path_len,
				    &node->right, source_shapes, n_src, shapes,
				    &rs)) {
					size_t rows[3] = { ls.k, ls.v, rs.v };

					sh.k = proj_arity(node->projection.key,
					    rows, 3);
					sh.v = proj_arity(node->projection.val,
					    rows, 3);
					have = true;
				}
				break;
			}
			case DD_N_REDUCE:
				have = shape_of_ref(root, path, path_len,
				    &node->input, source_shapes, n_src, shapes,
				    &sh);
				if (have) {
					switch (node->reducer) {
					case DD_RED_DISTINCT:
						sh.v = 0;
						break;
					case DD_RED_MIN:
						break;
					case DD_RED_COUNT:
					case DD_RED_COLLECT:
						sh.v = 1;
						break;
					}
				}
				break;
			}
			if (have)
				shape_set(shapes, oa, sh);
			else
				dd_addr_free_fields(&oa);
		} else {
			const dd_scope *child = item->sub;
			size_t *cp = __dtfl_alloc((path_len + 1) * sizeof(size_t));

			if (path_len)
				memcpy(cp, path, path_len * sizeof(size_t));
			cp[path_len] = i;
			walk_shapes(root, child, cp, path_len + 1,
			    source_shapes, n_src, shapes);
			__dtfl_free(cp);
		}
	}
}

static void
site_shapes(const dtfl_ddir_program *p, const dd_shape *source_shapes,
    size_t n_src, shape_map *shapes)
{
	size_t before;

	do {
		before = shapes->len;
		walk_shapes(&p->root, &p->root, NULL, 0, source_shapes, n_src,
		    shapes);
	} while (shapes->len != before);
}

/* Total operator count across all scopes. */
static size_t
scope_op_count(const dd_scope *s)
{
	size_t n = 0, i;

	for (i = 0; i < s->items.len; i++) {
		const dd_item *it = s->items.data[i];

		if (it->kind == DD_I_OP)
			n++;
		else
			n += scope_op_count(it->sub);
	}
	return n;
}

size_t
dd_op_count(const dtfl_ddir_program *p)
{
	return scope_op_count(&p->root);
}

/* ------------------------------------------------------------------ */
/* contribs: a Target -> vec<dd_ref> map (sorted array)                */
/* ------------------------------------------------------------------ */

typedef struct ref_vec {
	dd_ref *data;
	size_t len, cap;
} ref_vec;

static void
ref_vec_push(ref_vec *v, dd_ref r)
{
	if (v->len == v->cap) {
		v->cap = v->cap ? v->cap * 2 : 4;
		v->data = __dtfl_realloc(v->data, v->cap * sizeof(dd_ref));
	}
	v->data[v->len++] = r;
}

typedef struct contrib_entry {
	dd_target key;
	ref_vec refs;
} contrib_entry;

typedef struct contrib_map {
	contrib_entry *data;
	size_t len, cap;
} contrib_map;

static size_t
contrib_find(const contrib_map *m, const dd_target *key, bool *found)
{
	size_t lo = 0, hi = m->len;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		int c = dd_target_cmp(key, &m->data[mid].key);

		if (c == 0) {
			*found = true;
			return mid;
		}
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	*found = false;
	return lo;
}

/* Append `r` to the contrib list for `key` (clones key on first insert). */
static void
contrib_push(contrib_map *m, const dd_target *key, dd_ref r)
{
	bool found;
	size_t idx = contrib_find(m, key, &found);

	if (!found) {
		if (m->len == m->cap) {
			m->cap = m->cap ? m->cap * 2 : 8;
			m->data = __dtfl_realloc(m->data,
			    m->cap * sizeof(*m->data));
		}
		memmove(&m->data[idx + 1], &m->data[idx],
		    (m->len - idx) * sizeof(*m->data));
		m->data[idx].key.kind = key->kind;
		m->data[idx].key.source = key->source;
		m->data[idx].key.site = key->kind == DD_TGT_SITE
		    ? dd_addr_clone(&key->site)
		    : (dd_addr){ NULL, 0, DD_SITE_OP, 0 };
		m->data[idx].refs = (ref_vec){ 0 };
		m->len++;
	}
	ref_vec_push(&m->data[idx].refs, r);
}

/* Remove and return the contrib list for `key` (ownership of refs moves out);
 * returns an empty vec if absent. */
static ref_vec
contrib_take(contrib_map *m, const dd_target *key)
{
	bool found;
	size_t idx = contrib_find(m, key, &found);
	ref_vec out = { 0 };

	if (!found)
		return out;
	out = m->data[idx].refs;
	dd_target_free_fields(&m->data[idx].key);
	memmove(&m->data[idx], &m->data[idx + 1],
	    (m->len - idx - 1) * sizeof(*m->data));
	m->len--;
	return out;
}

static void
contrib_map_free(contrib_map *m)
{
	size_t i;

	for (i = 0; i < m->len; i++) {
		dd_target_free_fields(&m->data[i].key);
		__dtfl_free(m->data[i].refs.data);
	}
	__dtfl_free(m->data);
}

/* ------------------------------------------------------------------ */
/* distinct_full: set-level distinct on whole (K ; V ++ chain ++ q)    */
/* rows (mod.rs Sb::distinct_full).  v_arity is the full value width.   */
/* ------------------------------------------------------------------ */

static dd_ref
sb_distinct_full(dd_sb *b, dd_ref input, size_t k_arity, size_t v_arity)
{
	dd_ref packed = dd_sb_project(b, input,
	    dd_val_pack(k_arity, v_arity, 0, false));
	dd_ref dist = dd_sb_reduce(b, packed, DD_RED_DISTINCT);

	return dd_sb_project(b, dist,
	    dd_val_distinct_unpack(k_arity, v_arity));
}

/* semijoin_data: keep left's (K; V) rows that match right by (K). */
static dd_ref
sb_semijoin_data(dd_sb *b, dd_ref left, dd_ref right, size_t k, size_t v)
{
	return dd_sb_join(b, left, right, dd_val_project_kv(k, v));
}

/* ------------------------------------------------------------------ */
/* The reverse walk (mod.rs Reverse)                                   */
/* ------------------------------------------------------------------ */

typedef struct reverse {
	const dd_scope *orig;
	const shape_map *shapes;
	const dd_shape *source_shapes;
	size_t n_src;
	const dd_addr_map *wit;	/* site -> witness ref */
	const dd_addr_map *fwd;	/* site -> forward ref */
	const dd_ref *ex_src;	/* source k -> import ref in explain scope */
	const dd_ref *fwd_inputs; /* source k -> restricted forward input */
	dd_addr_map demand;	/* site -> demand ref */
	contrib_map contribs;
	dd_df df;		/* the Sb-backed Dataflow for the reverse rules */
	dd_sb *ex;		/* the explain scope builder */
} reverse;

/* Shape at an Addr (must exist). */
static dd_shape
shape_at(const reverse *rv, const dd_addr *a)
{
	dd_shape sh;
	bool ok = shape_get(rv->shapes, a, &sh);

	DTFL_ASSERT(ok);
	(void)ok;
	return sh;
}

/* Box a dd_ref into a dd_df handle (tracked by the Sb). */
static void *
rv_box(reverse *rv, dd_ref r)
{
	return sb_box(rv->ex, r);
}

/* Unbox a dd_df handle back to its dd_ref. */
static dd_ref
rv_unbox(void *h)
{
	return *(dd_ref *)h;
}

/* The host side for a target (mod.rs Reverse::side), with boxed handles. */
static dd_side
rv_side(reverse *rv, const dd_target *t)
{
	dd_side s;

	if (t->kind == DD_TGT_SITE) {
		const dd_ref *w = dd_addr_map_get(rv->wit, &t->site);
		const dd_ref *f = dd_addr_map_get(rv->fwd, &t->site);

		DTFL_ASSERT(w != NULL && f != NULL);
		s.witness = rv_box(rv, *w);
		s.forward = rv_box(rv, *f);
		s.shape = shape_at(rv, &t->site);
		s.user_len = t->site.path_len;
	} else {
		s.witness = rv_box(rv, rv->ex_src[t->source]);
		s.forward = rv_box(rv, rv->fwd_inputs[t->source]);
		s.shape = rv->source_shapes[t->source];
		s.user_len = 0;
	}
	return s;
}

/* route: adapt a contrib of chain length from_len to the target's depth. */
static dd_ref
rv_route(reverse *rv, dd_ref contrib, size_t from_len, const dd_target *target)
{
	size_t to_len = target->kind == DD_TGT_SITE
	    ? target->site.path_len : 0;

	if (to_len == from_len)
		return contrib;
	{
		dd_side side = rv_side(rv, target);
		void *out = dd_shape_preserving_lookup(&rv->df,
		    rv_box(rv, contrib), &side, from_len);

		return rv_unbox(out);
	}
}

/* push: route a contrib into the target of `input` and record it. */
static void
rv_push(reverse *rv, const size_t *path, size_t path_len, const dd_ref *input,
    dd_ref contrib, size_t from_len)
{
	dd_target target = resolve(rv->orig, path, path_len, input);
	dd_ref routed = rv_route(rv, contrib, from_len, &target);

	contrib_push(&rv->contribs, &target, routed);
	dd_target_free_fields(&target);
}

static void rv_walk(reverse *rv, dd_sb *ex, const dd_scope *s,
    const size_t *path, size_t path_len);
static dd_ref dd_flatmap_lookup(dd_sb *b, dd_ref dep, const dd_side *side,
    dd_shape output_shape, size_t out_user_len, const dd_term *list_term);

/* A site's reverse rule (mod.rs Reverse::site). */
static void
rv_site(reverse *rv, dd_sb *ex, const size_t *path, size_t path_len, size_t i,
    const dd_node *node)
{
	dd_addr addr = dd_addr_make(path, path_len, DD_SITE_OP, i);
	dd_target self = tgt_site(dd_addr_clone(&addr));
	ref_vec cs = contrib_take(&rv->contribs, &self);
	dd_shape sh;
	size_t out_user_len = path_len;
	dd_ref combined, dist, dep_this;
	char *lbl;

	dd_target_free_fields(&self);
	if (cs.len == 0) {
		dd_addr_free_fields(&addr);
		__dtfl_free(cs.data);
		return;
	}
	combined = dd_sb_concat(ex, cs.data, cs.len);
	__dtfl_free(cs.data);
	sh = shape_at(rv, &addr);
	dist = sb_distinct_full(ex, combined, sh.k, sh.v + out_user_len + 1);
	{
		char buf[64];

		snprintf(buf, sizeof buf, "demand_op_%zu", i);
		lbl = __dtfl_strdup(buf);
	}
	dd_sb_debug_inspect(ex, dist, lbl);
	dd_addr_map_set(&rv->demand, dd_addr_clone(&addr), dist);
	dep_this = dist;

	switch (node->kind) {
	case DD_N_LINEAR: {
		const dd_linear_op *op;

		DTFL_ASSERT(node->ops.len == 1);
		op = node->ops.data[0];
		switch (op->kind) {
		case DD_LIN_PROJECT: {
			dd_target target = resolve(rv->orig, path, path_len,
			    &node->input);
			dd_side side = rv_side(rv, &target);
			dd_shape os = { sh.k, sh.v };
			void *contrib = dd_lossy_lookup(&rv->df,
			    rv_box(rv, dep_this), &side, os, out_user_len,
			    &op->proj);

			contrib_push(&rv->contribs, &target, rv_unbox(contrib));
			dd_target_free_fields(&target);
			break;
		}
		case DD_LIN_FILTER: {
			dd_ref contrib = dd_sb_filter(ex, dep_this,
			    dd_term_clone(op->term));

			rv_push(rv, path, path_len, &node->input, contrib,
			    out_user_len);
			break;
		}
		case DD_LIN_NEGATE:
		case DD_LIN_ENTER_AT:
			rv_push(rv, path, path_len, &node->input, dep_this,
			    out_user_len);
			break;
		case DD_LIN_LIFT_ITER:
			__dtfl_panic("explain: LiftIter in user program");
		case DD_LIN_FLATMAP: {
			dd_target target = resolve(rv->orig, path, path_len,
			    &node->input);
			dd_side side = rv_side(rv, &target);
			dd_shape os = { sh.k, sh.v };
			dd_ref contrib = dd_flatmap_lookup(ex, dep_this, &side,
			    os, out_user_len, op->term);

			contrib_push(&rv->contribs, &target, contrib);
			dd_target_free_fields(&target);
			break;
		}
		}
		break;
	}
	case DD_N_CONCAT: {
		size_t j;

		for (j = 0; j < node->refs.len; j++) {
			dd_target target = resolve(rv->orig, path, path_len,
			    node->refs.data[j]);
			dd_side side = rv_side(rv, &target);
			void *contrib = dd_shape_preserving_lookup(&rv->df,
			    rv_box(rv, dep_this), &side, out_user_len);

			contrib_push(&rv->contribs, &target, rv_unbox(contrib));
			dd_target_free_fields(&target);
		}
		break;
	}
	case DD_N_ARRANGE:
	case DD_N_INSPECT:
		rv_push(rv, path, path_len, &node->input, dep_this,
		    out_user_len);
		break;
	case DD_N_REDUCE: {
		dd_target target = resolve(rv->orig, path, path_len,
		    &node->input);
		dd_side side = rv_side(rv, &target);
		dd_shape os = { sh.k, sh.v };
		void *contrib = dd_keyed_lookup(&rv->df, rv_box(rv, dep_this),
		    &side, os, out_user_len, node->reducer == DD_RED_MIN);

		contrib_push(&rv->contribs, &target, rv_unbox(contrib));
		dd_target_free_fields(&target);
		break;
	}
	case DD_N_JOIN: {
		dd_target lt = resolve(rv->orig, path, path_len, &node->input);
		dd_target rt = resolve(rv->orig, path, path_len, &node->right);
		dd_side ls = rv_side(rv, &lt);
		dd_side rs = rv_side(rv, &rt);
		dd_shape os = { sh.k, sh.v };
		void *lc, *rc;

		dd_join_lookup(&rv->df, rv_box(rv, dep_this), &ls, &rs, os,
		    out_user_len, &node->projection, &lc, &rc);
		contrib_push(&rv->contribs, &lt, rv_unbox(lc));
		contrib_push(&rv->contribs, &rt, rv_unbox(rc));
		dd_target_free_fields(&lt);
		dd_target_free_fields(&rt);
		break;
	}
	}
	dd_addr_free_fields(&addr);
}

static void
rv_walk(reverse *rv, dd_sb *ex, const dd_scope *s, const size_t *path,
    size_t path_len)
{
	size_t i;

	/* Binds first: route each var's demand into its value's contribs,
	 * inverting the feedback's iter advance. */
	for (i = 0; i < s->binds.len; i++) {
		const dd_bind *b = s->binds.data[i];
		dd_addr va = dd_addr_make(path, path_len, DD_SITE_VAR, b->var);
		const dd_ref *dvp = dd_addr_map_get(&rv->demand, &va);
		dd_shape vsh = shape_at(rv, &va);
		size_t var_user_len = path_len;
		dd_ref dv, filtered, contrib;
		dd_term *bf;

		DTFL_ASSERT(dvp != NULL);
		dv = *dvp;
		bf = dd_val_bind_filter(vsh.v);
		filtered = bf != NULL ? dd_sb_filter(ex, dv, bf) : dv;
		contrib = dd_sb_project(ex, filtered,
		    dd_val_bind_decrement(vsh.k, vsh.v, var_user_len));
		rv_push(rv, path, path_len, &b->value, contrib,
		    var_user_len);
		dd_addr_free_fields(&va);
	}
	/* Items in reverse. */
	for (i = s->items.len; i-- > 0;) {
		const dd_item *item = s->items.data[i];

		if (item->kind == DD_I_OP) {
			rv_site(rv, ex, path, path_len, i, item->op);
		} else {
			const dd_scope *child = item->sub;
			size_t *cp = __dtfl_alloc((path_len + 1) *
			    sizeof(size_t));

			if (path_len)
				memcpy(cp, path, path_len * sizeof(size_t));
			cp[path_len] = i;
			rv_walk(rv, ex, child, cp, path_len + 1);
			__dtfl_free(cp);
		}
	}
	/* Close this scope's feedback variables. */
	for (i = 0; i < s->vars.len; i++) {
		dd_addr addr = dd_addr_make(path, path_len, DD_SITE_VAR, i);
		const dd_ref *dvp = dd_addr_map_get(&rv->demand, &addr);
		dd_target self = tgt_site(dd_addr_clone(&addr));
		ref_vec cs = contrib_take(&rv->contribs, &self);
		dd_ref dv;

		DTFL_ASSERT(dvp != NULL);
		dv = *dvp;
		dd_target_free_fields(&self);
		if (cs.len == 0) {
			dd_sb_bind(ex, dv, dv);
			dd_addr_free_fields(&addr);
			__dtfl_free(cs.data);
			continue;
		}
		{
			dd_ref combined = dd_sb_concat(ex, cs.data, cs.len);
			dd_shape sh = shape_at(rv, &addr);
			dd_ref dist = sb_distinct_full(ex, combined, sh.k,
			    sh.v + path_len + 1);
			char buf[64];

			snprintf(buf, sizeof buf, "demand_var_%zu", i);
			dd_sb_debug_inspect(ex, dist, __dtfl_strdup(buf));
			dd_sb_bind(ex, dv, dist);
		}
		__dtfl_free(cs.data);
		dd_addr_free_fields(&addr);
	}
}

/* ------------------------------------------------------------------ */
/* collect_var_addrs                                                   */
/* ------------------------------------------------------------------ */

typedef struct addr_list {
	dd_addr *data;
	size_t len, cap;
} addr_list;

static void
addr_list_push(addr_list *l, dd_addr a)
{
	if (l->len == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 8;
		l->data = __dtfl_realloc(l->data, l->cap * sizeof(dd_addr));
	}
	l->data[l->len++] = a;
}

static void
collect_var_addrs(const dd_scope *s, const size_t *path, size_t path_len,
    addr_list *out)
{
	size_t i;

	for (i = 0; i < s->vars.len; i++)
		addr_list_push(out, dd_addr_make(path, path_len, DD_SITE_VAR, i));
	for (i = 0; i < s->items.len; i++) {
		const dd_item *item = s->items.data[i];

		if (item->kind == DD_I_SUB) {
			size_t *cp = __dtfl_alloc((path_len + 1) *
			    sizeof(size_t));

			if (path_len)
				memcpy(cp, path, path_len * sizeof(size_t));
			cp[path_len] = i;
			collect_var_addrs(item->sub, cp, path_len + 1, out);
			__dtfl_free(cp);
		}
	}
}

/* ------------------------------------------------------------------ */
/* explain                                                             */
/* ------------------------------------------------------------------ */

/* strdup via snprintf of a format with one arg. */
static char *
fmt1(const char *fmt, size_t a)
{
	char buf[96];

	snprintf(buf, sizeof buf, fmt, a);
	return __dtfl_strdup(buf);
}

/* A $wit:<path>:<site> name (debug readability only). */
static char *
wit_name(const dd_addr *a)
{
	char buf[160];
	size_t off = 0, i;

	off += (size_t)snprintf(buf + off, sizeof buf - off, "$wit:[");
	for (i = 0; i < a->path_len && off < sizeof buf; i++)
		off += (size_t)snprintf(buf + off, sizeof buf - off, "%s%zu",
		    i ? "," : "", a->path[i]);
	snprintf(buf + off, sizeof buf - off, "]:%s%zu",
	    a->site_kind == DD_SITE_OP ? "Op" : "Var", a->site);
	return __dtfl_strdup(buf);
}

/* ------------------------------------------------------------------ */
/* FlatMap (UNNEST) reverse rule (mod.rs emit_lookup_flatmap)          */
/* ------------------------------------------------------------------ */

/* Proj of an arbitrary base term by index i. */
static dd_term *
fld(dd_term *base, size_t i)
{
	dd_term *t = dd_term_new(DD_T_PROJ);

	t->u.proj.base = base;
	t->u.proj.index = i;
	return t;
}

/* The FlatMap backward rule: build the (output -> input) pair table by
 * running the op forward on the input side keyed by the whole input row,
 * join the demand on the packed output, recover the input row.  FlatMap is
 * same-depth; the chain rides unchanged and chain_in <= chain_out guards
 * soundness.  output_shape.v == 2 (the tuple(pos, elem) value). */
static dd_ref
dd_flatmap_lookup(dd_sb *b, dd_ref dep, const dd_side *side,
    dd_shape output_shape, size_t out_user_len, const dd_term *list_term)
{
	size_t v_in = side->shape.v;
	size_t v_out = output_shape.v; /* 2: pos, elem */
	size_t d = out_user_len;
	dd_ref srcrefs[2], src, rekeyed, exploded, pair, dep_keyed, joined;
	dd_ref filtered;
	dd_vec wide, val;
	dd_term *cond;
	size_t c;

	srcrefs[0] = *(dd_ref *)side->witness;
	srcrefs[1] = *(dd_ref *)side->forward;
	src = dd_sb_concat(b, srcrefs, 2);
	/* rekey the input by the whole input row: key tuple($0,$1), val $1. */
	{
		dd_term *key[2];

		key[0] = dd_val_var(0);
		key[1] = dd_val_var(1);
		rekeyed = dd_sb_project(b, src,
		    make_proj(dd_val_tup((dd_term *[]){ key[0], key[1] }, 2),
		    dd_val_var(1)));
	}
	/* explode. */
	{
		dd_linear_op *op = __dtfl_calloc(1, sizeof *op);
		dd_vec ops = { 0 };

		op->kind = DD_LIN_FLATMAP;
		op->term = dd_term_clone(list_term);
		dd_vec_push(&ops, op);
		exploded = dd_sb_linear(b, rekeyed, ops);
	}
	/* pair: key tuple(f(0,0), f(1,0), f(1,1)), val $0 (the source row). */
	{
		dd_term *key[3];

		key[0] = dd_val_f(0, 0);
		key[1] = dd_val_f(1, 0);
		key[2] = dd_val_f(1, 1);
		pair = dd_sb_project(b, exploded,
		    make_proj(dd_val_tup((dd_term *[]){ key[0], key[1], key[2] },
		    3), dd_val_var(0)));
	}
	/* dep rekeyed by the same packed output; keep its chain ++ q. */
	{
		dd_term *key[3];

		key[0] = dd_val_var(0);
		key[1] = dd_val_f(1, 0);
		key[2] = dd_val_f(1, 1);
		dep_keyed = dd_sb_project(b, dep,
		    make_proj(dd_val_tup((dd_term *[]){ key[0], key[1], key[2] },
		    3), tup_from_vec(dd_val_fidx(1, v_out, v_out + d + 1))));
	}
	/* join into a wide row [V_in | chain_in | chain_out | q].  Source is
	 * $2 = (k, host_val); host_val = [V_in | chain_in].  dep' is $1 =
	 * [chain_out | q]. */
	wide = (dd_vec){ 0 };
	for (c = 0; c < v_in; c++)
		dd_vec_push(&wide, fld(dd_val_f(2, 1), c));	  /* V_in */
	for (c = 0; c < d; c++)
		dd_vec_push(&wide, fld(dd_val_f(2, 1), v_in + c)); /* chain_in */
	vec_extend(&wide, dd_val_fidx(1, 0, d));		  /* chain_out */
	dd_vec_push(&wide, dd_val_f(1, d));			  /* q */
	joined = dd_sb_join(b, dep_keyed, pair,
	    make_proj(dd_val_f(2, 0), tup_from_vec(wide)));
	/* chain_in <= chain_out (same depth, coordinate-aligned). */
	{
		dd_vec conds = { 0 };

		for (c = 0; c < d; c++)
			dd_vec_push(&conds, val_le(dd_val_f(1, v_in + c),
			    dd_val_f(1, v_in + d + c)));
		cond = and_all(conds);
	}
	filtered = cond != NULL ? dd_sb_filter(b, joined, cond) : joined;
	/* reform the demanded input (k ; V_in ++ chain_out ++ q). */
	val = dd_val_fidx(1, 0, v_in);
	vec_extend(&val, dd_val_fidx(1, v_in + d, v_in + 2 * d));
	dd_vec_push(&val, dd_val_f(1, v_in + 2 * d));
	return dd_sb_project(b, filtered,
	    make_proj(dd_val_var(0), tup_from_vec(val)));
}

dtfl_ddir_program *
dd_explain(const dtfl_ddir_program *p, const size_t (*source_shapes_raw)[2],
    size_t n_shapes)
{
	size_t n_sources = p->root.imports.len;
	dd_shape *shapes_arr;
	shape_map shapes = { 0 };
	size_t max_input = 0, i;
	dd_sb root, ex;
	dd_ref *src_refs, query_ref;
	dd_addr_map witness = { 0 };
	dd_ref *ex_src, ex_query;
	dd_addr_map wit = { 0 };
	dd_ref *dsets, *fwd_inputs;
	dd_addr_map forward = { 0 };
	addr_list var_addrs = { 0 };
	reverse rv;
	dd_target first_target;
	dd_ref seeded, ex_query_routed;
	char **demand_names;
	size_t *demand_js;
	size_t ex_idx;
	dtfl_ddir_program *out;

	DTFL_ASSERT(n_shapes == n_sources);
	shapes_arr = n_sources
	    ? __dtfl_alloc(n_sources * sizeof(dd_shape)) : NULL;
	for (i = 0; i < n_sources; i++) {
		shapes_arr[i].k = source_shapes_raw[i][0];
		shapes_arr[i].v = source_shapes_raw[i][1];
	}
	site_shapes(p, shapes_arr, n_sources, &shapes);
	/* The largest positional-input index + 1. */
	for (i = 0; i < n_sources; i++) {
		const dd_import *imp = p->root.imports.data[i];

		if (imp->from.kind == DD_SRC_INPUT &&
		    imp->from.u.input + 1 > max_input)
			max_input = imp->from.u.input + 1;
	}

	/* ---- output root: original sources + query input + witness clone ---- */
	dd_sb_init(&root, p->root.name);
	for (i = 0; i < n_sources; i++) {
		const dd_import *imp = p->root.imports.data[i];
		dd_import *ni = __dtfl_alloc(sizeof *ni);

		ni->name = __dtfl_strdup(imp->name);
		ni->from = imp->from;
		if (imp->from.kind == DD_SRC_TRACE)
			ni->from.u.trace = __dtfl_strdup(imp->from.u.trace);
		dd_vec_push(&root.s.imports, ni);
	}
	src_refs = n_sources
	    ? __dtfl_alloc(n_sources * sizeof(dd_ref)) : NULL;
	for (i = 0; i < n_sources; i++)
		src_refs[i] = ref_import(i);
	{
		dd_source qs;

		qs.kind = DD_SRC_INPUT;
		qs.u.input = max_input;
		query_ref = dd_sb_import(&root, __dtfl_strdup("query"), qs);
	}
	dd_clone_into(&p->root, &root.s, src_refs, n_sources, &witness);
	/* The witness clone re-exported the original exports; drop them. */
	for (i = 0; i < root.s.exports.len; i++)
		export_free_one(root.s.exports.data[i]);
	dd_vec_free(&root.s.exports);
	root.s.exports = (dd_vec){ 0 };

	/* ---- explain scope ---- */
	dd_sb_init(&ex, "explain");
	ex_src = n_sources ? __dtfl_alloc(n_sources * sizeof(dd_ref)) : NULL;
	for (i = 0; i < n_sources; i++)
		ex_src[i] = dd_sb_import(&ex, fmt1("$src:%zu", i),
		    src_parent(src_refs[i]));
	ex_query = dd_sb_import(&ex, __dtfl_strdup("$query"),
	    src_parent(query_ref));
	/* Import every witness site into the explain scope. */
	for (i = 0; i < witness.len; i++) {
		dd_addr_entry *e = &witness.data[i];
		dd_ref wr = dd_sb_import(&ex, wit_name(&e->key),
		    src_parent(e->val));

		dd_addr_map_set(&wit, dd_addr_clone(&e->key), wr);
	}

	/* Demand-set variables, one per source; forward inputs are the
	 * sources restricted to the demand-sets. */
	dsets = n_sources ? __dtfl_alloc(n_sources * sizeof(dd_ref)) : NULL;
	for (i = 0; i < n_sources; i++)
		dsets[i] = dd_sb_variable(&ex, fmt1("demand_set_%zu", i));
	fwd_inputs = n_sources
	    ? __dtfl_alloc(n_sources * sizeof(dd_ref)) : NULL;
	for (i = 0; i < n_sources; i++)
		fwd_inputs[i] = sb_semijoin_data(&ex, dsets[i], ex_src[i],
		    shapes_arr[i].k, shapes_arr[i].v);
	dd_clone_into(&p->root, &ex.s, fwd_inputs, n_sources, &forward);
	for (i = 0; i < ex.s.exports.len; i++)
		export_free_one(ex.s.exports.data[i]);
	dd_vec_free(&ex.s.exports);
	ex.s.exports = (dd_vec){ 0 };

	/* Pre-allocate demand variables for the original feedback vars. */
	collect_var_addrs(&p->root, NULL, 0, &var_addrs);
	memset(&rv, 0, sizeof rv);
	for (i = 0; i < var_addrs.len; i++) {
		dd_ref dvr = dd_sb_variable(&ex,
		    fmt1("demand_var_%zu", i));

		dd_addr_map_set(&rv.demand, dd_addr_clone(&var_addrs.data[i]),
		    dvr);
	}

	/* The reverse walk. */
	rv.orig = &p->root;
	rv.shapes = &shapes;
	rv.source_shapes = shapes_arr;
	rv.n_src = n_sources;
	rv.wit = &wit;
	rv.fwd = &forward;
	rv.ex_src = ex_src;
	rv.fwd_inputs = fwd_inputs;
	rv.ex = &ex;
	rv.df = dd_df_sb(&ex);

	/* Seed: the query rows are demand against the first export's target. */
	{
		const dd_export *first = p->root.exports.data[0];

		first_target = resolve(&p->root, NULL, 0, &first->value);
		ex_query_routed = ex_query;
		seeded = rv_route(&rv, ex_query_routed, 0, &first_target);
		contrib_push(&rv.contribs, &first_target, seeded);
	}
	rv_walk(&rv, &ex, &p->root, NULL, 0);

	/* Demand-set closure per source: strip q, restrict, accumulate,
	 * export. */
	demand_names = n_sources
	    ? __dtfl_alloc(n_sources * sizeof(char *)) : NULL;
	demand_js = n_sources
	    ? __dtfl_alloc(n_sources * sizeof(size_t)) : NULL;
	for (i = 0; i < n_sources; i++) {
		dd_shape ssh = shapes_arr[i];
		dd_target src_t = tgt_source(i);
		ref_vec cs = contrib_take(&rv.contribs, &src_t);
		dd_ref combined, dist;
		const dd_import *imp = p->root.imports.data[i];
		char *name;

		if (cs.len == 0) {
			combined = dsets[i];
		} else {
			dd_ref merged = dd_sb_concat(&ex, cs.data, cs.len);
			dd_ref stripped = dd_sb_project(&ex, merged,
			    dd_val_project_kv(ssh.k, ssh.v));
			dd_ref semi = sb_semijoin_data(&ex, stripped, ex_src[i],
			    ssh.k, ssh.v);
			dd_ref two[2];

			two[0] = dsets[i];
			two[1] = semi;
			combined = dd_sb_concat(&ex, two, 2);
		}
		__dtfl_free(cs.data);
		dist = sb_distinct_full(&ex, combined, ssh.k, ssh.v);
		dd_sb_debug_inspect(&ex, dist, fmt1("demand_set:%zu", i));
		dd_sb_bind(&ex, dsets[i], dist);
		if (imp->from.kind == DD_SRC_INPUT)
			name = fmt1("demand:input%zu", imp->from.u.input);
		else
			name = __dtfl_strdup("demand:trace");
		demand_names[i] = name;
		demand_js[i] = dd_sb_export(&ex, __dtfl_strdup(name), dsets[i]);
	}

	/* Close: the explain scope becomes a Sub of the root; its demand
	 * exports become the program's exports.  Move the explain scope into
	 * a heap scope (no deep copy). */
	{
		dd_scope *child = __dtfl_calloc(1, sizeof *child);

		*child = ex.s;
		memset(&ex.s, 0, sizeof ex.s);
		ex_idx = root.s.items.len;
		scope_push_sub(&root.s, child);
	}
	for (i = 0; i < n_sources; i++) {
		/* demand_names[i] ownership transfers to the root export. */
		dd_sb_export(&root, demand_names[i],
		    ref_child_export(ex_idx, demand_js[i]));
	}

	/* Clean up. */
	dd_sb_free_boxes(&ex);
	dd_sb_free_boxes(&root);
	contrib_map_free(&rv.contribs);
	dd_addr_map_free(&rv.demand);
	for (i = 0; i < var_addrs.len; i++)
		dd_addr_free_fields(&var_addrs.data[i]);
	__dtfl_free(var_addrs.data);
	dd_target_free_fields(&first_target);
	dd_addr_map_free(&witness);
	dd_addr_map_free(&wit);
	dd_addr_map_free(&forward);
	shape_map_free(&shapes);
	__dtfl_free(shapes_arr);
	__dtfl_free(src_refs);
	__dtfl_free(ex_src);
	__dtfl_free(dsets);
	__dtfl_free(fwd_inputs);
	__dtfl_free(demand_names);
	__dtfl_free(demand_js);

	out = __dtfl_calloc(1, sizeof *out);
	out->root = root.s;
	return out;
}
