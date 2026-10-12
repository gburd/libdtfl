/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_collection.c: the differential programming model.
 *
 * A single worker builds a collection from an input session, applies the
 * linear operators (map, filter, negate, concat) and consolidate, and an
 * inspect sink records the consolidated (record, time, diff) multiset.
 * The session inserts and retracts records across several epochs; after
 * stepping to termination the output must equal the net multiset, with
 * cancelling updates gone and no zero diffs.  A final Process run of
 * three workers inserts and retracts the same keys and converges to the
 * same net multiset, proving the model end to end across workers.
 */
#include "dtfl_test.h"

#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 4096

/* One consolidated update the inspect sink recorded. */
typedef struct out_upd {
	uint64_t rec;
	uint64_t time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAX_OUT];
	size_t n;
} collected;

static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;

	(void)len;
	if (c->n >= MAX_OUT)
		return;
	c->u[c->n].rec = dtfl_get_u64(rec);
	c->u[c->n].time = time[0];
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* diff for key `rec` at `time` in the collected output, or 0. */
static int64_t
out_diff(const collected *c, uint64_t rec, uint64_t time)
{
	size_t i;

	for (i = 0; i < c->n; i++)
		if (c->u[i].rec == rec && c->u[i].time == time)
			return c->u[i].diff;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Operator callbacks                                                  */
/* ------------------------------------------------------------------ */

/* map: double the key. */
static void
map_double(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	dtfl_put_u64(buf, dtfl_get_u64(rec) * 2);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* filter: keep keys below 1000. */
static bool
filter_small(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)len;
	return dtfl_get_u64(rec) < 1000;
}

/* ------------------------------------------------------------------ */
/* Single worker: insert/retract, map/filter/negate/concat/consolidate */
/* ------------------------------------------------------------------ */

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
} build_ctx;

/*
 * The dataflow: start from the input collection, map (double), filter,
 * concat with a negated copy of itself, consolidate, inspect.  The
 * concat with the negation cancels everything that both arms carry, so
 * the net result is empty; this exercises negate+concat+consolidate.
 * To keep a non-empty expected output, the negated arm is filtered to
 * drop one key, so exactly that key survives.
 */
static void
build_dataflow(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *doubled = dtfl_collection_map(base, dtfl_type_u64(),
	    map_double, DTFL_NOCTX);
	dtfl_collection *kept = dtfl_collection_filter(doubled, filter_small,
	    DTFL_NOCTX);
	dtfl_collection *neg = dtfl_collection_negate(kept);
	dtfl_collection *merged = dtfl_collection_concat(kept, neg);
	dtfl_collection *con = dtfl_collection_consolidate(merged);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(merged);
	dtfl_collection_free(neg);
	dtfl_collection_free(kept);
	dtfl_collection_free(doubled);
	dtfl_collection_free(base);
}

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

DTFL_TEST(collection__cancellation_consolidates_to_empty)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out;
	uint64_t round;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_dataflow, &c), ==, 0);

	/* Insert keys 0..4 at epoch 0, retract 0..3 at epoch 1. */
	for (round = 0; round < 2; round++) {
		unsigned char key[8];
		uint64_t next = round + 1;
		int k, upto = round == 0 ? 5 : 4;

		for (k = 0; k < upto; k++) {
			dtfl_put_u64(key, (uint64_t)k);
			if (round == 0)
				dtfl_input_session_insert(c.in, key, sizeof key);
			else
				dtfl_input_session_remove(c.in, key, sizeof key);
		}
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		{
			probe_cond pc = { c.probe, next };

			dtfl_worker_step_while(w, probe_behind, &pc);
		}
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	assert_true(dtfl_probe_done(c.probe));

	/*
	 * kept concat negate(kept) is identically empty at every time, so
	 * the consolidated output is empty for every (record, time).
	 */
	assert_size(out.n, ==, 0);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Single worker: a plain insert/retract that leaves a net multiset    */
/* ------------------------------------------------------------------ */

static void
build_plain(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *con = dtfl_collection_consolidate(base);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(base);
}

DTFL_TEST(collection__net_multiset_after_updates)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out;
	unsigned char key[8];
	uint64_t one = 1;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "plain", tc, build_plain, &c), ==, 0);

	/*
	 * At epoch 0: insert key 7 three times, key 8 once, retract key 7
	 * once.  Net at time 0: key 7 x2, key 8 x1.  Then advance and
	 * retract key 8 at epoch 1: net at time 1: key 8 x-1.
	 */
	dtfl_put_u64(key, 7);
	dtfl_input_session_insert(c.in, key, sizeof key);
	dtfl_input_session_insert(c.in, key, sizeof key);
	dtfl_input_session_insert(c.in, key, sizeof key);
	dtfl_input_session_remove(c.in, key, sizeof key);
	dtfl_put_u64(key, 8);
	dtfl_input_session_insert(c.in, key, sizeof key);
	dtfl_input_session_advance_to(c.in, &one);
	dtfl_input_session_flush(c.in);
	{
		probe_cond pc = { c.probe, one };

		dtfl_worker_step_while(w, probe_behind, &pc);
	}
	dtfl_put_u64(key, 8);
	dtfl_input_session_remove(c.in, key, sizeof key);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	assert_true(dtfl_probe_done(c.probe));

	/* Expected consolidated multiset. */
	assert_int64(out_diff(&out, 7, 0), ==, 2);
	assert_int64(out_diff(&out, 8, 0), ==, 1);
	assert_int64(out_diff(&out, 8, 1), ==, -1);
	/* key 7 did not change at time 1, so it is absent there. */
	assert_int64(out_diff(&out, 7, 1), ==, 0);
	/* No zero diffs are emitted. */
	{
		size_t i;

		for (i = 0; i < out.n; i++)
			assert_int64(out.u[i].diff, !=, 0);
	}

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Multi worker: Process(3) insert/retract converges to a net multiset */
/* ------------------------------------------------------------------ */

static dtfl_time_class *g_mw_tc;

typedef struct mw_env {
	collected out[8];	/* consolidated updates per worker */
} mw_env;

typedef struct mw_build {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
} mw_build;

static void
mw_build_fn(dtfl_scope *scope, void *arg)
{
	mw_build *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *con = dtfl_collection_consolidate(base);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(base);
}

static void
mw_worker(dtfl_worker *w, void *arg)
{
	mw_env *e = arg;
	size_t idx = dtfl_worker_index(w);
	dtfl_diff *r = dtfl_diff_i64();
	mw_build c;
	uint64_t round;

	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &e->out[idx];
	assert_int(dtfl_worker_dataflow(w, "mw", g_mw_tc, mw_build_fn, &c), ==, 0);

	/*
	 * Every worker inserts the same ten keys at epoch 0 (net +3 each
	 * after exchange collects all workers' contributions), then at
	 * epoch 1 every worker retracts the same keys once (net -3 each),
	 * so each key cancels to zero across the two epochs when summed.
	 * Consolidate exchanges by key, so one worker owns each key's
	 * updates and must produce +3 at time 0 and -3 at time 1.
	 */
	for (round = 0; round < 2; round++) {
		uint64_t next = round + 1;
		int k;

		for (k = 0; k < 10; k++) {
			unsigned char key[8];

			dtfl_put_u64(key, (uint64_t)k);
			if (round == 0)
				dtfl_input_session_insert(c.in, key, sizeof key);
			else
				dtfl_input_session_remove(c.in, key, sizeof key);
		}
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		{
			probe_cond pc = { c.probe, next };

			dtfl_worker_step_while(w, probe_behind, &pc);
		}
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	assert_true(dtfl_probe_done(c.probe));
	dtfl_probe_free(c.probe);
	dtfl_diff_free(r);
}

DTFL_TEST(collection__multi_worker_insert_retract_converges)
{
	dtfl_config *cfg = dtfl_config_new();
	mw_env e;
	size_t i, k;
	int64_t net_t0[10], net_t1[10];

	(void)params; (void)data;
	memset(&e, 0, sizeof e);
	memset(net_t0, 0, sizeof net_t0);
	memset(net_t1, 0, sizeof net_t1);
	g_mw_tc = dtfl_time_u64();
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, 3), ==, 0);
	assert_int(dtfl_execute(cfg, mw_worker, &e, NULL), ==, 0);

	/*
	 * Sum the consolidated output of every worker.  Each key is owned
	 * by exactly one worker after the exchange, so the sums below are
	 * the whole computation's output.  Three workers each inserted one
	 * copy at time 0 and retracted one copy at time 1.
	 */
	for (i = 0; i < 3; i++)
		for (k = 0; k < 10; k++) {
			net_t0[k] += out_diff(&e.out[i], (uint64_t)k, 0);
			net_t1[k] += out_diff(&e.out[i], (uint64_t)k, 1);
		}
	for (k = 0; k < 10; k++) {
		assert_int64(net_t0[k], ==, 3);
		assert_int64(net_t1[k], ==, -3);
	}
	/* No worker emitted a zero diff. */
	for (i = 0; i < 3; i++) {
		size_t j;

		for (j = 0; j < e.out[i].n; j++)
			assert_int64(e.out[i].u[j].diff, !=, 0);
	}

	dtfl_time_class_free(g_mw_tc);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(collection__cancellation_consolidates_to_empty),
	DTFL_T(collection__net_multiset_after_updates),
	DTFL_T(collection__multi_worker_insert_retract_converges),
	DTFL_T_END
};

DTFL_TEST_MAIN("collection", tests)
