/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/scope_ir.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/scope_ir.c: the scope-tree IR and its optimizer.
 *
 * A Program is a tree of Scopes, each owning its IR (imports, feedback
 * vars, items in dependency order, binds, exports), with cross-scope flow
 * as explicit import/export edges.  A scope iterates iff it has vars.
 * Scope::optimize runs three within-scope rewrites to a fixed point:
 * collapse an Arrange of an arrangement, fuse a Linear into its sole
 * Linear consumer, and deduplicate structurally identical operators; a
 * scope boundary is a semantic barrier, so nothing merges across one.
 */
#include "ddir_int.h"

#include "../base/base_int.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Node / item / scope allocation and release                          */
/* ------------------------------------------------------------------ */

static void
source_free_fields(dd_source *s)
{
	if (s->kind == DD_SRC_TRACE)
		__dtfl_free(s->u.trace);
}

static void
import_free(dd_import *imp)
{
	__dtfl_free(imp->name);
	source_free_fields(&imp->from);
	__dtfl_free(imp);
}

static void
export_free(dd_export *e)
{
	__dtfl_free(e->name);
	__dtfl_free(e);
}

static void
node_free(dd_node *n)
{
	size_t i;

	switch (n->kind) {
	case DD_N_LINEAR:
		for (i = 0; i < n->ops.len; i++) {
			dd_linear_op *op = n->ops.data[i];

			dd_linear_op_free_fields(op);
			__dtfl_free(op);
		}
		dd_vec_free(&n->ops);
		break;
	case DD_N_CONCAT:
		for (i = 0; i < n->refs.len; i++)
			__dtfl_free(n->refs.data[i]);
		dd_vec_free(&n->refs);
		break;
	case DD_N_JOIN:
		dd_projection_free_fields(&n->projection);
		break;
	case DD_N_INSPECT:
		__dtfl_free(n->label);
		break;
	case DD_N_ARRANGE:
	case DD_N_REDUCE:
		break;
	}
	__dtfl_free(n);
}

static void
item_free(dd_item *it)
{
	if (it->kind == DD_I_OP)
		node_free(it->op);
	else
		dd_scope_free(it->sub);
	__dtfl_free(it);
}

void
dd_scope_free_fields(dd_scope *s)
{
	size_t i;

	__dtfl_free(s->name);
	for (i = 0; i < s->imports.len; i++)
		import_free(s->imports.data[i]);
	dd_vec_free(&s->imports);
	for (i = 0; i < s->vars.len; i++) {
		dd_var *v = s->vars.data[i];

		__dtfl_free(v->name);
		__dtfl_free(v);
	}
	dd_vec_free(&s->vars);
	for (i = 0; i < s->items.len; i++)
		item_free(s->items.data[i]);
	dd_vec_free(&s->items);
	for (i = 0; i < s->binds.len; i++)
		__dtfl_free(s->binds.data[i]);
	dd_vec_free(&s->binds);
	for (i = 0; i < s->exports.len; i++)
		export_free(s->exports.data[i]);
	dd_vec_free(&s->exports);
}

void
dd_scope_free(dd_scope *s)
{
	if (s == NULL)
		return;
	dd_scope_free_fields(s);
	__dtfl_free(s);
}

/* ------------------------------------------------------------------ */
/* Deep clone (used by backend_vec evaluate, which clones the program)  */
/* ------------------------------------------------------------------ */

static dd_ref *
ref_dup(const dd_ref *r)
{
	dd_ref *out = __dtfl_alloc(sizeof *out);

	*out = *r;
	return out;
}

static dd_node *
node_clone(const dd_node *n)
{
	dd_node *o = __dtfl_calloc(1, sizeof *o);
	size_t i;

	o->kind = n->kind;
	o->input = n->input;
	switch (n->kind) {
	case DD_N_LINEAR:
		for (i = 0; i < n->ops.len; i++) {
			dd_linear_op *op = __dtfl_alloc(sizeof *op);

			*op = dd_linear_op_clone(n->ops.data[i]);
			dd_vec_push(&o->ops, op);
		}
		break;
	case DD_N_CONCAT:
		for (i = 0; i < n->refs.len; i++)
			dd_vec_push(&o->refs, ref_dup(n->refs.data[i]));
		break;
	case DD_N_JOIN:
		o->right = n->right;
		o->projection = dd_projection_clone(&n->projection);
		break;
	case DD_N_REDUCE:
		o->reducer = n->reducer;
		break;
	case DD_N_INSPECT:
		o->label = __dtfl_strdup(n->label);
		break;
	case DD_N_ARRANGE:
		break;
	}
	return o;
}

dd_scope *
dd_scope_clone(const dd_scope *s)
{
	dd_scope *o = __dtfl_calloc(1, sizeof *o);
	size_t i;

	o->name = __dtfl_strdup(s->name);
	for (i = 0; i < s->imports.len; i++) {
		const dd_import *src = s->imports.data[i];
		dd_import *imp = __dtfl_alloc(sizeof *imp);

		imp->name = __dtfl_strdup(src->name);
		imp->from = src->from;
		if (src->from.kind == DD_SRC_TRACE)
			imp->from.u.trace = __dtfl_strdup(src->from.u.trace);
		dd_vec_push(&o->imports, imp);
	}
	for (i = 0; i < s->vars.len; i++) {
		const dd_var *src = s->vars.data[i];
		dd_var *v = __dtfl_alloc(sizeof *v);

		v->name = __dtfl_strdup(src->name);
		dd_vec_push(&o->vars, v);
	}
	for (i = 0; i < s->items.len; i++) {
		const dd_item *src = s->items.data[i];
		dd_item *it = __dtfl_alloc(sizeof *it);

		it->kind = src->kind;
		it->op = NULL;
		it->sub = NULL;
		if (src->kind == DD_I_OP)
			it->op = node_clone(src->op);
		else
			it->sub = dd_scope_clone(src->sub);
		dd_vec_push(&o->items, it);
	}
	for (i = 0; i < s->binds.len; i++) {
		const dd_bind *src = s->binds.data[i];
		dd_bind *b = __dtfl_alloc(sizeof *b);

		*b = *src;
		dd_vec_push(&o->binds, b);
	}
	for (i = 0; i < s->exports.len; i++) {
		const dd_export *src = s->exports.data[i];
		dd_export *e = __dtfl_alloc(sizeof *e);

		e->name = __dtfl_strdup(src->name);
		e->value = src->value;
		dd_vec_push(&o->exports, e);
	}
	return o;
}

/* ------------------------------------------------------------------ */
/* optimize (scope_ir.rs Scope::optimize)                              */
/* ------------------------------------------------------------------ */

/* Apply f to every Ref in this scope: op inputs, child import edges,
 * binds, exports (scope_ir.rs for_each_ref). */
static void
for_each_ref(dd_scope *s, void (*f)(dd_ref *, void *), void *ctx)
{
	size_t i, j;

	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];

		if (it->kind == DD_I_OP) {
			dd_node *n = it->op;

			switch (n->kind) {
			case DD_N_LINEAR:
			case DD_N_ARRANGE:
			case DD_N_REDUCE:
			case DD_N_INSPECT:
				f(&n->input, ctx);
				break;
			case DD_N_JOIN:
				f(&n->input, ctx); /* left */
				f(&n->right, ctx);
				break;
			case DD_N_CONCAT:
				for (j = 0; j < n->refs.len; j++)
					f(n->refs.data[j], ctx);
				break;
			}
		} else {
			dd_scope *child = it->sub;

			for (j = 0; j < child->imports.len; j++) {
				dd_import *imp = child->imports.data[j];

				if (imp->from.kind == DD_SRC_PARENT)
					f(&imp->from.u.parent, ctx);
			}
		}
	}
	for (i = 0; i < s->binds.len; i++)
		f(&((dd_bind *)s->binds.data[i])->value, ctx);
	for (i = 0; i < s->exports.len; i++)
		f(&((dd_export *)s->exports.data[i])->value, ctx);
}

/* Count references to each item from LIVE holders only (use_counts). */
static void
tally_ref(const dd_ref *r, size_t *counts)
{
	if (r->kind == DD_REF_LOCAL)
		counts[r->u.item]++;
	else if (r->kind == DD_REF_CHILD_EXPORT)
		counts[r->u.child_export.item]++;
}

static void
use_counts(dd_scope *s, const bool *dead, size_t *counts)
{
	size_t i, j;

	for (i = 0; i < s->items.len; i++)
		counts[i] = 0;
	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];

		if (dead[i])
			continue;
		if (it->kind == DD_I_OP) {
			dd_node *n = it->op;

			switch (n->kind) {
			case DD_N_LINEAR:
			case DD_N_ARRANGE:
			case DD_N_REDUCE:
			case DD_N_INSPECT:
				tally_ref(&n->input, counts);
				break;
			case DD_N_JOIN:
				tally_ref(&n->input, counts);
				tally_ref(&n->right, counts);
				break;
			case DD_N_CONCAT:
				for (j = 0; j < n->refs.len; j++)
					tally_ref(n->refs.data[j], counts);
				break;
			}
		} else {
			dd_scope *child = it->sub;

			for (j = 0; j < child->imports.len; j++) {
				dd_import *imp = child->imports.data[j];

				if (imp->from.kind == DD_SRC_PARENT)
					tally_ref(&imp->from.u.parent, counts);
			}
		}
	}
	for (i = 0; i < s->binds.len; i++)
		tally_ref(&((dd_bind *)s->binds.data[i])->value, counts);
	for (i = 0; i < s->exports.len; i++)
		tally_ref(&((dd_export *)s->exports.data[i])->value, counts);
}

struct redirect_ctx {
	size_t from;
	dd_ref to;
};

static void
redirect_apply(dd_ref *r, void *vctx)
{
	struct redirect_ctx *rc = vctx;

	if (r->kind == DD_REF_LOCAL && r->u.item == rc->from)
		*r = rc->to;
}

/* Redirect every Ref::Local(from) to `to`, and mark `from` dead. */
static void
redirect(dd_scope *s, size_t from, dd_ref to, bool *dead)
{
	struct redirect_ctx rc = { from, to };

	for_each_ref(s, redirect_apply, &rc);
	dead[from] = true;
}

static dd_ref
ref_local(size_t i)
{
	dd_ref r;

	r.kind = DD_REF_LOCAL;
	r.u.item = i;
	return r;
}

/* Arrange of an arrangement (an Arrange or Reduce item) is redundant. */
static bool
collapse_arranges(dd_scope *s, bool *dead)
{
	bool changed = false;
	size_t i;

	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];
		dd_node *n;
		size_t j;
		dd_item *target;

		if (dead[i] || it->kind != DD_I_OP)
			continue;
		n = it->op;
		if (n->kind != DD_N_ARRANGE || n->input.kind != DD_REF_LOCAL)
			continue;
		j = n->input.u.item;
		if (dead[j])
			continue;
		target = s->items.data[j];
		if (target->kind == DD_I_OP &&
		    (target->op->kind == DD_N_ARRANGE ||
		     target->op->kind == DD_N_REDUCE)) {
			redirect(s, i, ref_local(j), dead);
			changed = true;
		}
	}
	return changed;
}

/* Fuse a Linear into its Linear input when it is the only consumer. */
static bool
fuse_linear(dd_scope *s, bool *dead)
{
	size_t *counts = __dtfl_calloc(s->items.len ? s->items.len : 1,
	    sizeof *counts);
	bool changed = false;
	size_t i;

	use_counts(s, dead, counts);
	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];
		dd_node *n, *inner;
		dd_item *jt;
		size_t j, k;
		dd_vec fused = { 0 };

		if (dead[i] || it->kind != DD_I_OP)
			continue;
		n = it->op;
		if (n->kind != DD_N_LINEAR || n->input.kind != DD_REF_LOCAL)
			continue;
		j = n->input.u.item;
		if (dead[j] || counts[j] != 1)
			continue;
		jt = s->items.data[j];
		if (jt->kind != DD_I_OP || jt->op->kind != DD_N_LINEAR)
			continue;
		inner = jt->op;
		/* fused = inner.ops ++ this.ops, input = inner.input. */
		for (k = 0; k < inner->ops.len; k++)
			dd_vec_push(&fused, inner->ops.data[k]);
		for (k = 0; k < n->ops.len; k++)
			dd_vec_push(&fused, n->ops.data[k]);
		dd_vec_free(&inner->ops);
		dd_vec_free(&n->ops);
		n->ops = fused;
		n->input = inner->input;
		inner->ops = (dd_vec){ 0 };
		dead[j] = true;
		changed = true;
	}
	__dtfl_free(counts);
	return changed;
}

/* Format a node structurally for dedup keying (format!("{:?}", node)). */
static void
fmt_ref_key(const dd_ref *r, char **buf, size_t *len, size_t *cap)
{
	char tmp[48];

	switch (r->kind) {
	case DD_REF_LOCAL: snprintf(tmp, sizeof tmp, "L%zu", r->u.item); break;
	case DD_REF_IMPORT: snprintf(tmp, sizeof tmp, "I%zu", r->u.import); break;
	case DD_REF_VAR: snprintf(tmp, sizeof tmp, "V%zu", r->u.var); break;
	case DD_REF_CHILD_EXPORT:
		snprintf(tmp, sizeof tmp, "C%zu.%zu", r->u.child_export.item,
		    r->u.child_export.field);
		break;
	}
	{
		size_t n = strlen(tmp);

		while (*len + n + 1 > *cap) {
			*cap = *cap ? *cap * 2 : 64;
			*buf = __dtfl_realloc(*buf, *cap);
		}
		memcpy(*buf + *len, tmp, n + 1);
		*len += n;
	}
}

static void
append_str(char **buf, size_t *len, size_t *cap, const char *s)
{
	size_t n = strlen(s);

	while (*len + n + 1 > *cap) {
		*cap = *cap ? *cap * 2 : 64;
		*buf = __dtfl_realloc(*buf, *cap);
	}
	memcpy(*buf + *len, s, n + 1);
	*len += n;
}

static void fmt_term_key(const dd_term *t, char **buf, size_t *len, size_t *cap);

static void
fmt_term_vec(const dd_vec *v, char **buf, size_t *len, size_t *cap)
{
	size_t i;

	append_str(buf, len, cap, "[");
	for (i = 0; i < v->len; i++) {
		if (i)
			append_str(buf, len, cap, ",");
		fmt_term_key(v->data[i], buf, len, cap);
	}
	append_str(buf, len, cap, "]");
}

/* A structural key for a term, enough to tell identical ops apart. */
static void
fmt_term_key(const dd_term *t, char **buf, size_t *len, size_t *cap)
{
	char tmp[48];

	switch (t->kind) {
	case DD_T_VAR: snprintf(tmp, sizeof tmp, "v%zu", t->u.var); append_str(buf, len, cap, tmp); break;
	case DD_T_BOUND: snprintf(tmp, sizeof tmp, "b%zu", t->u.bound); append_str(buf, len, cap, tmp); break;
	case DD_T_INT: snprintf(tmp, sizeof tmp, "i%lld", (long long)t->u.int_lit); append_str(buf, len, cap, tmp); break;
	case DD_T_TUPLE: append_str(buf, len, cap, "T"); fmt_term_vec(&t->u.seq, buf, len, cap); break;
	case DD_T_LIST: append_str(buf, len, cap, "L"); fmt_term_vec(&t->u.seq, buf, len, cap); break;
	case DD_T_HASH: append_str(buf, len, cap, "H"); fmt_term_vec(&t->u.hash, buf, len, cap); break;
	case DD_T_SPREAD: append_str(buf, len, cap, "S("); fmt_term_key(t->u.spread, buf, len, cap); append_str(buf, len, cap, ")"); break;
	case DD_T_PROJ:
		append_str(buf, len, cap, "P(");
		fmt_term_key(t->u.proj.base, buf, len, cap);
		snprintf(tmp, sizeof tmp, ",%zu)", t->u.proj.index);
		append_str(buf, len, cap, tmp);
		break;
	case DD_T_INJECT:
		append_str(buf, len, cap, "J(");
		fmt_term_key(t->u.inject.tag, buf, len, cap);
		append_str(buf, len, cap, ",");
		fmt_term_key(t->u.inject.payload, buf, len, cap);
		append_str(buf, len, cap, ")");
		break;
	case DD_T_CASE:
		append_str(buf, len, cap, "C(");
		fmt_term_key(t->u.case_.scrutinee, buf, len, cap);
		fmt_term_vec(&t->u.case_.arms, buf, len, cap);
		if (t->u.case_.dflt) {
			append_str(buf, len, cap, "d");
			fmt_term_key(t->u.case_.dflt, buf, len, cap);
		}
		append_str(buf, len, cap, ")");
		break;
	case DD_T_FOLD:
		append_str(buf, len, cap, "F(");
		fmt_term_key(t->u.fold.list, buf, len, cap);
		append_str(buf, len, cap, ",");
		fmt_term_key(t->u.fold.init, buf, len, cap);
		append_str(buf, len, cap, ",");
		fmt_term_key(t->u.fold.step, buf, len, cap);
		append_str(buf, len, cap, ")");
		break;
	case DD_T_IF:
		append_str(buf, len, cap, "?(");
		fmt_term_key(t->u.if_.cond, buf, len, cap);
		append_str(buf, len, cap, ",");
		fmt_term_key(t->u.if_.then_, buf, len, cap);
		append_str(buf, len, cap, ",");
		fmt_term_key(t->u.if_.els, buf, len, cap);
		append_str(buf, len, cap, ")");
		break;
	case DD_T_UNARY:
		snprintf(tmp, sizeof tmp, "u%d.%u(", (int)t->u.unary.op, t->u.unary.tag);
		append_str(buf, len, cap, tmp);
		fmt_term_key(t->u.unary.arg, buf, len, cap);
		append_str(buf, len, cap, ")");
		break;
	case DD_T_BINARY:
		snprintf(tmp, sizeof tmp, "o%d(", (int)t->u.binary.op);
		append_str(buf, len, cap, tmp);
		fmt_term_key(t->u.binary.l, buf, len, cap);
		append_str(buf, len, cap, ",");
		fmt_term_key(t->u.binary.r, buf, len, cap);
		append_str(buf, len, cap, ")");
		break;
	}
}

static char *
node_key(const dd_node *n)
{
	char *buf = NULL;
	size_t len = 0, cap = 0, i;
	char tmp[48];

	switch (n->kind) {
	case DD_N_LINEAR:
		append_str(&buf, &len, &cap, "lin:");
		fmt_ref_key(&n->input, &buf, &len, &cap);
		for (i = 0; i < n->ops.len; i++) {
			dd_linear_op *op = n->ops.data[i];

			snprintf(tmp, sizeof tmp, "|%d:", (int)op->kind);
			append_str(&buf, &len, &cap, tmp);
			if (op->kind == DD_LIN_PROJECT) {
				fmt_term_key(op->proj.key, &buf, &len, &cap);
				append_str(&buf, &len, &cap, ";");
				fmt_term_key(op->proj.val, &buf, &len, &cap);
			} else if (op->term != NULL) {
				fmt_term_key(op->term, &buf, &len, &cap);
			}
		}
		break;
	case DD_N_CONCAT:
		append_str(&buf, &len, &cap, "cat:");
		for (i = 0; i < n->refs.len; i++) {
			fmt_ref_key(n->refs.data[i], &buf, &len, &cap);
			append_str(&buf, &len, &cap, "+");
		}
		break;
	case DD_N_ARRANGE:
		append_str(&buf, &len, &cap, "arr:");
		fmt_ref_key(&n->input, &buf, &len, &cap);
		break;
	case DD_N_JOIN:
		append_str(&buf, &len, &cap, "join:");
		fmt_ref_key(&n->input, &buf, &len, &cap);
		append_str(&buf, &len, &cap, ",");
		fmt_ref_key(&n->right, &buf, &len, &cap);
		append_str(&buf, &len, &cap, "(");
		fmt_term_key(n->projection.key, &buf, &len, &cap);
		append_str(&buf, &len, &cap, ";");
		fmt_term_key(n->projection.val, &buf, &len, &cap);
		append_str(&buf, &len, &cap, ")");
		break;
	case DD_N_REDUCE:
		snprintf(tmp, sizeof tmp, "red%d:", (int)n->reducer);
		append_str(&buf, &len, &cap, tmp);
		fmt_ref_key(&n->input, &buf, &len, &cap);
		break;
	case DD_N_INSPECT:
		append_str(&buf, &len, &cap, "ins:");
		fmt_ref_key(&n->input, &buf, &len, &cap);
		append_str(&buf, &len, &cap, n->label);
		break;
	}
	return buf;
}

/* Deduplicate structurally identical operators (later redirects to the
 * earlier, so references keep pointing backward). */
static bool
dedup(dd_scope *s, bool *dead)
{
	char **keys = __dtfl_calloc(s->items.len ? s->items.len : 1,
	    sizeof *keys);
	bool changed = false;
	size_t i, j;

	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];
		char *key;
		size_t first = SIZE_MAX;

		if (dead[i] || it->kind != DD_I_OP) {
			keys[i] = NULL;
			continue;
		}
		key = node_key(it->op);
		for (j = 0; j < i; j++)
			if (keys[j] != NULL && strcmp(keys[j], key) == 0) {
				first = j;
				break;
			}
		if (first != SIZE_MAX) {
			redirect(s, i, ref_local(first), dead);
			changed = true;
			__dtfl_free(key);
			keys[i] = NULL;
		} else {
			keys[i] = key;
		}
	}
	for (i = 0; i < s->items.len; i++)
		__dtfl_free(keys[i]);
	__dtfl_free(keys);
	return changed;
}

/* Remap an item-indexed Ref to compacted positions. */
static void
compact_apply(dd_ref *r, void *vremap)
{
	size_t *remap = vremap;

	if (r->kind == DD_REF_LOCAL) {
		DTFL_ASSERT(remap[r->u.item] != SIZE_MAX);
		r->u.item = remap[r->u.item];
	} else if (r->kind == DD_REF_CHILD_EXPORT) {
		DTFL_ASSERT(remap[r->u.child_export.item] != SIZE_MAX);
		r->u.child_export.item = remap[r->u.child_export.item];
	}
}

/* Drop dead items and remap every item-indexed Ref. */
static void
compact(dd_scope *s, const bool *dead)
{
	size_t n = s->items.len;
	size_t *remap = __dtfl_alloc((n ? n : 1) * sizeof *remap);
	size_t next = 0, i, w = 0;

	for (i = 0; i < n; i++) {
		if (!dead[i])
			remap[i] = next++;
		else
			remap[i] = SIZE_MAX;
	}
	for (i = 0; i < n; i++) {
		if (dead[i])
			item_free(s->items.data[i]);
		else
			s->items.data[w++] = s->items.data[i];
	}
	s->items.len = w;
	for_each_ref(s, compact_apply, remap);
	__dtfl_free(remap);
}

void
dd_scope_optimize(dd_scope *s)
{
	bool *dead;
	size_t i;

	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];

		if (it->kind == DD_I_SUB)
			dd_scope_optimize(it->sub);
	}
	dead = __dtfl_calloc(s->items.len ? s->items.len : 1, sizeof *dead);
	for (;;) {
		bool changed = false;

		changed |= collapse_arranges(s, dead);
		changed |= fuse_linear(s, dead);
		changed |= dedup(s, dead);
		if (!changed)
			break;
	}
	compact(s, dead);
	__dtfl_free(dead);
}

void
dd_program_optimize(dtfl_ddir_program *p)
{
	dd_scope_optimize(&p->root);
}

void
dd_program_free(dtfl_ddir_program *p)
{
	if (p == NULL)
		return;
	dd_scope_free_fields(&p->root);
	__dtfl_free(p);
}
