/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ops_result.c: Result stream helpers (P4).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/vec/result.rs (tests)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * The input stream is [Ok(0), Err(())], as in the upstream tests.  A
 * Result record is a tag byte (0 Ok, 1 Err) then the value; Ok holds a
 * u64 and Err holds the unit (). Each test applies one helper, collects
 * the output through a sink, and checks it against the upstream result.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OUT 64

/* Collected output records (raw bytes). */
typedef struct collected {
	unsigned char recs[MAX_OUT][16];
	size_t lens[MAX_OUT];
	size_t n;
} collected;

/* Feeder sends the two fixed Result records at time 0. */
static bool
feeder_logic(dtfl_op *op, void *vctx)
{
	dtfl_cap *cap = dtfl_op_initial_cap(op, 0);
	dtfl_session *s;
	unsigned char ok[9], err[1];

	(void)vctx;
	if (cap == NULL)
		return false;
	s = dtfl_op_session(op, 0, cap);
	ok[0] = 0;			/* Ok */
	dtfl_put_u64(ok + 1, 0);	/* value 0 */
	dtfl_session_give(s, ok, sizeof ok);
	err[0] = 1;			/* Err(()) : no value bytes */
	dtfl_session_give(s, err, sizeof err);
	dtfl_session_close(s);
	dtfl_cap_drop(cap);
	return false;
}

static dtfl_stream *
make_feeder(dtfl_scope *scope)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Feeder");
	dtfl_stream *out = dtfl_op_builder_output(b, dtfl_type_bytes());

	dtfl_op_builder_build(b, feeder_logic, NULL, NULL);
	return out;
}

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	collected *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n && c->n < MAX_OUT; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			if (len <= sizeof c->recs[0]) {
				if (len > 0)
					memcpy(c->recs[c->n], rec, len);
				c->lens[c->n] = len;
				c->n++;
			}
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

/* The helper under test transforms the feeder stream into the sink. */
typedef dtfl_stream *(*apply_fn)(dtfl_stream *in);

typedef struct build_ctx {
	apply_fn apply;
	collected *out;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;

	make_sink(c->apply(make_feeder(scope)), c->out);
}

static void
run(apply_fn apply, collected *out)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c = { apply, out };

	memset(out, 0, sizeof *out);
	assert_int(dtfl_worker_dataflow(w, "result", tc, build, &c), ==, 0);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
}

static int
cmp_rec(const void *va, const void *vb)
{
	return memcmp(va, vb, 16);
}

/* ------------------------------------------------------------------ */
/* test_ok / test_err                                                  */
/* ------------------------------------------------------------------ */

static dtfl_stream *apply_ok(dtfl_stream *in)
{
	return dtfl_stream_ok(in, dtfl_type_u64());
}

DTFL_TEST(dataflow_operators_vec_result__test_ok)
{
	collected out;

	(void)params; (void)data;
	run(apply_ok, &out);
	/* Only the Ok arm's value survives: [0]. */
	assert_size(out.n, ==, 1);
	assert_size(out.lens[0], ==, 8);
	assert_uint64(dtfl_get_u64(out.recs[0]), ==, 0);
	return MUNIT_OK;
}

static dtfl_stream *apply_err(dtfl_stream *in)
{
	return dtfl_stream_err(in, dtfl_type_unit());
}

DTFL_TEST(dataflow_operators_vec_result__test_err)
{
	collected out;

	(void)params; (void)data;
	run(apply_err, &out);
	/* Only the Err arm's value survives: [()], an empty value. */
	assert_size(out.n, ==, 1);
	assert_size(out.lens[0], ==, 0);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* test_map_ok / test_map_err                                          */
/* ------------------------------------------------------------------ */

static size_t
to_ten(void *ctx, const void *val, size_t len, void *out, size_t cap)
{
	(void)ctx; (void)val; (void)len; (void)cap;
	dtfl_put_u64(out, 10);
	return 8;
}

static dtfl_stream *apply_map_ok(dtfl_stream *in)
{
	return dtfl_stream_map_ok(in, to_ten, DTFL_NOCTX);
}

DTFL_TEST(dataflow_operators_vec_result__test_map_ok)
{
	collected out;
	unsigned char want_ok[9], want_err[1];

	(void)params; (void)data;
	run(apply_map_ok, &out);
	/* [Ok(10), Err(())] */
	assert_size(out.n, ==, 2);
	want_ok[0] = 0;
	dtfl_put_u64(want_ok + 1, 10);
	want_err[0] = 1;
	/* order preserved: record 0 is Ok(10), record 1 is Err(()). */
	assert_size(out.lens[0], ==, 9);
	assert_memory_equal(9, out.recs[0], want_ok);
	assert_size(out.lens[1], ==, 1);
	assert_memory_equal(1, out.recs[1], want_err);
	return MUNIT_OK;
}

static dtfl_stream *apply_map_err(dtfl_stream *in)
{
	return dtfl_stream_map_err(in, to_ten, DTFL_NOCTX);
}

DTFL_TEST(dataflow_operators_vec_result__test_map_err)
{
	collected out;
	unsigned char want_ok[9], want_err[9];

	(void)params; (void)data;
	run(apply_map_err, &out);
	/* [Ok(0), Err(10)] */
	assert_size(out.n, ==, 2);
	want_ok[0] = 0;
	dtfl_put_u64(want_ok + 1, 0);
	want_err[0] = 1;
	dtfl_put_u64(want_err + 1, 10);
	assert_size(out.lens[0], ==, 9);
	assert_memory_equal(9, out.recs[0], want_ok);
	assert_size(out.lens[1], ==, 9);
	assert_memory_equal(9, out.recs[1], want_err);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* test_and_then                                                       */
/* ------------------------------------------------------------------ */

static size_t
to_one(void *ctx, const void *val, size_t len, void *out, size_t cap,
    bool *is_ok)
{
	(void)ctx; (void)val; (void)len; (void)cap;
	*is_ok = true;
	dtfl_put_u64(out, 1);
	return 8;
}

static dtfl_stream *apply_and_then(dtfl_stream *in)
{
	return dtfl_stream_and_then(in, to_one, DTFL_NOCTX);
}

DTFL_TEST(dataflow_operators_vec_result__test_and_then)
{
	collected out;
	unsigned char want_ok[9], want_err[1];

	(void)params; (void)data;
	run(apply_and_then, &out);
	/* [Ok(1), Err(())] */
	assert_size(out.n, ==, 2);
	want_ok[0] = 0;
	dtfl_put_u64(want_ok + 1, 1);
	want_err[0] = 1;
	assert_size(out.lens[0], ==, 9);
	assert_memory_equal(9, out.recs[0], want_ok);
	assert_size(out.lens[1], ==, 1);
	assert_memory_equal(1, out.recs[1], want_err);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* test_unwrap_or_else                                                 */
/* ------------------------------------------------------------------ */

static size_t
err_to_ten(void *ctx, const void *val, size_t len, void *out, size_t cap)
{
	(void)ctx; (void)val; (void)len; (void)cap;
	dtfl_put_u64(out, 10);
	return 8;
}

static dtfl_stream *apply_unwrap(dtfl_stream *in)
{
	return dtfl_stream_unwrap_or_else(in, dtfl_type_u64(), err_to_ten,
	    DTFL_NOCTX);
}

DTFL_TEST(dataflow_operators_vec_result__test_unwrap_or_else)
{
	collected out;
	size_t i;

	(void)params; (void)data;
	run(apply_unwrap, &out);
	/* [0, 10]: Ok(0) unwrapped to 0, Err(()) mapped to 10. */
	assert_size(out.n, ==, 2);
	for (i = 0; i < 2; i++)
		assert_size(out.lens[i], ==, 8);
	qsort(out.recs, out.n, sizeof out.recs[0], cmp_rec);
	assert_uint64(dtfl_get_u64(out.recs[0]), ==, 0);
	assert_uint64(dtfl_get_u64(out.recs[1]), ==, 10);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_operators_vec_result__test_ok),
	DTFL_T(dataflow_operators_vec_result__test_err),
	DTFL_T(dataflow_operators_vec_result__test_map_ok),
	DTFL_T(dataflow_operators_vec_result__test_map_err),
	DTFL_T(dataflow_operators_vec_result__test_and_then),
	DTFL_T(dataflow_operators_vec_result__test_unwrap_or_else),
	DTFL_T_END
};

DTFL_TEST_MAIN("ops_result", tests)
