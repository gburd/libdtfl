/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_builder.c: the generic builder capability checks.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/generic/builder_rc.rs (tests)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * incorrect_capabilities: opening an output session with a capability
 * minted for a different output is a contract violation and panics.
 * correct_capabilities: using each output's own capability does not.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"

/* ------------------------------------------------------------------ */

static dtfl_cap *g_c0, *g_c1;

static bool
incorrect_logic(dtfl_op *op, void *ctx)
{
	dtfl_session *s;

	(void)ctx;
	g_c0 = dtfl_op_initial_cap(op, 0);
	g_c1 = dtfl_op_initial_cap(op, 1);
	if (g_c0 == NULL)
		return false;	/* only the first call acts */
	/* Open output 1's session with output 0's capability: panics. */
	s = dtfl_op_session(op, 1, g_c0);
	dtfl_session_close(s);
	dtfl_cap_drop(g_c0);
	dtfl_cap_drop(g_c1);
	g_c0 = g_c1 = NULL;
	return false;
}

static void
build_incorrect(dtfl_scope *scope, void *ctx)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Failure");

	(void)dtfl_op_builder_output(b, dtfl_type_u64());
	(void)dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, incorrect_logic, ctx, NULL);
}

static dtfl_worker *g_worker;
static dtfl_time_class *g_tc;

static void
run(dtfl_build_fn build)
{
	g_worker = dtfl_worker_new_standalone();
	g_tc = dtfl_time_u64();
	assert_int(dtfl_worker_dataflow(g_worker, "t", g_tc, build, NULL), ==, 0);
	while (dtfl_worker_step(g_worker))
		;
}

DTFL_TEST(dataflow_operators_generic_builder_rc__incorrect_capabilities)
{
	(void)params; (void)data;
	g_worker = NULL;
	g_tc = NULL;
	g_c0 = g_c1 = NULL;
	DTFL_EXPECT_PANIC(run(build_incorrect));
	/* The panic skipped the drops; release what it left (no ASan leak). */
	dtfl_cap_drop(g_c0);
	dtfl_cap_drop(g_c1);
	dtfl_worker_free(g_worker);
	dtfl_time_class_free(g_tc);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */

static bool
correct_logic(dtfl_op *op, void *ctx)
{
	dtfl_cap *c0, *c1;
	dtfl_session *s;

	(void)ctx;
	c0 = dtfl_op_initial_cap(op, 0);
	c1 = dtfl_op_initial_cap(op, 1);
	if (c0 == NULL)
		return false;
	/* Each output's own capability is valid. */
	s = dtfl_op_session(op, 0, c0);
	dtfl_session_close(s);
	s = dtfl_op_session(op, 1, c1);
	dtfl_session_close(s);
	dtfl_cap_drop(c0);
	dtfl_cap_drop(c1);
	return false;
}

static void
build_correct(dtfl_scope *scope, void *ctx)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Success");

	(void)dtfl_op_builder_output(b, dtfl_type_u64());
	(void)dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, correct_logic, ctx, NULL);
}

DTFL_TEST(dataflow_operators_generic_builder_rc__correct_capabilities)
{
	(void)params; (void)data;
	run(build_correct);
	dtfl_worker_free(g_worker);
	dtfl_time_class_free(g_tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_operators_generic_builder_rc__incorrect_capabilities),
	DTFL_T(dataflow_operators_generic_builder_rc__correct_capabilities),
	DTFL_T_END
};

DTFL_TEST_MAIN("builder", tests)
