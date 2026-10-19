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
 * sparse_connections_shape_frontiers checks the (port, summary) builder
 * calls, which are libdtfl's form of upstream's connection iterators.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

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

/* ------------------------------------------------------------------ */

/*
 * The sparse connection calls.  Outputs 0 and 1 are added unconnected,
 * then input 0 (from A) names output 1 twice, with +3 and +7, which
 * merge to the antichain {+3}; input 1 (from B) names nothing; output 2
 * is then added connected from input 1 by the identity.  The logic
 * drops its initial capabilities and never sends, so each output's
 * frontier is exactly its inputs' frontiers through those summaries:
 * output 0 is done while A and B are open, output 1 is A + 3, output 2
 * is B.
 */
typedef struct sparse_ctx {
	dtfl_input *a, *b;
	dtfl_probe *p[3];
} sparse_ctx;

static bool
drop_caps_logic(dtfl_op *op, void *ctx)
{
	size_t o;

	(void)ctx;
	for (o = 0; o < dtfl_op_outputs(op); o++)
		dtfl_cap_drop(dtfl_op_initial_cap(op, o));
	return false;
}

static void
build_sparse(dtfl_scope *scope, void *arg)
{
	sparse_ctx *c = arg;
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Sparse");
	uint64_t plus3 = 3, plus7 = 7, ident = 0;
	const uint64_t *sums[2];
	size_t ports[2];
	dtfl_stream *out[3];

	out[0] = dtfl_op_builder_output_sparse(b, dtfl_type_u64(), 0, NULL,
	    NULL);
	out[1] = dtfl_op_builder_output_sparse(b, dtfl_type_u64(), 0, NULL,
	    NULL);
	ports[0] = ports[1] = 1;
	sums[0] = &plus3;
	sums[1] = &plus7;
	assert_size(dtfl_op_builder_input_sparse(b,
	    dtfl_input_to_stream(c->a, scope), dtfl_pact_pipeline(), 2, ports,
	    sums), ==, 0);
	assert_size(dtfl_op_builder_input_sparse(b,
	    dtfl_input_to_stream(c->b, scope), dtfl_pact_pipeline(), 0, NULL,
	    NULL), ==, 1);
	ports[0] = 1;
	sums[0] = &ident;
	out[2] = dtfl_op_builder_output_sparse(b, dtfl_type_u64(), 1, ports,
	    sums);
	dtfl_op_builder_build(b, drop_caps_logic, NULL, NULL);
	(void)dtfl_stream_probe_with(out[0], c->p[0]);
	(void)dtfl_stream_probe_with(out[1], c->p[1]);
	(void)dtfl_stream_probe_with(out[2], c->p[2]);
}

/* The probe's frontier as a single time; asserts it has one element. */
static uint64_t
probe_time(const dtfl_probe *p)
{
	size_t n;
	const uint64_t *f = dtfl_probe_frontier(p, &n);

	assert_size(n, ==, 1);
	return f[0];
}

DTFL_TEST(sparse_connections_shape_frontiers)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t t10 = 10, t4 = 4;
	sparse_ctx c;
	int i;

	(void)params; (void)data;
	c.a = dtfl_input_new(dtfl_type_u64());
	c.b = dtfl_input_new(dtfl_type_u64());
	for (i = 0; i < 3; i++)
		c.p[i] = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "sparse", tc, build_sparse, &c),
	    ==, 0);
	dtfl_input_advance_to(c.a, &t10);
	dtfl_input_advance_to(c.b, &t4);
	for (i = 0; i < 100; i++)
		(void)dtfl_worker_step(w);
	assert_true(dtfl_probe_done(c.p[0]));
	assert_uint64(probe_time(c.p[1]), ==, 13);
	assert_uint64(probe_time(c.p[2]), ==, 4);

	dtfl_input_close(c.a);
	dtfl_input_close(c.b);
	for (i = 0; i < 100000 && dtfl_worker_step(w); i++)
		;
	for (i = 0; i < 3; i++) {
		assert_true(dtfl_probe_done(c.p[i]));
		dtfl_probe_free(c.p[i]);
	}
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_operators_generic_builder_rc__incorrect_capabilities),
	DTFL_T(dataflow_operators_generic_builder_rc__correct_capabilities),
	DTFL_T(sparse_connections_shape_frontiers),
	DTFL_T_END
};

DTFL_TEST_MAIN("builder", tests)
