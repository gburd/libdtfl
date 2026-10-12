/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/tests/join.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dd/join.c: the upstream join test suite.
 *
 * join, join_map, semijoin and antijoin over small fixed collections,
 * reproducing the Rust test bodies: a worker builds two input collections,
 * joins them, inspects the (record, time, diff) output, and the test
 * asserts the consolidated multiset.  join_scale_* exercise the operator at
 * scale through count(); count lands with the reduce operator, so they
 * report SKIP until it is available (AGENTS.md: a missing prerequisite is
 * SKIP, not a false pass).
 */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
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
	/* The embedded time may be unaligned after a variable-length record. */
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
		if (c->u[i].rlen == rlen && memcmp(c->u[i].rec, rec, rlen) == 0 &&
		    c->u[i].time == time)
			sum += c->u[i].diff;
	return sum;
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

/* Appends a u64 to buf at *off. */
static void
put_u64(unsigned char *buf, size_t *off, uint64_t v)
{
	*off += dtfl_put_u64(buf + *off, v);
}

/* ------------------------------------------------------------------ */
/* Shared build: two input sessions, join via `build_join`, inspect.   */
/* ------------------------------------------------------------------ */

typedef dtfl_collection *(*join_fn)(dtfl_collection *c1, dtfl_collection *c2);

typedef struct build_ctx {
	dtfl_input_session *in1;
	dtfl_input_session *in2;
	dtfl_probe *probe;
	collected *out;
	join_fn join;
} build_ctx;

static void
build_join(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *c1 = dtfl_input_session_to_collection(c->in1, scope);
	dtfl_collection *c2 = dtfl_input_session_to_collection(c->in2, scope);
	dtfl_collection *j = c->join(c1, c2);
	dtfl_collection *ins = dtfl_collection_inspect(j, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(j);
	dtfl_collection_free(c1);
	dtfl_collection_free(c2);
}

/* Runs one epoch to completion. */
static void
run_epoch(dtfl_worker *w, build_ctx *c, uint64_t next)
{
	probe_cond pc = { c->probe, next };

	dtfl_input_session_advance_to(c->in1, &next);
	dtfl_input_session_advance_to(c->in2, &next);
	dtfl_input_session_flush(c->in1);
	dtfl_input_session_flush(c->in2);
	dtfl_worker_step_while(w, probe_behind, &pc);
}

/* ------------------------------------------------------------------ */
/* join: ((0,0),(1,2)) join ((0,'a'),(1,'B')) == ((0,(0,'a')),(1,(2,'B'))). */
/* ------------------------------------------------------------------ */

static dtfl_collection *
do_join(dtfl_collection *c1, dtfl_collection *c2)
{
	const dtfl_type *fields[3] = { dtfl_type_u64(), dtfl_type_u64(),
	    dtfl_type_u8() };
	const dtfl_type *out_type = dtfl_type_tuple(fields, 3);
	dtfl_collection *j = dtfl_collection_join(c1, c2, dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_u8(), out_type);

	dtfl_type_free(out_type);
	return j;
}

DTFL_TEST(join__join)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *t1 = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	const dtfl_type *t2 = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u8());
	build_ctx c;
	collected out;
	unsigned char rec[32];
	size_t off;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in1 = dtfl_input_session_new(t1, r);
	c.in2 = dtfl_input_session_new(t2, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.join = do_join;
	assert_int(dtfl_worker_dataflow(w, "join", tc, build_join, &c), ==, 0);

	/* col1 = {(0,0), (1,2)}; col2 = {(0,'a'), (1,'B')}. */
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 0);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 2);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 0); off += dtfl_put_u8(rec + off, 'a');
	dtfl_input_session_insert(c.in2, rec, off);
	off = 0; put_u64(rec, &off, 1); off += dtfl_put_u8(rec + off, 'B');
	dtfl_input_session_insert(c.in2, rec, off);
	run_epoch(w, &c, 1);
	dtfl_input_session_close(c.in1);
	dtfl_input_session_close(c.in2);
	while (dtfl_worker_step(w))
		;

	/* Expect (0,(0,'a')) and (1,(2,'B')) each with diff +1 at time 0. */
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 0);
	off += dtfl_put_u8(rec + off, 'a');
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 2);
	off += dtfl_put_u8(rec + off, 'B');
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	assert_size(out.n, ==, 2);

	dtfl_probe_free(c.probe);
	dtfl_type_free(t1);
	dtfl_type_free(t2);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* join_map: produce (k + v1, v2): (0,'a') and (3,'B').                */
/* ------------------------------------------------------------------ */

/* result closure: emit (key + val1) as a u64, then val2 as a u8. */
static void
map_result(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char rec[16];
	size_t off = 0;
	uint64_t sum;

	(void)ctx; (void)klen; (void)v1len; (void)v2len;
	sum = dtfl_get_u64(key) + dtfl_get_u64(v1);
	off += dtfl_put_u64(rec, sum);
	memcpy(rec + off, v2, v2len);
	off += v2len;
	dtfl_join_emit_record(out, rec, off);
}

static dtfl_collection *
do_join_map(dtfl_collection *c1, dtfl_collection *c2)
{
	const dtfl_type *out_type = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u8());
	dtfl_collection *j = dtfl_collection_join_map(c1, c2, dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_u8(), out_type, map_result, DTFL_NOCTX);

	dtfl_type_free(out_type);
	return j;
}

DTFL_TEST(join__join_map)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *t1 = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	const dtfl_type *t2 = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u8());
	build_ctx c;
	collected out;
	unsigned char rec[32];
	size_t off;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in1 = dtfl_input_session_new(t1, r);
	c.in2 = dtfl_input_session_new(t2, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.join = do_join_map;
	assert_int(dtfl_worker_dataflow(w, "jm", tc, build_join, &c), ==, 0);

	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 0);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 2);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 0); off += dtfl_put_u8(rec + off, 'a');
	dtfl_input_session_insert(c.in2, rec, off);
	off = 0; put_u64(rec, &off, 1); off += dtfl_put_u8(rec + off, 'B');
	dtfl_input_session_insert(c.in2, rec, off);
	run_epoch(w, &c, 1);
	dtfl_input_session_close(c.in1);
	dtfl_input_session_close(c.in2);
	while (dtfl_worker_step(w))
		;

	/* Expect (0,'a') and (3,'B') each with diff +1 at time 0. */
	off = 0; put_u64(rec, &off, 0); off += dtfl_put_u8(rec + off, 'a');
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	off = 0; put_u64(rec, &off, 3); off += dtfl_put_u8(rec + off, 'B');
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	assert_size(out.n, ==, 2);

	dtfl_probe_free(c.probe);
	dtfl_type_free(t1);
	dtfl_type_free(t2);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* semijoin: {(0,0),(1,2)} semijoin {0} keeps (0,0).                   */
/* ------------------------------------------------------------------ */

static dtfl_collection *
do_semijoin(dtfl_collection *c1, dtfl_collection *keys)
{
	const dtfl_type *out_type = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_collection *j = dtfl_collection_semijoin(c1, keys, dtfl_type_u64(),
	    dtfl_type_u64(), out_type);

	dtfl_type_free(out_type);
	return j;
}

DTFL_TEST(join__semijoin)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *t1 = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	build_ctx c;
	collected out;
	unsigned char rec[32];
	size_t off;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in1 = dtfl_input_session_new(t1, r);
	c.in2 = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.join = do_semijoin;
	assert_int(dtfl_worker_dataflow(w, "sj", tc, build_join, &c), ==, 0);

	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 0);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 2);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 0);
	dtfl_input_session_insert(c.in2, rec, off);
	run_epoch(w, &c, 1);
	dtfl_input_session_close(c.in1);
	dtfl_input_session_close(c.in2);
	while (dtfl_worker_step(w))
		;

	/* Expect only (0,0), diff +1; (1,2) is discarded. */
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 0);
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 2);
	assert_int64(out_diff(&out, rec, off, 0), ==, 0);
	assert_size(out.n, ==, 1);

	dtfl_probe_free(c.probe);
	dtfl_type_free(t1);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* antijoin: {(0,0),(1,2)} antijoin {0} keeps (1,2).                   */
/* ------------------------------------------------------------------ */

static dtfl_collection *
do_antijoin(dtfl_collection *c1, dtfl_collection *keys)
{
	const dtfl_type *out_type = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_collection *aj = dtfl_collection_antijoin(c1, keys,
	    dtfl_type_u64(), dtfl_type_u64(), out_type);
	dtfl_collection *con = dtfl_collection_consolidate(aj);

	dtfl_type_free(out_type);
	dtfl_collection_free(aj);
	return con;
}

DTFL_TEST(join__antijoin)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *t1 = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	build_ctx c;
	collected out;
	unsigned char rec[32];
	size_t off, i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in1 = dtfl_input_session_new(t1, r);
	c.in2 = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.join = do_antijoin;
	assert_int(dtfl_worker_dataflow(w, "aj", tc, build_join, &c), ==, 0);

	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 0);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 2);
	dtfl_input_session_insert(c.in1, rec, off);
	off = 0; put_u64(rec, &off, 0);
	dtfl_input_session_insert(c.in2, rec, off);
	run_epoch(w, &c, 1);
	dtfl_input_session_close(c.in1);
	dtfl_input_session_close(c.in2);
	while (dtfl_worker_step(w))
		;

	/* Expect only (1,2), diff +1; (0,0) is discarded. */
	off = 0; put_u64(rec, &off, 1); put_u64(rec, &off, 2);
	assert_int64(out_diff(&out, rec, off, 0), ==, 1);
	off = 0; put_u64(rec, &off, 0); put_u64(rec, &off, 0);
	assert_int64(out_diff(&out, rec, off, 0), ==, 0);
	assert_size(out.n, ==, 1);
	for (i = 0; i < out.n; i++)
		assert_int64(out.u[i].diff, !=, 0);

	dtfl_probe_free(c.probe);
	dtfl_type_free(t1);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* join_scale_*: exercise join at scale through count().               */
/* ------------------------------------------------------------------ */

/*
 * The upstream join_scale_* tests build counts with .count(), filter into
 * odds and evens, and join them (the result is empty because at each time a
 * single count is either odd or even, never both).  count() is the reduce
 * operator's, which lands next door; until it is linked these report SKIP
 * rather than a false pass (AGENTS.md section 4).
 */
static MunitResult
join_scale_skip(void)
{
	munit_logf(MUNIT_LOG_INFO, "%s",
	    "SKIP: join_scale needs count() (reduce operator, pending)");
	return MUNIT_SKIP;
}

DTFL_TEST(join__join_scale_1) { (void)params; (void)data; return join_scale_skip(); }
DTFL_TEST(join__join_scale_10) { (void)params; (void)data; return join_scale_skip(); }
DTFL_TEST(join__join_scale_100) { (void)params; (void)data; return join_scale_skip(); }
DTFL_TEST(join__join_scale_1000) { (void)params; (void)data; return join_scale_skip(); }
DTFL_TEST(join__join_scale_10000) { (void)params; (void)data; return join_scale_skip(); }
DTFL_TEST(join__join_scale_100000) { (void)params; (void)data; return join_scale_skip(); }
DTFL_TEST(join__join_scale_1000000) { (void)params; (void)data; return join_scale_skip(); }

static MunitTest tests[] = {
	DTFL_T(join__join),
	DTFL_T(join__join_map),
	DTFL_T(join__semijoin),
	DTFL_T(join__antijoin),
	DTFL_T(join__join_scale_1),
	DTFL_T(join__join_scale_10),
	DTFL_T(join__join_scale_100),
	DTFL_T(join__join_scale_1000),
	DTFL_T(join__join_scale_10000),
	DTFL_T(join__join_scale_100000),
	DTFL_T(join__join_scale_1000000),
	DTFL_T_END
};

DTFL_TEST_MAIN("join", tests)
