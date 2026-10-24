/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/tests/gh_523.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * tests/upstream/timely/gh_523.c: the ported gh_523.rs regression test.
 *
 * A unary operator reads each input time's data and, for each incoming
 * container, emits an empty container followed by the container's
 * records on the same capability.  The output is exchanged by value and
 * probed.  The regression (timely issue 523) is that emitting an empty
 * container must not confuse progress tracking: the dataflow must still
 * drain and the probe must reach done.  The test sends rounds 0 and 1,
 * advances the input past each, closes it, and steps until the probe is
 * done; reaching done without hanging is the pass.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

/* Route each u64 record to a worker by its value (exchange(|x| *x)). */
static uint64_t
by_value(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

/*
 * The "Test" operator: for each input message, emit an empty give then
 * the message's records, on the message's own capability.  This is the
 * give_container(&mut Vec::new()) then give_container(data) of the Rust
 * test, which exercises emitting an empty batch.
 */
static bool
test_logic(dtfl_op *op, void *vctx)
{
	dtfl_message *m;

	(void)vctx;
	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *empty = dtfl_op_session(op, 0, cap);
		dtfl_session *full;
		size_t i, n = dtfl_message_len(m);

		/* An empty container: open and close with no records. */
		dtfl_session_close(empty);
		/* Then the real records. */
		full = dtfl_op_session(op, 0, cap);
		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			dtfl_session_give(full, rec, len);
		}
		dtfl_session_close(full);
		dtfl_cap_drop(cap);
	}
	return false;
}

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *in = dtfl_input_to_stream(c->input, scope);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Test");
	dtfl_stream *out;
	dtfl_stream *ex;

	dtfl_op_builder_input(b, in, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, test_logic, NULL, NULL);
	ex = dtfl_stream_exchange(out, by_value, NULL);
	dtfl_stream_probe_with(ex, c->probe);
}

DTFL_TEST(gh_523__gh_523)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c;
	uint64_t round;
	int steps;

	(void)params; (void)data;
	c.input = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "gh_523", tc, build, &c), ==, 0);

	for (round = 0; round < 2; round++) {
		unsigned char buf[8];
		uint64_t next = round + 1;

		dtfl_put_u64(buf, round);
		dtfl_input_send(c.input, buf, 8);
		dtfl_input_advance_to(c.input, &next);
		dtfl_input_flush(c.input);
	}
	dtfl_input_close(c.input);

	for (steps = 0; steps < 2000000 && !dtfl_probe_done(c.probe); steps++)
		dtfl_worker_step(w);
	assert_true(dtfl_probe_done(c.probe));
	assert_int(steps, <, 2000000);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(gh_523__gh_523),
	DTFL_T_END
};

DTFL_TEST_MAIN("timely/gh_523", tests)
