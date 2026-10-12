/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_capture.c: capture, replay and extract round-trip (P4).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/capture/capture.rs (doctest)
 *   timely/src/dataflow/operators/core/capture/extract.rs (doctest)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A first dataflow sends records on an input and captures the stream
 * into a shared event link.  A second dataflow replays the link and
 * captures the replayed stream into another link.  Extract on the second
 * link must return the same (time, record) multiset that was sent.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OUT 512

/* A feeder sending u64 records at chosen times (mirrors test_ops_data). */
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

/* ------------------------------------------------------------------ */
/* Collected (time, value) pairs from an extract                       */
/* ------------------------------------------------------------------ */

typedef struct collected {
	uint64_t vals[MAX_OUT];
	uint64_t times[MAX_OUT];
	size_t n;
} collected;

static void
extract_each(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	collected *c = ctx;

	(void)len;
	if (c->n < MAX_OUT) {
		c->vals[c->n] = dtfl_get_u64(rec);
		c->times[c->n] = time[0];
		c->n++;
	}
}

/* ------------------------------------------------------------------ */
/* The dataflows                                                       */
/* ------------------------------------------------------------------ */

typedef struct capture_build {
	feeder_ctx *feeder;
	dtfl_event_pusher pusher;
} capture_build;

static void
build_capture(dtfl_scope *scope, void *arg)
{
	capture_build *c = arg;
	dtfl_stream *s = make_feeder(scope, c->feeder);

	dtfl_stream_capture_into(s, c->pusher, DTFL_NOCTX);
}

typedef struct replay_build {
	dtfl_event_link *src;	/* the link replayed */
	dtfl_event_pusher pusher;	/* where the replayed stream is captured */
} replay_build;

static void
build_replay(dtfl_scope *scope, void *arg)
{
	replay_build *c = arg;
	dtfl_stream *s = dtfl_event_link_replay(c->src, scope);

	dtfl_stream_capture_into(s, c->pusher, DTFL_NOCTX);
}

static int
cmp_pair(const void *va, const void *vb)
{
	const uint64_t *a = va, *b = vb;

	if (a[0] != b[0])
		return a[0] < b[0] ? -1 : 1;
	return a[1] < b[1] ? -1 : (a[1] > b[1]);
}

/* ------------------------------------------------------------------ */
/* The round-trip                                                      */
/* ------------------------------------------------------------------ */

static void
run_round_trip(const uint64_t *vals, const uint64_t *times, size_t n,
    collected *out)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_event_link *l1 = dtfl_event_link_new(tc, dtfl_type_u64());
	dtfl_event_link *l2 = dtfl_event_link_new(tc, dtfl_type_u64());
	feeder_ctx f = { vals, times, n };
	capture_build cb = { &f, dtfl_event_link_pusher(l1) };
	replay_build rb = { l1, dtfl_event_link_pusher(l2) };

	assert_int(dtfl_worker_dataflow(w, "capture", tc, build_capture, &cb), ==, 0);
	assert_int(dtfl_worker_dataflow(w, "replay", tc, build_replay, &rb), ==, 0);
	while (dtfl_worker_step(w))
		;
	dtfl_event_link_extract(l2, extract_each, out);

	dtfl_event_link_free(l1);
	dtfl_event_link_free(l2);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
}

/* capture.rs / extract.rs doctest: 0..10 at time 0 survives the trip. */
DTFL_TEST(dataflow_operators_core_capture__round_trip)
{
	uint64_t vals[10], times[10];
	collected out;
	uint64_t pairs[10][2];
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	for (i = 0; i < 10; i++) {
		vals[i] = i;
		times[i] = 0;
	}
	run_round_trip(vals, times, 10, &out);

	assert_size(out.n, ==, 10);
	for (i = 0; i < 10; i++) {
		pairs[i][0] = out.times[i];
		pairs[i][1] = out.vals[i];
	}
	qsort(pairs, out.n, sizeof pairs[0], cmp_pair);
	for (i = 0; i < 10; i++) {
		assert_uint64(pairs[i][0], ==, 0);
		assert_uint64(pairs[i][1], ==, i);
	}
	return MUNIT_OK;
}

/* Multiple times: records survive with their times. */
DTFL_TEST(dataflow_operators_core_capture__round_trip_times)
{
	static const uint64_t vals[] = { 10, 11, 20, 21, 22, 30 };
	static const uint64_t times[] = { 1, 1, 2, 2, 2, 3 };
	collected out;
	uint64_t pairs[6][2];
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	run_round_trip(vals, times, 6, &out);

	assert_size(out.n, ==, 6);
	for (i = 0; i < 6; i++) {
		pairs[i][0] = out.times[i];
		pairs[i][1] = out.vals[i];
	}
	qsort(pairs, out.n, sizeof pairs[0], cmp_pair);
	for (i = 0; i < 6; i++) {
		assert_uint64(pairs[i][0], ==, times[i]);
		assert_uint64(pairs[i][1], ==, vals[i]);
	}
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_operators_core_capture__round_trip),
	DTFL_T(dataflow_operators_core_capture__round_trip_times),
	DTFL_T_END
};

DTFL_TEST_MAIN("capture", tests)
