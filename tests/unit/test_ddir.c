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

static MunitTest tests[] = {
	DTFL_T(ddir__value_cmp),
	DTFL_T(ddir__value_clone_roundtrip),
	DTFL_T(ddir__eval_arith),
	DTFL_T(ddir__eval_if),
	DTFL_T(ddir__eval_proj),
	DTFL_T_END
};

DTFL_TEST_MAIN("ddir", tests)
