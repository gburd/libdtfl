/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_reduce.c: the reduce family across epochs.
 *
 * The subtle property of reduce is the retract-old / assert-new delta: when a
 * key's accumulated input changes at a new time, the operator retracts the
 * output it produced before and asserts the new output, so the collection it
 * defines equals the function of its input at every time.  These tests drive
 * the SAME key through several epochs with a changing value and check that the
 * net output accumulated at each time is exactly the reference result, with no
 * doubling and no stale output.  A multiplicity min reducer, count, distinct
 * and threshold are each exercised.
 */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 8192

typedef struct out_upd {
	uint64_t key;
	int64_t ival;		/* value field decoded as i64 */
	uint64_t uval;		/* value field decoded as u64 */
	uint64_t time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAX_OUT];
	size_t n;
} collected;

static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;

	if (c->n >= MAX_OUT)
		return;
	c->u[c->n].key = dtfl_get_u64(rec);
	c->u[c->n].uval = len >= 16 ? dtfl_get_u64((const unsigned char *)rec + 8) : 0;
	c->u[c->n].ival = len >= 16 ? dtfl_get_i64((const unsigned char *)rec + 8) : 0;
	c->u[c->n].time = time[0];
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* Net diff for (key, uval) accumulated at times <= `at`. */
static int64_t
acc_u(const collected *c, uint64_t key, uint64_t uval, uint64_t at)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].key == key && c->u[i].uval == uval &&
		    c->u[i].time <= at)
			s += c->u[i].diff;
	return s;
}

/* As acc_u, for a value field that is an i64 (a count_total count). */
static int64_t
acc_i(const collected *c, uint64_t key, int64_t ival, uint64_t at)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].key == key && c->u[i].ival == ival &&
		    c->u[i].time <= at)
			s += c->u[i].diff;
	return s;
}

/* Net diff for a key (whole record is the key) accumulated at <= `at`. */
static int64_t
acc_key(const collected *c, uint64_t key, uint64_t at)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].key == key && c->u[i].time <= at)
			s += c->u[i].diff;
	return s;
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

/* min reducer: emit the smallest value with diff 1. */
static void
min_logic(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;
	unsigned char vbuf[8];

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_put_u64(vbuf, dtfl_get_u64(vals[0]));
	dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &one);
}

/* ------------------------------------------------------------------ */
/* reduce: a key whose minimum value changes across epochs.             */
/* ------------------------------------------------------------------ */

typedef struct build_min {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
} build_min;

static void
build_reduce_min(dtfl_scope *scope, void *arg)
{
	build_min *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_diff *od = dtfl_diff_i64();
	dtfl_collection *red = dtfl_collection_reduce(base, dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_u64(), od, min_logic, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(red, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(red);
	dtfl_diff_free(od);
	dtfl_collection_free(base);
}

static void
put_kv(unsigned char *b, uint64_t k, uint64_t v)
{
	dtfl_put_u64(b, k);
	dtfl_put_u64(b + 8, v);
}

static void
step_to(dtfl_worker *w, dtfl_input_session *in, dtfl_probe *p, uint64_t t)
{
	probe_cond pc = { p, t };

	dtfl_input_session_advance_to(in, &t);
	dtfl_input_session_flush(in);
	dtfl_worker_step_while(w, probe_behind, &pc);
}

DTFL_TEST(reduce__min_across_epochs)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *f[2] = { dtfl_type_u64(), dtfl_type_u64() };
	const dtfl_type *rt = dtfl_type_tuple(f, 2);
	build_min c;
	collected out;
	unsigned char kv[16];

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(rt, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_reduce_min, &c),
	    ==, 0);

	/* Epoch 0: key 7 has values {5, 9}; min is 5. */
	put_kv(kv, 7, 5);
	dtfl_input_session_insert(c.in, kv, sizeof kv);
	put_kv(kv, 7, 9);
	dtfl_input_session_insert(c.in, kv, sizeof kv);
	step_to(w, c.in, c.probe, 1);

	/* Epoch 1: add value 3 to key 7; min becomes 3. */
	put_kv(kv, 7, 3);
	dtfl_input_session_insert(c.in, kv, sizeof kv);
	step_to(w, c.in, c.probe, 2);

	/* Epoch 2: remove value 3; min returns to 5. */
	put_kv(kv, 7, 3);
	dtfl_input_session_remove(c.in, kv, sizeof kv);
	step_to(w, c.in, c.probe, 3);

	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	/* At time 0: min is 5, so (7,5) accumulates to +1, (7,3) to 0. */
	assert_int64(acc_u(&out, 7, 5, 0), ==, 1);
	assert_int64(acc_u(&out, 7, 3, 0), ==, 0);
	assert_int64(acc_u(&out, 7, 9, 0), ==, 0);
	/* At time 1: min is 3; (7,3) is +1 and (7,5) is retracted to 0. */
	assert_int64(acc_u(&out, 7, 3, 1), ==, 1);
	assert_int64(acc_u(&out, 7, 5, 1), ==, 0);
	/* At time 2: min returns to 5; (7,5) is +1 and (7,3) back to 0. */
	assert_int64(acc_u(&out, 7, 5, 2), ==, 1);
	assert_int64(acc_u(&out, 7, 3, 2), ==, 0);

	dtfl_probe_free(c.probe);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* count / distinct / threshold across epochs.                          */
/* ------------------------------------------------------------------ */

typedef enum which { W_COUNT, W_DISTINCT, W_THRESH, W_COUNT_T, W_DISTINCT_T,
    W_THRESH_T } which;

typedef struct build_cdt {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
	which w;
} build_cdt;

/* threshold: emit 1 if the accumulated input diff is even (c % 2 == 0). */
static bool
even_thresh(void *ctx, const void *key, size_t klen, const void *acc_diff,
    void *out)
{
	int64_t c;
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen;
	memcpy(&c, acc_diff, sizeof c);
	if ((c % 2) == 0 && c != 0) {
		memcpy(out, &one, sizeof one);
		return true;
	}
	return false;
}

static void
build_cdt_fn(dtfl_scope *scope, void *arg)
{
	build_cdt *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_diff *od = dtfl_diff_i64();
	dtfl_collection *red;
	dtfl_collection *ins, *probed;

	if (c->w == W_COUNT)
		red = dtfl_collection_count(base, od);
	else if (c->w == W_DISTINCT)
		red = dtfl_collection_distinct(base, od);
	else if (c->w == W_THRESH)
		red = dtfl_collection_threshold(base, od, even_thresh,
		    DTFL_NOCTX);
	else if (c->w == W_COUNT_T)
		red = dtfl_collection_count_total(base, od);
	else if (c->w == W_DISTINCT_T)
		red = dtfl_collection_distinct_total(base, od);
	else
		red = dtfl_collection_threshold_total(base, od, even_thresh,
		    DTFL_NOCTX);
	ins = dtfl_collection_inspect(red, sink, DTFL_CTX(c->out, NULL));
	probed = dtfl_collection_probe_with(ins, c->probe);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(red);
	dtfl_diff_free(od);
	dtfl_collection_free(base);
}

static collected
run_cdt(which which_op)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_cdt c;
	collected out;
	unsigned char k[8];

	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.w = which_op;
	dtfl_worker_dataflow(w, "dd", tc, build_cdt_fn, &c);

	/* Epoch 0: key 2 inserted twice (count 2), key 3 once (count 1). */
	dtfl_put_u64(k, 2);
	dtfl_input_session_insert(c.in, k, sizeof k);
	dtfl_input_session_insert(c.in, k, sizeof k);
	dtfl_put_u64(k, 3);
	dtfl_input_session_insert(c.in, k, sizeof k);
	step_to(w, c.in, c.probe, 1);

	/* Epoch 1: one more key 2 (count 3), remove key 3 (count 0). */
	dtfl_put_u64(k, 2);
	dtfl_input_session_insert(c.in, k, sizeof k);
	dtfl_put_u64(k, 3);
	dtfl_input_session_remove(c.in, k, sizeof k);
	step_to(w, c.in, c.probe, 2);

	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return out;
}

DTFL_TEST(reduce__count_across_epochs)
{
	collected out;

	(void)params; (void)data;
	out = run_cdt(W_COUNT);
	/* count output value is the summed input diff (i64).  At time 0, key 2
	 * count is 2 and key 3 count is 1.  At time 1, key 2 count is 3 and
	 * key 3 count is 0 (retracted). */
	{
		size_t i;
		int64_t k2_t0 = 0, k2_t1 = 0, k3_t0 = 0, k3_t1 = 0;

		for (i = 0; i < out.n; i++) {
			if (out.u[i].key == 2 && out.u[i].ival == 2 &&
			    out.u[i].time <= 0)
				k2_t0 += out.u[i].diff;
			if (out.u[i].key == 2 && out.u[i].ival == 3 &&
			    out.u[i].time <= 1)
				k2_t1 += out.u[i].diff;
			if (out.u[i].key == 3 && out.u[i].ival == 1 &&
			    out.u[i].time <= 0)
				k3_t0 += out.u[i].diff;
			if (out.u[i].key == 3 && out.u[i].time <= 1)
				k3_t1 += out.u[i].diff;
		}
		assert_int64(k2_t0, ==, 1);	/* (2, count=2) present at t0 */
		assert_int64(k2_t1, ==, 1);	/* (2, count=3) present at t1 */
		assert_int64(k3_t0, ==, 1);	/* (3, count=1) present at t0 */
		assert_int64(k3_t1, ==, 0);	/* key 3 gone at t1 */
	}
	return MUNIT_OK;
}

DTFL_TEST(reduce__distinct_across_epochs)
{
	collected out;

	(void)params; (void)data;
	out = run_cdt(W_DISTINCT);
	/* distinct: each key with non-zero accumulation is +1. */
	assert_int64(acc_key(&out, 2, 0), ==, 1);
	assert_int64(acc_key(&out, 3, 0), ==, 1);
	assert_int64(acc_key(&out, 2, 1), ==, 1);
	assert_int64(acc_key(&out, 3, 1), ==, 0);	/* retracted at t1 */
	return MUNIT_OK;
}

DTFL_TEST(reduce__threshold_across_epochs)
{
	collected out;

	(void)params; (void)data;
	out = run_cdt(W_THRESH);
	/* threshold emits 1 when the accumulated count is even and non-zero.
	 * key 2: count 2 (even) at t0 -> +1; count 3 (odd) at t1 -> retracted.
	 * key 3: count 1 (odd) at t0 -> nothing; count 0 at t1 -> nothing. */
	assert_int64(acc_key(&out, 2, 0), ==, 1);
	assert_int64(acc_key(&out, 2, 1), ==, 0);
	assert_int64(acc_key(&out, 3, 0), ==, 0);
	assert_int64(acc_key(&out, 3, 1), ==, 0);
	return MUNIT_OK;
}

/* The _total specializations must match the general versions. */
DTFL_TEST(reduce__count_total_matches)
{
	collected out;

	(void)params; (void)data;
	out = run_cdt(W_COUNT_T);
	{
		size_t i;
		int64_t k2_t0 = 0, k2_t1 = 0, k3_t0 = 0, k3_t1 = 0;

		for (i = 0; i < out.n; i++) {
			if (out.u[i].key == 2 && out.u[i].ival == 2 &&
			    out.u[i].time <= 0)
				k2_t0 += out.u[i].diff;
			if (out.u[i].key == 2 && out.u[i].ival == 3 &&
			    out.u[i].time <= 1)
				k2_t1 += out.u[i].diff;
			if (out.u[i].key == 3 && out.u[i].ival == 1 &&
			    out.u[i].time <= 0)
				k3_t0 += out.u[i].diff;
			if (out.u[i].key == 3 && out.u[i].time <= 1)
				k3_t1 += out.u[i].diff;
		}
		assert_int64(k2_t0, ==, 1);
		assert_int64(k2_t1, ==, 1);
		assert_int64(k3_t0, ==, 1);
		assert_int64(k3_t1, ==, 0);
	}
	return MUNIT_OK;
}

/*
 * count_total, distinct_total and threshold_total must stay correct when
 * one activation receives several input batches: each batch's counts are
 * relative to the trace through the earlier batches.  Seal several epochs
 * before stepping, then compare every key's accumulated output with the
 * from-scratch answer.  This caught the per-batch cursor while arrange kept
 * one capability per time and sealed a batch per epoch.  Arrange now holds
 * an antichain (CapabilitySet), so a total order seals one batch per
 * activation and this case no longer queues several; it still checks the
 * operators' answers across epochs.
 */
static collected
run_total_queued(which which_op)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_cdt c;
	collected out;
	unsigned char k[8];
	uint64_t t;

	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.w = which_op;
	dtfl_worker_dataflow(w, "dd", tc, build_cdt_fn, &c);

	/* Epoch t inserts key 5 once; no stepping until all four are sealed. */
	for (t = 0; t < 4; t++) {
		uint64_t next = t + 1;

		dtfl_put_u64(k, 5);
		dtfl_input_session_insert(c.in, k, sizeof k);
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return out;
}

DTFL_TEST(reduce__total_ops_queued_batches)
{
	collected out;
	uint64_t t, v;

	(void)params; (void)data;
	/* count_total: key 5 has count t+1 as of epoch t, and only that. */
	out = run_total_queued(W_COUNT_T);
	for (t = 0; t < 4; t++)
		for (v = 1; v <= 4; v++)
			assert_int64(acc_i(&out, 5, (int64_t)v, t), ==,
			    (int64_t)(v == t + 1 ? 1 : 0));
	/* distinct_total: key 5 present once from epoch 0 on. */
	out = run_total_queued(W_DISTINCT_T);
	for (t = 0; t < 4; t++)
		assert_int64(acc_key(&out, 5, t), ==, 1);
	/* threshold_total (even, non-zero): present at counts 2 and 4. */
	out = run_total_queued(W_THRESH_T);
	assert_int64(acc_key(&out, 5, 0), ==, 0);
	assert_int64(acc_key(&out, 5, 1), ==, 1);
	assert_int64(acc_key(&out, 5, 2), ==, 0);
	assert_int64(acc_key(&out, 5, 3), ==, 1);
	return MUNIT_OK;
}

DTFL_TEST(reduce__distinct_total_matches)
{
	collected out;

	(void)params; (void)data;
	out = run_cdt(W_DISTINCT_T);
	assert_int64(acc_key(&out, 2, 0), ==, 1);
	assert_int64(acc_key(&out, 3, 0), ==, 1);
	assert_int64(acc_key(&out, 2, 1), ==, 1);
	assert_int64(acc_key(&out, 3, 1), ==, 0);
	return MUNIT_OK;
}

DTFL_TEST(reduce__threshold_total_matches)
{
	collected out;

	(void)params; (void)data;
	out = run_cdt(W_THRESH_T);
	assert_int64(acc_key(&out, 2, 0), ==, 1);
	assert_int64(acc_key(&out, 2, 1), ==, 0);
	assert_int64(acc_key(&out, 3, 0), ==, 0);
	assert_int64(acc_key(&out, 3, 1), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(reduce__min_across_epochs),
	DTFL_T(reduce__count_across_epochs),
	DTFL_T(reduce__distinct_across_epochs),
	DTFL_T(reduce__threshold_across_epochs),
	DTFL_T(reduce__count_total_matches),
	DTFL_T(reduce__total_ops_queued_batches),
	DTFL_T(reduce__distinct_total_matches),
	DTFL_T(reduce__threshold_total_matches),
	DTFL_T_END
};

DTFL_TEST_MAIN("unit/reduce", tests)
