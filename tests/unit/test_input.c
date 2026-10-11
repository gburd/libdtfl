/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_input.c: inputs, probes, loops and reclock (P4).
 *
 * The ordered input (ops_input.c) feeds records at monotonic epochs
 * through a map-like operator to a sink while a probe tracks the input
 * frontier; stepping until dtfl_probe_done terminates.  A feedback edge
 * (ops_feedback.c) circulates a bounded number of iterations and stops
 * when the loop coordinate overflows the declared width.  reclock
 * (ops_reclock.c) releases buffered data at clock times.  Finally a
 * Process run of three workers advances an input through several rounds
 * and every worker converges and terminates, exercising the progress
 * protocol end to end.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <string.h>

#define MAX_OUT 4096

typedef struct collected {
	uint64_t vals[MAX_OUT];
	uint64_t times[MAX_OUT];
	size_t n;
} collected;

/* step_while cond: run until the probe is no longer behind `target`. */
typedef struct probe_cond {
	dtfl_probe *probe;
	uint64_t target;
} probe_cond;

static bool
probe_behind(void *vctx)
{
	probe_cond *pc = vctx;

	return dtfl_probe_less_than(pc->probe, &pc->target);
}

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	collected *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t i, k = dtfl_message_len(m);
		const uint64_t *t = dtfl_message_time(m);

		for (i = 0; i < k && c->n < MAX_OUT; i++) {
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

/* ------------------------------------------------------------------ */
/* Test: input -> sink, with a probe, stepped to completion            */
/* ------------------------------------------------------------------ */

typedef struct in_build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
	collected *out;
} in_build_ctx;

static void
build_input_probe(dtfl_scope *scope, void *arg)
{
	in_build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);

	s = dtfl_stream_probe_with(s, c->probe);
	make_sink(s, c->out);
}

DTFL_TEST(input__probe_tracks_and_terminates)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	in_build_ctx c;
	collected out;
	uint64_t round;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.input = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "in", tc, build_input_probe, &c), ==, 0);

	/* Send one record per round at that round's epoch. */
	for (round = 0; round < 8; round++) {
		unsigned char buf[8];
		uint64_t next = round + 1;

		dtfl_put_u64(buf, round * 10);
		dtfl_input_send(c.input, buf, sizeof buf);
		/* The probe is at or before the current epoch. */
		assert_true(dtfl_probe_less_equal(c.probe, &round));
		dtfl_input_advance_to(c.input, &next);
		/* Step until the probe passes this epoch (progress advanced). */
		{
			probe_cond pc = { c.probe, next };

			dtfl_worker_step_while(w, probe_behind, &pc);
		}
	}

	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	assert_true(dtfl_probe_done(c.probe));

	/* Every record came out exactly once, at its epoch. */
	assert_size(out.n, ==, 8);
	for (round = 0; round < out.n; round++) {
		assert_uint64(out.vals[round], ==, out.times[round] * 10);
		assert_uint64(out.times[round], <, 8);
	}

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Test: a feedback loop that circulates a bounded number of times     */
/* ------------------------------------------------------------------ */

/*
 * A seed operator emits one record (value 0) at time 0, then drops its
 * capability.  A concat-free loop: the seed feeds a counter operator
 * that increments the record's value and re-emits it; the counter's
 * output is the feedback edge, re-timed by the summary (+1 on the loop
 * coordinate), so it circulates until the time overflows the declared
 * width.  The sink records every value it sees.
 */
typedef struct loop_ctx {
	dtfl_input *input;
	dtfl_feedback *fb;
	collected *out;
	unsigned width_cap;	/* feedback coordinate width in bits */
} loop_ctx;

/* Increment each record and forward it; also forward to the sink. */
static bool
inc_logic(dtfl_op *op, void *vctx)
{
	dtfl_message *m;

	(void)vctx;
	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			unsigned char buf[8];

			dtfl_put_u64(buf, dtfl_get_u64(rec) + 1);
			dtfl_session_give(s, buf, sizeof buf);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static dtfl_stream *
make_inc(dtfl_stream *in)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(in), "Inc");
	dtfl_stream *out;

	dtfl_op_builder_input(b, in, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, inc_logic, NULL, NULL);
	return out;
}

/* Concat two streams into one output (so the seed and the loop merge). */
typedef struct concat_ctx { int unused; } concat_ctx;

static bool
concat_logic(dtfl_op *op, void *vctx)
{
	dtfl_message *m;
	size_t in;

	(void)vctx;
	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	for (in = 0; in < 2; in++) {
		while ((m = dtfl_op_input_next(op, in)) != NULL) {
			dtfl_cap *cap = dtfl_op_input_cap(op, in, 0);
			dtfl_session *s = dtfl_op_session(op, 0, cap);

			dtfl_session_give_message(s, m);
			dtfl_session_close(s);
			dtfl_cap_drop(cap);
		}
	}
	return false;
}

static dtfl_stream *
make_concat(dtfl_stream *a, dtfl_stream *b)
{
	dtfl_op_builder *bld = dtfl_op_builder_new(dtfl_stream_scope(a), "Concat");
	dtfl_stream *out;

	dtfl_op_builder_input(bld, a, dtfl_pact_pipeline());
	dtfl_op_builder_input(bld, b, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(bld, dtfl_stream_type(a));
	dtfl_op_builder_build(bld, concat_logic, NULL, NULL);
	return out;
}

static void
build_loop(dtfl_scope *scope, void *arg)
{
	loop_ctx *c = arg;
	uint64_t summary[DTFL_TIME_MAX];
	dtfl_stream *seed = dtfl_input_to_stream(c->input, scope);
	dtfl_stream *cycle, *merged, *inc;

	/* Summary that advances the single coordinate by one per lap. */
	dtfl_summary_default(dtfl_scope_time_class(scope), summary);
	summary[0] = 1;

	cycle = dtfl_feedback_new(scope, dtfl_type_u64(), summary, &c->fb);
	merged = make_concat(seed, cycle);
	inc = make_inc(merged);
	make_sink(inc, c->out);
	dtfl_feedback_connect(c->fb, inc);
}

DTFL_TEST(feedback__bounded_loop_terminates)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	/* An 8-bit coordinate overflows after 255 laps, so the loop is bounded. */
	dtfl_time_class *tc = dtfl_time_total((dtfl_coord){ DTFL_COORD_UNSIGNED, 8 });
	loop_ctx c;
	collected out;
	unsigned char buf[8];
	uint64_t zero = 0;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.input = dtfl_input_new(dtfl_type_u64());
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "loop", tc, build_loop, &c), ==, 0);

	/* Inject one record at time 0, then seal the input. */
	dtfl_put_u64(buf, 0);
	(void)zero;
	dtfl_input_send(c.input, buf, sizeof buf);
	dtfl_input_close(c.input);

	while (dtfl_worker_step(w))
		;

	/*
	 * The record circulates until the 8-bit loop coordinate cannot
	 * advance (summary.results_in overflows), so it is seen a bounded
	 * number of times and the computation terminates.
	 */
	assert_size(out.n, >, 0);
	assert_size(out.n, <=, 256);
	/* Each lap increments the value, so the values are 1, 2, 3, ... */
	{
		size_t i;

		for (i = 0; i < out.n; i++)
			assert_uint64(out.vals[i], ==, (uint64_t)(i + 1));
	}

	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Test: reclock releases data at clock times                         */
/* ------------------------------------------------------------------ */

typedef struct feeder_ctx {
	const uint64_t *vals;
	const uint64_t *times;
	size_t n;
	bool is_clock;	/* clock records carry a unit payload */
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
		dtfl_session_give(s, buf, f->is_clock ? 0 : sizeof buf);
		dtfl_session_close(s);
		dtfl_cap_drop(at);
	}
	dtfl_cap_drop(cap);
	return false;
}

static dtfl_stream *
make_feeder(dtfl_scope *scope, feeder_ctx *f, const dtfl_type *type)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Feeder");
	dtfl_stream *out = dtfl_op_builder_output(b, type);

	dtfl_op_builder_build(b, feeder_logic, f, NULL);
	return out;
}

typedef struct reclock_build_ctx {
	feeder_ctx data;
	feeder_ctx clock;
	collected *out;
} reclock_build_ctx;

static void
build_reclock(dtfl_scope *scope, void *arg)
{
	reclock_build_ctx *c = arg;
	dtfl_stream *data = make_feeder(scope, &c->data, dtfl_type_u64());
	dtfl_stream *clock = make_feeder(scope, &c->clock, dtfl_type_unit());
	dtfl_stream *out = dtfl_stream_reclock(data, clock);

	make_sink(out, c->out);
}

DTFL_TEST(reclock__releases_at_clock_times)
{
	/* Data 0..9 at times 0..9; clock ticks at 3, 5, 8 (reclock.rs). */
	static const uint64_t dvals[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	static const uint64_t dtimes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	static const uint64_t cvals[] = { 0, 0, 0 };
	static const uint64_t ctimes[] = { 3, 5, 8 };
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	reclock_build_ctx c;
	collected out;
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.data.vals = dvals; c.data.times = dtimes; c.data.n = 10; c.data.is_clock = false;
	c.clock.vals = cvals; c.clock.times = ctimes; c.clock.n = 3; c.clock.is_clock = true;
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "reclock", tc, build_reclock, &c), ==, 0);
	while (dtfl_worker_step(w))
		;

	/* 0,1,2,3 at t=3; 4,5 at t=5; 6,7,8 at t=8; 9 is never released. */
	assert_size(out.n, ==, 9);
	for (i = 0; i < out.n; i++) {
		if (out.vals[i] <= 3)
			assert_uint64(out.times[i], ==, 3);
		else if (out.vals[i] <= 5)
			assert_uint64(out.times[i], ==, 5);
		else
			assert_uint64(out.times[i], ==, 8);
	}

	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Test: a Process run of three workers converges and terminates       */
/* ------------------------------------------------------------------ */

static dtfl_time_class *g_mw_tc;

typedef struct mw_ctx {
	size_t total_seen[8];	/* records seen per worker */
} mw_ctx;

typedef struct mw_build {
	dtfl_input *input;
	dtfl_probe *probe;
	size_t *seen;	/* records this worker's sink saw */
} mw_build;

static bool
mw_sink_logic(dtfl_op *op, void *vctx)
{
	size_t *seen = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL)
		*seen += dtfl_message_len(m);
	return false;
}

static void
mw_build_fn(dtfl_scope *scope, void *arg)
{
	mw_build *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);
	dtfl_op_builder *b;

	s = dtfl_stream_probe_with(s, c->probe);
	/* Exchange by record value, so records spread across workers. */
	b = dtfl_op_builder_new(scope, "Sink");
	dtfl_op_builder_input(b, s, dtfl_pact_exchange(NULL, NULL));
	dtfl_op_builder_build(b, mw_sink_logic, c->seen, NULL);
}

static void
mw_worker(dtfl_worker *w, void *arg)
{
	mw_ctx *e = arg;
	size_t idx = dtfl_worker_index(w);
	mw_build c;
	uint64_t round;

	c.input = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	c.seen = &e->total_seen[idx];
	assert_int(dtfl_worker_dataflow(w, "mw", g_mw_tc, mw_build_fn, &c), ==, 0);

	/* Each worker feeds four records per round, advancing five rounds. */
	for (round = 0; round < 5; round++) {
		uint64_t next = round + 1;
		int k;

		for (k = 0; k < 4; k++) {
			unsigned char buf[8];

			dtfl_put_u64(buf, (uint64_t)(idx * 100 + round * 4 + k));
			dtfl_input_send(c.input, buf, sizeof buf);
		}
		dtfl_input_advance_to(c.input, &next);
		{
			probe_cond pc = { c.probe, next };

			dtfl_worker_step_while(w, probe_behind, &pc);
		}
	}
	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	assert_true(dtfl_probe_done(c.probe));
	dtfl_probe_free(c.probe);
}

DTFL_TEST(input__multi_worker_multi_round_terminates)
{
	dtfl_config *cfg = dtfl_config_new();
	mw_ctx e;
	size_t total = 0, i;

	(void)params; (void)data;
	memset(&e, 0, sizeof e);
	g_mw_tc = dtfl_time_u64();
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, 3), ==, 0);
	assert_int(dtfl_execute(cfg, mw_worker, &e, NULL), ==, 0);

	/* 3 workers x 5 rounds x 4 records = 60 records, spread by exchange. */
	for (i = 0; i < 3; i++)
		total += e.total_seen[i];
	assert_size(total, ==, 60);

	dtfl_time_class_free(g_mw_tc);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Test: unordered input sends under caller-managed capabilities       */
/* ------------------------------------------------------------------ */

typedef struct uin_build_ctx {
	dtfl_unordered_input *handle;
	dtfl_cap *cap;
	collected *out;
} uin_build_ctx;

static void
build_unordered(dtfl_scope *scope, void *arg)
{
	uin_build_ctx *c = arg;
	dtfl_stream *s = dtfl_unordered_input_new(scope, dtfl_type_u64(),
	    &c->handle, &c->cap);

	make_sink(s, c->out);
}

DTFL_TEST(unordered_input__sends_at_capability_times)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	uin_build_ctx c;
	collected out;
	uint64_t round;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "u", tc, build_unordered, &c), ==, 0);

	/* Feed values 0..9 at times 0..9, delaying the capability each round. */
	for (round = 0; round < 10; round++) {
		unsigned char buf[8];
		uint64_t next = round + 1;
		dtfl_cap *later;

		dtfl_put_u64(buf, round);
		dtfl_unordered_input_send(c.handle, c.cap, buf, sizeof buf);
		later = dtfl_cap_delayed(c.cap, &next);
		dtfl_cap_drop(c.cap);
		c.cap = later;
		dtfl_worker_step(w);
	}
	dtfl_cap_drop(c.cap);	/* dropping the last capability seals the input */
	dtfl_unordered_input_free(c.handle);
	while (dtfl_worker_step(w))
		;

	assert_size(out.n, ==, 10);
	for (round = 0; round < out.n; round++) {
		assert_uint64(out.vals[round], ==, out.times[round]);
		assert_uint64(out.times[round], <, 10);
	}

	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(input__probe_tracks_and_terminates),
	DTFL_T(feedback__bounded_loop_terminates),
	DTFL_T(reclock__releases_at_clock_times),
	DTFL_T(input__multi_worker_multi_round_terminates),
	DTFL_T(unordered_input__sends_at_capability_times),
	DTFL_T_END
};

DTFL_TEST_MAIN("input", tests)
