/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_arrange_by_key.c: the public arrange_by_key family.
 *
 * These tests exercise the public forwarders over the internal arrange /
 * join / reduce entry points, proving that the public wrappers reproduce
 * the internal behavior:
 *   - arrange two keyed collections by key, join_core them, and confirm the
 *     result equals a reference equijoin computed by hand (the pattern from
 *     tests/upstream/dd/join.c);
 *   - arrange one keyed collection by key, reduce_core it to a per-key sum
 *     of the values, and confirm the output against a reference.
 * The test uses only the public dtfl_* API, so it also demonstrates the API
 * is complete for building these pipelines without the internals.
 */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 256

typedef struct out_upd {
	unsigned char rec[64];
	size_t rlen;
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
	uint64_t t;

	if (c->n >= MAX_OUT || len > sizeof c->u[0].rec)
		return;
	memcpy(c->u[c->n].rec, rec, len);
	c->u[c->n].rlen = len;
	memcpy(&t, time, sizeof t);
	c->u[c->n].time = t;
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* Net diff for a record (bytes) at a time, summed over the output. */
static int64_t
out_diff(const collected *c, const void *rec, size_t rlen, uint64_t time)
{
	size_t i;
	int64_t sum = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].rlen == rlen &&
		    memcmp(c->u[i].rec, rec, rlen) == 0 &&
		    c->u[i].time == time)
			sum += c->u[i].diff;
	return sum;
}

/* The number of distinct (record, time) entries with a non-zero net diff. */
static size_t
out_distinct(const collected *c)
{
	size_t i, j, n = 0;

	for (i = 0; i < c->n; i++) {
		int seen = 0;

		for (j = 0; j < i; j++)
			if (c->u[j].rlen == c->u[i].rlen &&
			    memcmp(c->u[j].rec, c->u[i].rec, c->u[i].rlen) == 0 &&
			    c->u[j].time == c->u[i].time) {
				seen = 1;
				break;
			}
		if (!seen &&
		    out_diff(c, c->u[i].rec, c->u[i].rlen, c->u[i].time) != 0)
			n++;
	}
	return n;
}

static void
put_u64(unsigned char *buf, size_t *off, uint64_t v)
{
	*off += dtfl_put_u64(buf + *off, v);
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

/* ------------------------------------------------------------------ */
/* join_core: arrange two (u64 key, u64 val) collections and join.     */
/* ------------------------------------------------------------------ */

/* result: emit key || val1 || val2 as three u64 fields. */
static void
join_result(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[24];
	size_t off = 0;

	(void)ctx; (void)klen; (void)v1len; (void)v2len;
	memcpy(buf + off, key, 8); off += 8;
	memcpy(buf + off, v1, 8); off += 8;
	memcpy(buf + off, v2, 8); off += 8;
	dtfl_join_emit_record(out, buf, off);
}

typedef struct join_build {
	dtfl_input_session *in1;
	dtfl_input_session *in2;
	dtfl_probe *probe;
	collected *out;
} join_build;

static void
build_join_core(dtfl_scope *scope, void *arg)
{
	join_build *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *fields[3] = { u64, u64, u64 };
	const dtfl_type *out_type = dtfl_type_tuple(fields, 3);
	dtfl_collection *c1 = dtfl_input_session_to_collection(c->in1, scope);
	dtfl_collection *c2 = dtfl_input_session_to_collection(c->in2, scope);
	dtfl_arranged *a1 = dtfl_collection_arrange_by_key(c1, u64, u64,
	    "Arrange1");
	dtfl_arranged *a2 = dtfl_collection_arrange_by_key(c2, u64, u64,
	    "Arrange2");
	dtfl_collection *j = dtfl_arranged_join_core(a1, a2, out_type,
	    join_result, DTFL_NOCTX);
	dtfl_collection *con = dtfl_collection_consolidate(j);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_type_free(out_type);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(j);
	dtfl_arranged_free(a1);
	dtfl_arranged_free(a2);
	dtfl_collection_free(c1);
	dtfl_collection_free(c2);
}

DTFL_TEST(abk__join_core)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	join_build c;
	collected out;
	probe_cond pc;
	unsigned char rec[32];
	size_t off;
	uint64_t next = 1;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in1 = dtfl_input_session_new(pair, r);
	c.in2 = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "jc", tc, build_join_core, &c), ==,
	    0);

	/*
	 * left  = {(0,10), (0,11), (1,12), (2,13)}
	 * right = {(0,20), (1,21), (1,22)}
	 * equijoin by key -> (key, l, r):
	 *   key 0: (0,10,20), (0,11,20)
	 *   key 1: (1,12,21), (1,12,22)
	 *   key 2: no match
	 */
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 10);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 11);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 12);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 2); put_u64(rec, &off, 13);
	dtfl_input_session_insert(c.in1, rec, off);

	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 20);
	dtfl_input_session_insert(c.in2, rec, off);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 21);
	dtfl_input_session_insert(c.in2, rec, off);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 22);
	dtfl_input_session_insert(c.in2, rec, off);

	dtfl_input_session_advance_to(c.in1, &next);
	dtfl_input_session_advance_to(c.in2, &next);
	dtfl_input_session_flush(c.in1);
	dtfl_input_session_flush(c.in2);
	pc.probe = c.probe; pc.target = next;
	dtfl_worker_step_while(w, probe_behind, &pc);
	dtfl_input_session_close(c.in1);
	dtfl_input_session_close(c.in2);
	while (dtfl_worker_step(w))
		;

	/* Reference equijoin: the four matched triples, each with diff +1. */
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 10);
	put_u64(rec, &off, 20);
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 11);
	put_u64(rec, &off, 20);
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 12);
	put_u64(rec, &off, 21);
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 12);
	put_u64(rec, &off, 22);
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	/* key 2 did not match: no triple with leading key 2. */
	off = 0; put_u64(rec, &off, 2); put_u64(rec, &off, 13);
	put_u64(rec, &off, 0);
	assert_int64(out_diff(&out, rec, off, 0), ==, 0);
	/* Exactly four distinct non-zero output records. */
	assert_size(out_distinct(&out), ==, 4);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* reduce_core: arrange a (u64 key, u64 val) collection and reduce.    */
/* ------------------------------------------------------------------ */

/*
 * reduce_core logic: sum the input values (scaled by their diffs) into a
 * single output value (i64).  reduce_core shows the tentative accumulated
 * output; it does not retract it automatically, so this logic retracts the
 * tentative output (diff -1 each) and emits the fresh sum with diff +1.
 */
static void
sum_logic(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, dtfl_reduce_emit *out)
{
	int64_t sum = 0;
	size_t i;
	const int64_t *idiffs = in_diffs;
	const int64_t *odiffs = out_diffs;
	unsigned char vbuf[8];
	int64_t neg;

	(void)ctx; (void)key; (void)klen; (void)in_vlens;

	/* Retract every tentative output entry. */
	for (i = 0; i < m; i++) {
		neg = -odiffs[i];
		dtfl_reduce_emit_pair(out, out_vals[i], out_vlens[i], &neg);
	}
	/* The new output value: the diff-weighted sum of the input values. */
	for (i = 0; i < n; i++)
		sum += (int64_t)dtfl_get_u64(in_vals[i]) * idiffs[i];
	dtfl_put_i64(vbuf, sum);
	neg = 1;
	dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &neg);
}

typedef struct reduce_build {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
} reduce_build;

/* as_collection kv fn: emit key || out_val as the output record. */
static void
kv_concat(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	unsigned char rec[32];

	(void)ctx;
	memcpy(rec, key, klen);
	memcpy(rec + klen, val, vlen);
	dtfl_arranged_emit_record(out, rec, klen + vlen);
}

static void
build_reduce_core(dtfl_scope *scope, void *arg)
{
	reduce_build *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *i64 = dtfl_type_i64();
	dtfl_diff *odiff = dtfl_diff_i64();
	const dtfl_type *out_rec = dtfl_type_pair(u64, i64);
	dtfl_collection *coll = dtfl_input_session_to_collection(c->in, scope);
	dtfl_arranged *a = dtfl_collection_arrange_by_key(coll, u64, u64,
	    "ArrangeRed");
	dtfl_arranged *red = dtfl_arranged_reduce_core(a, "SumReduce", i64,
	    odiff, sum_logic, DTFL_NOCTX);
	dtfl_collection *out = dtfl_arranged_as_collection(red, out_rec,
	    kv_concat, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(out, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_diff_free(odiff);
	dtfl_type_free(out_rec);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(out);
	dtfl_arranged_free(red);
	dtfl_arranged_free(a);
	dtfl_collection_free(coll);
}

DTFL_TEST(abk__reduce_core)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	reduce_build c;
	collected out;
	probe_cond pc;
	unsigned char rec[32];
	size_t off;
	uint64_t next = 1;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "rc", tc, build_reduce_core, &c),
	    ==, 0);

	/*
	 * input = {(0,10), (0,11), (1,12), (1,20), (2,13)}
	 * per-key sum of values:
	 *   key 0: 21, key 1: 32, key 2: 13
	 */
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 10);
	dtfl_input_session_insert(c.in, rec, off);
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 11);
	dtfl_input_session_insert(c.in, rec, off);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 12);
	dtfl_input_session_insert(c.in, rec, off);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 20);
	dtfl_input_session_insert(c.in, rec, off);
	off = 0; put_u64(rec, &off, 2); put_u64(rec, &off, 13);
	dtfl_input_session_insert(c.in, rec, off);

	dtfl_input_session_advance_to(c.in, &next);
	dtfl_input_session_flush(c.in);
	pc.probe = c.probe; pc.target = next;
	dtfl_worker_step_while(w, probe_behind, &pc);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* Reference: (key, sum) with diff +1 at time 0. */
	off = 0; put_u64(rec, &off, 0); off += dtfl_put_i64(rec + off, 21);
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	off = 0; put_u64(rec, &off, 1); off += dtfl_put_i64(rec + off, 32);
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	off = 0; put_u64(rec, &off, 2); off += dtfl_put_i64(rec + off, 13);
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	assert_size(out_distinct(&out), ==, 3);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(abk__join_core),
	DTFL_T(abk__reduce_core),
	DTFL_T_END
};

DTFL_TEST_MAIN("arrange_by_key", tests)
