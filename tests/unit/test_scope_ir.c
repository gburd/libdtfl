/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_scope_ir.c: the scope-tree IR can express a scoped,
 * iterative program (scope_ir.rs tests).
 *
 * Builds a reach-shaped program by hand (a root scope with two inputs and
 * one child scope that joins a feedback var against an import and binds
 * the var back), exercising items (Op and Sub), every Ref variant and the
 * Input/Parent Source variants, and proving the shape round-trips.
 */
#include "dtfl_test.h"

#include "dtfl/ddir.h"
#include "ddir/ddir_int.h"
#include "base/base_int.h"

#include <string.h>

static dd_ref
ref(dd_ref_kind k, size_t a, size_t b)
{
	dd_ref r;

	r.kind = k;
	if (k == DD_REF_LOCAL)
		r.u.item = a;
	else if (k == DD_REF_IMPORT)
		r.u.import = a;
	else if (k == DD_REF_VAR)
		r.u.var = a;
	else {
		r.u.child_export.item = a;
		r.u.child_export.field = b;
	}
	return r;
}

static dd_projection
noproj(void)
{
	dd_projection p;

	p.key = dd_term_new(DD_T_TUPLE);
	p.val = dd_term_new(DD_T_TUPLE);
	return p;
}

static void
push_import(dd_scope *s, const char *name, dd_source src)
{
	dd_import *imp = __dtfl_alloc(sizeof *imp);

	imp->name = __dtfl_strdup(name);
	imp->from = src;
	dd_vec_push(&s->imports, imp);
}

static dd_source
src_input(size_t n)
{
	dd_source s;

	s.kind = DD_SRC_INPUT;
	s.u.input = n;
	return s;
}

static dd_source
src_parent(dd_ref r)
{
	dd_source s;

	s.kind = DD_SRC_PARENT;
	s.u.parent = r;
	return s;
}

static void
push_var(dd_scope *s, const char *name)
{
	dd_var *v = __dtfl_alloc(sizeof *v);

	v->name = __dtfl_strdup(name);
	dd_vec_push(&s->vars, v);
}

static void
push_op(dd_scope *s, dd_node *n)
{
	dd_item *it = __dtfl_alloc(sizeof *it);

	it->kind = DD_I_OP;
	it->op = n;
	it->sub = NULL;
	dd_vec_push(&s->items, it);
}

static void
push_sub(dd_scope *s, dd_scope *child)
{
	dd_item *it = __dtfl_alloc(sizeof *it);

	it->kind = DD_I_SUB;
	it->op = NULL;
	it->sub = child;
	dd_vec_push(&s->items, it);
}

static void
push_bind(dd_scope *s, size_t var, dd_ref value)
{
	dd_bind *b = __dtfl_alloc(sizeof *b);

	b->var = var;
	b->value = value;
	dd_vec_push(&s->binds, b);
}

static void
push_export(dd_scope *s, const char *name, dd_ref value)
{
	dd_export *e = __dtfl_alloc(sizeof *e);

	e->name = __dtfl_strdup(name);
	e->value = value;
	dd_vec_push(&s->exports, e);
}

DTFL_TEST(scope_ir__expresses_a_scoped_iterative_program)
{
	dd_scope *inner = __dtfl_calloc(1, sizeof *inner);
	dtfl_ddir_program prog;
	dd_item *item0;
	dd_scope *reach;
	dd_node *join, *concat;

	(void)params; (void)data;
	inner->name = __dtfl_strdup("reach");
	push_import(inner, "edges", src_parent(ref(DD_REF_IMPORT, 0, 0)));
	push_import(inner, "roots", src_parent(ref(DD_REF_IMPORT, 1, 0)));
	push_var(inner, "reach");
	/* proposals = reach JOIN edges  (Var + Import) */
	join = __dtfl_calloc(1, sizeof *join);
	join->kind = DD_N_JOIN;
	join->input = ref(DD_REF_VAR, 0, 0);
	join->right = ref(DD_REF_IMPORT, 0, 0);
	join->projection = noproj();
	push_op(inner, join);
	/* body = roots + proposals  (Import + Local) */
	concat = __dtfl_calloc(1, sizeof *concat);
	concat->kind = DD_N_CONCAT;
	{
		dd_ref *r0 = __dtfl_alloc(sizeof *r0);
		dd_ref *r1 = __dtfl_alloc(sizeof *r1);

		*r0 = ref(DD_REF_IMPORT, 1, 0);
		*r1 = ref(DD_REF_LOCAL, 0, 0);
		dd_vec_push(&concat->refs, r0);
		dd_vec_push(&concat->refs, r1);
	}
	push_op(inner, concat);
	push_bind(inner, 0, ref(DD_REF_LOCAL, 1, 0)); /* reach <- body */
	push_export(inner, "reach", ref(DD_REF_VAR, 0, 0));

	memset(&prog, 0, sizeof prog);
	prog.root.name = __dtfl_strdup("root");
	push_import(&prog.root, "in0", src_input(0));
	push_import(&prog.root, "in1", src_input(1));
	push_sub(&prog.root, inner);
	push_export(&prog.root, "result", ref(DD_REF_CHILD_EXPORT, 0, 0));

	assert_size(prog.root.items.len, ==, 1);
	item0 = prog.root.items.data[0];
	assert_int(item0->kind, ==, DD_I_SUB);
	reach = item0->sub;
	assert_size(reach->vars.len, ==, 1);
	assert_int(((dd_export *)(void *)prog.root.exports.data[0])->value.kind, ==,
	    DD_REF_CHILD_EXPORT);
	assert_int(((dd_import *)(void *)prog.root.imports.data[0])->from.kind, ==,
	    DD_SRC_INPUT);
	assert_int(((dd_bind *)(void *)reach->binds.data[0])->value.kind, ==,
	    DD_REF_LOCAL);
	assert_size(((dd_bind *)(void *)reach->binds.data[0])->value.u.item, ==, 1);

	dd_scope_free_fields(&prog.root);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(scope_ir__expresses_a_scoped_iterative_program),
	DTFL_T_END
};

DTFL_TEST_MAIN("scope_ir", tests)
