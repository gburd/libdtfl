/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ops_data.c: the stateless/per-record operators (P4).
 *
 * Builds feeder -> operator -> sink dataflows on a standalone worker and
 * checks the operator outputs: flat_map, filter, inspect, concat,
 * exchange, ok_err, partition, to_stream and count.  The feeder and sink
 * mirror tests/unit/test_op.c (a no-input source sending u64 records at
 * chosen times, and a collecting one-input sink).
 *
 * flat_map__frontier_interest_never counts schedules: the record
 * operators built on Operator::unary or ::binary (generic/operator.rs)
 * declare FrontierInterest::Never, so a frontier change alone does not
 * schedule them, and the cost of advancing an input past a chain of maps
 * does not grow with the chain (examples/event_driven.rs).
 *
 * Ported in part from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/map.rs (test_builder)
 */
#include "dtfl_test.h"

#include "dtfl/log.h"
#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OUT 512

/* ------------------------------------------------------------------ */
/* Shared feeder and sink                                              */
/* ------------------------------------------------------------------ */

typedef struct collected {
	uint64_t vals[MAX_OUT];
	uint64_t times[MAX_OUT];
	size_t n;
} collected;

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
/* flat_map: port of core/map.rs test_builder                         */
/* ------------------------------------------------------------------ */

/* map(+1): one output per input. */
static void
plus_one(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)time; (void)len;
	dtfl_put_u64(buf, dtfl_get_u64(rec) + 1);
	dtfl_emit_record(out, buf, sizeof buf);
}

typedef struct map_build_ctx {
	feeder_ctx feeder;
	collected *out;
} map_build_ctx;

static void
build_map(dtfl_scope *scope, void *arg)
{
	map_build_ctx *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);

	/* flat_map(+1) four times: each input x becomes x + 4 (test_builder). */
	s = dtfl_stream_flat_map(s, dtfl_type_u64(), plus_one, DTFL_NOCTX);
	s = dtfl_stream_flat_map(s, dtfl_type_u64(), plus_one, DTFL_NOCTX);
	s = dtfl_stream_flat_map(s, dtfl_type_u64(), plus_one, DTFL_NOCTX);
	s = dtfl_stream_flat_map(s, dtfl_type_u64(), plus_one, DTFL_NOCTX);
	make_sink(s, c->out);
}

DTFL_TEST(dataflow_operators_core_map__test_builder)
{
	uint64_t vals[10], times[10];
	collected out;
	map_build_ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	for (i = 0; i < 10; i++) {
		vals[i] = i;
		times[i] = 0;
	}
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 10;
	c.out = &out;
	run_standalone(build_map, &c);

	assert_size(out.n, ==, 10);
	qsort(out.vals, out.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 10; i++)
		assert_uint64(out.vals[i], ==, i + 4);	/* 4..14 */
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* flat_map: zero and many outputs per input                          */
/* ------------------------------------------------------------------ */

/* Emit x copies of x (0 produces nothing, so it is a flattening map). */
static void
replicate(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	uint64_t x = dtfl_get_u64(rec);
	uint64_t k;

	(void)ctx; (void)time; (void)len;
	for (k = 0; k < x; k++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, x);
		dtfl_emit_record(out, buf, sizeof buf);
	}
}

static void
build_flat_map(dtfl_scope *scope, void *arg)
{
	map_build_ctx *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);

	make_sink(dtfl_stream_flat_map(s, dtfl_type_u64(), replicate, DTFL_NOCTX),
	    c->out);
}

DTFL_TEST(flat_map__variadic)
{
	static const uint64_t vals[] = { 0, 1, 2, 3 };
	static const uint64_t times[] = { 0, 0, 0, 0 };
	collected out;
	map_build_ctx c;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 4;
	c.out = &out;
	run_standalone(build_flat_map, &c);

	/* 0 -> {}, 1 -> {1}, 2 -> {2,2}, 3 -> {3,3,3}: 6 records. */
	assert_size(out.n, ==, 6);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* filter                                                              */
/* ------------------------------------------------------------------ */

static bool
is_even(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)len;
	return dtfl_get_u64(rec) % 2 == 0;
}

static void
build_filter(dtfl_scope *scope, void *arg)
{
	map_build_ctx *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);

	make_sink(dtfl_stream_filter(s, is_even, DTFL_NOCTX), c->out);
}

DTFL_TEST(filter__even)
{
	uint64_t vals[10], times[10];
	collected out;
	map_build_ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	for (i = 0; i < 10; i++) {
		vals[i] = i;
		times[i] = 0;
	}
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 10;
	c.out = &out;
	run_standalone(build_filter, &c);

	assert_size(out.n, ==, 5);	/* 0,2,4,6,8 */
	qsort(out.vals, out.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 5; i++)
		assert_uint64(out.vals[i], ==, 2 * i);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* inspect: passes records through, sees each (time, record)          */
/* ------------------------------------------------------------------ */

typedef struct inspect_probe {
	uint64_t sum;		/* sum of inspected records */
	size_t count;		/* records inspected */
} inspect_probe;

static void
inspect_each(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	inspect_probe *p = ctx;

	(void)time; (void)len;
	p->sum += dtfl_get_u64(rec);
	p->count++;
}

typedef struct inspect_build_ctx {
	feeder_ctx feeder;
	collected *out;
	inspect_probe *probe;
} inspect_build_ctx;

static void
build_inspect(dtfl_scope *scope, void *arg)
{
	inspect_build_ctx *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);

	s = dtfl_stream_inspect(s, inspect_each, DTFL_CTX(c->probe, NULL));
	make_sink(s, c->out);	/* inspect passes records through unchanged */
}

DTFL_TEST(inspect__passthrough)
{
	uint64_t vals[10], times[10];
	collected out;
	inspect_probe probe;
	inspect_build_ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	memset(&probe, 0, sizeof probe);
	for (i = 0; i < 10; i++) {
		vals[i] = i;
		times[i] = 0;
	}
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 10;
	c.out = &out;
	c.probe = &probe;
	run_standalone(build_inspect, &c);

	assert_size(out.n, ==, 10);		/* passed through unchanged */
	assert_size(probe.count, ==, 10);	/* every record inspected */
	assert_uint64(probe.sum, ==, 45);	/* 0 + 1 + ... + 9 */
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* concat: merges two streams                                         */
/* ------------------------------------------------------------------ */

typedef struct concat_build_ctx {
	feeder_ctx a, b;
	collected *out;
} concat_build_ctx;

static void
build_concat(dtfl_scope *scope, void *arg)
{
	concat_build_ctx *c = arg;
	dtfl_stream *sa = make_feeder(scope, &c->a);
	dtfl_stream *sb = make_feeder(scope, &c->b);

	make_sink(dtfl_stream_concat(sa, sb), c->out);
}

DTFL_TEST(concat__merges)
{
	static const uint64_t av[] = { 1, 2, 3 }, at[] = { 0, 0, 0 };
	static const uint64_t bv[] = { 4, 5 }, bt[] = { 0, 0 };
	collected out;
	concat_build_ctx c;
	uint64_t want[] = { 1, 2, 3, 4, 5 };
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.a.vals = av; c.a.times = at; c.a.n = 3;
	c.b.vals = bv; c.b.times = bt; c.b.n = 2;
	c.out = &out;
	run_standalone(build_concat, &c);

	assert_size(out.n, ==, 5);
	qsort(out.vals, out.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 5; i++)
		assert_uint64(out.vals[i], ==, want[i]);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* exchange: single standalone worker routes to itself (peers == 1)   */
/* ------------------------------------------------------------------ */

static void
build_exchange(dtfl_scope *scope, void *arg)
{
	map_build_ctx *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);

	/* One worker: every record routes back to this worker, unchanged. */
	make_sink(dtfl_stream_exchange(s, NULL, NULL), c->out);
}

DTFL_TEST(exchange__single_worker)
{
	uint64_t vals[8], times[8];
	collected out;
	map_build_ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	for (i = 0; i < 8; i++) {
		vals[i] = i;
		times[i] = 0;
	}
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 8;
	c.out = &out;
	run_standalone(build_exchange, &c);

	assert_size(out.n, ==, 8);
	qsort(out.vals, out.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 8; i++)
		assert_uint64(out.vals[i], ==, i);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* ok_err: splits a stream by a predicate                             */
/* ------------------------------------------------------------------ */

typedef struct ok_err_build_ctx {
	feeder_ctx feeder;
	collected *ok, *err;
} ok_err_build_ctx;

static void
build_ok_err(dtfl_scope *scope, void *arg)
{
	ok_err_build_ctx *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);
	dtfl_stream *ok, *err;

	dtfl_stream_ok_err(s, is_even, DTFL_NOCTX, &ok, &err);
	make_sink(ok, c->ok);
	make_sink(err, c->err);
}

DTFL_TEST(ok_err__split)
{
	uint64_t vals[10], times[10];
	collected ok, err;
	ok_err_build_ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&ok, 0, sizeof ok);
	memset(&err, 0, sizeof err);
	for (i = 0; i < 10; i++) {
		vals[i] = i;
		times[i] = 0;
	}
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 10;
	c.ok = &ok;
	c.err = &err;
	run_standalone(build_ok_err, &c);

	assert_size(ok.n, ==, 5);	/* evens */
	assert_size(err.n, ==, 5);	/* odds */
	for (i = 0; i < ok.n; i++)
		assert_uint64(ok.vals[i] % 2, ==, 0);
	for (i = 0; i < err.n; i++)
		assert_uint64(err.vals[i] % 2, ==, 1);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* partition: route records into parts                                */
/* ------------------------------------------------------------------ */

static size_t
mod3(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)len;
	return (size_t)(dtfl_get_u64(rec) % 3);
}

typedef struct part_build_ctx {
	feeder_ctx feeder;
	collected out[3];
} part_build_ctx;

static void
build_partition(dtfl_scope *scope, void *arg)
{
	part_build_ctx *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);
	dtfl_stream *outs[3];
	size_t i;

	dtfl_stream_partition(s, 3, mod3, DTFL_NOCTX, outs);
	for (i = 0; i < 3; i++)
		make_sink(outs[i], &c->out[i]);
}

DTFL_TEST(partition__mod3)
{
	uint64_t vals[9], times[9];
	part_build_ctx c;
	size_t i, p;

	(void)params; (void)data;
	memset(&c, 0, sizeof c);
	for (i = 0; i < 9; i++) {
		vals[i] = i;
		times[i] = 0;
	}
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 9;
	run_standalone(build_partition, &c);

	for (p = 0; p < 3; p++) {
		assert_size(c.out[p].n, ==, 3);	/* 9 records / 3 parts */
		for (i = 0; i < c.out[p].n; i++)
			assert_size((size_t)(c.out[p].vals[i] % 3), ==, p);
	}
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* to_stream: replays an iterator at the minimum time                 */
/* ------------------------------------------------------------------ */

typedef struct iter_state {
	uint64_t next, end;
} iter_state;

static bool
range_next(void *ctx, dtfl_emit *out)
{
	iter_state *it = ctx;
	unsigned char buf[8];

	if (it->next >= it->end)
		return false;
	dtfl_put_u64(buf, it->next++);
	dtfl_emit_record(out, buf, sizeof buf);
	return true;
}

typedef struct to_stream_build_ctx {
	iter_state it;
	collected *out;
} to_stream_build_ctx;

static void
build_to_stream(dtfl_scope *scope, void *arg)
{
	to_stream_build_ctx *c = arg;
	dtfl_stream *s = dtfl_to_stream(scope, dtfl_type_u64(), range_next,
	    DTFL_CTX(&c->it, NULL));

	make_sink(s, c->out);
}

DTFL_TEST(to_stream__range)
{
	collected out;
	to_stream_build_ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.it.next = 0;
	c.it.end = 100;		/* spans several chunks */
	c.out = &out;
	run_standalone(build_to_stream, &c);

	assert_size(out.n, ==, 100);
	qsort(out.vals, out.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 100; i++) {
		assert_uint64(out.vals[i], ==, i);
		assert_uint64(out.times[i], ==, 0);	/* minimum time */
	}
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* count: records per time, emitted when the frontier passes          */
/* ------------------------------------------------------------------ */

typedef struct count_build_ctx {
	feeder_ctx feeder;
	collected *out;
} count_build_ctx;

static void
build_count(dtfl_scope *scope, void *arg)
{
	count_build_ctx *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);

	make_sink(dtfl_stream_count(s), c->out);
}

DTFL_TEST(count__per_time)
{
	/* 3 records at time 0, 2 at time 1, 1 at time 2. */
	static const uint64_t vals[] = { 0, 0, 0, 0, 0, 0 };
	static const uint64_t times[] = { 0, 0, 0, 1, 1, 2 };
	collected out;
	count_build_ctx c;
	size_t i;
	uint64_t count_at[3] = { 0, 0, 0 };

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 6;
	c.out = &out;
	run_standalone(build_count, &c);

	assert_size(out.n, ==, 3);	/* one count per distinct time */
	for (i = 0; i < out.n; i++)
		count_at[out.times[i]] = out.vals[i];
	assert_uint64(count_at[0], ==, 3);
	assert_uint64(count_at[1], ==, 2);
	assert_uint64(count_at[2], ==, 1);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* flat_map: frontier changes alone do not schedule it                 */
/* ------------------------------------------------------------------ */

/* FlatMap Schedule-start events seen while `counting` is set. */
typedef struct sched_count {
	size_t ids[1024];
	size_t nids;
	bool counting;
	size_t starts;
} sched_count;

static void
count_flat_map_starts(const dtfl_log_batch *b, void *ctx)
{
	sched_count *sc = ctx;
	size_t n = dtfl_log_batch_len(b), i, k;

	for (i = 0; i < n; i++) {
		switch (dtfl_log_event_kind_at(b, i)) {
		case DTFL_LOG_OPERATES:
			if (strcmp(dtfl_log_operates_name(b, i), "FlatMap") == 0 &&
			    sc->nids < sizeof sc->ids / sizeof sc->ids[0])
				sc->ids[sc->nids++] = dtfl_log_operates_id(b, i);
			break;
		case DTFL_LOG_SCHEDULE:
			if (!sc->counting || !dtfl_log_schedule_start(b, i))
				break;
			for (k = 0; k < sc->nids; k++)
				if (sc->ids[k] == dtfl_log_schedule_id(b, i))
					sc->starts++;
			break;
		default:
			break;
		}
	}
}

static void
identity_unit(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	(void)ctx; (void)time;
	dtfl_emit_record(out, rec, len);
}

typedef struct chain_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
	size_t length;
	dtfl_stream *outer, *result;
} chain_ctx;

static void
build_chain_region(dtfl_scope *inner, void *arg)
{
	chain_ctx *c = arg;
	dtfl_stream *s = dtfl_stream_enter(c->outer, inner);
	size_t k;

	for (k = 0; k < c->length; k++)
		s = dtfl_stream_flat_map(s, dtfl_type_unit(), identity_unit,
		    DTFL_NOCTX);
	c->result = dtfl_stream_leave(s);
}

static void
build_chain(dtfl_scope *scope, void *arg)
{
	chain_ctx *c = arg;

	c->outer = dtfl_input_to_stream(c->input, scope);
	assert_int(dtfl_scope_region(scope, "region", build_chain_region, c),
	    ==, DTFL_OK);
	dtfl_stream_probe_with(c->result, c->probe);
}

/*
 * The event_driven.rs shape: input -> region(length maps) -> probe.
 * Returns the FlatMap schedules taken by `rounds` input advances with
 * no data, after the build's initial schedules.
 */
static size_t
flat_map_schedules(size_t length, uint64_t rounds)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	static sched_count sc;
	chain_ctx c;
	uint64_t round;

	memset(&sc, 0, sizeof sc);
	assert_int(dtfl_worker_log_subscribe(w, "timely",
	    count_flat_map_starts, &sc, NULL), ==, DTFL_OK);
	c.input = dtfl_input_new(dtfl_type_unit());
	c.probe = dtfl_probe_new();
	c.length = length;
	assert_int(dtfl_worker_dataflow(w, "chain", tc, build_chain, &c), ==, 0);
	/* Settle the build: every operator runs once on its initial activation. */
	round = 1;
	dtfl_input_advance_to(c.input, &round);
	while (dtfl_probe_less_than(c.probe, &round))
		dtfl_worker_step(w);
	dtfl_worker_step(w);
	assert_size(sc.nids, ==, length);
	sc.counting = true;
	for (round = 2; round < rounds + 2; round++) {
		dtfl_input_advance_to(c.input, &round);
		while (dtfl_probe_less_than(c.probe, &round))
			dtfl_worker_step(w);
	}
	sc.counting = false;
	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return sc.starts;
}

DTFL_TEST(flat_map__frontier_interest_never)
{
	(void)params; (void)data;
	/*
	 * No records flow, so no map has input to read: advancing the
	 * frontier must not schedule any of them, however long the chain.
	 */
	assert_size(flat_map_schedules(4, 50), ==, 0);
	assert_size(flat_map_schedules(64, 50), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_operators_core_map__test_builder),
	DTFL_T(flat_map__variadic),
	DTFL_T(flat_map__frontier_interest_never),
	DTFL_T(filter__even),
	DTFL_T(inspect__passthrough),
	DTFL_T(concat__merges),
	DTFL_T(exchange__single_worker),
	DTFL_T(ok_err__split),
	DTFL_T(partition__mod3),
	DTFL_T(to_stream__range),
	DTFL_T(count__per_time),
	DTFL_T_END
};

DTFL_TEST_MAIN("ops_data", tests)
