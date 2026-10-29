/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/lower.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/lower.c: AST to scope-tree IR lowering.
 *
 * Each `{ .. }` block becomes an owned child scope; cross-scope flow
 * becomes explicit import/export edges; feedback vars are first-class.
 * Statement order within a scope does not matter: a scope buckets its
 * statements, pre-declares its vars, topologically lowers lets and child
 * scopes by dependency (a let cycle is an error; use a var for
 * recursion), then lowers exports and the var bodies.  input/import
 * external sources are accepted only at the root scope.  AST terms are
 * owned by the parse arena, so lowering clones them into the independently
 * owned scope_ir nodes.
 *
 * Upstream panics on a program that parses but does not lower (an
 * unresolved name, an unknown scope, a let cycle); its server recovers with
 * catch_unwind.  Here those are errors (ADR-0012, 2026-10-29): the first
 * one is recorded in a lower_err, the failing lookup yields a placeholder
 * ref, and each scope stops at its next check and frees its scratch on the
 * normal path.  dd_lower_tree then frees the half-built tree, whose
 * placeholder refs are never followed.
 */
#include "ddir_int.h"

#include "../base/base_int.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* The first lowering error of a dd_lower_tree call; later ones are dropped. */
typedef struct lower_err {
	bool set;
	char msg[256];
} lower_err;

static dd_ref lower_fail(lower_err *le, const char *fmt, ...)
    DTFL_PRINTF(2, 3);

/* Record an error (if none yet) and return a placeholder ref. */
static dd_ref
lower_fail(lower_err *le, const char *fmt, ...)
{
	dd_ref r = { DD_REF_LOCAL, { 0 } };
	va_list ap;

	if (!le->set) {
		va_start(ap, fmt);
		vsnprintf(le->msg, sizeof le->msg, fmt, ap);
		va_end(ap);
		le->set = true;
	}
	return r;
}

/* ------------------------------------------------------------------ */
/* A tiny string-keyed map: name -> value (linear scan; programs are     */
/* small).                                                              */
/* ------------------------------------------------------------------ */

typedef struct name_ref {
	char *name; /* owned */
	dd_ref ref;
} name_ref;

typedef struct env_map {
	dd_vec entries; /* vec of name_ref* */
} env_map;

static void
env_set(env_map *m, const char *name, dd_ref r)
{
	size_t i;
	name_ref *e;

	for (i = 0; i < m->entries.len; i++) {
		e = m->entries.data[i];
		if (strcmp(e->name, name) == 0) {
			e->ref = r;
			return;
		}
	}
	e = __dtfl_alloc(sizeof *e);
	e->name = __dtfl_strdup(name);
	e->ref = r;
	dd_vec_push(&m->entries, e);
}

static bool
env_get(const env_map *m, const char *name, dd_ref *out)
{
	size_t i;

	for (i = 0; i < m->entries.len; i++) {
		name_ref *e = m->entries.data[i];

		if (strcmp(e->name, name) == 0) {
			*out = e->ref;
			return true;
		}
	}
	return false;
}

static void
env_free(env_map *m)
{
	size_t i;

	for (i = 0; i < m->entries.len; i++) {
		name_ref *e = m->entries.data[i];

		__dtfl_free(e->name);
		__dtfl_free(e);
	}
	dd_vec_free(&m->entries);
}

/* ------------------------------------------------------------------ */
/* Free-name collection                                                */
/* ------------------------------------------------------------------ */

/* A set of strings (owned copies). */
typedef struct str_set {
	dd_vec names; /* vec of char* */
} str_set;

static void
set_insert(str_set *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->names.len; i++)
		if (strcmp(s->names.data[i], name) == 0)
			return;
	dd_vec_push(&s->names, __dtfl_strdup(name));
}

static bool
set_contains(const str_set *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->names.len; i++)
		if (strcmp(s->names.data[i], name) == 0)
			return true;
	return false;
}

static void
set_free(str_set *s)
{
	size_t i;

	for (i = 0; i < s->names.len; i++)
		__dtfl_free(s->names.data[i]);
	dd_vec_free(&s->names);
}

/* Names this expression refers to that the surrounding scope must resolve.
 * For Name the name; for Qualified the scope name (lower.rs
 * expr_free_names). */
static void
expr_free_names(const dd_expr *e, str_set *out)
{
	size_t i;

	switch (e->kind) {
	case DD_E_INPUT:
	case DD_E_IMPORT:
		break;
	case DD_E_NAME:
		set_insert(out, e->u.name);
		break;
	case DD_E_QUALIFIED:
		set_insert(out, e->u.qualified.scope);
		break;
	case DD_E_MAP:
		expr_free_names(e->u.map.e, out);
		break;
	case DD_E_REDUCE:
		expr_free_names(e->u.reduce.e, out);
		break;
	case DD_E_FILTER:
	case DD_E_ENTER_AT:
	case DD_E_FLATMAP:
		expr_free_names(e->u.op_term.e, out);
		break;
	case DD_E_NEGATE:
	case DD_E_LIFT_ITER:
	case DD_E_ARRANGE:
		expr_free_names(e->u.op.e, out);
		break;
	case DD_E_INSPECT:
		expr_free_names(e->u.inspect.e, out);
		break;
	case DD_E_JOIN:
		expr_free_names(e->u.join.l, out);
		expr_free_names(e->u.join.r, out);
		break;
	case DD_E_CONCAT:
		for (i = 0; i < e->u.concat.len; i++)
			expr_free_names(e->u.concat.data[i], out);
		break;
	}
}

/* Collect names referenced in `body` not bound within body itself
 * (lower.rs collect_body_free_names).  Descends through nested scopes,
 * masking their local bindings. */
static void
collect_body_free_names(const dd_vec *body, str_set *out)
{
	str_set local = { 0 };
	str_set inner = { 0 };
	size_t i;

	for (i = 0; i < body->len; i++) {
		dd_stmt *st = body->data[i];

		if (st->name != NULL)
			set_insert(&local, st->name);
	}
	for (i = 0; i < body->len; i++) {
		dd_stmt *st = body->data[i];

		if (st->kind == DD_S_SCOPE)
			collect_body_free_names(&st->body, &inner);
		else
			expr_free_names(st->expr, &inner);
	}
	for (i = 0; i < inner.names.len; i++)
		if (!set_contains(&local, inner.names.data[i]))
			set_insert(out, inner.names.data[i]);
	set_free(&local);
	set_free(&inner);
}

/* ------------------------------------------------------------------ */
/* The lowering state for one scope                                    */
/* ------------------------------------------------------------------ */

typedef struct child_info {
	char *name;		/* child scope name (owned) */
	size_t item;		/* its index in items */
	str_set exports;	/* its export field order */
} child_info;

typedef struct scope_lower {
	bool is_root;
	dd_scope *out;		   /* the scope being built */
	env_map env;		   /* local names + imports -> ref */
	/* input n -> import idx (root, deduped) */
	dd_vec input_import_n;	   /* vec of size_t* keys */
	dd_vec input_import_i;	   /* vec of size_t* idxs */
	dd_vec trace_import_name;  /* vec of char* */
	dd_vec trace_import_i;	   /* vec of size_t* */
	dd_vec children;	   /* vec of child_info* */
	lower_err *err;		   /* shared by the whole tree */
} scope_lower;

static dd_scope *lower_scope_tree(const char *scope_name, const dd_vec *body,
    const env_map *parent_env, const str_set *exports_wanted, bool is_root,
    lower_err *le);

static dd_ref
push_node(scope_lower *sl, dd_node *n)
{
	dd_item *it = __dtfl_alloc(sizeof *it);
	dd_ref r;

	it->kind = DD_I_OP;
	it->op = n;
	it->sub = NULL;
	r.kind = DD_REF_LOCAL;
	r.u.item = sl->out->items.len;
	dd_vec_push(&sl->out->items, it);
	return r;
}

static dd_node *
node_new(dd_node_kind kind)
{
	dd_node *n = __dtfl_calloc(1, sizeof *n);

	n->kind = kind;
	return n;
}

static dd_ref
input_ref(scope_lower *sl, size_t n)
{
	size_t i;
	dd_import *imp;
	size_t *keyp, *idxp;
	dd_ref r;
	char buf[32];

	if (!sl->is_root)
		return lower_fail(sl->err,
		    "`input %zu` used outside the root scope", n);
	for (i = 0; i < sl->input_import_n.len; i++)
		if (*(size_t *)(void *)sl->input_import_n.data[i] == n) {
			r.kind = DD_REF_IMPORT;
			r.u.import = *(size_t *)(void *)sl->input_import_i.data[i];
			return r;
		}
	imp = __dtfl_alloc(sizeof *imp);
	snprintf(buf, sizeof buf, "input%zu", n);
	imp->name = __dtfl_strdup(buf);
	imp->from.kind = DD_SRC_INPUT;
	imp->from.u.input = n;
	r.kind = DD_REF_IMPORT;
	r.u.import = sl->out->imports.len;
	dd_vec_push(&sl->out->imports, imp);
	keyp = __dtfl_alloc(sizeof *keyp);
	*keyp = n;
	idxp = __dtfl_alloc(sizeof *idxp);
	*idxp = r.u.import;
	dd_vec_push(&sl->input_import_n, keyp);
	dd_vec_push(&sl->input_import_i, idxp);
	return r;
}

static dd_ref
trace_ref(scope_lower *sl, const char *name)
{
	size_t i;
	dd_import *imp;
	size_t *idxp;
	dd_ref r;

	if (!sl->is_root)
		return lower_fail(sl->err,
		    "`import \"%s\"` used outside the root scope", name);
	for (i = 0; i < sl->trace_import_name.len; i++)
		if (strcmp(sl->trace_import_name.data[i], name) == 0) {
			r.kind = DD_REF_IMPORT;
			r.u.import = *(size_t *)(void *)sl->trace_import_i.data[i];
			return r;
		}
	imp = __dtfl_alloc(sizeof *imp);
	imp->name = __dtfl_strdup(name);
	imp->from.kind = DD_SRC_TRACE;
	imp->from.u.trace = __dtfl_strdup(name);
	r.kind = DD_REF_IMPORT;
	r.u.import = sl->out->imports.len;
	dd_vec_push(&sl->out->imports, imp);
	dd_vec_push(&sl->trace_import_name, __dtfl_strdup(name));
	idxp = __dtfl_alloc(sizeof *idxp);
	*idxp = r.u.import;
	dd_vec_push(&sl->trace_import_i, idxp);
	return r;
}

static const child_info *
find_child(const scope_lower *sl, const char *scope)
{
	size_t i;

	for (i = 0; i < sl->children.len; i++) {
		child_info *c = sl->children.data[i];

		if (strcmp(c->name, scope) == 0)
			return c;
	}
	return NULL;
}

static dd_ref
qualified_ref(const scope_lower *sl, const char *scope, const char *field)
{
	const child_info *c = find_child(sl, scope);
	size_t pos, i;
	dd_ref r;

	if (c == NULL)
		return lower_fail(sl->err, "unknown scope `%s`", scope);
	pos = SIZE_MAX;
	for (i = 0; i < c->exports.names.len; i++)
		if (strcmp(c->exports.names.data[i], field) == 0) {
			pos = i;
			break;
		}
	/* Not reachable from input: c->exports is qualified_fields() of the
	 * body this expression came from, so it holds every `scope::field`
	 * the body names, and the child exports all of them or fails. */
	if (pos == SIZE_MAX)
		__dtfl_panic("scope `%s` does not export `%s`", scope, field);
	r.kind = DD_REF_CHILD_EXPORT;
	r.u.child_export.item = c->item;
	r.u.child_export.field = pos;
	return r;
}

/* A single-op Linear node over `input`. */
static dd_ref
push_linear1(scope_lower *sl, dd_ref input, dd_linear_op op)
{
	dd_node *n = node_new(DD_N_LINEAR);
	dd_linear_op *opp = __dtfl_alloc(sizeof *opp);

	n->input = input;
	*opp = op;
	dd_vec_push(&n->ops, opp);
	return push_node(sl, n);
}

static dd_ref lower_expr(scope_lower *sl, const dd_expr *e);

/* Build a one-op Linear carrying a cloned projection. */
static dd_ref
lower_project(scope_lower *sl, const dd_expr *e, const dd_projection *proj)
{
	dd_ref r = lower_expr(sl, e);
	dd_linear_op op;

	op.kind = DD_LIN_PROJECT;
	op.proj = dd_projection_clone(proj);
	op.term = NULL;
	return push_linear1(sl, r, op);
}

static dd_ref
lower_term_op(scope_lower *sl, const dd_expr *e, dd_linear_kind kind,
    const dd_term *term)
{
	dd_ref r = lower_expr(sl, e);
	dd_linear_op op;

	op.kind = kind;
	op.proj.key = NULL;
	op.proj.val = NULL;
	op.term = dd_term_clone(term);
	return push_linear1(sl, r, op);
}

static dd_ref
lower_simple_op(scope_lower *sl, const dd_expr *e, dd_linear_kind kind)
{
	dd_ref r = lower_expr(sl, e);
	dd_linear_op op;

	op.kind = kind;
	op.proj.key = NULL;
	op.proj.val = NULL;
	op.term = NULL;
	return push_linear1(sl, r, op);
}

static dd_ref
push_arrange(scope_lower *sl, dd_ref input)
{
	dd_node *n = node_new(DD_N_ARRANGE);

	n->input = input;
	return push_node(sl, n);
}

static dd_ref
lower_expr(scope_lower *sl, const dd_expr *e)
{
	dd_ref r;

	switch (e->kind) {
	case DD_E_INPUT:
		return input_ref(sl, e->u.input);
	case DD_E_IMPORT:
		return trace_ref(sl, e->u.name);
	case DD_E_NAME:
		if (!env_get(&sl->env, e->u.name, &r))
			return lower_fail(sl->err, "unresolved name `%s`",
			    e->u.name);
		return r;
	case DD_E_QUALIFIED:
		return qualified_ref(sl, e->u.qualified.scope,
		    e->u.qualified.field);
	case DD_E_MAP:
		return lower_project(sl, e->u.map.e, &e->u.map.proj);
	case DD_E_FILTER:
		return lower_term_op(sl, e->u.op_term.e, DD_LIN_FILTER,
		    e->u.op_term.term);
	case DD_E_NEGATE:
		return lower_simple_op(sl, e->u.op.e, DD_LIN_NEGATE);
	case DD_E_ENTER_AT:
		return lower_term_op(sl, e->u.op_term.e, DD_LIN_ENTER_AT,
		    e->u.op_term.term);
	case DD_E_FLATMAP:
		return lower_term_op(sl, e->u.op_term.e, DD_LIN_FLATMAP,
		    e->u.op_term.term);
	case DD_E_LIFT_ITER:
		return lower_simple_op(sl, e->u.op.e, DD_LIN_LIFT_ITER);
	case DD_E_ARRANGE: {
		dd_ref in = lower_expr(sl, e->u.op.e);

		return push_arrange(sl, in);
	}
	case DD_E_JOIN: {
		/* Join/Reduce consume arrangements; arrange inputs explicitly
		 * so optimize's within-scope dedup can share them. */
		dd_ref lr = lower_expr(sl, e->u.join.l);
		dd_ref la = push_arrange(sl, lr);
		dd_ref rr = lower_expr(sl, e->u.join.r);
		dd_ref ra = push_arrange(sl, rr);
		dd_node *n = node_new(DD_N_JOIN);

		n->input = la; /* left */
		n->right = ra;
		n->projection = dd_projection_clone(&e->u.join.proj);
		return push_node(sl, n);
	}
	case DD_E_REDUCE: {
		dd_ref in = lower_expr(sl, e->u.reduce.e);
		dd_ref a = push_arrange(sl, in);
		dd_node *n = node_new(DD_N_REDUCE);

		n->input = a;
		n->reducer = e->u.reduce.reducer;
		return push_node(sl, n);
	}
	case DD_E_INSPECT: {
		dd_ref in = lower_expr(sl, e->u.inspect.e);
		dd_node *n = node_new(DD_N_INSPECT);

		n->input = in;
		n->label = __dtfl_strdup(e->u.inspect.label);
		return push_node(sl, n);
	}
	case DD_E_CONCAT: {
		dd_node *n = node_new(DD_N_CONCAT);
		size_t i;

		for (i = 0; i < e->u.concat.len; i++) {
			dd_ref cr = lower_expr(sl, e->u.concat.data[i]);
			dd_ref *rp = __dtfl_alloc(sizeof *rp);

			*rp = cr;
			dd_vec_push(&n->refs, rp);
		}
		return push_node(sl, n);
	}
	}
	/* Not reachable from input: the parsers build only the kinds above. */
	__dtfl_panic("lower_expr: bad expr kind");
}

/* ------------------------------------------------------------------ */
/* Qualified-field discovery                                           */
/* ------------------------------------------------------------------ */

static void
collect_qualified(const dd_expr *e, const char *scope, str_set *out)
{
	size_t i;

	switch (e->kind) {
	case DD_E_QUALIFIED:
		if (strcmp(e->u.qualified.scope, scope) == 0)
			set_insert(out, e->u.qualified.field);
		break;
	case DD_E_MAP:
		collect_qualified(e->u.map.e, scope, out);
		break;
	case DD_E_REDUCE:
		collect_qualified(e->u.reduce.e, scope, out);
		break;
	case DD_E_FILTER:
	case DD_E_ENTER_AT:
	case DD_E_FLATMAP:
		collect_qualified(e->u.op_term.e, scope, out);
		break;
	case DD_E_NEGATE:
	case DD_E_LIFT_ITER:
	case DD_E_ARRANGE:
		collect_qualified(e->u.op.e, scope, out);
		break;
	case DD_E_INSPECT:
		collect_qualified(e->u.inspect.e, scope, out);
		break;
	case DD_E_JOIN:
		collect_qualified(e->u.join.l, scope, out);
		collect_qualified(e->u.join.r, scope, out);
		break;
	case DD_E_CONCAT:
		for (i = 0; i < e->u.concat.len; i++)
			collect_qualified(e->u.concat.data[i], scope, out);
		break;
	case DD_E_INPUT:
	case DD_E_IMPORT:
	case DD_E_NAME:
		break;
	}
}

/* The fields of child scope `scope` that this body references via
 * `scope::field` (lower.rs qualified_fields). */
static void
qualified_fields(const dd_vec *body, const char *scope, str_set *out)
{
	size_t i;

	for (i = 0; i < body->len; i++) {
		dd_stmt *st = body->data[i];

		if (st->kind != DD_S_SCOPE)
			collect_qualified(st->expr, scope, out);
	}
}

/* ------------------------------------------------------------------ */
/* Dependency ordering of lets and child scopes                        */
/* ------------------------------------------------------------------ */

/* Free names of `expr` defined at this level, excluding self_name. */
static void
expr_deps(const dd_expr *e, const str_set *defined, const char *self_name,
    str_set *out)
{
	str_set free = { 0 };
	size_t i;

	expr_free_names(e, &free);
	for (i = 0; i < free.names.len; i++) {
		const char *n = free.names.data[i];

		if (strcmp(n, self_name) != 0 && set_contains(defined, n))
			set_insert(out, n);
	}
	set_free(&free);
}

static void
scope_body_deps(const dd_vec *body, const str_set *defined,
    const char *self_name, str_set *out)
{
	str_set free = { 0 };
	size_t i;

	collect_body_free_names(body, &free);
	for (i = 0; i < free.names.len; i++) {
		const char *n = free.names.data[i];

		if (strcmp(n, self_name) != 0 && set_contains(defined, n))
			set_insert(out, n);
	}
	set_free(&free);
}

typedef enum item_kind { IK_LET, IK_SCOPE } item_kind;

typedef struct pending_item {
	item_kind kind;
	const char *name;
	const dd_expr *expr; /* IK_LET */
	const dd_vec *body;  /* IK_SCOPE */
	str_set deps;
	bool done;
} pending_item;

/* Record the let/scope cycle among the items not yet lowered. */
static void
lower_cycle(lower_err *le, const dd_vec *pending)
{
	size_t i, n;
	const char *sep = "";

	if (le->set)
		return;
	le->set = true;
	n = (size_t)snprintf(le->msg, sizeof le->msg, "Cyclic let/scope "
	    "dependency; use `var` for recursion: [");
	for (i = 0; i < pending->len && n < sizeof le->msg; i++) {
		const pending_item *pi = pending->data[i];

		if (pi->done)
			continue;
		n += (size_t)snprintf(le->msg + n, sizeof le->msg - n,
		    "%s\"%s\"", sep, pi->name);
		sep = ", ";
	}
	if (n < sizeof le->msg)
		snprintf(le->msg + n, sizeof le->msg - n, "]");
}

/* ------------------------------------------------------------------ */
/* lower_scope_tree                                                    */
/* ------------------------------------------------------------------ */

/*
 * Always returns a scope the caller owns and can free.  On an error it is
 * partial, le->set is true, and the caller discards it.
 */
static dd_scope *
lower_scope_tree(const char *scope_name, const dd_vec *body,
    const env_map *parent_env, const str_set *exports_wanted, bool is_root,
    lower_err *le)
{
	dd_scope *s = __dtfl_calloc(1, sizeof *s);
	scope_lower sl;
	str_set defined = { 0 };
	dd_vec pending = { 0 };	/* vec of pending_item* */
	dd_vec var_names = { 0 };	/* vec of const char* */
	dd_vec var_exprs = { 0 };	/* vec of const dd_expr* */
	dd_vec var_ids = { 0 };		/* vec of size_t* (var index) */
	size_t i, remaining;

	s->name = __dtfl_strdup(scope_name);
	memset(&sl, 0, sizeof sl);
	sl.is_root = is_root;
	sl.out = s;
	sl.err = le;

	/* Pre-create imports from the scope's transitive free names. */
	if (!is_root) {
		str_set free = { 0 };

		collect_body_free_names(body, &free);
		for (i = 0; i < free.names.len; i++) {
			const char *name = free.names.data[i];
			dd_ref from, r;
			dd_import *imp;

			if (!env_get(parent_env, name, &from)) {
				(void)lower_fail(le, "scope references unknown "
				    "outer name `%s`", name);
				break;
			}
			imp = __dtfl_alloc(sizeof *imp);
			imp->name = __dtfl_strdup(name);
			imp->from.kind = DD_SRC_PARENT;
			imp->from.u.parent = from;
			r.kind = DD_REF_IMPORT;
			r.u.import = s->imports.len;
			dd_vec_push(&s->imports, imp);
			env_set(&sl.env, name, r);
		}
		set_free(&free);
		if (le->set)
			goto cleanup;
	}

	/* Bucket statements: lets/scopes into `pending` (dependency order),
	 * vars pre-declared, exports handled last. */
	for (i = 0; i < body->len; i++) {
		dd_stmt *st = body->data[i];

		if (st->kind == DD_S_LET) {
			pending_item *pi = __dtfl_calloc(1, sizeof *pi);

			pi->kind = IK_LET;
			pi->name = st->name;
			pi->expr = st->expr;
			dd_vec_push(&pending, pi);
			set_insert(&defined, st->name);
		} else if (st->kind == DD_S_SCOPE) {
			pending_item *pi = __dtfl_calloc(1, sizeof *pi);

			pi->kind = IK_SCOPE;
			pi->name = st->name;
			pi->body = &st->body;
			dd_vec_push(&pending, pi);
			set_insert(&defined, st->name);
		}
	}

	/* Pre-declare feedback variables. */
	for (i = 0; i < body->len; i++) {
		dd_stmt *st = body->data[i];

		if (st->kind == DD_S_VAR) {
			dd_var *v = __dtfl_alloc(sizeof *v);
			size_t idx = s->vars.len;
			dd_ref r;
			size_t *idp;

			v->name = __dtfl_strdup(st->name);
			dd_vec_push(&s->vars, v);
			r.kind = DD_REF_VAR;
			r.u.var = idx;
			env_set(&sl.env, st->name, r);
			dd_vec_push(&var_names, (void *)st->name);
			dd_vec_push(&var_exprs, (void *)st->expr);
			idp = __dtfl_alloc(sizeof *idp);
			*idp = idx;
			dd_vec_push(&var_ids, idp);
		}
	}

	/* Compute deps (restricted to local let/scope names). */
	for (i = 0; i < pending.len; i++) {
		pending_item *pi = pending.data[i];

		if (pi->kind == IK_LET)
			expr_deps(pi->expr, &defined, pi->name, &pi->deps);
		else
			scope_body_deps(pi->body, &defined, pi->name, &pi->deps);
	}

	/* Topologically lower (deps emptied as items complete). */
	remaining = pending.len;
	while (remaining > 0) {
		size_t pos = SIZE_MAX;
		pending_item *pi;

		for (i = 0; i < pending.len; i++) {
			pi = pending.data[i];
			if (!pi->done && pi->deps.names.len == 0) {
				pos = i;
				break;
			}
		}
		if (pos == SIZE_MAX) {
			lower_cycle(le, &pending);
			goto cleanup;
		}
		pi = pending.data[pos];
		pi->done = true;
		remaining--;
		/* Remove pi->name from the deps of the rest. */
		for (i = 0; i < pending.len; i++) {
			pending_item *other = pending.data[i];
			size_t j;

			if (other->done)
				continue;
			for (j = 0; j < other->deps.names.len; j++)
				if (strcmp(other->deps.names.data[j],
				    pi->name) == 0) {
					__dtfl_free(other->deps.names.data[j]);
					other->deps.names.data[j] =
					    other->deps.names.data[
					    other->deps.names.len - 1];
					other->deps.names.len--;
					break;
				}
		}
		if (pi->kind == IK_LET) {
			dd_ref r = lower_expr(&sl, pi->expr);

			env_set(&sl.env, pi->name, r);
		} else {
			str_set wanted = { 0 };
			dd_scope *child;
			dd_item *it;
			size_t cidx;
			child_info *ci;

			qualified_fields(body, pi->name, &wanted);
			child = lower_scope_tree(pi->name, pi->body, &sl.env,
			    &wanted, false, le);
			it = __dtfl_alloc(sizeof *it);
			it->kind = DD_I_SUB;
			it->op = NULL;
			it->sub = child;
			cidx = s->items.len;
			dd_vec_push(&s->items, it);
			ci = __dtfl_alloc(sizeof *ci);
			ci->name = __dtfl_strdup(pi->name);
			ci->item = cidx;
			ci->exports = wanted; /* move */
			dd_vec_push(&sl.children, ci);
		}
		if (le->set)
			goto cleanup;
	}

	/* Lower var bodies and emit binds. */
	for (i = 0; i < var_exprs.len; i++) {
		dd_ref r = lower_expr(&sl, var_exprs.data[i]);
		dd_bind *b = __dtfl_alloc(sizeof *b);

		b->var = *(size_t *)(void *)var_ids.data[i];
		b->value = r;
		dd_vec_push(&s->binds, b);
	}
	if (le->set)
		goto cleanup;

	/* Exports: root takes its export statements; a child surrenders the
	 * fields its parent asked for. */
	if (is_root) {
		for (i = 0; i < body->len; i++) {
			dd_stmt *st = body->data[i];
			dd_export *e;

			if (st->kind != DD_S_EXPORT)
				continue;
			e = __dtfl_alloc(sizeof *e);
			e->name = __dtfl_strdup(st->name);
			e->value = lower_expr(&sl, st->expr);
			dd_vec_push(&s->exports, e);
		}
	} else {
		for (i = 0; i < exports_wanted->names.len; i++) {
			const char *field = exports_wanted->names.data[i];
			dd_export *e;
			dd_ref r;

			if (!env_get(&sl.env, field, &r)) {
				(void)lower_fail(le, "scope export `%s` is not "
				    "a local name", field);
				break;
			}
			e = __dtfl_alloc(sizeof *e);
			e->name = __dtfl_strdup(field);
			e->value = r;
			dd_vec_push(&s->exports, e);
		}
	}

cleanup:
	/* Lowering scratch; the scope itself goes to the caller. */
	for (i = 0; i < pending.len; i++) {
		pending_item *pi = pending.data[i];

		set_free(&pi->deps);
		__dtfl_free(pi);
	}
	dd_vec_free(&pending);
	dd_vec_free(&var_names);
	dd_vec_free(&var_exprs);
	for (i = 0; i < var_ids.len; i++)
		__dtfl_free(var_ids.data[i]);
	dd_vec_free(&var_ids);
	for (i = 0; i < sl.children.len; i++) {
		child_info *ci = sl.children.data[i];

		__dtfl_free(ci->name);
		set_free(&ci->exports);
		__dtfl_free(ci);
	}
	dd_vec_free(&sl.children);
	for (i = 0; i < sl.input_import_n.len; i++)
		__dtfl_free(sl.input_import_n.data[i]);
	dd_vec_free(&sl.input_import_n);
	for (i = 0; i < sl.input_import_i.len; i++)
		__dtfl_free(sl.input_import_i.data[i]);
	dd_vec_free(&sl.input_import_i);
	for (i = 0; i < sl.trace_import_name.len; i++)
		__dtfl_free(sl.trace_import_name.data[i]);
	dd_vec_free(&sl.trace_import_name);
	for (i = 0; i < sl.trace_import_i.len; i++)
		__dtfl_free(sl.trace_import_i.data[i]);
	dd_vec_free(&sl.trace_import_i);
	env_free(&sl.env);
	set_free(&defined);
	return s;
}

/*
 * Lower a parsed AST (a vec of dd_stmt*) into a Program.  The returned
 * program's root scope owns the IR independently of the AST arena.  A
 * program that does not lower returns NULL with the reason in
 * dtfl_last_error_detail.
 */
dtfl_ddir_program *
dd_lower_tree(const dd_vec *stmts)
{
	dtfl_ddir_program *p;
	env_map empty = { 0 };
	str_set no_exports = { 0 };
	lower_err le = { false, "" };
	dd_scope *root = lower_scope_tree("root", stmts, &empty, &no_exports,
	    true, &le);

	if (le.set) {
		dd_scope_free(root);
		(void)__dtfl_err(DTFL_EINVAL, "%s", le.msg);
		return NULL;
	}
	p = __dtfl_alloc(sizeof *p);
	p->root = *root;
	__dtfl_free(root);
	return p;
}
