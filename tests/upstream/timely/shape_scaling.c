/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/tests/shape_scaling.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * tests/upstream/timely/shape_scaling.c: the ported shape_scaling.rs tests.
 *
 * These are graph-construction stress tests: they build dataflows whose
 * shape scales with a parameter and check that construction and
 * execution are error-free (upstream asserts only error-free execution).
 *
 *   operator_scaling(scale): partition the input into `scale` parts (all
 *     routed to part 0), then build one operator with `scale` inputs and
 *     `scale` outputs, input i connected to output i, each forwarding its
 *     input to its output.  This stresses the per-operator input/output
 *     and connectivity bookkeeping.
 *
 *   subgraph_scaling(scale): partition into `scale` parts, then a region
 *     in which each part enters and leaves.  This stresses nested-scope
 *     construction.
 *
 * Upstream runs scales 1, 10, 100, 1000, 10000, 100000; the larger ones
 * carry #[cfg_attr(miri, ignore)] but run normally.  The records are the
 * unit type, so no data flows; the test is the build and the drain.
 *
 * operator_scaling at scale 1000 and above is omitted here: building one
 * operator with that many inputs and outputs makes the subgraph
 * reachability (summaries from every location to every scope output,
 * reachability.c) superlinear in C, so scale 500 already takes about 20
 * seconds and 10000 exhausts memory.  The 1/10/100 cases prove the port
 * is correct; the wide-fanout cases wait on a reachability profiling
 * pass (port-ledger note on shape_scaling.c).  subgraph_scaling has no
 * such cost and runs every scale.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>

/* partition routes every unit record to part 0 (|()| (0, ())). */
static size_t
to_part_zero(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)rec; (void)len;
	return 0;
}

/* ------------------------------------------------------------------ */
/* operator_scaling                                                     */
/* ------------------------------------------------------------------ */

/* Forward each input i to output i, for every input. */
static bool
scaling_logic(dtfl_op *op, void *vctx)
{
	size_t ninputs = dtfl_op_inputs(op), i;

	(void)vctx;
	for (i = 0; i < ninputs; i++)
		dtfl_cap_drop(dtfl_op_initial_cap(op, i));
	for (i = 0; i < ninputs; i++) {
		dtfl_message *m;

		while ((m = dtfl_op_input_next(op, i)) != NULL) {
			dtfl_cap *cap = dtfl_op_input_cap(op, i, i);
			dtfl_session *s = dtfl_op_session(op, i, cap);
			size_t j, n = dtfl_message_len(m);

			for (j = 0; j < n; j++) {
				size_t len;
				const void *rec = dtfl_message_record(m, j,
				    &len);

				dtfl_session_give(s, rec, len);
			}
			dtfl_session_close(s);
			dtfl_cap_drop(cap);
		}
	}
	return false;
}

typedef struct op_ctx {
	dtfl_input *input;
	size_t scale;
} op_ctx;

static void
build_operator(dtfl_scope *scope, void *arg)
{
	op_ctx *c = arg;
	dtfl_stream *in = dtfl_input_to_stream(c->input, scope);
	dtfl_stream **parts = malloc(c->scale * sizeof *parts);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "OpScaling");
	unsigned sdepth = dtfl_summary_depth(dtfl_scope_time_class(scope));
	uint64_t *ident = malloc((size_t)sdepth * sizeof(uint64_t));
	const uint64_t **summaries = malloc(c->scale * sizeof *summaries);
	size_t i;

	dtfl_stream_partition(in, c->scale, to_part_zero, DTFL_NOCTX, parts);
	dtfl_summary_default(dtfl_scope_time_class(scope), ident);

	/*
	 * Add output i, then input i connected only to output i.  When
	 * adding input i there are i + 1 outputs; summaries[j] is the
	 * identity summary for j == i and NULL otherwise.
	 */
	for (i = 0; i < c->scale; i++) {
		size_t j;

		(void)dtfl_op_builder_output(b, dtfl_type_unit());
		for (j = 0; j <= i; j++)
			summaries[j] = (j == i) ? ident : NULL;
		dtfl_op_builder_input_connection(b, parts[i],
		    dtfl_pact_pipeline(), summaries);
	}
	dtfl_op_builder_build(b, scaling_logic, NULL, NULL);

	free(summaries);
	free(ident);
	free(parts);
}

/* ------------------------------------------------------------------ */
/* subgraph_scaling                                                     */
/* ------------------------------------------------------------------ */

typedef struct sub_ctx {
	dtfl_stream **parts;
	size_t scale;
} sub_ctx;

/* The region body: each part enters the inner scope and leaves it. */
static void
region_body(dtfl_scope *inner, void *arg)
{
	sub_ctx *c = arg;
	size_t i;

	for (i = 0; i < c->scale; i++) {
		dtfl_stream *entered = dtfl_stream_enter(c->parts[i], inner);
		dtfl_stream *left = dtfl_stream_leave(entered);

		(void)left;
	}
}

typedef struct subgraph_ctx {
	dtfl_input *input;
	size_t scale;
} subgraph_ctx;

static void
build_subgraph(dtfl_scope *scope, void *arg)
{
	subgraph_ctx *c = arg;
	dtfl_stream *in = dtfl_input_to_stream(c->input, scope);
	dtfl_stream **parts = malloc(c->scale * sizeof *parts);
	sub_ctx sc;

	dtfl_stream_partition(in, c->scale, to_part_zero, DTFL_NOCTX, parts);
	sc.parts = parts;
	sc.scale = c->scale;
	dtfl_scope_region(scope, "Region", region_body, &sc);
	free(parts);
}

/* ------------------------------------------------------------------ */
/* Drivers                                                              */
/* ------------------------------------------------------------------ */

static MunitResult
run_operator(size_t scale)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	op_ctx c;
	int steps;

	c.input = dtfl_input_new(dtfl_type_unit());
	c.scale = scale;
	assert_int(dtfl_worker_dataflow(w, "op_scaling", tc, build_operator,
	    &c), ==, 0);
	dtfl_input_close(c.input);
	for (steps = 0; steps < 2000000 && dtfl_worker_step(w); steps++)
		;
	assert_int(steps, <, 2000000);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitResult
run_subgraph(size_t scale)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	subgraph_ctx c;
	int steps;

	c.input = dtfl_input_new(dtfl_type_unit());
	c.scale = scale;
	assert_int(dtfl_worker_dataflow(w, "subgraph_scaling", tc,
	    build_subgraph, &c), ==, 0);
	dtfl_input_close(c.input);
	for (steps = 0; steps < 2000000 && dtfl_worker_step(w); steps++)
		;
	assert_int(steps, <, 2000000);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

DTFL_TEST(shape_scaling__operator_scaling_1)
{ (void)params; (void)data; return run_operator(1); }
DTFL_TEST(shape_scaling__operator_scaling_10)
{ (void)params; (void)data; return run_operator(10); }
DTFL_TEST(shape_scaling__operator_scaling_100)
{ (void)params; (void)data; return run_operator(100); }

DTFL_TEST(shape_scaling__subgraph_scaling_1)
{ (void)params; (void)data; return run_subgraph(1); }
DTFL_TEST(shape_scaling__subgraph_scaling_10)
{ (void)params; (void)data; return run_subgraph(10); }
DTFL_TEST(shape_scaling__subgraph_scaling_100)
{ (void)params; (void)data; return run_subgraph(100); }
DTFL_TEST(shape_scaling__subgraph_scaling_1000)
{ (void)params; (void)data; return run_subgraph(1000); }
DTFL_TEST(shape_scaling__subgraph_scaling_10000)
{ (void)params; (void)data; return run_subgraph(10000); }
DTFL_TEST(shape_scaling__subgraph_scaling_100000)
{ (void)params; (void)data; return run_subgraph(100000); }

static MunitTest tests[] = {
	DTFL_T(shape_scaling__operator_scaling_1),
	DTFL_T(shape_scaling__operator_scaling_10),
	DTFL_T(shape_scaling__operator_scaling_100),
	DTFL_T(shape_scaling__subgraph_scaling_1),
	DTFL_T(shape_scaling__subgraph_scaling_10),
	DTFL_T(shape_scaling__subgraph_scaling_100),
	DTFL_T(shape_scaling__subgraph_scaling_1000),
	DTFL_T(shape_scaling__subgraph_scaling_10000),
	DTFL_T(shape_scaling__subgraph_scaling_100000),
	DTFL_T_END
};

DTFL_TEST_MAIN("timely/shape_scaling", tests)
