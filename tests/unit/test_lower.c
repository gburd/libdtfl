/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_lower.c: AST to scope-tree lowering and the optimizer
 * (lower.rs tree_tests and optimize_tests).
 *
 * Each test parses a pipe program, lowers it to the scope tree, and
 * asserts the resulting shape (imports, child scopes, feedback vars,
 * binds, exports) or, for the optimizer tests, the operator counts after
 * optimize collapses redundant arranges, fuses linear chains and dedups.
 */
#include "dtfl_test.h"

#include "dtfl/ddir.h"
#include "ddir/ddir_int.h"
#include "ddir/parse_int.h"
#include "base/base_int.h"

#include <string.h>

/* Parse a pipe program and lower it; returns the program (NULL on parse
 * error).  The caller frees via dd_program_free and dd_arena_free_all(pc). */
static dtfl_ddir_program *
parse_and_lower(const char *src, dd_parse_ctx *pc)
{
	dd_vec stmts = { 0 };
	dtfl_ddir_program *p;

	if (!dd_parse_pipe(src, &stmts, pc))
		return NULL;
	p = dd_lower_tree(&stmts);
	__dtfl_free(stmts.data);
	dd_arena_free_all(pc);
	return p;
}

/* Count the Sub children of a scope. */
static size_t
sub_count(const dd_scope *s)
{
	size_t i, n = 0;

	for (i = 0; i < s->items.len; i++)
		if (((dd_item *)(void *)s->items.data[i])->kind == DD_I_SUB)
			n++;
	return n;
}

static dd_scope *
first_sub(const dd_scope *s)
{
	size_t i;

	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];

		if (it->kind == DD_I_SUB)
			return it->sub;
	}
	return NULL;
}

static bool
import_named(const dd_scope *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->imports.len; i++)
		if (strcmp(((dd_import *)(void *)s->imports.data[i])->name, name) == 0)
			return true;
	return false;
}

static bool
export_named(const dd_scope *s, const char *name)
{
	size_t i;

	for (i = 0; i < s->exports.len; i++)
		if (strcmp(((dd_export *)(void *)s->exports.data[i])->name, name) == 0)
			return true;
	return false;
}

static bool
all_imports_input(const dd_scope *s)
{
	size_t i;

	for (i = 0; i < s->imports.len; i++)
		if (((dd_import *)(void *)s->imports.data[i])->from.kind != DD_SRC_INPUT)
			return false;
	return true;
}

static bool
all_imports_parent(const dd_scope *s)
{
	size_t i;

	for (i = 0; i < s->imports.len; i++)
		if (((dd_import *)(void *)s->imports.data[i])->from.kind !=
		    DD_SRC_PARENT)
			return false;
	return true;
}

static bool
reads_child_export(const dd_scope *s)
{
	size_t i;

	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];

		if (it->kind == DD_I_OP && it->op->kind == DD_N_LINEAR &&
		    it->op->input.kind == DD_REF_CHILD_EXPORT)
			return true;
	}
	return false;
}

static size_t
op_count(const dd_scope *s)
{
	size_t i, n = 0;

	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];

		if (it->kind == DD_I_OP)
			n++;
		else
			n += op_count(it->sub);
	}
	return n;
}

static size_t
count_kind(const dd_scope *s, dd_node_kind k)
{
	size_t i, n = 0;

	for (i = 0; i < s->items.len; i++) {
		dd_item *it = s->items.data[i];

		if (it->kind == DD_I_OP && it->op->kind == k)
			n++;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Lowering shape                                                      */
/* ------------------------------------------------------------------ */

DTFL_TEST(lower_tree_tests__lowers_reach_to_tree)
{
	const char *src =
	    "let edges = input 0 | key($0[0] ; $0[1]);\n"
	    "let roots = input 1 | key($0[0] ;);\n"
	    "reach: {\n"
	    "    let proposals = reach | join(edges, ($2 ;));\n"
	    "    var reach = roots + proposals | distinct;\n"
	    "}\n"
	    "export \"result\" = reach::reach | key(;) | arrange | inspect(total);";
	dd_parse_ctx pc;
	dtfl_ddir_program *p;
	dd_scope *reach;

	(void)params; (void)data;
	p = parse_and_lower(src, &pc);
	assert_not_null(p);
	assert_size(p->root.imports.len, ==, 2);
	assert_true(all_imports_input(&p->root));
	assert_size(sub_count(&p->root), ==, 1);
	assert_size(p->root.exports.len, ==, 1);
	assert_string_equal(((dd_export *)(void *)p->root.exports.data[0])->name,
	    "result");

	reach = first_sub(&p->root);
	assert_size(reach->vars.len, ==, 1);
	assert_size(reach->imports.len, ==, 2);
	assert_true(all_imports_parent(reach));
	assert_size(reach->binds.len, ==, 1);
	assert_size(reach->exports.len, ==, 1);
	assert_string_equal(((dd_export *)(void *)reach->exports.data[0])->name,
	    "reach");
	assert_true(reads_child_export(&p->root));

	dd_program_free(p);
	return MUNIT_OK;
}

DTFL_TEST(lower_tree_tests__lowers_two_level_nesting_with_transitive_import)
{
	const char *src =
	    "let i = input 0 | key($0[0] ; $0[1]);\n"
	    "outer: {\n"
	    "    inner: {\n"
	    "        var x = i | distinct;\n"
	    "    }\n"
	    "    let y = inner::x;\n"
	    "}\n"
	    "export \"result\" = outer::y;";
	dd_parse_ctx pc;
	dtfl_ddir_program *p;
	dd_scope *outer, *inner;

	(void)params; (void)data;
	p = parse_and_lower(src, &pc);
	assert_not_null(p);
	assert_size(sub_count(&p->root), ==, 1);
	outer = first_sub(&p->root);
	assert_size(sub_count(outer), ==, 1);
	inner = first_sub(outer);
	assert_size(inner->vars.len, ==, 1);
	assert_true(import_named(outer, "i"));
	assert_true(import_named(inner, "i"));

	dd_program_free(p);
	return MUNIT_OK;
}

DTFL_TEST(lower_tree_tests__lowers_scc_depth_two)
{
	const char *src =
	    "let edges = input 0 | key($0[0] ; $0[1]);\n"
	    "let trans = edges | key($1 ; $0);\n"
	    "outer: {\n"
	    "    let scc = edges + trim;\n"
	    "    fwd: {\n"
	    "        let nodes = edges | key($1 ; $1) | enter_at($1[0]);\n"
	    "        let labels = proposals + nodes | min;\n"
	    "        var proposals = labels | join(scc, ($2 ; $1));\n"
	    "    }\n"
	    "    let trim_fwd = edges\n"
	    "        | join(fwd::labels, ($1 ; $0, $2))\n"
	    "        | join(fwd::labels, ($0 ; $1, $2))\n"
	    "        | filter($1[1] == $1[2])\n"
	    "        | key($0 ; $1[0]);\n"
	    "    bwd: {\n"
	    "        let nodes = trans | key($1 ; $1) | enter_at($1[0]);\n"
	    "        let labels = proposals + nodes | min;\n"
	    "        var proposals = labels | join(trim_fwd, ($2 ; $1));\n"
	    "    }\n"
	    "    let trim_bwd = trans\n"
	    "        | join(bwd::labels, ($1 ; $0, $2))\n"
	    "        | join(bwd::labels, ($0 ; $1, $2))\n"
	    "        | filter($1[1] == $1[2])\n"
	    "        | key($0 ; $1[0]);\n"
	    "    var trim = trim_bwd - edges;\n"
	    "}\n"
	    "export \"result\" = outer::scc | map(;) | arrange | inspect(total);";
	dd_parse_ctx pc;
	dtfl_ddir_program *p;
	dd_scope *outer;
	size_t i;

	(void)params; (void)data;
	p = parse_and_lower(src, &pc);
	assert_not_null(p);
	assert_size(sub_count(&p->root), ==, 1);
	outer = first_sub(&p->root);
	assert_size(sub_count(outer), ==, 2); /* fwd, bwd */
	assert_size(outer->vars.len, ==, 1);  /* trim */
	assert_true(export_named(outer, "scc"));
	for (i = 0; i < outer->items.len; i++) {
		dd_item *it = outer->items.data[i];

		if (it->kind == DD_I_SUB) {
			assert_size(it->sub->vars.len, ==, 1); /* proposals */
			assert_true(export_named(it->sub, "labels"));
		}
	}
	assert_true(reads_child_export(&p->root)); /* root reads outer::scc */

	dd_program_free(p);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Optimizer                                                           */
/* ------------------------------------------------------------------ */

DTFL_TEST(lower_optimize_tests__fuses_linear_chains)
{
	const char *src =
	    "let a = input 0 | key($0[0] ; $0[1]) | filter($0[0] != $1[0]) "
	    "| negate;\nexport \"result\" = a;";
	dd_parse_ctx pc;
	dtfl_ddir_program *p;
	size_t i;
	dd_node *lin = NULL;

	(void)params; (void)data;
	p = parse_and_lower(src, &pc);
	assert_not_null(p);
	assert_size(op_count(&p->root), ==, 3); /* three single-op Linears */
	dd_program_optimize(p);
	assert_size(op_count(&p->root), ==, 1); /* fuse to one Linear */
	for (i = 0; i < p->root.items.len; i++) {
		dd_item *it = p->root.items.data[i];

		if (it->kind == DD_I_OP && it->op->kind == DD_N_LINEAR)
			lin = it->op;
	}
	assert_not_null(lin);
	assert_size(lin->ops.len, ==, 3); /* all three ops in order */
	assert_int(((dd_linear_op *)(void *)lin->ops.data[0])->kind, ==,
	    DD_LIN_PROJECT);
	assert_int(((dd_linear_op *)(void *)lin->ops.data[2])->kind, ==,
	    DD_LIN_NEGATE);

	dd_program_free(p);
	return MUNIT_OK;
}

DTFL_TEST(lower_optimize_tests__shares_arrangements_across_joins)
{
	const char *src =
	    "let a = input 0 | key($0[0] ; $0[1]);\n"
	    "let b = input 0 | key($0[1] ; $0[0]);\n"
	    "let j1 = a | join(b, ($1 ; $2));\n"
	    "let j2 = a | join(b, ($2 ; $1));\n"
	    "export \"result\" = j1 + j2;";
	dd_parse_ctx pc;
	dtfl_ddir_program *p;

	(void)params; (void)data;
	p = parse_and_lower(src, &pc);
	assert_not_null(p);
	dd_program_optimize(p);
	/* one shared arrangement per distinct input (a, b) */
	assert_size(count_kind(&p->root, DD_N_ARRANGE), ==, 2);

	dd_program_free(p);
	return MUNIT_OK;
}

DTFL_TEST(lower_optimize_tests__collapses_arrange_of_reduce)
{
	const char *src =
	    "let a = input 0 | key($0[0] ; $0[1]) | distinct;\n"
	    "let b = input 0 | key($0[0] ; $0[1]);\n"
	    "export \"result\" = a | join(b, ($1 ;));";
	dd_parse_ctx pc;
	dtfl_ddir_program *p;
	size_t i;

	(void)params; (void)data;
	p = parse_and_lower(src, &pc);
	assert_not_null(p);
	dd_program_optimize(p);
	/* No Arrange may target a Reduce item. */
	for (i = 0; i < p->root.items.len; i++) {
		dd_item *it = p->root.items.data[i];

		if (it->kind == DD_I_OP && it->op->kind == DD_N_ARRANGE &&
		    it->op->input.kind == DD_REF_LOCAL) {
			dd_item *tgt = p->root.items.data[it->op->input.u.item];

			assert_false(tgt->kind == DD_I_OP &&
			    tgt->op->kind == DD_N_REDUCE);
		}
	}

	dd_program_free(p);
	return MUNIT_OK;
}

DTFL_TEST(lower_optimize_tests__optimizes_within_nested_scopes)
{
	const char *src =
	    "let e = input 0 | key($0[0] ; $0[1]);\n"
	    "s: {\n"
	    "    var x = e | key($0 ; $1) | filter($0[0] != $1[0]) | distinct;\n"
	    "}\n"
	    "export \"result\" = s::x;";
	dd_parse_ctx pc;
	dtfl_ddir_program *p;
	dd_scope *child;
	size_t i, lin_ops = SIZE_MAX, lin_count = 0;

	(void)params; (void)data;
	p = parse_and_lower(src, &pc);
	assert_not_null(p);
	dd_program_optimize(p);
	child = first_sub(&p->root);
	assert_not_null(child);
	for (i = 0; i < child->items.len; i++) {
		dd_item *it = child->items.data[i];

		if (it->kind == DD_I_OP && it->op->kind == DD_N_LINEAR) {
			lin_ops = it->op->ops.len;
			lin_count++;
		}
	}
	/* the key|filter chain fuses inside the child scope (one Linear, 2 ops) */
	assert_size(lin_count, ==, 1);
	assert_size(lin_ops, ==, 2);

	dd_program_free(p);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(lower_tree_tests__lowers_reach_to_tree),
	DTFL_T(lower_tree_tests__lowers_two_level_nesting_with_transitive_import),
	DTFL_T(lower_tree_tests__lowers_scc_depth_two),
	DTFL_T(lower_optimize_tests__fuses_linear_chains),
	DTFL_T(lower_optimize_tests__shares_arrangements_across_joins),
	DTFL_T(lower_optimize_tests__collapses_arrange_of_reduce),
	DTFL_T(lower_optimize_tests__optimizes_within_nested_scopes),
	DTFL_T_END
};

DTFL_TEST_MAIN("lower", tests)
