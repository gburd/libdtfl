/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_explain.c: the explain clone machinery (explain/mod.rs).
 *
 * clone_into clones an original program into an output scope, lifting and
 * exporting every value-producing site so it has a host-visible form at
 * the embedding level.  These structural tests pin that: one host-visible
 * ref per op and var, depth-2 sites surface via child exports, nested
 * exports carry one LiftIter per level, and clone_identity preserves the
 * named exports and feedback variables while adding the lift chains.
 * (explain__every_site_is_host_visible, nested_exports_carry_one_lift_
 * per_level, identity_clone_preserves_structure.)
 */
#include "dtfl_test.h"

#include "dtfl/ddir.h"
#include "ddir/ddir_int.h"
#include "ddir/parse_int.h"
#include "ddir/explain_int.h"
#include "base/base_int.h"

#include <string.h>

/* The scc program from explain.rs's test module (minus the final map/arrange/
 * inspect, which the structural tests do not need). */
static const char *SCC =
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
    "export \"result\" = outer::scc;\n";

/* Lower (no optimize), the shape the explain tests start from. */
static dtfl_ddir_program *
lowered(const char *src)
{
	dd_parse_ctx pc;
	dd_vec stmts = { 0 };
	dtfl_ddir_program *p;

	if (!dd_parse_pipe(src, &stmts, &pc))
		return NULL;
	p = dd_lower_tree(&stmts);
	__dtfl_free(stmts.data);
	dd_arena_free_all(&pc);
	return p;
}

/* Total feedback variables across all scopes. */
static size_t
vars_total(const dd_scope *s)
{
	size_t n = s->vars.len, i;

	for (i = 0; i < s->items.len; i++) {
		const dd_item *it = s->items.data[i];

		if (it->kind == DD_I_SUB)
			n += vars_total(it->sub);
	}
	return n;
}

/* clone_into a fresh root with the program's imports mapped to themselves. */
static dd_addr_map
clone_into_fresh(const dtfl_ddir_program *p, dd_scope *out)
{
	dd_ref *import_map;
	dd_addr_map visible = { 0 };
	size_t i;

	memset(out, 0, sizeof *out);
	out->name = __dtfl_strdup(p->root.name);
	for (i = 0; i < p->root.imports.len; i++) {
		const dd_import *imp = p->root.imports.data[i];
		dd_import *ni = __dtfl_alloc(sizeof *ni);

		ni->name = __dtfl_strdup(imp->name);
		ni->from = imp->from;
		if (imp->from.kind == DD_SRC_TRACE)
			ni->from.u.trace = __dtfl_strdup(imp->from.u.trace);
		dd_vec_push(&out->imports, ni);
	}
	import_map = p->root.imports.len
	    ? __dtfl_alloc(p->root.imports.len * sizeof(dd_ref)) : NULL;
	for (i = 0; i < p->root.imports.len; i++) {
		import_map[i].kind = DD_REF_IMPORT;
		import_map[i].u.import = i;
	}
	dd_clone_into(&p->root, out, import_map, p->root.imports.len, &visible);
	__dtfl_free(import_map);
	return visible;
}

/* every_site_is_host_visible: one host-visible ref per op and var, and a
 * depth-2 site (inside fwd) surfaces as a ChildExport at the root. */
DTFL_TEST(explain__every_site_is_host_visible)
{
	dtfl_ddir_program *p = lowered(SCC);
	dd_scope out;
	dd_addr_map visible;
	size_t sites, i;
	bool depth2_child_export = false;

	(void)params; (void)data;
	assert_not_null(p);
	visible = clone_into_fresh(p, &out);
	sites = dd_op_count(p) + vars_total(&p->root);
	assert_size(visible.len, ==, sites);
	for (i = 0; i < visible.len; i++) {
		const dd_addr_entry *e = &visible.data[i];

		if (e->key.path_len == 2 &&
		    e->val.kind == DD_REF_CHILD_EXPORT)
			depth2_child_export = true;
	}
	assert_true(depth2_child_export);
	dd_addr_map_free(&visible);
	dd_scope_free_fields(&out);
	dtfl_ddir_program_free(p);
	return MUNIT_OK;
}

/* nested_exports_carry_one_lift_per_level: outer's $host: exports are each a
 * LiftIter Linear, and the ones re-exporting fwd/bwd internals chain a
 * second lift (the lift's input is itself a ChildExport). */
DTFL_TEST(explain__nested_exports_carry_one_lift_per_level)
{
	dtfl_ddir_program *p = lowered(SCC);
	dd_scope out;
	dd_addr_map visible;
	const dd_scope *outer = NULL;
	size_t i, depth2_chains = 0;

	(void)params; (void)data;
	assert_not_null(p);
	visible = clone_into_fresh(p, &out);
	/* The first Sub item of the output root is outer's clone. */
	for (i = 0; i < out.items.len; i++) {
		const dd_item *it = out.items.data[i];

		if (it->kind == DD_I_SUB) {
			outer = it->sub;
			break;
		}
	}
	assert_not_null(outer);
	for (i = 0; i < outer->exports.len; i++) {
		const dd_export *e = outer->exports.data[i];
		const dd_item *it;
		const dd_node *n;

		if (strncmp(e->name, "$host:", 6) != 0)
			continue;
		assert_int(e->value.kind, ==, DD_REF_LOCAL);
		it = outer->items.data[e->value.u.item];
		assert_int(it->kind, ==, DD_I_OP);
		n = it->op;
		assert_int(n->kind, ==, DD_N_LINEAR);
		assert_size(n->ops.len, ==, 1);
		assert_int(((dd_linear_op *)n->ops.data[0])->kind, ==,
		    DD_LIN_LIFT_ITER);
		if (n->input.kind == DD_REF_CHILD_EXPORT)
			depth2_chains++;
	}
	assert_size(depth2_chains, >, 0);
	dd_addr_map_free(&visible);
	dd_scope_free_fields(&out);
	dtfl_ddir_program_free(p);
	return MUNIT_OK;
}

/* identity_clone_preserves_structure: clone_identity keeps the named exports
 * (no $host: at the root) and the feedback variables, and adds lift chains. */
DTFL_TEST(explain__identity_clone_preserves_structure)
{
	dtfl_ddir_program *p = lowered(SCC);
	dtfl_ddir_program *c;

	(void)params; (void)data;
	assert_not_null(p);
	c = dd_clone_identity(p);
	assert_size(c->root.exports.len, ==, p->root.exports.len);
	assert_string_equal(
	    ((dd_export *)c->root.exports.data[0])->name, "result");
	assert_size(vars_total(&c->root), ==, vars_total(&p->root));
	assert_size(dd_op_count(c), >, dd_op_count(p));
	dtfl_ddir_program_free(c);
	dtfl_ddir_program_free(p);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(explain__every_site_is_host_visible),
	DTFL_T(explain__nested_exports_carry_one_lift_per_level),
	DTFL_T(explain__identity_clone_preserves_structure),
	DTFL_T_END
};

DTFL_TEST_MAIN("explain", tests)
