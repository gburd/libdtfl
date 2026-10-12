/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_capture.c: capture -> replay -> extract round-trips.
 *
 * For an arbitrary multiset of (time, value) pairs sent on a stream,
 * capturing the stream into an event link, replaying that link into a
 * second dataflow, and extracting the replayed capture must return the
 * same (time, value) multiset.  This exercises the Event framing, the
 * link, the replay protocol and the extractor together.
 */
#include "pbt.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>

#define MAX_RECS 128

typedef struct feeder_ctx {
	uint64_t vals[MAX_RECS];
	uint64_t times[MAX_RECS];
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
/* Collected (time, value) pairs                                       */
/* ------------------------------------------------------------------ */

typedef struct collected {
	uint64_t *pairs;	/* heap: 2 words per pair (time, value) */
	size_t n, cap;
} collected;

static void
collect_push(collected *c, uint64_t time, uint64_t val)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 32;
		c->pairs = realloc(c->pairs, c->cap * 2 * sizeof(uint64_t));
	}
	c->pairs[c->n * 2] = time;
	c->pairs[c->n * 2 + 1] = val;
	c->n++;
}

static void
extract_each(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)len;
	collect_push(ctx, time[0], dtfl_get_u64(rec));
}

/* ------------------------------------------------------------------ */
/* Scenario: worker, links and result buffers torn down on case end   */
/* ------------------------------------------------------------------ */

typedef struct scenario {
	dtfl_worker *w;
	dtfl_time_class *tc;
	dtfl_event_link *l1, *l2;
	collected out;
} scenario;

static void
scenario_free(void *vp)
{
	scenario *s = vp;

	dtfl_event_link_free(s->l1);
	dtfl_event_link_free(s->l2);
	dtfl_worker_free(s->w);
	dtfl_time_class_free(s->tc);
	free(s->out.pairs);
	free(s);
}

typedef struct capture_build {
	feeder_ctx *feeder;
	dtfl_event_pusher pusher;
} capture_build;

static void
build_capture(dtfl_scope *scope, void *arg)
{
	capture_build *c = arg;

	dtfl_stream_capture_into(make_feeder(scope, c->feeder), c->pusher,
	    DTFL_NOCTX);
}

typedef struct replay_build {
	dtfl_event_link *src;
	dtfl_event_pusher pusher;
} replay_build;

static void
build_replay(dtfl_scope *scope, void *arg)
{
	replay_build *c = arg;

	dtfl_stream_capture_into(dtfl_event_link_replay(c->src, scope),
	    c->pusher, DTFL_NOCTX);
}

static int
cmp_pair(const void *va, const void *vb)
{
	const uint64_t *a = va, *b = vb;

	if (a[0] != b[0])
		return a[0] < b[0] ? -1 : 1;
	return a[1] < b[1] ? -1 : (a[1] > b[1]);
}

static void
prop_round_trip(pbt_tc *tc)
{
	size_t n = (size_t)pbt_int(tc, 0, MAX_RECS);
	static feeder_ctx feeder;	/* captured by the build callback */
	scenario *s;
	uint64_t *want;
	size_t i;

	feeder.n = n;
	for (i = 0; i < n; i++) {
		feeder.times[i] = (uint64_t)pbt_int(tc, 0, 20);
		feeder.vals[i] = (uint64_t)pbt_int(tc, 0, 1000000);
	}

	s = calloc(1, sizeof *s);
	pbt_defer(tc, scenario_free, s);
	s->w = dtfl_worker_new_standalone();
	s->tc = dtfl_time_u64();
	s->l1 = dtfl_event_link_new(s->tc, dtfl_type_u64());
	s->l2 = dtfl_event_link_new(s->tc, dtfl_type_u64());

	{
		capture_build cb = { &feeder, dtfl_event_link_pusher(s->l1) };
		replay_build rb = { s->l1, dtfl_event_link_pusher(s->l2) };

		PBT_ASSERT(dtfl_worker_dataflow(s->w, "capture", s->tc,
		    build_capture, &cb) == 0);
		PBT_ASSERT(dtfl_worker_dataflow(s->w, "replay", s->tc,
		    build_replay, &rb) == 0);
		while (dtfl_worker_step(s->w))
			;
	}
	dtfl_event_link_extract(s->l2, extract_each, &s->out);

	PBT_ASSERT(s->out.n == n);
	if (n == 0)
		return;

	/* Compare the (time, value) multisets. */
	want = malloc(n * 2 * sizeof(uint64_t));
	pbt_defer(tc, free, want);
	for (i = 0; i < n; i++) {
		want[i * 2] = feeder.times[i];
		want[i * 2 + 1] = feeder.vals[i];
	}
	qsort(want, n, 2 * sizeof(uint64_t), cmp_pair);
	qsort(s->out.pairs, n, 2 * sizeof(uint64_t), cmp_pair);
	for (i = 0; i < n * 2; i++)
		PBT_ASSERT(s->out.pairs[i] == want[i]);
}

static const pbt_prop props[] = {
	{ "round_trip", prop_round_trip, 0 },
};

PBT_MAIN("capture", props)
