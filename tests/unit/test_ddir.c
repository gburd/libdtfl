/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ddir.c: the DDIR core (ir, parse, lower, backend).
 *
 * The IR layer: the Value data model's structural ordering and the Term
 * interpreter (dd_eval).  Later layers add parse/lower/build/evaluate.
 */
#include "dtfl_test.h"

#include "dtfl/ddir.h"
#include "ddir/ddir_int.h"
#include "ddir/parse_int.h"
#include "base/base_int.h"

#include <string.h>

/* Evaluate a term against an environment of owned values. */
static dd_value *
eval_env(const dd_term *t, dd_value **env0, size_t n)
{
	dd_value **env = NULL;
	size_t len = 0, cap = 0, i;
	dd_value *r;

	for (i = 0; i < n; i++) {
		if (len == cap) {
			cap = cap ? cap * 2 : 8;
			env = __dtfl_realloc(env, cap * sizeof(dd_value *));
		}
		env[len++] = env0[i];
	}
	r = dd_eval(t, &env, &len, &cap);
	__dtfl_free(env);
	return r;
}

/* ------------------------------------------------------------------ */
/* Value ordering                                                      */
/* ------------------------------------------------------------------ */

DTFL_TEST(ddir__value_cmp)
{
	dd_value *i1 = dd_value_int(1), *i2 = dd_value_int(2);
	dd_value *t = dd_value_unit();
	dd_value *v = dd_value_variant(0, dd_value_int(0));

	(void)params; (void)data;
	/* Int < Int by value. */
	assert_int(dd_value_cmp(i1, i2), <, 0);
	assert_int(dd_value_cmp(i2, i1), >, 0);
	assert_int(dd_value_cmp(i1, i1), ==, 0);
	/* Discriminant order: Int < Tuple < Variant < List. */
	assert_int(dd_value_cmp(i1, t), <, 0);
	assert_int(dd_value_cmp(t, v), <, 0);
	dd_value_free(i1);
	dd_value_free(i2);
	dd_value_free(t);
	dd_value_free(v);
	return MUNIT_OK;
}

DTFL_TEST(ddir__value_clone_roundtrip)
{
	dd_value **items = __dtfl_alloc(2 * sizeof(dd_value *));
	dd_value *tup, *dup;

	(void)params; (void)data;
	items[0] = dd_value_int(7);
	items[1] = dd_value_variant(3, dd_value_int(-1));
	tup = dd_value_tuple(items, 2);
	dup = dd_value_clone(tup);
	assert_int(dd_value_cmp(tup, dup), ==, 0);
	dd_value_free(tup);
	dd_value_free(dup);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Term interpreter                                                    */
/* ------------------------------------------------------------------ */

/* $0 + $1 over env {3, 4} = 7. */
DTFL_TEST(ddir__eval_arith)
{
	dd_term *v0 = dd_term_new(DD_T_VAR);
	dd_term *v1 = dd_term_new(DD_T_VAR);
	dd_term *add = dd_term_new(DD_T_BINARY);
	dd_value *env[2] = { dd_value_int(3), dd_value_int(4) };
	dd_value *r;

	(void)params; (void)data;
	v0->u.var = 0;
	v1->u.var = 1;
	add->u.binary.op = DD_BIN_ADD;
	add->u.binary.l = v0;
	add->u.binary.r = v1;
	r = eval_env(add, env, 2);
	assert_int64(dd_value_as_int(r), ==, 7);
	dd_value_free(r);
	dd_value_free(env[0]);
	dd_value_free(env[1]);
	dd_term_free(add);
	return MUNIT_OK;
}

/* if(0, 1, 2) short-circuits to the else branch. */
DTFL_TEST(ddir__eval_if)
{
	dd_term *cond = dd_term_new(DD_T_INT);
	dd_term *then_ = dd_term_new(DD_T_INT);
	dd_term *els = dd_term_new(DD_T_INT);
	dd_term *iff = dd_term_new(DD_T_IF);
	dd_value *r;

	(void)params; (void)data;
	cond->u.int_lit = 0;
	then_->u.int_lit = 1;
	els->u.int_lit = 2;
	iff->u.if_.cond = cond;
	iff->u.if_.then_ = then_;
	iff->u.if_.els = els;
	r = eval_env(iff, NULL, 0);
	assert_int64(dd_value_as_int(r), ==, 2);
	dd_value_free(r);
	dd_term_free(iff);
	return MUNIT_OK;
}

/* proj($0, 1) over env {tuple(10, 20)} = 20. */
DTFL_TEST(ddir__eval_proj)
{
	dd_value **items = __dtfl_alloc(2 * sizeof(dd_value *));
	dd_value *env[1];
	dd_term *base = dd_term_new(DD_T_VAR);
	dd_term *proj = dd_term_new(DD_T_PROJ);
	dd_value *r;

	(void)params; (void)data;
	items[0] = dd_value_int(10);
	items[1] = dd_value_int(20);
	env[0] = dd_value_tuple(items, 2);
	base->u.var = 0;
	proj->u.proj.base = base;
	proj->u.proj.index = 1;
	r = eval_env(proj, env, 1);
	assert_int64(dd_value_as_int(r), ==, 20);
	dd_value_free(r);
	dd_value_free(env[0]);
	dd_term_free(proj);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

/* A small pipe program parses to the expected statement shape. */
DTFL_TEST(ddir__parse_pipe_basic)
{
	const char *src =
	    "let edges = input 0 | key($0[0] ; $0[1]);\n"
	    "export \"result\" = edges;\n";
	dd_vec stmts = { 0 };
	dd_parse_ctx pc;
	dd_stmt *s0, *s1;

	(void)params; (void)data;
	assert_true(dd_parse_pipe(src, &stmts, &pc));
	assert_size(stmts.len, ==, 2);
	s0 = stmts.data[0];
	s1 = stmts.data[1];
	assert_int(s0->kind, ==, DD_S_LET);
	assert_string_equal(s0->name, "edges");
	/* edges = Map(Input 0, proj) */
	assert_int(s0->expr->kind, ==, DD_E_MAP);
	assert_int(s0->expr->u.map.e->kind, ==, DD_E_INPUT);
	assert_int(s1->kind, ==, DD_S_EXPORT);
	assert_string_equal(s1->name, "result");
	__dtfl_free(stmts.data);
	dd_arena_free_all(&pc);
	return MUNIT_OK;
}

/* The same program in applicative syntax parses to the same shape. */
DTFL_TEST(ddir__parse_applicative_basic)
{
	const char *src =
	    "let edges = MAP(INPUT 0, ($0[0] ; $0[1]));\n"
	    "export \"result\" = JOIN(edges, edges, ($1 ;));\n";
	dd_vec stmts = { 0 };
	dd_parse_ctx pc;
	dd_stmt *s1;

	(void)params; (void)data;
	assert_true(dd_parse_applicative(src, &stmts, &pc));
	assert_size(stmts.len, ==, 2);
	s1 = stmts.data[1];
	assert_int(s1->kind, ==, DD_S_EXPORT);
	assert_int(s1->expr->kind, ==, DD_E_JOIN);
	__dtfl_free(stmts.data);
	dd_arena_free_all(&pc);
	return MUNIT_OK;
}

/* A malformed program returns an error, not a crash, with a message. */
DTFL_TEST(ddir__parse_error_no_crash)
{
	dd_vec stmts = { 0 };
	dd_parse_ctx pc;

	(void)params; (void)data;
	/* missing semicolon / RHS */
	assert_false(dd_parse_pipe("let x = ;", &stmts, &pc));
	assert_true(pc.error);
	assert_size(strlen(pc.msg), >, 0);
	/* unknown builtin in a term */
	assert_false(dd_parse_pipe("let x = input 0 | filter(bogus(1));",
	    &stmts, &pc));
	assert_true(pc.error);
	/* unterminated string */
	assert_false(dd_parse_pipe("export \"oops = input 0;", &stmts, &pc));
	assert_true(pc.error);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(ddir__value_cmp),
	DTFL_T(ddir__value_clone_roundtrip),
	DTFL_T(ddir__eval_arith),
	DTFL_T(ddir__eval_if),
	DTFL_T(ddir__eval_proj),
	DTFL_T(ddir__parse_pipe_basic),
	DTFL_T(ddir__parse_applicative_basic),
	DTFL_T(ddir__parse_error_no_crash),
	DTFL_T_END
};

DTFL_TEST_MAIN("ddir", tests)
