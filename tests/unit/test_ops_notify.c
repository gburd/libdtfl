/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ops_notify.c: the notification-based operators (P4).
 *
 * Builds feeder -> operator -> sink dataflows on a standalone worker
 * (and a two-process worker for broadcast) and checks the outputs:
 *   delay          re-times each record and the output frontier follows;
 *   aggregate      folds (key, value) within each time;
 *   state_machine  threads per-key state across records;
 *   branch         splits by a record/time predicate;
 *   broadcast      replicates every record to every worker.
 * The feeder and sink mirror tests/unit/test_ops_data.c.
 *
 * The delay, aggregate, state_machine, branch and broadcast operators
 * are public at timely-v0.31.0 and were removed upstream afterward; the
 * tests follow their pinned behaviour (delay.rs, aggregate.rs,
 * state_machine.rs, branch.rs, broadcast.rs).
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>
#include <string.h>

#define MAX_OUT 512
#define MAX_WORKERS 8

/* ------------------------------------------------------------------ */
/* Shared feeder and sink                                              */
/* ------------------------------------------------------------------ */

typedef struct collected {
	uint64_t vals[MAX_OUT];
	uint64_t keys[MAX_OUT];	/* for pair outputs; else 0 */
	uint64_t times[MAX_OUT];
	size_t n;
} collected;

typedef struct feeder_ctx {
	const uint64_t *vals;
	const uint64_t *times;
	size_t n;
	const dtfl_type *type;	/* u64, or pair(u64,u64) when keys != NULL */
	const uint64_t *keys;	/* NULL for a plain u64 record */
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
		unsigned char buf[16];

		if (f->keys != NULL) {
			dtfl_put_u64(buf, f->keys[i]);
			dtfl_put_u64(buf + 8, f->vals[i]);
			dtfl_session_give(s, buf, 16);
		} else {
			dtfl_put_u64(buf, f->vals[i]);
			dtfl_session_give(s, buf, 8);
		}
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
	dtfl_stream *out = dtfl_op_builder_output(b, f->type);

	dtfl_op_builder_build(b, feeder_logic, f, NULL);
	return out;
}

/* A sink collecting u64 values (or pair keys+values) with their times. */
typedef struct sink_ctx {
	collected *c;
	bool pair;
} sink_ctx;

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	sink_ctx *sc = vctx;
	collected *c = sc->c;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t i, n = dtfl_message_len(m);
		const uint64_t *t = dtfl_message_time(m);

		for (i = 0; i < n && c->n < MAX_OUT; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			if (sc->pair) {
				c->keys[c->n] = dtfl_get_u64(rec);
				c->vals[c->n] = dtfl_get_u64((const unsigned char *)rec + 8);
			} else {
				c->keys[c->n] = 0;
				c->vals[c->n] = dtfl_get_u64(rec);
			}
			c->times[c->n] = t[0];
			c->n++;
		}
	}
	return false;
}

static void
make_sink2(dtfl_stream *in, collected *c, bool pair)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(in), "Sink");
	sink_ctx *sc = malloc(sizeof *sc);

	sc->c = c;
	sc->pair = pair;
	dtfl_op_builder_input(b, in, dtfl_pact_pipeline());
	dtfl_op_builder_build(b, sink_logic, sc, free);
}

static void
make_sink(dtfl_stream *in, collected *c)
{
	make_sink2(in, c, false);
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
/* delay: re-time each record i (at time 0) to time i                  */
/* ------------------------------------------------------------------ */

/* new_time = value (delay.rs example: 0..10 at time 0 to time i). */
static void
delay_to_value(void *ctx, const void *rec, size_t len, const uint64_t *time,
    uint64_t *new_time)
{
	(void)ctx; (void)len; (void)time;
	new_time[0] = dtfl_get_u64(rec);
}

typedef struct one_build {
	feeder_ctx feeder;
	collected *out;
	dtfl_probe *probe;
} one_build;

static void
build_delay(dtfl_scope *scope, void *arg)
{
	one_build *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);

	s = dtfl_stream_delay(s, delay_to_value, DTFL_NOCTX);
	s = dtfl_stream_probe_with(s, c->probe);
	make_sink(s, c->out);
}

DTFL_TEST(delay__shifts_times_and_frontier)
{
	uint64_t vals[10], times[10];
	collected out;
	one_build c;
	size_t i;
	uint64_t at[10] = { 0 };

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	for (i = 0; i < 10; i++) {
		vals[i] = i;
		times[i] = 0;	/* all fed at time 0 */
	}
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 10;
	c.feeder.type = dtfl_type_u64();
	c.feeder.keys = NULL;
	c.out = &out;
	c.probe = dtfl_probe_new();
	run_standalone(build_delay, &c);

	/* Each value i is now at time i; the probe is empty when done. */
	assert_size(out.n, ==, 10);
	for (i = 0; i < out.n; i++)
		at[out.vals[i]] = out.times[i];
	for (i = 0; i < 10; i++)
		assert_uint64(at[i], ==, i);	/* value i delivered at time i */
	assert_true(dtfl_probe_done(c.probe));	/* output frontier emptied */
	dtfl_probe_free(c.probe);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* aggregate: sum values per key within each time                      */
/* ------------------------------------------------------------------ */

static void
agg_sum(void *ctx, const void *key, size_t klen, const void *val, size_t vlen,
    void *agg)
{
	(void)ctx; (void)key; (void)klen; (void)vlen;
	*(uint64_t *)agg += dtfl_get_u64(val);
}

static void
agg_emit(void *ctx, const void *key, size_t klen, const void *agg,
    dtfl_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)klen;
	dtfl_put_u64(buf, dtfl_get_u64(key));
	dtfl_put_u64(buf + 8, *(const uint64_t *)agg);
	dtfl_emit_record(out, buf, 16);
}

static void
build_aggregate(dtfl_scope *scope, void *arg)
{
	one_build *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());

	s = dtfl_stream_aggregate(s, sizeof(uint64_t), agg_sum, agg_emit, pair,
	    DTFL_NOCTX);
	make_sink2(s, c->out, true);
	dtfl_type_free(pair);
}

DTFL_TEST(aggregate__sums_per_key_per_time)
{
	/* (key % 2, value) for 0..10 at time 0: sums 0+2+..+8=20, 1+..+9=25. */
	uint64_t keys[10], vals[10], times[10];
	collected out;
	one_build c;
	size_t i;
	uint64_t sum[2] = { 0, 0 };

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	for (i = 0; i < 10; i++) {
		keys[i] = i % 2;
		vals[i] = i;
		times[i] = 0;
	}
	c.feeder.keys = keys;
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 10;
	c.feeder.type = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	c.out = &out;
	run_standalone(build_aggregate, &c);
	dtfl_type_free(c.feeder.type);

	assert_size(out.n, ==, 2);	/* one result per key */
	for (i = 0; i < out.n; i++)
		sum[out.keys[i]] = out.vals[i];
	assert_uint64(sum[0], ==, 20);
	assert_uint64(sum[1], ==, 25);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* state_machine: running sum per key, emit (key, running sum)         */
/* ------------------------------------------------------------------ */

static bool
sm_running_sum(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, void *state, dtfl_emit *out)
{
	uint64_t *acc = state;
	unsigned char buf[16];

	(void)ctx; (void)klen; (void)vlen;
	*acc += dtfl_get_u64(val);
	dtfl_put_u64(buf, dtfl_get_u64(key));
	dtfl_put_u64(buf + 8, *acc);
	dtfl_emit_record(out, buf, 16);
	return false;	/* keep the state */
}

static void
build_state_machine(dtfl_scope *scope, void *arg)
{
	one_build *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());

	s = dtfl_stream_state_machine(s, sizeof(uint64_t), sm_running_sum, pair,
	    DTFL_NOCTX);
	make_sink2(s, c->out, true);
	dtfl_type_free(pair);
}

DTFL_TEST(state_machine__threads_state)
{
	/* (key % 2, value) for 0..10, each at its own time so order is fixed. */
	uint64_t keys[10], vals[10], times[10];
	collected out;
	one_build c;
	size_t i;
	uint64_t last[2] = { 0, 0 };

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	for (i = 0; i < 10; i++) {
		keys[i] = i % 2;
		vals[i] = i;
		times[i] = i;	/* distinct times keep the fold ordered */
	}
	c.feeder.keys = keys;
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 10;
	c.feeder.type = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	c.out = &out;
	run_standalone(build_state_machine, &c);
	dtfl_type_free(c.feeder.type);

	/* 10 transitions, one output each; the last per key is the total. */
	assert_size(out.n, ==, 10);
	for (i = 0; i < out.n; i++)
		last[out.keys[i]] = out.vals[i];
	assert_uint64(last[0], ==, 20);	/* 0+2+4+6+8 */
	assert_uint64(last[1], ==, 25);	/* 1+3+5+7+9 */
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* branch: split evens (predicate true) from odds                      */
/* ------------------------------------------------------------------ */

static bool
pred_even(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)ctx; (void)time; (void)len;
	return dtfl_get_u64(rec) % 2 == 0;
}

typedef struct branch_build {
	feeder_ctx feeder;
	collected *yes, *no;
} branch_build;

static void
build_branch(dtfl_scope *scope, void *arg)
{
	branch_build *c = arg;
	dtfl_stream *s = make_feeder(scope, &c->feeder);
	dtfl_stream *yes, *no;

	dtfl_stream_branch(s, pred_even, DTFL_NOCTX, &yes, &no);
	make_sink(yes, c->yes);
	make_sink(no, c->no);
}

DTFL_TEST(branch__splits_by_predicate)
{
	uint64_t vals[10], times[10];
	collected yes, no;
	branch_build c;
	size_t i;

	(void)params; (void)data;
	memset(&yes, 0, sizeof yes);
	memset(&no, 0, sizeof no);
	for (i = 0; i < 10; i++) {
		vals[i] = i;
		times[i] = 0;
	}
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 10;
	c.feeder.type = dtfl_type_u64();
	c.feeder.keys = NULL;
	c.yes = &yes;
	c.no = &no;
	run_standalone(build_branch, &c);

	assert_size(yes.n, ==, 5);	/* evens */
	assert_size(no.n, ==, 5);	/* odds */
	qsort(yes.vals, yes.n, sizeof(uint64_t), cmp_u64);
	qsort(no.vals, no.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 5; i++) {
		assert_uint64(yes.vals[i], ==, 2 * i);
		assert_uint64(no.vals[i], ==, 2 * i + 1);
	}
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* broadcast: every record reaches every worker (2 process workers)    */
/* ------------------------------------------------------------------ */

static dtfl_time_class *g_shared_tc;

typedef struct bcast_ctx {
	collected per_worker[MAX_WORKERS];
} bcast_ctx;

static void
build_broadcast(dtfl_scope *scope, void *arg)
{
	bcast_ctx *e = arg;
	size_t idx = dtfl_scope_index(scope);
	/* Only worker 0 feeds; after broadcast every worker sees the data. */
	static const uint64_t vals[] = { 10, 20, 30, 40 };
	static const uint64_t times[] = { 0, 0, 0, 0 };
	static feeder_ctx feeders[MAX_WORKERS];
	dtfl_stream *src;

	feeders[idx].vals = vals;
	feeders[idx].times = times;
	feeders[idx].n = (idx == 0) ? 4 : 0;
	feeders[idx].type = dtfl_type_u64();
	feeders[idx].keys = NULL;
	src = make_feeder(scope, &feeders[idx]);
	make_sink(dtfl_stream_broadcast(src), &e->per_worker[idx]);
}

static void
bcast_worker(dtfl_worker *w, void *arg)
{
	bcast_ctx *e = arg;

	assert_int(dtfl_worker_dataflow(w, "bcast", g_shared_tc, build_broadcast,
	    e), ==, 0);
}

DTFL_TEST(broadcast__replicates_to_all_peers)
{
	dtfl_config *cfg = dtfl_config_new();
	bcast_ctx e;
	size_t peers = 2, w, i;

	(void)params; (void)data;
	memset(&e, 0, sizeof e);
	g_shared_tc = dtfl_time_u64();
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, peers), ==, 0);
	assert_int(dtfl_execute(cfg, bcast_worker, &e, NULL), ==, 0);

	/* Worker 0 fed 4 records; every worker receives all 4. */
	for (w = 0; w < peers; w++) {
		uint64_t seen[4] = { 0, 0, 0, 0 };

		assert_size(e.per_worker[w].n, ==, 4);
		qsort(e.per_worker[w].vals, e.per_worker[w].n, sizeof(uint64_t),
		    cmp_u64);
		for (i = 0; i < 4; i++)
			seen[i] = e.per_worker[w].vals[i];
		assert_uint64(seen[0], ==, 10);
		assert_uint64(seen[1], ==, 20);
		assert_uint64(seen[2], ==, 30);
		assert_uint64(seen[3], ==, 40);
	}
	dtfl_time_class_free(g_shared_tc);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(delay__shifts_times_and_frontier),
	DTFL_T(aggregate__sums_per_key_per_time),
	DTFL_T(state_machine__threads_state),
	DTFL_T(branch__splits_by_predicate),
	DTFL_T(broadcast__replicates_to_all_peers),
	DTFL_T_END
};

DTFL_TEST_MAIN("ops_notify", tests)
