/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ops_probe.c: a probe tracks an input's frontier (P4).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/probe.rs (test probe)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Upstream creates an input and a probe on its stream, then for each of
 * ten rounds asserts the probe is not done, is less-equal to the round
 * and less-than the next, advances the input, and steps.  It then closes
 * the input and steps until the probe reports done.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *stream = dtfl_input_to_stream(c->input, scope);

	(void)dtfl_stream_probe_with(stream, c->probe);
}

DTFL_TEST(dataflow_operators_core_probe__probe)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c;
	uint64_t round;

	(void)params; (void)data;
	c.input = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "probe", tc, build, &c), ==, 0);

	for (round = 0; round < 10; round++) {
		uint64_t next = round + 1;

		assert_false(dtfl_probe_done(c.probe));
		assert_true(dtfl_probe_less_equal(c.probe, &round));
		assert_true(dtfl_probe_less_than(c.probe, &next));
		dtfl_input_advance_to(c.input, &next);
		dtfl_worker_step(w);
	}

	/* Seal the input and finish any remaining work. */
	dtfl_input_close(c.input);
	dtfl_worker_step(w);
	dtfl_worker_step(w);
	dtfl_worker_step(w);
	dtfl_worker_step(w);
	assert_true(dtfl_probe_done(c.probe));

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/*
 * One handle probing two streams (probe_with twice, as differential's
 * arrange example shares one probe across three dataflows).  Upstream
 * mirrors each operator's frontier changes into the shared antichain,
 * so the handle holds the meet of both frontiers: it stays behind the
 * slower input however the operators are scheduled.
 */
typedef struct shared_ctx {
	dtfl_input *a, *b;
	dtfl_probe *probe;
} shared_ctx;

static void
build_shared(dtfl_scope *scope, void *arg)
{
	shared_ctx *c = arg;

	(void)dtfl_stream_probe_with(dtfl_input_to_stream(c->a, scope),
	    c->probe);
	(void)dtfl_stream_probe_with(dtfl_input_to_stream(c->b, scope),
	    c->probe);
}

DTFL_TEST(probe__shared_handle_holds_slowest)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	shared_ctx c;
	uint64_t t0 = 0, t5 = 5;
	int i;

	(void)params; (void)data;
	c.a = dtfl_input_new(dtfl_type_u64());
	c.b = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "shared", tc, build_shared, &c),
	    ==, 0);

	/* Only `a` advances; `b` still holds time 0. */
	dtfl_input_advance_to(c.a, &t5);
	for (i = 0; i < 8; i++)
		dtfl_worker_step(w);
	assert_true(dtfl_probe_less_equal(c.probe, &t0));
	assert_true(dtfl_probe_less_than(c.probe, &t5));

	/* Now `b` catches up; the probe passes time 0. */
	dtfl_input_advance_to(c.b, &t5);
	for (i = 0; i < 8; i++)
		dtfl_worker_step(w);
	assert_false(dtfl_probe_less_equal(c.probe, &t0));
	assert_true(dtfl_probe_less_equal(c.probe, &t5));

	dtfl_input_close(c.a);
	dtfl_input_close(c.b);
	while (dtfl_worker_step(w))
		;
	assert_true(dtfl_probe_done(c.probe));

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_operators_core_probe__probe),
	DTFL_T(probe__shared_handle_holds_slowest),
	DTFL_T_END
};

DTFL_TEST_MAIN("ops_probe", tests)
