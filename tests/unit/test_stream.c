/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_stream.c: a stream carries records to a sink.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/stream.rs (test_non_clone_stream)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Upstream's test checks that a single-consumer stream need not clone
 * its container.  libdtfl records are always refcounted bytes (PLAN D2),
 * so the C analog checks the same shape it tests: a feeder's records
 * reach a single pipeline sink unchanged.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"

#include <string.h>

#define N 5

typedef struct ctx {
	uint64_t got[N];
	size_t n;
} ctx;

static bool
feeder_logic(dtfl_op *op, void *vctx)
{
	dtfl_cap *cap = dtfl_op_initial_cap(op, 0);
	dtfl_session *s;
	uint64_t i;

	(void)vctx;
	if (cap == NULL)
		return false;
	s = dtfl_op_session(op, 0, cap);
	for (i = 0; i < N; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, i);
		dtfl_session_give(s, buf, sizeof buf);
	}
	dtfl_session_close(s);
	dtfl_cap_drop(cap);
	return false;
}

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k && c->n < N; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			c->got[c->n++] = dtfl_get_u64(rec);
		}
	}
	return false;
}

static void
build(dtfl_scope *scope, void *arg)
{
	dtfl_op_builder *fb = dtfl_op_builder_new(scope, "Feeder");
	dtfl_stream *src = dtfl_op_builder_output(fb, dtfl_type_u64());
	dtfl_op_builder *sb;

	dtfl_op_builder_build(fb, feeder_logic, NULL, NULL);
	sb = dtfl_op_builder_new(scope, "Sink");
	dtfl_op_builder_input(sb, src, dtfl_pact_pipeline());
	dtfl_op_builder_build(sb, sink_logic, arg, NULL);
}

DTFL_TEST(dataflow_stream__test_non_clone_stream)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&c, 0, sizeof c);
	assert_int(dtfl_worker_dataflow(w, "t", tc, build, &c), ==, 0);
	while (dtfl_worker_step(w))
		;
	assert_size(c.n, ==, N);
	for (i = 0; i < N; i++)
		assert_uint64(c.got[i], ==, i);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_stream__test_non_clone_stream),
	DTFL_T_END
};

DTFL_TEST_MAIN("stream", tests)
