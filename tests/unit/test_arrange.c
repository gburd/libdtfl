/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_arrange.c: the arrange operator and the Arranged type.
 *
 * A worker builds a collection from an input session, arranges it into a
 * shared trace, reads it back with as_collection, and an inspect sink
 * records the (record, time, diff) multiset.  The result must equal the
 * input consolidated: one net update per (record, time), zeros dropped.
 * Further tests check a multi-epoch insert then retract, enter/leave into a
 * region, and a shared import where two downstream operators read one trace.
 */
#include "dtfl_test.h"

#include "dd/arrange_int.h"
#include "dd/int_proxy_int.h"

#include "dtfl/arrange.h"
#include "dtfl/config.h"
#include "dtfl/iterate.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <string.h>

#define MAX_OUT 4096

typedef struct out_upd {
	uint64_t rec, time;
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

static int64_t
out_diff(const collected *c, uint64_t rec, uint64_t time)
{
	size_t i;

	for (i = 0; i < c->n; i++)
		if (c->u[i].rec == rec && c->u[i].time == time)
			return c->u[i].diff;
	return 0;
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
	collected *out2;	/* second import consumer */
} build_ctx;

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

/* ------------------------------------------------------------------ */
/* arrange then as_collection equals consolidate.                      */
/* ------------------------------------------------------------------ */

static void
build_arrange(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_arranged *arr = dtfl_collection_arrange(base, "Arrange");
	dtfl_collection *coll = dtfl_arranged_as_collection(arr,
	    dtfl_type_u64(), NULL, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(coll, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(coll);
	dtfl_arranged_free(arr);
	dtfl_collection_free(base);
}

DTFL_TEST(arrange__as_collection_equals_consolidate)
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
	c.out2 = NULL;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_arrange, &c), ==, 0);

	/*
	 * Epoch 0: insert keys 0..4, and insert key 2 a second time so its
	 * net multiplicity is +2.  Epoch 1: retract key 0.  Consolidated,
	 * the output is +1 for keys 1,3,4 and +2 for key 2 at time 0, and
	 * -1 for key 0 at time 1.
	 */
	{
		unsigned char key[8];
		uint64_t next = 1;
		int k;

		for (k = 0; k < 5; k++) {
			dtfl_put_u64(key, (uint64_t)k);
			dtfl_input_session_insert(c.in, key, sizeof key);
		}
		dtfl_put_u64(key, 2);
		dtfl_input_session_insert(c.in, key, sizeof key);
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		{
			probe_cond pc = { c.probe, next };

			dtfl_worker_step_while(w, probe_behind, &pc);
		}
	}
	{
		unsigned char key[8];
		uint64_t next = 2;

		dtfl_put_u64(key, 0);
		dtfl_input_session_remove(c.in, key, sizeof key);
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		{
			probe_cond pc = { c.probe, next };

			dtfl_worker_step_while(w, probe_behind, &pc);
		}
	}
	(void)round;
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	assert_true(dtfl_probe_done(c.probe));

	/* The consolidated multiset. */
	assert_int64(out_diff(&out, 0, 0), ==, 1);
	assert_int64(out_diff(&out, 1, 0), ==, 1);
	assert_int64(out_diff(&out, 2, 0), ==, 2);
	assert_int64(out_diff(&out, 3, 0), ==, 1);
	assert_int64(out_diff(&out, 4, 0), ==, 1);
	assert_int64(out_diff(&out, 0, 1), ==, -1);
	/* No other (record, time) carries a diff, and none is zero. */
	{
		size_t i;

		for (i = 0; i < out.n; i++)
			assert_int64(out.u[i].diff, !=, 0);
	}
	assert_size(out.n, ==, 6);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Multi-epoch: insert at t0, retract at t1; net is +1 at 0, -1 at 1.  */
/* ------------------------------------------------------------------ */

DTFL_TEST(arrange__multi_epoch_insert_retract)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.out2 = NULL;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_arrange, &c), ==, 0);

	/* Insert key 7 at t0, retract it at t1. */
	{
		unsigned char key[8];
		uint64_t next = 1;

		dtfl_put_u64(key, 7);
		dtfl_input_session_insert(c.in, key, sizeof key);
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		{ probe_cond pc = { c.probe, next }; dtfl_worker_step_while(w, probe_behind, &pc); }
	}
	{
		unsigned char key[8];
		uint64_t next = 2;

		dtfl_put_u64(key, 7);
		dtfl_input_session_remove(c.in, key, sizeof key);
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		{ probe_cond pc = { c.probe, next }; dtfl_worker_step_while(w, probe_behind, &pc); }
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* The trace, read as a collection, holds +1 at t0 and -1 at t1. */
	assert_int64(out_diff(&out, 7, 0), ==, 1);
	assert_int64(out_diff(&out, 7, 1), ==, -1);
	assert_size(out.n, ==, 2);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* enter / leave a region.                                             */
/* ------------------------------------------------------------------ */

/* The region body: enter the arrangement, read it back, inspect. */
static dtfl_arranged *region_arr_in;
static collected *region_out;
static dtfl_probe *region_probe;

static void
region_body(dtfl_scope *region, void *arg)
{
	dtfl_arranged *inner;
	dtfl_collection *coll;
	dtfl_collection *ins;
	dtfl_collection *probed;

	(void)arg;
	inner = dtfl_arranged_enter_region(region_arr_in, region);
	coll = dtfl_arranged_as_collection(inner, dtfl_type_u64(), NULL,
	    DTFL_NOCTX);
	ins = dtfl_collection_inspect(coll, inspect_sink,
	    DTFL_CTX(region_out, NULL));
	probed = dtfl_collection_probe_with(ins, region_probe);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(coll);
	dtfl_arranged_free(inner);
}

static void
build_enter_region(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_arranged *arr = dtfl_collection_arrange(base, "Arrange");

	region_arr_in = arr;
	region_out = c->out;
	region_probe = c->probe;
	assert_int(dtfl_scope_region(scope, "Region", region_body, NULL), ==,
	    0);
	dtfl_arranged_free(arr);
	dtfl_collection_free(base);
}

DTFL_TEST(arrange__enter_region)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.out2 = NULL;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_enter_region, &c),
	    ==, 0);

	{
		unsigned char key[8];
		uint64_t next = 1;
		int k;

		for (k = 0; k < 3; k++) {
			dtfl_put_u64(key, (uint64_t)k);
			dtfl_input_session_insert(c.in, key, sizeof key);
		}
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		{ probe_cond pc = { c.probe, next }; dtfl_worker_step_while(w, probe_behind, &pc); }
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* The region read the same arrangement: keys 0,1,2 at time 0, +1. */
	assert_int64(out_diff(&out, 0, 0), ==, 1);
	assert_int64(out_diff(&out, 1, 0), ==, 1);
	assert_int64(out_diff(&out, 2, 0), ==, 1);
	assert_size(out.n, ==, 3);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Shared import: two downstream operators read one trace.             */
/* ------------------------------------------------------------------ */

static void
build_import(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_arranged *arr = dtfl_collection_arrange(base, "Arrange");
	dtfl_trace *trace = dtfl_arranged_trace(arr);
	/* Two importers of the same shared trace. */
	dtfl_arranged *imp1 = dtfl_trace_import(trace, scope, "Import1");
	dtfl_arranged *imp2 = dtfl_trace_import(trace, scope, "Import2");
	dtfl_collection *coll1 = dtfl_arranged_as_collection(imp1,
	    dtfl_type_u64(), NULL, DTFL_NOCTX);
	dtfl_collection *coll2 = dtfl_arranged_as_collection(imp2,
	    dtfl_type_u64(), NULL, DTFL_NOCTX);
	dtfl_collection *ins1 = dtfl_collection_inspect(coll1, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *ins2 = dtfl_collection_inspect(coll2, inspect_sink,
	    DTFL_CTX(c->out2, NULL));
	dtfl_collection *p1 = dtfl_collection_probe_with(ins1, c->probe);
	dtfl_collection *p2 = dtfl_collection_probe_with(ins2, c->probe);

	dtfl_collection_free(p1);
	dtfl_collection_free(p2);
	dtfl_collection_free(ins1);
	dtfl_collection_free(ins2);
	dtfl_collection_free(coll1);
	dtfl_collection_free(coll2);
	dtfl_arranged_free(imp1);
	dtfl_arranged_free(imp2);
	dtfl_trace_free(trace);
	dtfl_arranged_free(arr);
	dtfl_collection_free(base);
}

DTFL_TEST(arrange__shared_import_two_readers)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out1, out2;

	(void)params; (void)data;
	memset(&out1, 0, sizeof out1);
	memset(&out2, 0, sizeof out2);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out1;
	c.out2 = &out2;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_import, &c), ==, 0);

	{
		unsigned char key[8];
		uint64_t next = 1;
		int k;

		for (k = 0; k < 4; k++) {
			dtfl_put_u64(key, (uint64_t)(k * 10));
			dtfl_input_session_insert(c.in, key, sizeof key);
		}
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		{ probe_cond pc = { c.probe, next }; dtfl_worker_step_while(w, probe_behind, &pc); }
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* Both importers saw the same data from the one shared trace. */
	{
		uint64_t keys[] = { 0, 10, 20, 30 };
		size_t i;

		for (i = 0; i < 4; i++) {
			assert_int64(out_diff(&out1, keys[i], 0), ==, 1);
			assert_int64(out_diff(&out2, keys[i], 0), ==, 1);
		}
	}
	assert_size(out1.n, ==, 4);
	assert_size(out2.n, ==, 4);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Compaction holds on a public trace handle.                          */
/* ------------------------------------------------------------------ */

static dtfl_trace *compact_trace;

static void
build_compact(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_arranged *arr = dtfl_collection_arrange(base, "Arrange");
	dtfl_collection *coll = dtfl_arranged_as_collection(arr,
	    dtfl_type_u64(), NULL, DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(coll, c->probe);

	compact_trace = dtfl_arranged_trace(arr);
	dtfl_collection_free(probed);
	dtfl_collection_free(coll);
	dtfl_arranged_free(arr);
	dtfl_collection_free(base);
}

typedef struct trace_rows {
	uint64_t key[64], val[64], time[64];
	int64_t diff[64];
	size_t n;
} trace_rows;

static void
trace_row(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, const uint64_t *time, const void *diff)
{
	trace_rows *r = ctx;

	(void)klen;
	if (r->n >= 64)
		return;
	r->key[r->n] = dtfl_get_u64(key);
	r->val[r->n] = vlen == 8 ? dtfl_get_u64(val) : 0;
	r->time[r->n] = time[0];
	memcpy(&r->diff[r->n], diff, sizeof(int64_t));
	r->n++;
}

/* Net diff of `key` at times <= `t` in a trace walk. */
static int64_t
rows_accum(const trace_rows *r, uint64_t key, uint64_t t)
{
	int64_t sum = 0;
	size_t i;

	for (i = 0; i < r->n; i++)
		if (r->key[i] == key && r->time[i] <= t)
			sum += r->diff[i];
	return sum;
}

/*
 * Updates `key` by `diff` at the session's current time, then seals that
 * time by advancing to `next` and stepping until the probe passes it.
 */
static void
feed(dtfl_worker *w, build_ctx *c, uint64_t key, int64_t diff, uint64_t next)
{
	unsigned char k[8];
	probe_cond pc = { c->probe, next };

	dtfl_put_u64(k, key);
	if (diff > 0)
		dtfl_input_session_insert(c->in, k, sizeof k);
	else
		dtfl_input_session_remove(c->in, k, sizeof k);
	dtfl_input_session_advance_to(c->in, &next);
	dtfl_input_session_flush(c->in);
	dtfl_worker_step_while(w, probe_behind, &pc);
}

DTFL_TEST(arrange__trace_compaction_holds)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	trace_rows before, after;
	const uint64_t *hold;
	uint64_t f, early = 1;
	size_t n, i;
	unsigned round;

	(void)params; (void)data;
	memset(&before, 0, sizeof before);
	memset(&after, 0, sizeof after);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = NULL;
	c.out2 = NULL;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_compact, &c), ==, 0);
	assert_not_null(compact_trace);

	/* A new handle holds both frontiers at the minimum time. */
	hold = dtfl_trace_logical_compaction(compact_trace, &n);
	assert_size(n, ==, 1);
	assert_uint64(hold[0], ==, 0);
	hold = dtfl_trace_physical_compaction(compact_trace, &n);
	assert_size(n, ==, 1);
	assert_uint64(hold[0], ==, 0);

	/* 7: +1 at t0, -1 at t1.  9: +1 at t2, +1 at t3. */
	feed(w, &c, 7, 1, 1);
	feed(w, &c, 7, -1, 2);
	feed(w, &c, 9, 1, 3);
	feed(w, &c, 9, 1, 4);

	/* Held at 0, the walk reports every update at its original time. */
	dtfl_trace_to_vec(compact_trace, trace_row, DTFL_CTX(&before, NULL));
	assert_size(before.n, ==, 4);
	assert_int64(rows_accum(&before, 7, 0), ==, 1);
	assert_int64(rows_accum(&before, 7, 1), ==, 0);
	assert_int64(rows_accum(&before, 9, 2), ==, 1);
	assert_int64(rows_accum(&before, 9, 3), ==, 2);

	/* Advance both holds to 4; the getters report the new hold. */
	f = 4;
	dtfl_trace_set_logical_compaction(compact_trace, &f, 1);
	dtfl_trace_set_physical_compaction(compact_trace, &f, 1);
	hold = dtfl_trace_logical_compaction(compact_trace, &n);
	assert_size(n, ==, 1);
	assert_uint64(hold[0], ==, 4);

	/* A frontier behind the hold is joined in, so the hold stays at 4. */
	dtfl_trace_set_logical_compaction(compact_trace, &early, 1);
	hold = dtfl_trace_logical_compaction(compact_trace, &n);
	assert_uint64(hold[0], ==, 4);

	/*
	 * Give the spine work to do so it merges: more epochs, each a no-op
	 * pair on an unrelated key, then close.  Every reader is past 4.
	 */
	for (round = 0; round < 8; round++) {
		feed(w, &c, 100 + round, 1, 5 + 2 * round);
		feed(w, &c, 100 + round, -1, 6 + 2 * round);
	}
	f = 20;
	dtfl_trace_set_logical_compaction(compact_trace, &f, 1);
	dtfl_trace_set_physical_compaction(compact_trace, &f, 1);
	/*
	 * Observe compaction while the dataflow is live: the input stays open
	 * and the arrange operator still runs, so no operator-held reader may
	 * pin the trace behind the handle's holds.  Two more epochs give the
	 * spine fuel to finish its merges.
	 */
	feed(w, &c, 200, 1, 21);
	feed(w, &c, 200, -1, 22);

	dtfl_trace_to_vec(compact_trace, trace_row, DTFL_CTX(&after, NULL));
	/* Accumulations at times beyond the frontier are unchanged. */
	assert_int64(rows_accum(&after, 7, UINT64_MAX), ==, 0);
	assert_int64(rows_accum(&after, 9, UINT64_MAX), ==, 2);
	for (round = 0; round < 8; round++)
		assert_int64(rows_accum(&after, 100 + round, UINT64_MAX), ==, 0);
	/*
	 * Compaction happened in the merged history: key 9's two updates
	 * (t2, t3) are one update advanced to the frontier, and key 7 and
	 * the early no-op keys cancelled out and are gone.  The spine merges
	 * with amortized effort, so the newest batches (keys 107 and 200)
	 * need not have merged yet; nothing older may survive unadvanced.
	 */
	for (i = 0; i < after.n; i++) {
		if (after.key[i] == 9) {
			assert_uint64(after.time[i], ==, 20);
			assert_int64(after.diff[i], ==, 2);
		} else {
			assert_true(after.key[i] == 107 || after.key[i] == 200);
		}
	}
	assert_size(after.n, <, before.n + 2 * 8);
	assert_int64(rows_accum(&after, 200, UINT64_MAX), ==, 0);

	/*
	 * A cut behind the physical hold has no clean cut: the batches below
	 * it may have merged (spine_fueled.rs batches_through asserts
	 * physical_frontier <= upper).  A cut at a batch boundary at or
	 * beyond the hold is clean.
	 */
	{
		trace_rows scratch;
		uint64_t behind = 5, at = 22;

		memset(&scratch, 0, sizeof scratch);
		assert_false(dtfl_trace_to_vec_through(compact_trace, &behind,
		    1, trace_row, DTFL_CTX(&scratch, NULL)));
		assert_size(scratch.n, ==, 0);
		assert_true(dtfl_trace_to_vec_through(compact_trace, &at, 1,
		    trace_row, DTFL_CTX(&scratch, NULL)));
	}

	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	dtfl_trace_free(compact_trace);
	compact_trace = NULL;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/*
 * __dtfl_caps_insert keeps the held capabilities an antichain, as timely's
 * Antichain<Capability<T>>::insert does: a capability above a held one is
 * dropped, and a capability below held ones replaces them.  The arrange,
 * reduce and upsert batch tilings depend on it; a set holding (0,4,2) and
 * (2,4,2) together tiles a degenerate [lower, lower) batch.
 */
typedef struct caps_build {
	dtfl_unordered_input *in;
	dtfl_cap *cap;
} caps_build;

static void
build_caps(dtfl_scope *scope, void *arg)
{
	caps_build *b = arg;

	(void)dtfl_unordered_input_new(scope, dtfl_type_u64(), &b->in,
	    &b->cap);
}

DTFL_TEST(arrange__caps_insert_antichain)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *u = dtfl_time_u64();
	dtfl_time_class *p2 = dtfl_time_product(u, u);
	dtfl_time_class *tc = dtfl_time_product(p2, u);
	const uint64_t lo[3] = { 0, 4, 2 }, hi[3] = { 2, 4, 2 };
	const uint64_t side[3] = { 3, 0, 0 };
	caps_build b;
	__dtfl_vec caps;
	size_t i;

	(void)params; (void)data;
	munit_assert_int(dtfl_worker_dataflow(w, "caps", tc, build_caps, &b),
	    ==, 0);
	__dtfl_vec_init(&caps, sizeof(dtfl_cap *));

	/* (2,4,2) then (0,4,2): the lower one replaces the higher. */
	__dtfl_caps_insert(&caps, tc, dtfl_cap_delayed(b.cap, hi));
	__dtfl_caps_insert(&caps, tc, dtfl_cap_delayed(b.cap, lo));
	munit_assert_size(caps.len, ==, 1);
	munit_assert_memory_equal(sizeof lo,
	    dtfl_cap_time(*DTFL_VEC_AT(&caps, dtfl_cap *, 0)), lo);

	/* (2,4,2) again: redundant, dropped. */
	__dtfl_caps_insert(&caps, tc, dtfl_cap_delayed(b.cap, hi));
	munit_assert_size(caps.len, ==, 1);

	/* (3,0,0) is incomparable to (0,4,2): both held. */
	__dtfl_caps_insert(&caps, tc, dtfl_cap_delayed(b.cap, side));
	munit_assert_size(caps.len, ==, 2);

	for (i = 0; i < caps.len; i++)
		dtfl_cap_drop(*DTFL_VEC_AT(&caps, dtfl_cap *, i));
	__dtfl_vec_free(&caps);
	dtfl_cap_drop(b.cap);
	dtfl_unordered_input_free(b.in);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
	dtfl_time_class_free(p2);
	dtfl_time_class_free(u);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/*
 * Config::idle_merge_effort (lib.rs): a trace left idle keeps merging
 * toward one batch only when the effort is set.  Epochs 0..3 load 64, 16,
 * 4 and 1 new keys, which leaves four Single layers in each spine; then
 * the input stays open at time 4 and the worker steps with nothing to do.
 * With effort, each operator's own activator reschedules it and each
 * activation's writer.exert() merges, until every trace is one batch and
 * the default logic answers None.  Without it, nothing is scheduled and
 * the batches stay.  The shapes cover each operator that installs the
 * logic: arrange (arrangement.rs), arrange_by_key and reduce (reduce.rs
 * output trace), and arrange_from_upsert (upsert.rs).
 */
static const size_t idle_sizes[] = { 64, 16, 4, 1 };

enum idle_shape { IDLE_ARRANGE, IDLE_REDUCE, IDLE_UPSERT };

typedef struct idle_ctx {
	enum idle_shape shape;
	bool pin;			/* hold a reader at physical 0 while loading */
	dtfl_trace *reader;		/* the pinned reader (pin only) */
	dtfl_trace *watch;		/* pin: an unpinned handle for stats */
	dtfl_trace_stats pinned_st;	/* pin: while the reader holds */
	dtfl_trace_stats final_st;	/* pin: after the idle merges */
	dtfl_input_session *in;
	dtfl_probe *probe;
	__dtfl_trace_box *box[2];	/* retained: traces outlive Arranged */
	size_t nbox;
	size_t loaded[2], idle[2];	/* batch counts after loading, idling */
	size_t steps;			/* idle steps taken */
} idle_ctx;

static void
idle_keep(idle_ctx *c, dtfl_arranged *a)
{
	c->box[c->nbox++] = __dtfl_trace_box_retain(
	    __dtfl_arranged_agent(a)->box);
}

static void
idle_count(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, dtfl_reduce_emit *out)
{
	int64_t one = 1;
	unsigned char b[8];

	(void)ctx; (void)key; (void)klen; (void)in_vals; (void)in_vlens;
	(void)in_diffs; (void)m; (void)out_vals; (void)out_vlens;
	(void)out_diffs;
	dtfl_put_u64(b, n);
	dtfl_reduce_emit_pair(out, b, 8, &one);
}

static void
build_idle(dtfl_scope *scope, void *arg)
{
	idle_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_arranged *arr, *red = NULL;

	/* No reader holds compaction back, so the spines may merge freely. */
	switch (c->shape) {
	case IDLE_ARRANGE:
		arr = dtfl_collection_arrange(base, "Arrange");
		break;
	case IDLE_REDUCE:
		arr = dtfl_collection_arrange_by_key(base, u64, u64, "ByKey");
		red = dtfl_arranged_reduce_abelian(arr, "Count", u64,
		    dtfl_arranged_diff(arr), idle_count, DTFL_NOCTX);
		break;
	default:
		arr = dtfl_arrange_from_upsert(base, u64, u64, "Upsert");
		break;
	}
	idle_keep(c, arr);
	if (c->pin) {
		uint64_t zero = 0;

		c->reader = dtfl_arranged_trace(arr);
		dtfl_trace_set_physical_compaction(c->reader, &zero, 1);
		/*
		 * A second reader for the stats.  A reader that never advances
		 * pins the trace, so this one releases both holds at once (the
		 * empty frontier), as a monitoring handle should.
		 */
		c->watch = dtfl_arranged_trace(arr);
		dtfl_trace_set_logical_compaction(c->watch, NULL, 0);
		dtfl_trace_set_physical_compaction(c->watch, NULL, 0);
	}
	if (red != NULL) {
		idle_keep(c, red);
		dtfl_stream_probe_with(dtfl_arranged_stream(red), c->probe);
		dtfl_arranged_free(red);
	} else {
		dtfl_stream_probe_with(dtfl_arranged_stream(arr), c->probe);
	}
	dtfl_arranged_free(arr);
	dtfl_collection_free(base);
}

static size_t
idle_most(const idle_ctx *c)
{
	size_t i, most = 0;

	for (i = 0; i < c->nbox; i++)
		most = DTFL_MAX(most, __dtfl_trace_batch_count(c->box[i]->trace));
	return most;
}

static void
idle_worker(dtfl_worker *w, void *arg)
{
	idle_ctx *c = arg;
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t e, key = 0;
	size_t i;

	munit_assert_int(dtfl_worker_dataflow(w, "idle", tc, build_idle, c),
	    ==, 0);
	for (e = 0; e < 4; e++) {
		uint64_t next = e + 1;
		probe_cond pc = { c->probe, next };

		for (i = 0; i < idle_sizes[e]; i++) {
			unsigned char r[17];
			size_t len = 16;

			dtfl_put_u64(r, key);
			if (c->shape == IDLE_UPSERT) {
				r[8] = 1;	/* Some(key) */
				dtfl_put_u64(r + 9, key);
				len = 17;
			} else {
				dtfl_put_u64(r + 8, key);
				if (c->shape == IDLE_ARRANGE)
					len = 8;
			}
			key++;
			dtfl_input_session_insert(c->in, r, len);
		}
		dtfl_input_session_advance_to(c->in, &next);
		dtfl_input_session_flush(c->in);
		dtfl_worker_step_while(w, probe_behind, &pc);
	}
	if (c->pin) {
		/*
		 * The pinned reader kept the batches pending; let the worker go
		 * quiet, then release them.  Only consider_merges' activation
		 * can now reschedule the operator: no input arrives.
		 */
		for (i = 0; i < 50; i++)
			dtfl_worker_step(w);
		c->pinned_st.size = sizeof c->pinned_st;
		munit_assert_int(dtfl_trace_stats_get(c->watch, &c->pinned_st),
		    ==, DTFL_OK);
		dtfl_trace_free(c->reader);
		c->reader = NULL;
	}
	for (i = 0; i < c->nbox; i++)
		c->loaded[i] = __dtfl_trace_batch_count(c->box[i]->trace);
	for (c->steps = 0; c->steps < 200 && idle_most(c) > 1; c->steps++)
		dtfl_worker_step(w);
	for (i = 0; i < c->nbox; i++)
		c->idle[i] = __dtfl_trace_batch_count(c->box[i]->trace);
	if (c->watch != NULL) {
		c->final_st.size = sizeof c->final_st;
		munit_assert_int(dtfl_trace_stats_get(c->watch, &c->final_st),
		    ==, DTFL_OK);
		dtfl_trace_free(c->watch);
		c->watch = NULL;
	}
	dtfl_input_session_close(c->in);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
}

static void run_idle_ctx(enum idle_shape shape, int64_t effort, idle_ctx *c);

static void
run_idle(enum idle_shape shape, int64_t effort, idle_ctx *c)
{
	memset(c, 0, sizeof *c);
	run_idle_ctx(shape, effort, c);
}

/* run_idle on a context the caller prepared (c->pin). */
static void
run_idle_ctx(enum idle_shape shape, int64_t effort, idle_ctx *c)
{
	dtfl_config *cfg = dtfl_config_new();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *rt = NULL, *fields[2];
	size_t i;

	munit_assert_int(dtfl_config_set_idle_merge_effort(cfg, effort), ==, 0);
	munit_assert_int64(dtfl_config_idle_merge_effort(cfg), ==, effort);
	c->shape = shape;
	fields[0] = dtfl_type_u64();
	switch (shape) {
	case IDLE_ARRANGE:
		c->in = dtfl_input_session_new(dtfl_type_u64(), r);
		break;
	case IDLE_REDUCE:
		rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
		c->in = dtfl_input_session_new(rt, r);
		break;
	default:
		fields[1] = dtfl_type_option(dtfl_type_u64());
		rt = dtfl_type_tuple(fields, 2);
		c->in = dtfl_input_session_new(rt, r);
		break;
	}
	c->probe = dtfl_probe_new();
	munit_assert_int(dtfl_execute(cfg, idle_worker, c, NULL), ==, 0);
	for (i = 0; i < c->nbox; i++) {
		munit_assert_size(c->loaded[i], >, 1);
		__dtfl_trace_box_release(c->box[i]);
	}
	dtfl_probe_free(c->probe);
	if (rt != NULL)
		dtfl_type_free((dtfl_type *)rt);
	if (shape == IDLE_UPSERT)
		dtfl_type_free((dtfl_type *)fields[1]);
	dtfl_diff_free(r);
	dtfl_config_free(cfg);
}

static void
check_idle(enum idle_shape shape, size_t nbox)
{
	idle_ctx c;
	size_t i;

	run_idle(shape, 0, &c);
	munit_assert_size(c.nbox, ==, nbox);
	for (i = 0; i < c.nbox; i++)
		munit_assert_size(c.idle[i], ==, c.loaded[i]);

	run_idle(shape, 16, &c);
	munit_assert_size(c.nbox, ==, nbox);
	for (i = 0; i < c.nbox; i++)
		munit_assert_size(c.idle[i], ==, 1);
}

DTFL_TEST(arrange__idle_merge_effort)
{
	dtfl_config *cfg = dtfl_config_new();

	(void)params; (void)data;
	munit_assert_int(dtfl_config_set_idle_merge_effort(cfg, -1), ==,
	    DTFL_EINVAL);
	munit_assert_int64(dtfl_config_idle_merge_effort(cfg), ==, 0);
	dtfl_config_free(cfg);

	check_idle(IDLE_ARRANGE, 1);
	check_idle(IDLE_REDUCE, 2);
	check_idle(IDLE_UPSERT, 1);
	return MUNIT_OK;
}

/*
 * The activation at the end of consider_merges (spine_fueled.rs 432-436):
 * a reader pins the physical frontier at 0 while the batches load, so they
 * stay pending; the worker goes quiet; then the reader is dropped, which
 * releases the hold and migrates the batches into the spine from the
 * compaction call, outside any operator activation.  With effort set, the
 * spine must reschedule its operator from there, or nothing ever merges.
 */
DTFL_TEST(arrange__idle_merge_after_release)
{
	idle_ctx c;

	(void)params; (void)data;
	memset(&c, 0, sizeof c);
	c.pin = true;
	run_idle_ctx(IDLE_ARRANGE, 16, &c);
	munit_assert_size(c.nbox, ==, 1);
	munit_assert_size(c.idle[0], ==, 1);
	return MUNIT_OK;
}

/*
 * dtfl_trace_stats_get on the pinned run above: while the pinned reader
 * holds the physical frontier at 0, the four loaded epochs (plus the
 * empty seals) wait as pending batches holding all 85 updates; after the
 * release and the idle merges, one batch holds them, nothing is pending
 * and no merge is in progress.  The byte count covers at least the 85
 * eight-byte keys.  A bad size is refused.
 */
DTFL_TEST(arrange__trace_stats)
{
	idle_ctx c;
	dtfl_trace_stats bad;

	(void)params; (void)data;
	memset(&c, 0, sizeof c);
	c.pin = true;
	run_idle_ctx(IDLE_ARRANGE, 16, &c);
	munit_assert_size(c.pinned_st.pending, >=, 4);
	munit_assert_size(c.pinned_st.batches, ==, c.pinned_st.pending);
	munit_assert_size(c.pinned_st.updates, ==, 85);
	munit_assert_size(c.pinned_st.merges, ==, 0);
	munit_assert_size(c.final_st.batches, ==, 1);
	munit_assert_size(c.final_st.pending, ==, 0);
	munit_assert_size(c.final_st.merges, ==, 0);
	munit_assert_size(c.final_st.updates, ==, 85);
	munit_assert_size(c.final_st.bytes, >=, 85 * 8);
	munit_assert_size(c.final_st.layers, >=, 1);

	memset(&bad, 0, sizeof bad);
	bad.size = sizeof bad + 8;
	munit_assert_int(dtfl_trace_stats_get(NULL, &bad), ==, DTFL_EINVAL);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* enter an iterative scope: join_core reads the trace in the inner class. */
/* ------------------------------------------------------------------ */

/*
 * Every (edge, outer, inner, diff) update join_core emits.  The entered
 * side's updates are outer times lifted by to_inner (inner word 0); the
 * probe side holds the root at iteration 0 and its retraction at 1, so
 * edge e joins +1 at (2e, 0) and -1 at (2e, 1).  The retraction arrives
 * after the edges, so it reads them back through the entered trace.
 */
typedef struct enter_seen {
	size_t n;
	uint64_t dst[MAX_OUT];
	uint64_t outer[MAX_OUT];
	uint64_t inner[MAX_OUT];
	int64_t diff[MAX_OUT];
} enter_seen;

static enter_seen enter_iter_seen;
static dtfl_arranged *enter_iter_arr;

static void
enter_iter_result(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1l, const void *v2, size_t v2l, dtfl_join_emit *out)
{
	(void)ctx; (void)klen; (void)v1; (void)v1l; (void)v2l;
	(void)key;
	dtfl_join_emit_record(out, v2, 8);
}

static void
enter_iter_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	enter_seen *s = ctx;

	(void)len;
	if (s->n >= MAX_OUT)
		return;
	s->dst[s->n] = dtfl_get_u64(rec);
	s->outer[s->n] = time[0];
	s->inner[s->n] = time[1];
	memcpy(&s->diff[s->n], diff, sizeof(int64_t));
	s->n++;
}

/* The summed diff of (dst, outer, inner) over everything seen. */
static int64_t
enter_seen_sum(const enter_seen *s, uint64_t dst, uint64_t outer,
    uint64_t inner)
{
	int64_t sum = 0;
	size_t i;

	for (i = 0; i < s->n; i++)
		if (s->dst[i] == dst && s->outer[i] == outer &&
		    s->inner[i] == inner)
			sum += s->diff[i];
	return sum;
}

/*
 * Checks the join output: edge 100 + e (e < 6, outer time 2e) sums to +1
 * at (2e, 0), -1 at (2e, 1), and nothing at any other time.
 */
static void
enter_seen_check(const enter_seen *s)
{
	uint64_t e, o, it;
	size_t i;

	for (i = 0; i < s->n; i++) {
		assert_uint64(s->outer[i], <=, 10);
		assert_uint64(s->inner[i], <=, 1);
		assert_uint64(s->dst[i], >=, 100);
		assert_uint64(s->dst[i], <, 106);
	}
	for (e = 0; e < 6; e++)
		for (o = 0; o <= 10; o++)
			for (it = 0; it < 2; it++) {
				int64_t want = o != 2 * e ? 0 :
				    it == 0 ? 1 : -1;

				assert_int64(enter_seen_sum(s, 100 + e, o, it),
				    ==, want);
			}
}

static bool
enter_iter_none(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)rec; (void)len;
	return false;
}

static dtfl_collection *
enter_iter_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	dtfl_arranged *edges = dtfl_arranged_enter(enter_iter_arr, inner);
	dtfl_arranged *probe = dtfl_collection_arrange_by_key(var,
	    dtfl_type_u64(), dtfl_type_unit(), "probe");
	dtfl_collection *joined = dtfl_arranged_join_core(probe, edges,
	    dtfl_type_u64(), enter_iter_result, DTFL_NOCTX);
	dtfl_collection *seen = dtfl_collection_inspect(joined,
	    enter_iter_sink, DTFL_CTX(&enter_iter_seen, NULL));
	/* The variable is the root at iteration 0 and empty after. */
	dtfl_collection *empty = dtfl_collection_filter(seen,
	    enter_iter_none, DTFL_NOCTX);

	(void)vctx;
	dtfl_collection_free(seen);
	dtfl_collection_free(joined);
	dtfl_arranged_free(probe);
	dtfl_arranged_free(edges);
	return empty;
}

typedef struct enter_iter_ctx {
	dtfl_input_session *edges;
	dtfl_input_session *roots;
	dtfl_probe *probe;
} enter_iter_ctx;

static void
build_enter_iterate(dtfl_scope *scope, void *arg)
{
	enter_iter_ctx *c = arg;
	dtfl_collection *e = dtfl_input_session_to_collection(c->edges, scope);
	dtfl_collection *r = dtfl_input_session_to_collection(c->roots, scope);
	dtfl_arranged *arr = dtfl_collection_arrange_by_key(e, dtfl_type_u64(),
	    dtfl_type_u64(), "edges");
	dtfl_collection *res, *probed;

	enter_iter_arr = arr;
	res = dtfl_collection_iterate(r, enter_iter_logic, DTFL_NOCTX);
	probed = dtfl_collection_probe_with(res, c->probe);
	dtfl_collection_free(probed);
	dtfl_collection_free(res);
	dtfl_arranged_free(arr);
	dtfl_collection_free(r);
	dtfl_collection_free(e);
}

DTFL_TEST(arrange__enter_iterate_inner_times)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	enter_iter_ctx c;
	uint64_t e;

	(void)params; (void)data;
	memset(&enter_iter_seen, 0, sizeof enter_iter_seen);
	c.edges = dtfl_input_session_new(pair, r);
	c.roots = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_enter_iterate, &c),
	    ==, 0);

	/*
	 * Six edges from node 0 at outer times 0, 2, ..., 10, all sealed
	 * into one batch of the outer trace.  Reading its one-word times as
	 * (outer, inner) pairs would take the next update's outer word as
	 * the inner coordinate, and the last update's from past the end.
	 */
	for (e = 0; e < 6; e++) {
		unsigned char rec[16];
		uint64_t t = 2 * e;

		dtfl_input_session_advance_to(c.edges, &t);
		dtfl_put_u64(rec, 0);
		dtfl_put_u64(rec + 8, 100 + e);
		dtfl_input_session_insert(c.edges, rec, sizeof rec);
	}
	dtfl_input_session_flush(c.edges);
	{
		unsigned char key[8];

		dtfl_put_u64(key, 0);
		dtfl_input_session_insert(c.roots, key, sizeof key);
		dtfl_input_session_flush(c.roots);
	}
	dtfl_input_session_close(c.edges);
	dtfl_input_session_close(c.roots);
	while (dtfl_worker_step(w))
		;

	enter_seen_check(&enter_iter_seen);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/*
 * The same shape without a Variable, so nothing retracts: an iterative
 * scope joins the roots against the edges entered with enter_at (or
 * enter), and through the int_proxy join and reduce tactics, which read
 * the trace's batches directly.  Edge e sits at outer time 2e; enter_at
 * places it at inner time e % 3, enter at 0.
 */
typedef enum enter_shape {
	ENTER_AT_JOIN = 0,	/* enter_at, join_core */
	ENTER_PROXY_JOIN,	/* enter, int_proxy join */
	ENTER_PROXY_REDUCE,	/* enter, int_proxy reduce (count) */
	ENTER_REDUCE		/* enter, reduce_abelian (count) */
} enter_shape;

typedef struct enter_flat_ctx {
	dtfl_input_session *edges;
	dtfl_input_session *roots;	/* (node, 7) pairs */
	enter_shape shape;
	dtfl_arranged *outer_edges;	/* set by the outer build */
	dtfl_collection *outer_roots;
} enter_flat_ctx;

/* enter_at logic: (t) -> (t, (dst - 100) % 3). */
static void
enter_at_logic(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, const uint64_t *outer_time, uint64_t *inner_time)
{
	(void)ctx; (void)key; (void)klen; (void)vlen;
	inner_time[0] = outer_time[0];
	inner_time[1] = (dtfl_get_u64(val) - 100) % 3;
}

/* enter_at prior: the outer coordinate, as the times enter at (t, _). */
static void
enter_at_prior(void *ctx, const uint64_t *inner_time, uint64_t *outer_time)
{
	(void)ctx;
	outer_time[0] = inner_time[0];
}

/* key || val as the record, for the reduce outputs. */
static void
enter_kv(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	unsigned char rec[16];

	(void)ctx;
	assert_size(klen + vlen, ==, sizeof rec);
	memcpy(rec, key, klen);
	memcpy(rec + klen, val, vlen);
	dtfl_arranged_emit_record(out, rec, sizeof rec);
}

/* Count the values (sum of diffs), retracting the prior output. */
static void
enter_count_proxy(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, __dtfl_reduce_change *ch)
{
	int64_t sum = 0, one = 1, d;
	unsigned char vb[8];
	size_t i;

	(void)ctx; (void)key; (void)klen; (void)in_vals; (void)in_vlens;
	for (i = 0; i < n; i++) {
		memcpy(&d, (const unsigned char *)in_diffs + 8 * i, 8);
		sum += d;
	}
	if (sum != 0) {
		dtfl_put_u64(vb, (uint64_t)sum);
		__dtfl_reduce_change_push(ch, vb, sizeof vb, &one);
	}
	for (i = 0; i < m; i++) {
		memcpy(&d, (const unsigned char *)out_diffs + 8 * i, 8);
		d = -d;
		__dtfl_reduce_change_push(ch, out_vals[i], out_vlens[i], &d);
	}
}

static void
enter_count(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, dtfl_reduce_emit *out)
{
	int64_t sum = 0, one = 1, d;
	unsigned char vb[8];
	size_t i;

	(void)ctx; (void)key; (void)klen; (void)in_vals; (void)in_vlens;
	(void)m; (void)out_vals; (void)out_vlens; (void)out_diffs;
	for (i = 0; i < n; i++) {
		memcpy(&d, (const unsigned char *)in_diffs + 8 * i, 8);
		sum += d;
	}
	dtfl_put_u64(vb, (uint64_t)sum);
	dtfl_reduce_emit_pair(out, vb, sizeof vb, &one);
}

/* For the reduce shapes the sink records (count, outer, inner, diff). */
static void
enter_count_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	(void)len;
	enter_iter_sink(ctx, (const unsigned char *)rec + 8, 8, time, diff);
}

static void
build_enter_flat_inner(dtfl_scope *inner, void *arg)
{
	enter_flat_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	dtfl_arranged *edges, *probe = NULL, *red = NULL;
	dtfl_collection *roots, *out = NULL, *seen;
	dtfl_collection_inspect_fn sink = enter_iter_sink;

	if (c->shape == ENTER_AT_JOIN)
		edges = __dtfl_arranged_enter_at(c->outer_edges, inner,
		    enter_at_logic, enter_at_prior, NULL, NULL);
	else
		edges = dtfl_arranged_enter(c->outer_edges, inner);
	roots = dtfl_collection_enter(c->outer_roots, inner);
	switch (c->shape) {
	case ENTER_AT_JOIN:
		probe = dtfl_collection_arrange_by_key(roots, u64, u64,
		    "probe");
		out = dtfl_arranged_join_core(probe, edges, u64,
		    enter_iter_result, DTFL_NOCTX);
		break;
	case ENTER_PROXY_JOIN:
		probe = dtfl_collection_arrange_by_key(roots, u64, u64,
		    "probe");
		out = __dtfl_arranged_join_with_proxy(probe, edges, u64,
		    __dtfl_proxy_join_u64_backend(probe, edges,
		    enter_iter_result, NULL, NULL));
		break;
	case ENTER_PROXY_REDUCE: {
		dtfl_diff *od = dtfl_diff_i64();

		red = __dtfl_arranged_reduce_with_proxy(edges, "CountProxy",
		    u64, od->cls, __dtfl_proxy_reduce_u64_backend(edges, u64,
		    od->cls, enter_count_proxy, NULL, NULL));
		dtfl_diff_free(od);
		break;
	}
	case ENTER_REDUCE: {
		dtfl_diff *od = dtfl_diff_i64();

		red = dtfl_arranged_reduce_abelian(edges, "Count", u64, od,
		    enter_count, DTFL_NOCTX);
		dtfl_diff_free(od);
		break;
	}
	}
	if (red != NULL) {
		const dtfl_type *pr = dtfl_type_pair(u64, u64);

		out = dtfl_arranged_as_collection(red, pr, enter_kv,
		    DTFL_NOCTX);
		dtfl_type_free(pr);
		sink = enter_count_sink;
	}
	seen = dtfl_collection_inspect(out, sink,
	    DTFL_CTX(&enter_iter_seen, NULL));
	dtfl_collection_free(seen);
	dtfl_collection_free(out);
	dtfl_arranged_free(red);
	dtfl_arranged_free(probe);
	dtfl_collection_free(roots);
	dtfl_arranged_free(edges);
}

static void
build_enter_flat(dtfl_scope *scope, void *arg)
{
	enter_flat_ctx *c = arg;
	dtfl_collection *e = dtfl_input_session_to_collection(c->edges, scope);

	c->outer_roots = dtfl_input_session_to_collection(c->roots, scope);
	c->outer_edges = dtfl_collection_arrange_by_key(e, dtfl_type_u64(),
	    dtfl_type_u64(), "edges");
	assert_int(dtfl_scope_iterative(scope, "Iterate",
	    build_enter_flat_inner, c), ==, 0);
	dtfl_arranged_free(c->outer_edges);
	dtfl_collection_free(c->outer_roots);
	dtfl_collection_free(e);
}

static void
run_enter_flat(enter_shape shape)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	enter_flat_ctx c;
	unsigned char rec[16];
	uint64_t e;

	memset(&enter_iter_seen, 0, sizeof enter_iter_seen);
	memset(&c, 0, sizeof c);
	c.edges = dtfl_input_session_new(pair, r);
	c.roots = dtfl_input_session_new(pair, r);
	c.shape = shape;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_enter_flat, &c),
	    ==, 0);
	/*
	 * Two outer batches of three edges each (times 0, 2, 4 and 6, 8,
	 * 10), so the second batch's reduce reads the first back through
	 * the entered trace.  The root comes after both, so the join reads
	 * the edges back through it too.
	 */
	for (e = 0; e < 6; e++) {
		uint64_t t = 2 * e;
		int i;

		dtfl_input_session_advance_to(c.edges, &t);
		dtfl_put_u64(rec, 0);
		dtfl_put_u64(rec + 8, 100 + e);
		dtfl_input_session_insert(c.edges, rec, sizeof rec);
		if (e == 2) {
			t = 6;
			dtfl_input_session_advance_to(c.edges, &t);
			dtfl_input_session_flush(c.edges);
			for (i = 0; i < 50; i++)
				(void)dtfl_worker_step(w);
		}
	}
	dtfl_input_session_close(c.edges);
	for (e = 0; e < 50; e++)
		(void)dtfl_worker_step(w);
	dtfl_put_u64(rec, 0);
	dtfl_put_u64(rec + 8, 7);
	dtfl_input_session_insert(c.roots, rec, sizeof rec);
	dtfl_input_session_close(c.roots);
	while (dtfl_worker_step(w))
		;
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
}

/* Edge 100 + e joins +1 at (2e, it(e)) and nowhere else. */
static void
enter_flat_check_join(bool at)
{
	const enter_seen *s = &enter_iter_seen;
	uint64_t e, o, it;
	size_t i;

	assert_size(s->n, >, 0);
	for (i = 0; i < s->n; i++) {
		assert_uint64(s->outer[i], <=, 10);
		assert_uint64(s->inner[i], <=, 2);
	}
	for (e = 0; e < 6; e++)
		for (o = 0; o <= 10; o++)
			for (it = 0; it < 3; it++) {
				uint64_t want_it = at ? e % 3 : 0;
				int64_t want = o == 2 * e && it == want_it;

				assert_int64(enter_seen_sum(s, 100 + e, o, it),
				    ==, want);
			}
}

/* The count of key 0 is e + 1 from (2e, 0) on: +1 there, -1 at 2e + 2. */
static void
enter_flat_check_count(void)
{
	const enter_seen *s = &enter_iter_seen;
	uint64_t k, o;
	size_t i;

	assert_size(s->n, >, 0);
	for (i = 0; i < s->n; i++) {
		assert_uint64(s->outer[i], <=, 10);
		assert_uint64(s->inner[i], ==, 0);
	}
	for (k = 1; k <= 6; k++)
		for (o = 0; o <= 10; o++) {
			int64_t want = o == 2 * (k - 1) ? 1 :
			    (k < 6 && o == 2 * k) ? -1 : 0;

			assert_int64(enter_seen_sum(s, k, o, 0), ==, want);
		}
}

DTFL_TEST(arrange__enter_at_join_inner_times)
{
	(void)params; (void)data;
	run_enter_flat(ENTER_AT_JOIN);
	enter_flat_check_join(true);
	return MUNIT_OK;
}

DTFL_TEST(arrange__enter_proxy_join_inner_times)
{
	(void)params; (void)data;
	run_enter_flat(ENTER_PROXY_JOIN);
	enter_flat_check_join(false);
	return MUNIT_OK;
}

DTFL_TEST(arrange__enter_proxy_reduce_inner_times)
{
	(void)params; (void)data;
	run_enter_flat(ENTER_PROXY_REDUCE);
	enter_flat_check_count();
	return MUNIT_OK;
}

DTFL_TEST(arrange__enter_reduce_inner_times)
{
	(void)params; (void)data;
	run_enter_flat(ENTER_REDUCE);
	enter_flat_check_count();
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* An entered trace handle: compaction, to_vec and a quiet input.      */
/* ------------------------------------------------------------------ */

/*
 * set_*_compaction through an entered handle (enter.rs): the frontier,
 * in the inner class, reaches the shared trace mapped with to_outer.
 * {(4, 6), (2, 9)} maps to {4, 2}, whose antichain is {2}.  Read as
 * one-word outer times it would be {4, 6}, that is {4}, which compacts
 * the outer trace past a cut an outer reader may still take.
 */
typedef struct ecomp_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	dtfl_arranged *outer;		/* build only */
	dtfl_trace *outer_t;		/* a reader of the outer arrangement */
	dtfl_trace *ent_t;		/* a reader of the entered one */
	__dtfl_trace_box *box;		/* retained: read the shared holds */
} ecomp_ctx;

static void
build_ecomp_inner(dtfl_scope *inner, void *arg)
{
	ecomp_ctx *c = arg;
	dtfl_arranged *ent = dtfl_arranged_enter(c->outer, inner);

	c->ent_t = dtfl_arranged_trace(ent);
	dtfl_arranged_free(ent);
}

static void
build_ecomp(dtfl_scope *scope, void *arg)
{
	ecomp_ctx *c = arg;
	dtfl_collection *e = dtfl_input_session_to_collection(c->in, scope);

	c->outer = dtfl_collection_arrange_by_key(e, dtfl_type_u64(),
	    dtfl_type_u64(), "edges");
	(void)dtfl_stream_probe_with(dtfl_arranged_stream(c->outer), c->probe);
	c->outer_t = dtfl_arranged_trace(c->outer);
	c->box = __dtfl_trace_box_retain(__dtfl_arranged_agent(c->outer)->box);
	assert_int(dtfl_scope_iterative(scope, "It", build_ecomp_inner, c),
	    ==, 0);
	dtfl_arranged_free(c->outer);
	c->outer = NULL;
	dtfl_collection_free(e);
}

DTFL_TEST(arrange__enter_compaction_maps_to_outer)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	const uint64_t f[4] = { 4, 6, 2, 9 };
	const __dtfl_antichain *held;
	const uint64_t *h;
	ecomp_ctx c;
	trace_rows rows;
	uint64_t t, three = 3;
	size_t n, i;

	(void)params; (void)data;
	memset(&c, 0, sizeof c);
	c.in = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_ecomp, &c), ==, 0);

	/* (0, 100 + t) at each outer time t < 8, one batch per time. */
	for (t = 0; t < 8; t++) {
		unsigned char rec[16];
		uint64_t next = t + 1;
		probe_cond pc = { c.probe, next };

		dtfl_put_u64(rec, 0);
		dtfl_put_u64(rec + 8, 100 + t);
		dtfl_input_session_insert(c.in, rec, sizeof rec);
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		dtfl_worker_step_while(w, probe_behind, &pc);
	}

	/* The outer reader lets go, so the entered reader alone holds. */
	dtfl_trace_set_logical_compaction(c.outer_t, NULL, 0);
	dtfl_trace_set_physical_compaction(c.outer_t, NULL, 0);
	dtfl_trace_set_logical_compaction(c.ent_t, f, 2);
	dtfl_trace_set_physical_compaction(c.ent_t, f, 2);

	/* The shared trace and its box hold at to_outer, {2}. */
	held = __dtfl_trace_get_logical_compaction(c.box->trace);
	assert_size(__dtfl_antichain_len(held), ==, 1);
	assert_uint64(__dtfl_antichain_at(held, 0)[0], ==, 2);
	held = __dtfl_trace_get_physical_compaction(c.box->trace);
	assert_size(__dtfl_antichain_len(held), ==, 1);
	assert_uint64(__dtfl_antichain_at(held, 0)[0], ==, 2);
	assert_size(__dtfl_mantichain_frontier_len(&c.box->logical_compaction),
	    ==, 1);
	assert_uint64(__dtfl_mantichain_frontier(&c.box->logical_compaction)[0],
	    ==, 2);
	assert_size(__dtfl_mantichain_frontier_len(&c.box->physical_compaction),
	    ==, 1);
	assert_uint64(
	    __dtfl_mantichain_frontier(&c.box->physical_compaction)[0], ==, 2);

	/* The entered handle reports its holds in the inner class, (2, 0). */
	h = dtfl_trace_logical_compaction(c.ent_t, &n);
	assert_size(n, ==, 1);
	assert_uint64(h[0], ==, 2);
	assert_uint64(h[1], ==, 0);
	h = dtfl_trace_physical_compaction(c.ent_t, &n);
	assert_size(n, ==, 1);
	assert_uint64(h[0], ==, 2);
	assert_uint64(h[1], ==, 0);

	/*
	 * Not over-compacted: an outer reader can still cut at 3, and the
	 * cut holds the three values at times up to 2 (advanced to 2 at
	 * most) and nothing later.
	 */
	memset(&rows, 0, sizeof rows);
	assert_true(dtfl_trace_to_vec_through(c.outer_t, &three, 1, trace_row,
	    DTFL_CTX(&rows, NULL)));
	assert_size(rows.n, ==, 3);
	for (i = 0; i < rows.n; i++) {
		assert_uint64(rows.key[i], ==, 0);
		assert_uint64(rows.time[i], <=, 2);
		assert_int64(rows.diff[i], ==, 1);
	}

	/*
	 * Logical compaction at 2 may advance the times before 2 to 2 and
	 * must leave every later time distinct.
	 */
	memset(&rows, 0, sizeof rows);
	dtfl_trace_to_vec(c.outer_t, trace_row, DTFL_CTX(&rows, NULL));
	assert_size(rows.n, ==, 8);
	for (i = 0; i < rows.n; i++) {
		t = rows.val[i] - 100;
		if (t >= 2)
			assert_uint64(rows.time[i], ==, t);
		else
			assert_true(rows.time[i] == t || rows.time[i] == 2);
	}

	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	dtfl_trace_free(c.ent_t);
	dtfl_trace_free(c.outer_t);
	__dtfl_trace_box_release(c.box);
	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/*
 * dtfl_trace_to_vec on an entered handle reads through the agent's
 * cursor, which must carry the enter chain: each time arrives in the
 * handle's class, the outer time then zero inner words.  Entered once
 * (two words), twice (three), and twice then left once (two).  The six
 * updates share one outer batch, so a one-word time read as two would
 * take the next update's time as its inner word.
 */
typedef struct evec_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	dtfl_arranged *outer, *mid;	/* build only */
	dtfl_trace *t1;			/* entered once */
	dtfl_trace *t2;			/* entered twice */
	dtfl_trace *tl;			/* entered twice, left once */
} evec_ctx;

typedef struct evec {
	unsigned depth;
	size_t n;
} evec;

/* Value 100 + e sits at outer time 2e; every inner word is zero. */
static void
evec_row(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, const uint64_t *time, const void *diff)
{
	evec *v = ctx;
	uint64_t e = dtfl_get_u64(val) - 100;
	int64_t d;
	unsigned j;

	(void)klen; (void)vlen;
	assert_uint64(dtfl_get_u64(key), ==, 0);
	assert_uint64(e, <, 6);
	assert_uint64(time[0], ==, 2 * e);
	for (j = 1; j < v->depth; j++)
		assert_uint64(time[j], ==, 0);
	memcpy(&d, diff, sizeof d);
	assert_int64(d, ==, 1);
	v->n++;
}

static void
evec_check(dtfl_trace *t, unsigned depth)
{
	evec v = { depth, 0 };

	dtfl_trace_to_vec(t, evec_row, DTFL_CTX(&v, NULL));
	assert_size(v.n, ==, 6);
}

static void
build_evec_inner2(dtfl_scope *inner, void *arg)
{
	evec_ctx *c = arg;
	dtfl_arranged *ent = dtfl_arranged_enter(c->mid, inner);
	dtfl_arranged *left = dtfl_arranged_leave(ent);

	c->t2 = dtfl_arranged_trace(ent);
	c->tl = dtfl_arranged_trace(left);
	dtfl_arranged_free(left);
	dtfl_arranged_free(ent);
}

static void
build_evec_inner1(dtfl_scope *inner, void *arg)
{
	evec_ctx *c = arg;

	c->mid = dtfl_arranged_enter(c->outer, inner);
	c->t1 = dtfl_arranged_trace(c->mid);
	assert_int(dtfl_scope_iterative(inner, "It2", build_evec_inner2, c),
	    ==, 0);
	dtfl_arranged_free(c->mid);
	c->mid = NULL;
}

static void
build_evec(dtfl_scope *scope, void *arg)
{
	evec_ctx *c = arg;
	dtfl_collection *e = dtfl_input_session_to_collection(c->in, scope);

	c->outer = dtfl_collection_arrange_by_key(e, dtfl_type_u64(),
	    dtfl_type_u64(), "edges");
	(void)dtfl_stream_probe_with(dtfl_arranged_stream(c->outer), c->probe);
	assert_int(dtfl_scope_iterative(scope, "It", build_evec_inner1, c),
	    ==, 0);
	dtfl_arranged_free(c->outer);
	c->outer = NULL;
	dtfl_collection_free(e);
}

DTFL_TEST(arrange__enter_trace_to_vec_inner_times)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	evec_ctx c;
	uint64_t e, end = 11;
	probe_cond pc;

	(void)params; (void)data;
	memset(&c, 0, sizeof c);
	c.in = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_evec, &c), ==, 0);
	for (e = 0; e < 6; e++) {
		unsigned char rec[16];
		uint64_t t = 2 * e;

		dtfl_input_session_advance_to(c.in, &t);
		dtfl_put_u64(rec, 0);
		dtfl_put_u64(rec + 8, 100 + e);
		dtfl_input_session_insert(c.in, rec, sizeof rec);
	}
	dtfl_input_session_advance_to(c.in, &end);
	dtfl_input_session_flush(c.in);
	pc.probe = c.probe;
	pc.target = end;
	dtfl_worker_step_while(w, probe_behind, &pc);

	evec_check(c.t1, 2);
	evec_check(c.t2, 3);
	evec_check(c.tl, 2);

	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	dtfl_trace_free(c.tl);
	dtfl_trace_free(c.t2);
	dtfl_trace_free(c.t1);
	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/*
 * A join over an entered arrangement whose input goes quiet: the outer
 * writer seals each empty epoch into the shared trace without sending a
 * batch, so only advance_upper, walking the trace's batches through the
 * enter chain, moves the join's acknowledged frontier.  The join then
 * releases physical compaction to it.  The join's reader is the trace's
 * only hold (the watch handle lets go of both), so the shared physical
 * frontier is that reader's.
 */
typedef struct equiet_ctx {
	dtfl_input_session *edges, *roots;
	dtfl_probe *probe;
	dtfl_arranged *outer;		/* build only */
	dtfl_collection *outer_roots;	/* build only */
	dtfl_trace *watch;		/* holds nothing; for stats */
	__dtfl_trace_box *box;		/* retained */
} equiet_ctx;

static void
build_equiet_inner(dtfl_scope *inner, void *arg)
{
	equiet_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	dtfl_arranged *edges = dtfl_arranged_enter(c->outer, inner);
	dtfl_collection *roots = dtfl_collection_enter(c->outer_roots, inner);
	dtfl_arranged *probe = dtfl_collection_arrange_by_key(roots, u64, u64,
	    "probe");
	dtfl_collection *out = dtfl_arranged_join_core(probe, edges, u64,
	    enter_iter_result, DTFL_NOCTX);
	dtfl_collection *seen = dtfl_collection_inspect(out, enter_iter_sink,
	    DTFL_CTX(&enter_iter_seen, NULL));

	dtfl_collection_free(seen);
	dtfl_collection_free(out);
	dtfl_arranged_free(probe);
	dtfl_collection_free(roots);
	dtfl_arranged_free(edges);
}

static void
build_equiet(dtfl_scope *scope, void *arg)
{
	equiet_ctx *c = arg;
	dtfl_collection *e = dtfl_input_session_to_collection(c->edges, scope);

	c->outer_roots = dtfl_input_session_to_collection(c->roots, scope);
	c->outer = dtfl_collection_arrange_by_key(e, dtfl_type_u64(),
	    dtfl_type_u64(), "edges");
	(void)dtfl_stream_probe_with(dtfl_arranged_stream(c->outer), c->probe);
	c->box = __dtfl_trace_box_retain(__dtfl_arranged_agent(c->outer)->box);
	c->watch = dtfl_arranged_trace(c->outer);
	dtfl_trace_set_logical_compaction(c->watch, NULL, 0);
	dtfl_trace_set_physical_compaction(c->watch, NULL, 0);
	assert_int(dtfl_scope_iterative(scope, "It", build_equiet_inner, c),
	    ==, 0);
	dtfl_arranged_free(c->outer);
	c->outer = NULL;
	dtfl_collection_free(c->outer_roots);
	c->outer_roots = NULL;
	dtfl_collection_free(e);
}

DTFL_TEST(arrange__enter_quiet_join_releases_physical)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	const __dtfl_antichain *ph;
	equiet_ctx c;
	unsigned char rec[16];
	uint64_t t;
	int i;

	(void)params; (void)data;
	memset(&enter_iter_seen, 0, sizeof enter_iter_seen);
	memset(&c, 0, sizeof c);
	c.edges = dtfl_input_session_new(pair, r);
	c.roots = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_equiet, &c), ==, 0);

	/* Edge (0, 100) and root (0, 7) at time 0, then six quiet epochs. */
	dtfl_put_u64(rec, 0);
	dtfl_put_u64(rec + 8, 100);
	dtfl_input_session_insert(c.edges, rec, sizeof rec);
	dtfl_put_u64(rec + 8, 7);
	dtfl_input_session_insert(c.roots, rec, sizeof rec);
	for (t = 1; t <= 6; t++) {
		probe_cond pc = { c.probe, t };

		dtfl_input_session_advance_to(c.edges, &t);
		dtfl_input_session_advance_to(c.roots, &t);
		dtfl_input_session_flush(c.edges);
		dtfl_input_session_flush(c.roots);
		dtfl_worker_step_while(w, probe_behind, &pc);
	}
	for (i = 0; i < 50; i++)
		(void)dtfl_worker_step(w);

	/* The writer sealed to 6; the join's hold followed it there. */
	ph = __dtfl_trace_get_physical_compaction(c.box->trace);
	assert_size(__dtfl_antichain_len(ph), ==, 1);
	assert_uint64(__dtfl_antichain_at(ph, 0)[0], ==, 6);

	dtfl_input_session_close(c.edges);
	dtfl_input_session_close(c.roots);
	while (dtfl_worker_step(w))
		;
	assert_int64(enter_seen_sum(&enter_iter_seen, 100, 0, 0), ==, 1);
	assert_size(enter_iter_seen.n, ==, 1);

	dtfl_trace_free(c.watch);
	__dtfl_trace_box_release(c.box);
	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(arrange__as_collection_equals_consolidate),
	DTFL_T(arrange__multi_epoch_insert_retract),
	DTFL_T(arrange__enter_region),
	DTFL_T(arrange__shared_import_two_readers),
	DTFL_T(arrange__trace_compaction_holds),
	DTFL_T(arrange__caps_insert_antichain),
	DTFL_T(arrange__idle_merge_effort),
	DTFL_T(arrange__idle_merge_after_release),
	DTFL_T(arrange__trace_stats),
	DTFL_T(arrange__enter_iterate_inner_times),
	DTFL_T(arrange__enter_at_join_inner_times),
	DTFL_T(arrange__enter_proxy_join_inner_times),
	DTFL_T(arrange__enter_proxy_reduce_inner_times),
	DTFL_T(arrange__enter_reduce_inner_times),
	DTFL_T(arrange__enter_compaction_maps_to_outer),
	DTFL_T(arrange__enter_trace_to_vec_inner_times),
	DTFL_T(arrange__enter_quiet_join_releases_physical),
	DTFL_T_END
};

DTFL_TEST_MAIN("arrange", tests)
