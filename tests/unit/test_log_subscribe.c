/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_log_subscribe.c: the public structured-logging
 * subscriber API (dtfl/log.h).
 *
 * Three checks:
 *   subscribe__dataflow      a real dtfl_worker_dataflow with a "timely"
 *                            subscriber yields the Operates / Channels /
 *                            Schedule events for the operators built,
 *                            with the right ids and names.
 *   subscribe__roundtrip     the internal encoders feed the public
 *                            decoder: every timely variant decoded back
 *                            reproduces the fields encoded.
 *   subscribe__differential  a "differential/arrange" subscriber decodes
 *                            every differential variant round-trip.
 *   subscribe__communication a "communication" subscriber decodes every
 *                            communication variant round-trip.
 *   subscribe__arrange_emits an arrangement fed several epochs emits
 *                            real Batch, Merge, Drop, TraceShare and
 *                            Batcher events under its operator id.
 */
#include "dtfl_test.h"

#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"
#include "dtfl/log.h"
#include "dtfl/arrange.h"
#include "dtfl/reduce.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"

#include "timely/scope_int.h"	/* struct dtfl_worker { __dtfl_worker *w; } */
#include "timely/worker_int.h"	/* __dtfl_worker_log_*, headers */
#include "comm/zc_int.h"	/* __dtfl_message_header */
#include "base/log_int.h"	/* __dtfl_log_event */
#include "timely/log_codec_int.h"	/* __dtfl_log_decode_and_deliver */

#include <string.h>

/* ------------------------------------------------------------------ */
/* A real dataflow with a timely subscriber                            */
/* ------------------------------------------------------------------ */

#define MAX_OPS 32
#define MAX_NAME 64

typedef struct recorded {
	size_t n_operates;
	size_t op_id[MAX_OPS];
	char op_name[MAX_OPS][MAX_NAME];
	size_t n_channels;
	size_t ch_id[MAX_OPS];
	size_t n_schedule;
	bool saw_start;
	bool saw_stop;
	bool saw_flush;		/* an empty batch */
} recorded;

static void
record_timely(const dtfl_log_batch *b, void *ctx)
{
	recorded *r = ctx;
	size_t n = dtfl_log_batch_len(b), i;

	if (n == 0)
		r->saw_flush = true;
	assert_string_equal(dtfl_log_batch_stream(b), "timely");
	for (i = 0; i < n; i++) {
		switch (dtfl_log_event_kind_at(b, i)) {
		case DTFL_LOG_OPERATES:
			if (r->n_operates < MAX_OPS) {
				const char *name = dtfl_log_operates_name(b, i);

				r->op_id[r->n_operates] = dtfl_log_operates_id(b, i);
				snprintf(r->op_name[r->n_operates], MAX_NAME, "%s",
				    name != NULL ? name : "");
				r->n_operates++;
			}
			break;
		case DTFL_LOG_CHANNELS:
			if (r->n_channels < MAX_OPS)
				r->ch_id[r->n_channels++] = dtfl_log_channels_id(b, i);
			break;
		case DTFL_LOG_SCHEDULE:
			r->n_schedule++;
			if (dtfl_log_schedule_start(b, i))
				r->saw_start = true;
			else
				r->saw_stop = true;
			break;
		default:
			break;
		}
	}
}

static bool
range_next(void *ctx, dtfl_emit *out)
{
	uint64_t *n = ctx;
	unsigned char buf[8];

	if (*n >= 4)
		return false;
	dtfl_put_u64(buf, (*n)++);
	dtfl_emit_record(out, buf, sizeof buf);
	return true;
}

static void
inspect_noop(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)ctx; (void)time; (void)rec; (void)len;
}

typedef struct build_ctx {
	uint64_t n;
} build_ctx;

static void
build_two_ops(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_to_stream(scope, dtfl_type_u64(), range_next,
	    DTFL_CTX(&c->n, NULL));

	(void)dtfl_stream_inspect(s, inspect_noop, DTFL_NOCTX);
}

DTFL_TEST(subscribe__dataflow)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	static recorded r;
	build_ctx c = { 0 };
	bool found_tostream = false, found_inspect = false;
	size_t i;

	(void)params; (void)data;
	memset(&r, 0, sizeof r);

	assert_int(dtfl_worker_log_subscribe(w, "timely", record_timely, &r,
	    NULL), ==, DTFL_OK);
	assert_int(dtfl_worker_dataflow(w, "test", tc, build_two_ops, &c), ==, 0);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);

	/* The two operators built must each have an Operates event. */
	for (i = 0; i < r.n_operates; i++) {
		if (strcmp(r.op_name[i], "ToStream") == 0)
			found_tostream = true;
		if (strcmp(r.op_name[i], "InspectBatch") == 0)
			found_inspect = true;
	}
	assert_true(found_tostream);
	assert_true(found_inspect);
	/* A dataflow with operators wires at least one channel. */
	assert_size(r.n_channels, >, 0);
	/* Scheduling drove the operators: both a start and a stop. */
	assert_true(r.saw_start);
	assert_true(r.saw_stop);
	/* The worker flushes at least once (the final shutdown flush). */
	assert_true(r.saw_flush);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Encoder -> decoder round trips                                      */
/* ------------------------------------------------------------------ */

typedef struct rt {
	int which;		/* a selector the subscriber checks against */
	bool ok;
	size_t count;
} rt;

static void
check_timely(const dtfl_log_batch *b, void *ctx)
{
	rt *t = ctx;
	size_t n = dtfl_log_batch_len(b), i;

	for (i = 0; i < n; i++) {
		size_t addr_n, scope_n, sn, sp, tn, tp;
		const size_t *addr, *scope;
		uint64_t to;

		t->count++;
		switch (dtfl_log_event_kind_at(b, i)) {
		case DTFL_LOG_OPERATES:
			assert_size(dtfl_log_operates_id(b, i), ==, 7);
			addr = dtfl_log_operates_addr(b, i, &addr_n);
			assert_size(addr_n, ==, 2);
			assert_size(addr[0], ==, 0);
			assert_size(addr[1], ==, 2);
			assert_string_equal(dtfl_log_operates_name(b, i), "Map");
			break;
		case DTFL_LOG_CHANNELS:
			assert_size(dtfl_log_channels_id(b, i), ==, 3);
			scope = dtfl_log_channels_scope(b, i, &scope_n);
			assert_size(scope_n, ==, 1);
			assert_size(scope[0], ==, 0);
			dtfl_log_channels_source(b, i, &sn, &sp);
			dtfl_log_channels_target(b, i, &tn, &tp);
			assert_size(sn, ==, 1);
			assert_size(sp, ==, 0);
			assert_size(tn, ==, 2);
			assert_size(tp, ==, 1);
			assert_string_equal(dtfl_log_channels_typ(b, i), "u64");
			break;
		case DTFL_LOG_SCHEDULE:
			assert_size(dtfl_log_schedule_id(b, i), ==, 7);
			assert_true(dtfl_log_schedule_start(b, i));
			break;
		case DTFL_LOG_SHUTDOWN:
			assert_size(dtfl_log_shutdown_id(b, i), ==, 7);
			break;
		case DTFL_LOG_MESSAGES:
			assert_true(dtfl_log_messages_is_send(b, i));
			assert_size(dtfl_log_messages_channel(b, i), ==, 3);
			assert_size(dtfl_log_messages_source(b, i), ==, 0);
			assert_size(dtfl_log_messages_target(b, i), ==, 1);
			assert_size(dtfl_log_messages_seq_no(b, i), ==, 5);
			assert_int64(dtfl_log_messages_record_count(b, i), ==, 42);
			break;
		case DTFL_LOG_PARK:
			assert_true(dtfl_log_park_is_park(b, i));
			assert_true(dtfl_log_park_timeout(b, i, &to));
			assert_uint64(to, ==, UINT64_C(1500000000));
			break;
		default:
			break;
		}
	}
}

DTFL_TEST(subscribe__roundtrip)
{
	dtfl_worker *pub = dtfl_worker_new_standalone();
	__dtfl_worker *w = pub->w;
	static rt t;
	const size_t op_addr[2] = { 0, 2 };
	const size_t ch_scope[1] = { 0 };

	(void)params; (void)data;
	memset(&t, 0, sizeof t);

	assert_int(dtfl_worker_log_subscribe(pub, "timely", check_timely, &t,
	    NULL), ==, DTFL_OK);
	__dtfl_worker_log_operates(w, 7, op_addr, 2, "Map");
	__dtfl_worker_log_channel(w, 3, ch_scope, 1, 1, 0, 2, 1, "u64");
	__dtfl_worker_log_schedule(w, 7, true);
	__dtfl_worker_log_shutdown(w, 7);
	__dtfl_worker_log_messages(w, true, 3, 0, 1, 5, 42);
	__dtfl_worker_log_park(w, true, UINT64_C(1500000000));
	__dtfl_log_registry_flush(w->log);

	assert_size(t.count, ==, 6);
	dtfl_worker_free(pub);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Differential round trip                                             */
/* ------------------------------------------------------------------ */

static void
check_dd(const dtfl_log_batch *b, void *ctx)
{
	rt *t = ctx;
	size_t n = dtfl_log_batch_len(b), i;

	assert_string_equal(dtfl_log_batch_stream(b), "differential/arrange");
	for (i = 0; i < n; i++) {
		size_t complete;
		int64_t r0, r1, r2, r3;

		t->count++;
		switch (dtfl_log_event_kind_at(b, i)) {
		case DTFL_LOG_DD_BATCH:
			assert_size(dtfl_log_dd_operator(b, i), ==, 11);
			assert_size(dtfl_log_dd_length(b, i), ==, 9);
			break;
		case DTFL_LOG_DD_MERGE:
			assert_size(dtfl_log_dd_operator(b, i), ==, 12);
			assert_size(dtfl_log_dd_merge_scale(b, i), ==, 3);
			assert_size(dtfl_log_dd_merge_length1(b, i), ==, 4);
			assert_size(dtfl_log_dd_merge_length2(b, i), ==, 5);
			assert_true(dtfl_log_dd_merge_complete(b, i, &complete));
			assert_size(complete, ==, 8);
			break;
		case DTFL_LOG_DD_DROP:
			assert_size(dtfl_log_dd_operator(b, i), ==, 13);
			assert_size(dtfl_log_dd_length(b, i), ==, 2);
			break;
		case DTFL_LOG_DD_MERGE_SHORTFALL:
			assert_size(dtfl_log_dd_operator(b, i), ==, 14);
			assert_size(dtfl_log_dd_shortfall_scale(b, i), ==, 6);
			assert_size(dtfl_log_dd_shortfall(b, i), ==, 7);
			break;
		case DTFL_LOG_DD_TRACE_SHARE:
			assert_size(dtfl_log_dd_operator(b, i), ==, 15);
			assert_int64(dtfl_log_dd_trace_share_diff(b, i), ==, -2);
			break;
		case DTFL_LOG_DD_BATCHER:
			assert_size(dtfl_log_dd_operator(b, i), ==, 16);
			dtfl_log_dd_batcher(b, i, &r0, &r1, &r2, &r3);
			assert_int64(r0, ==, 10);
			assert_int64(r1, ==, -20);
			assert_int64(r2, ==, 30);
			assert_int64(r3, ==, -40);
			break;
		default:
			break;
		}
	}
}

DTFL_TEST(subscribe__differential)
{
	dtfl_worker *pub = dtfl_worker_new_standalone();
	__dtfl_worker *w = pub->w;
	static rt t;

	(void)params; (void)data;
	memset(&t, 0, sizeof t);

	assert_int(dtfl_worker_log_subscribe(pub, "differential/arrange",
	    check_dd, &t, NULL), ==, DTFL_OK);
	__dtfl_worker_log_dd_batch(w, 11, 9);
	__dtfl_worker_log_dd_merge(w, 12, 3, 4, 5, true, 8);
	__dtfl_worker_log_dd_drop(w, 13, 2);
	__dtfl_worker_log_dd_merge_shortfall(w, 14, 6, 7);
	__dtfl_worker_log_dd_trace_share(w, 15, -2);
	__dtfl_worker_log_dd_batcher(w, 16, 10, -20, 30, -40);
	__dtfl_log_registry_flush(w->log);

	assert_size(t.count, ==, 6);
	dtfl_worker_free(pub);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Communication round trip                                            */
/* ------------------------------------------------------------------ */

static void
check_comm(const dtfl_log_batch *b, void *ctx)
{
	rt *t = ctx;
	size_t n = dtfl_log_batch_len(b), i;

	assert_string_equal(dtfl_log_batch_stream(b), "communication");
	for (i = 0; i < n; i++) {
		size_t ch, src, tl, tu, len, seq, remote;

		t->count++;
		switch (dtfl_log_event_kind_at(b, i)) {
		case DTFL_LOG_COMM_MESSAGE:
			assert_true(dtfl_log_comm_message_is_send(b, i));
			dtfl_log_comm_message_header(b, i, &ch, &src, &tl, &tu,
			    &len, &seq);
			assert_size(ch, ==, 2);
			assert_size(src, ==, 3);
			assert_size(tl, ==, 4);
			assert_size(tu, ==, 5);
			assert_size(len, ==, 128);
			assert_size(seq, ==, 9);
			break;
		case DTFL_LOG_COMM_STATE:
			assert_true(dtfl_log_comm_state_send(b, i));
			assert_size(dtfl_log_comm_state_process(b, i), ==, 1);
			assert_size(dtfl_log_comm_state_remote(b, i), ==, 2);
			assert_true(dtfl_log_comm_state_start(b, i));
			break;
		case DTFL_LOG_COMM_SETUP:
			assert_false(dtfl_log_comm_setup_sender(b, i));
			assert_size(dtfl_log_comm_setup_process(b, i), ==, 1);
			assert_true(dtfl_log_comm_setup_remote(b, i, &remote));
			assert_size(remote, ==, 7);
			break;
		default:
			break;
		}
	}
}

DTFL_TEST(subscribe__communication)
{
	dtfl_worker *pub = dtfl_worker_new_standalone();
	__dtfl_worker *w = pub->w;
	static rt t;
	__dtfl_message_header h;

	(void)params; (void)data;
	memset(&t, 0, sizeof t);
	memset(&h, 0, sizeof h);
	h.channel = 2;
	h.source = 3;
	h.target_lower = 4;
	h.target_upper = 5;
	h.length = 128;
	h.seqno = 9;

	assert_int(dtfl_worker_log_subscribe(pub, "communication", check_comm,
	    &t, NULL), ==, DTFL_OK);
	__dtfl_worker_log_comm_message(w, true, &h);
	__dtfl_worker_log_comm_state(w, true, 1, 2, true);
	__dtfl_worker_log_comm_setup(w, false, 1, true, 7);
	__dtfl_log_registry_flush(w->log);

	assert_size(t.count, ==, 3);
	dtfl_worker_free(pub);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Real differential emission from an arrangement                      */
/* ------------------------------------------------------------------ */

#define DD_EPOCHS 12
#define DD_PER_EPOCH 5
#define DD_MAX_OPS 8

/* Per-operator differential totals, keyed by the operator global id. */
typedef struct dd_op {
	size_t id;
	char name[MAX_NAME];
	size_t batches, batch_len;
	size_t merge_begin, merge_done;
	size_t drops, drop_len;
	int64_t shares;		/* running sum of TraceShare diffs */
	size_t share_events;
	int64_t batcher_records;	/* running sum of records_diff */
	size_t batcher_events;
} dd_op;

typedef struct dd_seen {
	dd_op ops[DD_MAX_OPS];	/* from the timely Operates events */
	size_t nops;
	size_t foreign;		/* dd events from an unknown operator */
	bool merge_bad;		/* complete != length1 + length2 */
	bool batcher_nonzero_bytes;
} dd_seen;

static dd_op *
dd_find(dd_seen *s, size_t id)
{
	size_t i;

	for (i = 0; i < s->nops; i++)
		if (s->ops[i].id == id)
			return &s->ops[i];
	return NULL;
}

static void
dd_timely(const dtfl_log_batch *b, void *ctx)
{
	dd_seen *s = ctx;
	size_t i;

	for (i = 0; i < dtfl_log_batch_len(b); i++)
		if (dtfl_log_event_kind_at(b, i) == DTFL_LOG_OPERATES &&
		    s->nops < DD_MAX_OPS) {
			dd_op *o = &s->ops[s->nops++];

			o->id = dtfl_log_operates_id(b, i);
			snprintf(o->name, sizeof o->name, "%s",
			    dtfl_log_operates_name(b, i));
		}
}

static void
dd_arrange(const dtfl_log_batch *b, void *ctx)
{
	dd_seen *s = ctx;
	size_t i, c;
	int64_t r, sz, cap, al;

	assert_string_equal(dtfl_log_batch_stream(b), "differential/arrange");
	for (i = 0; i < dtfl_log_batch_len(b); i++) {
		dd_op *o = dd_find(s, dtfl_log_dd_operator(b, i));

		if (o == NULL) {
			s->foreign++;
			continue;
		}
		switch (dtfl_log_event_kind_at(b, i)) {
		case DTFL_LOG_DD_BATCH:
			o->batches++;
			o->batch_len += dtfl_log_dd_length(b, i);
			break;
		case DTFL_LOG_DD_MERGE:
			if (dtfl_log_dd_merge_complete(b, i, &c)) {
				o->merge_done++;
				/* distinct keys: a merge consolidates nothing */
				if (c != dtfl_log_dd_merge_length1(b, i) +
				    dtfl_log_dd_merge_length2(b, i))
					s->merge_bad = true;
			} else {
				o->merge_begin++;
			}
			break;
		case DTFL_LOG_DD_DROP:
			o->drops++;
			o->drop_len += dtfl_log_dd_length(b, i);
			break;
		case DTFL_LOG_DD_TRACE_SHARE:
			o->share_events++;
			o->shares += dtfl_log_dd_trace_share_diff(b, i);
			break;
		case DTFL_LOG_DD_BATCHER:
			dtfl_log_dd_batcher(b, i, &r, &sz, &cap, &al);
			o->batcher_events++;
			o->batcher_records += r;
			if (sz != 0 || cap != 0 || al != 0)
				s->batcher_nonzero_bytes = true;
			break;
		default:
			s->foreign++;
			break;
		}
	}
}

typedef struct dd_build {
	dtfl_input_session *in;
	dtfl_probe *probe;
	dtfl_diff *diff;
} dd_build;

/* distinct = ArrangeBySelf (batcher + spine) -> Reduce (output spine). */
static void
dd_build_distinct(dtfl_scope *scope, void *arg)
{
	dd_build *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *d = dtfl_collection_distinct(base, c->diff);
	dtfl_collection *probed = dtfl_collection_probe_with(d, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(d);
	dtfl_collection_free(base);
}

static bool
dd_probe_behind(void *ctx)
{
	void **pc = ctx;

	return dtfl_probe_less_than(pc[0], pc[1]);
}

DTFL_TEST(subscribe__arrange_emits)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	static dd_seen s;
	dd_build c;
	dd_op *arr, *red;
	uint64_t e, next;
	size_t i, total = DD_EPOCHS * DD_PER_EPOCH;

	(void)params; (void)data;
	memset(&s, 0, sizeof s);
	assert_int(dtfl_worker_log_subscribe(w, "timely", dd_timely, &s,
	    NULL), ==, DTFL_OK);
	assert_int(dtfl_worker_log_subscribe(w, "differential/arrange",
	    dd_arrange, &s, NULL), ==, DTFL_OK);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.diff = r;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, dd_build_distinct, &c),
	    ==, 0);

	/* Distinct keys every epoch: no update cancels, even compacted. */
	for (e = 0; e < DD_EPOCHS; e++) {
		void *pc[2];

		for (i = 0; i < DD_PER_EPOCH; i++) {
			unsigned char key[8];

			dtfl_put_u64(key, e * DD_PER_EPOCH + i);
			dtfl_input_session_insert(c.in, key, sizeof key);
		}
		next = e + 1;
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		pc[0] = c.probe;
		pc[1] = &next;
		dtfl_worker_step_while(w, dd_probe_behind, pc);
	}
	/* distinct names both operators "Distinct": the arrange is the one
	 * with a batcher, the reduce the other spine that saw batches. */
	arr = red = NULL;
	for (i = 0; i < s.nops; i++) {
		if (s.ops[i].batcher_events > 0)
			arr = &s.ops[i];
		else if (s.ops[i].batches > 0)
			red = &s.ops[i];
	}
	assert_not_null(arr);
	assert_not_null(red);
	assert_string_equal(arr->name, "Distinct");
	assert_string_equal(red->name, "Distinct");

	/* Mid-run.  The arrange operator: a batch per epoch, every update
	 * accounted in the batcher, the trace shared by its readers. */
	assert_size(arr->batches, >=, DD_EPOCHS);
	assert_size(arr->batch_len, ==, total);
	assert_size(arr->batcher_events, >, 0);
	assert_int64(arr->batcher_records, ==, 0);	/* all sealed out */
	assert_int64(arr->shares, >=, 1);
	/* The reduce output spine compacts behind its reader, so it merges:
	 * a begin for every completion, and lengths that add up. */
	assert_size(red->batch_len, ==, total);
	assert_size(red->merge_begin, >, 0);
	assert_size(red->merge_done, >, 0);
	assert_size(red->merge_begin, >=, red->merge_done);
	assert_false(s.merge_bad);

	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_worker_free(w);

	/* Every event came from an operator the timely stream announced. */
	assert_size(s.foreign, ==, 0);
	/* Every reader that shared each trace let go of it. */
	assert_size(arr->share_events, >=, 2);
	assert_int64(arr->shares, ==, 0);
	assert_size(red->share_events, >=, 2);
	assert_int64(red->shares, ==, 0);
	/* Dropping each spine retired every update it still held. */
	assert_size(arr->drop_len, ==, total);
	assert_size(red->drops, >, 0);
	assert_size(red->drop_len, ==, total);
	/* The pin's vector merger reports no allocation figures. */
	assert_false(s.batcher_nonzero_bytes);

	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	return MUNIT_OK;
}

static void
empty_addr_check(const void *batch, void *ctx)
{
	const dtfl_log_batch *b = batch;
	size_t n = 99;

	(void)ctx;
	assert_int(dtfl_log_event_kind_at(b, 0), ==, DTFL_LOG_OPERATES);
	/* An empty address is a valid pointer to zero words, not NULL. */
	assert_not_null(dtfl_log_operates_addr(b, 0, &n));
	assert_size(n, ==, 0);
}

/*
 * A batch whose only event has an empty address used to return
 * NULL + 0 from the accessor, undefined behaviour that UBSan reports
 * (fuzz_log found it; reproducer tests/corpus/fuzz/fuzz_log/
 * regress-empty-addr).
 */
DTFL_TEST(subscribe__empty_addr)
{
	/* Operates { id 7, addr [], name "" } */
	static const unsigned char rec[4 + 8 + 8 + 8] = { 0, 0, 0, 0, 7 };
	__dtfl_log_event ev = { 0, rec, sizeof rec };
	void *scratch = __dtfl_log_scratch_new();

	(void)params; (void)data;
	__dtfl_log_decode_and_deliver(scratch, "timely", 0, 0, &ev, 1,
	    empty_addr_check, NULL);
	__dtfl_log_scratch_free(scratch);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(subscribe__empty_addr),
	DTFL_T(subscribe__dataflow),
	DTFL_T(subscribe__roundtrip),
	DTFL_T(subscribe__differential),
	DTFL_T(subscribe__communication),
	DTFL_T(subscribe__arrange_emits),
	DTFL_T_END
};

DTFL_TEST_MAIN("log_subscribe", tests)
