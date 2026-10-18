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

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
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

static MunitTest tests[] = {
	DTFL_T(arrange__as_collection_equals_consolidate),
	DTFL_T(arrange__multi_epoch_insert_retract),
	DTFL_T(arrange__enter_region),
	DTFL_T(arrange__shared_import_two_readers),
	DTFL_T(arrange__caps_insert_antichain),
	DTFL_T_END
};

DTFL_TEST_MAIN("arrange", tests)
