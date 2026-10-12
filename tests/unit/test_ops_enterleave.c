/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ops_enterleave.c: moving streams across scopes (P4).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/enterleave.rs (test_nested)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * test_nested builds a region inside a region, enters a stream into
 * both and leaves it back, and checks the records pass through
 * unchanged.  A second test enters an iterative (Product) child, so the
 * entering time gains an inner coordinate and the leaving time drops it;
 * the sink sees the outer times again.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OUT 512

typedef struct feeder_ctx {
	const uint64_t *vals;
	const uint64_t *times;
	size_t n;
} feeder_ctx;

static bool
feeder_logic(dtfl_op *op, void *vctx)
{
	feeder_ctx *f = vctx;
	dtfl_cap *cap = dtfl_op_initial_cap(op, 0);
	size_t i;

	if (cap == NULL)
		return false;
	for (i = 0; i < f->n; i++) {
		dtfl_cap *at = dtfl_cap_delayed(cap, &f->times[i]);
		dtfl_session *s = dtfl_op_session(op, 0, at);
		unsigned char buf[8];

		dtfl_put_u64(buf, f->vals[i]);
		dtfl_session_give(s, buf, sizeof buf);
		dtfl_session_close(s);
		dtfl_cap_drop(at);
	}
	dtfl_cap_drop(cap);
	return false;
}

static dtfl_stream *
make_feeder(dtfl_scope *scope, feeder_ctx *f)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Feeder");
	dtfl_stream *out = dtfl_op_builder_output(b, dtfl_type_u64());

	dtfl_op_builder_build(b, feeder_logic, f, NULL);
	return out;
}

typedef struct collected {
	uint64_t vals[MAX_OUT];
	uint64_t times[MAX_OUT];
	size_t n;
} collected;

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	collected *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t i, n = dtfl_message_len(m);
		const uint64_t *t = dtfl_message_time(m);

		for (i = 0; i < n && c->n < MAX_OUT; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			c->vals[c->n] = dtfl_get_u64(rec);
			c->times[c->n] = t[0];
			c->n++;
		}
	}
	return false;
}

static void
make_sink(dtfl_stream *in, collected *c)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(in), "Sink");

	dtfl_op_builder_input(b, in, dtfl_pact_pipeline());
	dtfl_op_builder_build(b, sink_logic, c, NULL);
}

static void
run_standalone(dtfl_build_fn build, void *ctx)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();

	assert_int(dtfl_worker_dataflow(w, "test", tc, build, ctx), ==, 0);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* ------------------------------------------------------------------ */
/* test_nested: a region inside a region, pass-through                 */
/* ------------------------------------------------------------------ */

/* Carries the outer stream into the inner region build callbacks. */
typedef struct region_ctx {
	dtfl_stream *data;	/* the outer stream to enter */
	dtfl_stream *result;	/* set by the innermost leave */
} region_ctx;

static void
build_inner2(dtfl_scope *inner2, void *arg)
{
	region_ctx *rc = arg;
	dtfl_stream *entered = dtfl_stream_enter(rc->data, inner2);

	/* leave back to inner (the parent of inner2). */
	rc->result = dtfl_stream_leave(entered);
}

static void
build_inner(dtfl_scope *inner, void *arg)
{
	region_ctx *outer = arg;
	dtfl_stream *entered = dtfl_stream_enter(outer->data, inner);
	region_ctx rc2 = { entered, NULL };

	dtfl_scope_region(inner, "inner2", build_inner2, &rc2);
	/* leave inner2's result back to the root. */
	outer->result = dtfl_stream_leave(rc2.result);
}

typedef struct nested_build {
	feeder_ctx feeder;
	collected *out;
} nested_build;

static void
build_nested(dtfl_scope *root, void *arg)
{
	nested_build *c = arg;
	dtfl_stream *data = make_feeder(root, &c->feeder);
	region_ctx rc = { data, NULL };

	dtfl_scope_region(root, "inner", build_inner, &rc);
	make_sink(rc.result, c->out);
}

DTFL_TEST(dataflow_operators_core_enterleave_test__test_nested)
{
	uint64_t vals[10], times[10];
	collected out;
	nested_build c;
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	for (i = 0; i < 10; i++) {
		vals[i] = i;
		times[i] = i;	/* each at its own time, as in test_nested */
	}
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 10;
	c.out = &out;
	run_standalone(build_nested, &c);

	assert_size(out.n, ==, 10);
	for (i = 0; i < 10; i++) {
		/* value i was sent at time i and must arrive at time i. */
		assert_uint64(out.times[i], ==, out.vals[i]);
	}
	qsort(out.vals, out.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 10; i++)
		assert_uint64(out.vals[i], ==, i);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Entering an iterative (Product) scope: times project and return     */
/* ------------------------------------------------------------------ */

static void
build_iter_child(dtfl_scope *child, void *arg)
{
	region_ctx *rc = arg;
	dtfl_stream *entered = dtfl_stream_enter(rc->data, child);

	/* to_inner added the inner coordinate; leave drops it (to_outer). */
	rc->result = dtfl_stream_leave(entered);
}

static void
build_iterative(dtfl_scope *root, void *arg)
{
	nested_build *c = arg;
	dtfl_stream *data = make_feeder(root, &c->feeder);
	region_ctx rc = { data, NULL };

	dtfl_scope_iterative(root, "loop", build_iter_child, &rc);
	make_sink(rc.result, c->out);
}

DTFL_TEST(enterleave__iterative_projects_times)
{
	static const uint64_t vals[] = { 5, 6, 7 };
	static const uint64_t times[] = { 2, 4, 8 };
	collected out;
	nested_build c;
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 3;
	c.out = &out;
	run_standalone(build_iterative, &c);

	assert_size(out.n, ==, 3);
	/* value v sent at outer time t must leave at outer time t again. */
	for (i = 0; i < out.n; i++) {
		size_t j;

		for (j = 0; j < 3; j++)
			if (vals[j] == out.vals[i])
				assert_uint64(out.times[i], ==, times[j]);
	}
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_operators_core_enterleave_test__test_nested),
	DTFL_T(enterleave__iterative_projects_times),
	DTFL_T_END
};

DTFL_TEST_MAIN("ops_enterleave", tests)
