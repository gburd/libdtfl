/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/backend/mod.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/backend.c: the scope-tree render walk.
 *
 * render_tree walks a scope at a given depth (root = 0): it creates the
 * feedback variables first (so items can refer to them), renders items in
 * dependency order, closes the loops with the binds, and surrenders the
 * exports.  An item resolves its input Refs to already-rendered
 * collections, variable collections or entered imports.  A nested { } is a
 * real timely region (same timestamp type) rendered at depth + 1; its
 * exports leave the region and then pop the child's PointStamp coordinate
 * with leave_dynamic(depth + 1).  Temporally, a scope that iterates (has
 * vars) advances its own PointStamp coordinate: dtfl_variable_new_at builds
 * a Variable whose feedback summary is feedback_summary(depth, 1) at that
 * coordinate.
 *
 * The whole program runs in one iterative dynamic scope (backend/vec.rs),
 * so the live dataflow's time is literally Product<u64, PointStamp<u64>>.
 */
#include "ddir_int.h"
#include "backend_int.h"

#include "../base/base_int.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/reduce.h"
#include "dtfl/record.h"
#include "dtfl/timely.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Rendered items                                                      */
/* ------------------------------------------------------------------ */

/* A rendered item is a collection (we treat Arrange as the identity: the
 * collection-level join/reduce arrange their inputs internally, so an
 * explicit Arrange node renders as a passthrough). */
typedef struct ritem {
	bool is_sub;
	dtfl_collection *col;	 /* Op: the rendered collection (owned) */
	dtfl_collection **exports; /* Sub: one per child export (owned) */
	size_t n_exports;
} ritem;

/* The state threaded through one scope's render. */
typedef struct render_ctx {
	const dd_scope *s;
	dtfl_scope *scope;
	size_t depth;			/* this scope's nesting depth (root 0) */
	dtfl_collection **imports;	/* one per import, entered (owned) */
	dtfl_collection **var_cols;	/* one per var (owned handles) */
	dtfl_variable **var_handles;	/* one per var (owned until set) */
	ritem *items;			/* one per item */
	int err;
} render_ctx;

static void render_scope(const dd_scope *s, dtfl_scope *scope, size_t depth,
    dtfl_collection *const *imports, dtfl_collection ***exports_out,
    size_t *n_exports_out, int *err);

/* Resolve a Ref to a borrowed collection (caller must clone to own). */
static dtfl_collection *
resolve(render_ctx *rc, const dd_ref *r)
{
	switch (r->kind) {
	case DD_REF_LOCAL: {
		ritem *it = &rc->items[r->u.item];

		DTFL_ASSERT(!it->is_sub);
		return it->col;
	}
	case DD_REF_IMPORT:
		return rc->imports[r->u.import];
	case DD_REF_VAR:
		return rc->var_cols[r->u.var];
	case DD_REF_CHILD_EXPORT: {
		ritem *it = &rc->items[r->u.child_export.item];

		DTFL_ASSERT(it->is_sub);
		return it->exports[r->u.child_export.field];
	}
	}
	__dtfl_panic("resolve: bad ref kind");
}

/* A keep-all filter: the cheapest way to make a fresh owned handle over a
 * borrowed collection's stream (the handle is a view; copying it is a
 * no-op operator). */
static bool
keep_all(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)rec; (void)len;
	return true;
}

static dtfl_collection *
copy_collection(dtfl_collection *c)
{
	return dtfl_collection_filter(c, keep_all, DTFL_NOCTX);
}

/* Render a Linear node: the whole op chain as one faithful operator at this
 * scope's depth (backend/vec.rs render_linear). */
static dtfl_collection *
render_linear(render_ctx *rc, const dd_node *n)
{
	dtfl_collection *in = resolve(rc, &n->input);

	return dd_render_linear(in, &n->ops, rc->depth);
}

static dtfl_collection *
render_concat(render_ctx *rc, const dd_node *n)
{
	dtfl_collection *acc = NULL;
	size_t i;

	for (i = 0; i < n->refs.len; i++) {
		dtfl_collection *r = resolve(rc, n->refs.data[i]);

		if (acc == NULL) {
			acc = copy_collection(r);
		} else {
			dtfl_collection *next = dtfl_collection_concat(acc, r);

			dtfl_collection_free(acc);
			acc = next;
		}
	}
	return acc;
}

static dtfl_reduce_fn
reducer_fn(dd_reducer r)
{
	switch (r) {
	case DD_RED_MIN: return reduce_min_fn;
	case DD_RED_DISTINCT: return reduce_distinct_fn;
	case DD_RED_COUNT: return reduce_count_fn;
	case DD_RED_COLLECT: return reduce_collect_fn;
	}
	__dtfl_panic("reducer_fn: bad reducer");
}

/* The inspect callback: upstream prints each update to stderr. */
static void
inspect_fn(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	(void)ctx; (void)rec; (void)len; (void)time; (void)diff;
	/* Silent by default; a debugging build may hook here. */
}

static dtfl_collection *
render_node(render_ctx *rc, const dd_node *n)
{
	switch (n->kind) {
	case DD_N_LINEAR:
		return render_linear(rc, n);
	case DD_N_CONCAT:
		return render_concat(rc, n);
	case DD_N_ARRANGE:
		/* Arrange is the identity for the collection-level ops that
		 * arrange internally; pass the input through as a copy. */
		return copy_collection(resolve(rc, &n->input));
	case DD_N_JOIN: {
		dtfl_collection *l = resolve(rc, &n->input);
		dtfl_collection *r = resolve(rc, &n->right);

		return dtfl_collection_join_map(l, r, dd_value_type(),
		    dd_value_type(), dd_value_type(), dd_pair_type(), join_fn,
		    DTFL_CTX((void *)&n->projection, NULL));
	}
	case DD_N_REDUCE: {
		dtfl_collection *in = resolve(rc, &n->input);
		dtfl_diff *i64 = dtfl_diff_i64();
		dtfl_collection *out = dtfl_collection_reduce(in, dd_value_type(),
		    dd_value_type(), dd_value_type(), i64, reducer_fn(n->reducer),
		    DTFL_NOCTX);

		dtfl_diff_free(i64);
		return out;
	}
	case DD_N_INSPECT:
		return dtfl_collection_inspect(resolve(rc, &n->input),
		    inspect_fn, DTFL_NOCTX);
	}
	__dtfl_panic("render_node: bad node kind");
}

/* The build callback for a child scope: a real timely region (same timestamp
 * type).  Enter the parent collections, render the child at depth + 1, and
 * leave its exports back to the parent.  Popping the child's PointStamp
 * coordinate (leave_dynamic(depth + 1)) is done by the caller after the
 * region closes. */
typedef struct child_build {
	const dd_scope *child;
	size_t child_depth;
	dtfl_collection *const *parent_cols; /* one per import (parent scope) */
	size_t n_imports;
	dtfl_collection **leaving;	/* exports, left to the parent (owned) */
	size_t n_exports;
	int err;
} child_build;

static void
child_build_enter(dtfl_scope *inner, void *arg)
{
	child_build *cb = arg;
	dtfl_collection **entered = cb->n_imports
	    ? __dtfl_alloc(cb->n_imports * sizeof(dtfl_collection *)) : NULL;
	dtfl_collection **exports = NULL;
	size_t n = 0, i;

	for (i = 0; i < cb->n_imports; i++)
		entered[i] = dtfl_collection_enter(
		    (dtfl_collection *)cb->parent_cols[i], inner);
	render_scope(cb->child, inner, cb->child_depth, entered, &exports, &n,
	    &cb->err);
	cb->n_exports = n;
	cb->leaving = n ? __dtfl_alloc(n * sizeof(dtfl_collection *)) : NULL;
	for (i = 0; i < n; i++) {
		cb->leaving[i] = dtfl_collection_leave(exports[i]);
		dtfl_collection_free(exports[i]);
	}
	__dtfl_free(exports);
	for (i = 0; i < cb->n_imports; i++)
		dtfl_collection_free(entered[i]);
	__dtfl_free(entered);
}

/* ------------------------------------------------------------------ */
/* render_scope: the core walk                                         */
/* ------------------------------------------------------------------ */

static void
render_scope(const dd_scope *s, dtfl_scope *scope, size_t depth,
    dtfl_collection *const *imports, dtfl_collection ***exports_out,
    size_t *n_exports_out, int *err)
{
	render_ctx rc;
	size_t i;
	dtfl_diff *i64 = dtfl_diff_i64();

	memset(&rc, 0, sizeof rc);
	rc.s = s;
	rc.scope = scope;
	rc.depth = depth;
	rc.imports = (dtfl_collection **)imports;
	rc.err = 0;
	rc.items = s->items.len
	    ? __dtfl_calloc(s->items.len, sizeof(ritem)) : NULL;

	/* Create feedback variables first.  Each advances the PointStamp
	 * coordinate at this scope's depth: feedback_summary(depth, 1), which
	 * dtfl_variable_new_at builds at 1-based level = depth. */
	if (s->vars.len) {
		rc.var_cols = __dtfl_calloc(s->vars.len, sizeof(dtfl_collection *));
		rc.var_handles = __dtfl_calloc(s->vars.len,
		    sizeof(dtfl_variable *));
		for (i = 0; i < s->vars.len; i++)
			rc.var_handles[i] = dtfl_variable_new_at(scope,
			    dd_pair_type(), i64, depth, &rc.var_cols[i]);
	}

	/* Render items in order. */
	for (i = 0; i < s->items.len && rc.err == 0; i++) {
		dd_item *it = s->items.data[i];

		if (it->kind == DD_I_OP) {
			rc.items[i].is_sub = false;
			rc.items[i].col = render_node(&rc, it->op);
			if (rc.items[i].col == NULL)
				rc.err = DTFL_ENOTSUP;
		} else {
			dd_scope *child = it->sub;
			child_build cb;
			dtfl_collection **parent_cols;
			size_t j;

			memset(&cb, 0, sizeof cb);
			cb.child = child;
			parent_cols = child->imports.len
			    ? __dtfl_alloc(child->imports.len *
			    sizeof(dtfl_collection *)) : NULL;
			for (j = 0; j < child->imports.len; j++) {
				dd_import *imp = child->imports.data[j];

				DTFL_ASSERT(imp->from.kind == DD_SRC_PARENT);
				parent_cols[j] = resolve(&rc,
				    &imp->from.u.parent);
			}
			cb.parent_cols = parent_cols;
			cb.n_imports = child->imports.len;
			cb.child_depth = depth + 1;
			cb.err = 0;
			/* Every { } is a timely region (same timestamp type).
			 * child_build_enter enters imports and leaves exports;
			 * popping the child's PointStamp coordinate is below. */
			dtfl_scope_region(scope, child->name,
			    child_build_enter, &cb);
			if (cb.err != 0)
				rc.err = cb.err;
			rc.items[i].is_sub = true;
			rc.items[i].n_exports = cb.n_exports;
			/* leave_dynamic(depth + 1): strip the child's iteration
			 * coordinate from each export's embedded time. */
			if (cb.n_exports) {
				dtfl_collection **lifted = __dtfl_alloc(
				    cb.n_exports * sizeof(dtfl_collection *));
				size_t e;

				for (e = 0; e < cb.n_exports; e++) {
					lifted[e] = dtfl_collection_leave_dynamic(
					    cb.leaving[e], depth + 1);
					dtfl_collection_free(cb.leaving[e]);
				}
				__dtfl_free(cb.leaving);
				rc.items[i].exports = lifted;
			} else {
				rc.items[i].exports = cb.leaving;
			}
			__dtfl_free(parent_cols);
		}
	}

	/* Close the loops: bind each var to its value. */
	for (i = 0; i < s->binds.len && rc.err == 0; i++) {
		dd_bind *b = s->binds.data[i];
		dtfl_collection *copy = copy_collection(resolve(&rc, &b->value));

		dtfl_variable_set(rc.var_handles[b->var], copy);
		rc.var_handles[b->var] = NULL;
	}
	/* Any var never bound (should not happen) must still be consumed. */

	/* Surrender exports (as owned copies). */
	if (rc.err == 0) {
		size_t ne = s->exports.len;
		dtfl_collection **out = ne
		    ? __dtfl_alloc(ne * sizeof(dtfl_collection *)) : NULL;

		for (i = 0; i < ne; i++) {
			dd_export *e = s->exports.data[i];

			out[i] = copy_collection(resolve(&rc, &e->value));
		}
		*exports_out = out;
		*n_exports_out = ne;
	} else {
		*exports_out = NULL;
		*n_exports_out = 0;
	}
	*err = rc.err;

	/* Free the rendered item handles and var handles. */
	for (i = 0; i < s->items.len; i++) {
		if (rc.items[i].is_sub) {
			size_t j;

			for (j = 0; j < rc.items[i].n_exports; j++)
				dtfl_collection_free(rc.items[i].exports[j]);
			__dtfl_free(rc.items[i].exports);
		} else if (rc.items[i].col != NULL) {
			dtfl_collection_free(rc.items[i].col);
		}
	}
	__dtfl_free(rc.items);
	for (i = 0; i < s->vars.len; i++)
		dtfl_collection_free(rc.var_cols[i]);
	__dtfl_free(rc.var_cols);
	__dtfl_free(rc.var_handles);
	dtfl_diff_free(i64);
	/* render_scope returns via the out params; no single collection. */
}

int
dd_render_program(const dtfl_ddir_program *p, dtfl_scope *scope,
    dtfl_collection *const *inputs, size_t n_inputs,
    dtfl_collection **exports)
{
	const dd_scope *root = &p->root;
	dtfl_collection **import_cols;
	dtfl_collection **out = NULL;
	size_t n_out = 0, i;
	int err = 0;

	/* Bind the root imports: Source::Input(n) -> inputs[n]. */
	import_cols = root->imports.len
	    ? __dtfl_alloc(root->imports.len * sizeof(dtfl_collection *)) : NULL;
	for (i = 0; i < root->imports.len; i++) {
		dd_import *imp = root->imports.data[i];

		if (imp->from.kind != DD_SRC_INPUT ||
		    imp->from.u.input >= n_inputs) {
			__dtfl_free(import_cols);
			return __dtfl_err(DTFL_EINVAL, "DDIR build: unbound "
			    "root import");
		}
		import_cols[i] = (dtfl_collection *)inputs[imp->from.u.input];
	}

	render_scope(root, scope, 0, import_cols, &out, &n_out, &err);
	__dtfl_free(import_cols);
	if (err != 0) {
		for (i = 0; i < n_out; i++)
			dtfl_collection_free(out[i]);
		__dtfl_free(out);
		return err;
	}
	for (i = 0; i < n_out; i++)
		exports[i] = out[i];
	__dtfl_free(out);
	return 0;
}
