/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/capture.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/unit/test_dd_capture.c: the CDC V2 capture and replay round trip.
 *
 * capture.rs has no #[test] of its own; its exit criterion (PLAN P10) is
 * that a collection captured and replayed equals the original multiset
 * even when the event stream is duplicated and reordered.  Each test
 * captures a consolidated collection into a message log, abuses the log
 * (duplicates every message, reverses the order, or both), replays it
 * through the deduplicating, reordering iterator, and checks that the
 * reconstructed (record, time, diff) multiset equals the captured one.
 */
#include "dtfl_test.h"

#include "dtfl/collection.h"
#include "dtfl/dd_capture.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

/* A reconstructed or captured update: a u64 record, a u64 time, an i64 diff. */
typedef struct upd {
	uint64_t rec;
	uint64_t time;
	int64_t diff;
} upd;

typedef struct updset {
	upd u[256];
	size_t n;
} updset;

static void
updset_add(updset *s, uint64_t rec, uint64_t time, int64_t diff)
{
	if (s->n < 256) {
		s->u[s->n].rec = rec;
		s->u[s->n].time = time;
		s->u[s->n].diff = diff;
		s->n++;
	}
}

/* The net diff of (rec, time) across a set. */
static int64_t
net_at(const updset *s, uint64_t rec, uint64_t time)
{
	size_t i;
	int64_t d = 0;

	for (i = 0; i < s->n; i++)
		if (s->u[i].rec == rec && s->u[i].time == time)
			d += s->u[i].diff;
	return d;
}

/* ------------------------------------------------------------------ */
/* Capture a fixed collection into a log.                               */
/* ------------------------------------------------------------------ */

/* The source updates to feed the input: (rec, time, diff). */
typedef struct src_upd {
	uint64_t rec, time;
	int64_t diff;
} src_upd;

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_cdc_log *log;
	dtfl_probe *probe;
} build_ctx;

static void
build_capture(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *in = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *con = dtfl_collection_consolidate(in);
	dtfl_collection *cap = dtfl_collection_capture(con, c->log);
	dtfl_collection *probed = dtfl_collection_probe_with(cap, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(cap);
	dtfl_collection_free(con);
	dtfl_collection_free(in);
}

/*
 * Runs the input updates through the capturing dataflow, filling and
 * returning a log of CDC V2 messages.
 */
static dtfl_cdc_log *
capture_collection(const src_upd *src, size_t n, size_t nepochs,
    const uint64_t *epochs, const size_t *epoch_end)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *rdiff = dtfl_diff_i64();
	dtfl_cdc_log *log = dtfl_cdc_log_new(dtfl_type_u64(), tc, rdiff);
	build_ctx c;
	size_t i, e, done = 0;
	int steps;

	c.in = dtfl_input_session_new(dtfl_type_u64(), rdiff);
	c.log = log;
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "capture", tc, build_capture, &c);

	/* Advance to each epoch, then insert that epoch's updates at it. */
	for (e = 0; e < nepochs; e++) {
		uint64_t now = epochs[e];

		dtfl_input_session_advance_to(c.in, &now);
		for (i = done; i < epoch_end[e] && i < n; i++) {
			unsigned char buf[8];
			int64_t d = src[i].diff;

			dtfl_put_u64(buf, src[i].rec);
			dtfl_input_session_update(c.in, buf, 8, &d);
		}
		done = epoch_end[e];
		dtfl_input_session_flush(c.in);
	}
	dtfl_input_session_close(c.in);

	for (steps = 0; steps < 2000000 && dtfl_worker_step(w); steps++)
		;

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(rdiff);
	dtfl_worker_free(w);
	return log;
}

/* ------------------------------------------------------------------ */
/* Replay a (possibly abused) log through the iterator.                 */
/* ------------------------------------------------------------------ */

static void
replay_sink(void *ctx, const void *rec, size_t reclen, const uint64_t *time,
    const void *diff)
{
	updset *out = ctx;
	int64_t d;

	assert_size(reclen, ==, 8);
	memcpy(&d, diff, sizeof d);
	updset_add(out, dtfl_get_u64(rec), time[0], d);
}

/* Replays `log` (fed in index order `order`) and fills `out`. */
static void
replay(const dtfl_cdc_log *log, const size_t *order, size_t m, updset *out)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *rdiff = dtfl_diff_i64();
	dtfl_cdc_iter *it = dtfl_cdc_iter_new(dtfl_type_u64(), tc, rdiff);
	size_t i;

	out->n = 0;
	for (i = 0; i < m; i++) {
		dtfl_cdc_iter_push(it, log, order[i]);
		while (dtfl_cdc_iter_next(it, replay_sink, out, NULL, NULL))
			;
	}
	/* Drain any remaining batches. */
	while (dtfl_cdc_iter_next(it, replay_sink, out, NULL, NULL))
		;

	dtfl_cdc_iter_free(it);
	dtfl_time_class_free(tc);
	dtfl_diff_free(rdiff);
}

/*
 * The reference multiset: the consolidated net (rec, time) diffs over
 * the source updates, keeping each epoch's time (the input time the
 * updates were inserted at, which is where consolidation leaves them).
 */
static void
oracle(const src_upd *src, size_t n, size_t nepochs, const uint64_t *epochs,
    const size_t *epoch_end, updset *out)
{
	size_t i, e, done = 0;

	out->n = 0;
	for (e = 0; e < nepochs; e++) {
		for (i = done; i < epoch_end[e] && i < n; i++)
			updset_add(out, src[i].rec, epochs[e], src[i].diff);
		done = epoch_end[e];
	}
}

/* Compares two consolidated multisets of (rec, time) net diffs. */
static void
assert_multiset_eq(const updset *a, const updset *b)
{
	size_t i;

	for (i = 0; i < a->n; i++)
		assert_int64(net_at(a, a->u[i].rec, a->u[i].time), ==,
		    net_at(b, a->u[i].rec, a->u[i].time));
	for (i = 0; i < b->n; i++)
		assert_int64(net_at(a, b->u[i].rec, b->u[i].time), ==,
		    net_at(b, b->u[i].rec, b->u[i].time));
}

/*
 * Runs one case: capture the source into a log, build the oracle
 * directly from the source, then replay the log four ways (as-is,
 * reversed, duplicated, and reversed+duplicated) and assert each
 * reconstruction equals the oracle.
 */
static MunitResult
run_case(const src_upd *src, size_t n, size_t nepochs, const uint64_t *epochs,
    const size_t *epoch_end)
{
	updset orc, got;
	dtfl_cdc_log *log = capture_collection(src, n, nepochs, epochs,
	    epoch_end);
	size_t len = dtfl_cdc_log_len(log);
	size_t order[1024];
	dtfl_cdc_log *dup;
	size_t i;

	oracle(src, n, nepochs, epochs, epoch_end, &orc);

	/* The log must carry at least one message (updates and progress). */
	assert_size(len, >, 0);

	/* 1. In order. */
	for (i = 0; i < len; i++)
		order[i] = i;
	replay(log, order, len, &got);
	assert_multiset_eq(&orc, &got);

	/* 2. Reversed. */
	for (i = 0; i < len; i++)
		order[i] = len - 1 - i;
	replay(log, order, len, &got);
	assert_multiset_eq(&orc, &got);

	/*
	 * 3 and 4.  Build a log in which every message appears twice, then
	 * replay it both in order and reversed.  Duplication must not change
	 * the reconstruction.
	 */
	{
		dtfl_time_class *tc = dtfl_time_u64();
		dtfl_diff *rdiff = dtfl_diff_i64();

		dup = dtfl_cdc_log_new(dtfl_type_u64(), tc, rdiff);
		dtfl_time_class_free(tc);
		dtfl_diff_free(rdiff);
	}
	for (i = 0; i < len; i++) {
		dtfl_cdc_log_append(dup, log, i);
		dtfl_cdc_log_append(dup, log, i);
	}
	{
		size_t dlen = dtfl_cdc_log_len(dup);

		assert_size(dlen, ==, 2 * len);
		for (i = 0; i < dlen; i++)
			order[i] = i;
		replay(dup, order, dlen, &got);
		assert_multiset_eq(&orc, &got);

		for (i = 0; i < dlen; i++)
			order[i] = dlen - 1 - i;
		replay(dup, order, dlen, &got);
		assert_multiset_eq(&orc, &got);
	}

	dtfl_cdc_log_free(dup);
	dtfl_cdc_log_free(log);
	return MUNIT_OK;
}

/*
 * A single epoch: three records inserted at time 1.  Consolidation nets
 * the +1/+1 and -1 for record 5 to leave +1, so the final multiset is
 * {(3,1,+1),(5,1,+1),(7,1,+1)}.
 */
DTFL_TEST(capture__single_epoch)
{
	static const src_upd src[] = {
	    { 3, 0, 1 }, { 5, 0, 1 }, { 7, 0, 1 }, { 5, 0, 1 }, { 5, 0, -1 },
	};
	static const uint64_t epochs[] = { 1 };
	static const size_t epoch_end[] = { 5 };

	(void)params; (void)data;
	return run_case(src, 5, 1, epochs, epoch_end);
}

/*
 * Two epochs: records at time 1, then more at time 2.  The capture emits
 * a Progress statement as the frontier passes each epoch, and the replay
 * must reconstruct both epochs' updates.
 */
DTFL_TEST(capture__two_epochs)
{
	static const src_upd src[] = {
	    { 1, 0, 1 }, { 2, 0, 1 },		/* epoch -> time 1 */
	    { 2, 0, 1 }, { 3, 0, 1 }, { 1, 0, -1 },	/* epoch -> time 2 */
	};
	static const uint64_t epochs[] = { 1, 2 };
	static const size_t epoch_end[] = { 2, 5 };

	(void)params; (void)data;
	return run_case(src, 5, 2, epochs, epoch_end);
}

/*
 * Three epochs with a retraction that fully cancels a record, so a time
 * carries a zero net change for one record but non-zero for others.
 */
DTFL_TEST(capture__three_epochs)
{
	static const src_upd src[] = {
	    { 10, 0, 1 }, { 20, 0, 2 },			/* -> time 1 */
	    { 30, 0, 1 },				/* -> time 2 */
	    { 10, 0, -1 }, { 40, 0, 1 }, { 20, 0, -2 },	/* -> time 3 */
	};
	static const uint64_t epochs[] = { 1, 2, 3 };
	static const size_t epoch_end[] = { 2, 3, 6 };

	(void)params; (void)data;
	return run_case(src, 6, 3, epochs, epoch_end);
}

static MunitTest tests[] = {
	DTFL_T(capture__single_epoch),
	DTFL_T(capture__two_epochs),
	DTFL_T(capture__three_epochs),
	DTFL_T_END
};

DTFL_TEST_MAIN("dd/capture", tests)
