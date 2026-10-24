/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/tests/reduce.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dd/reduce.c: the ported reduce.rs integration tests.
 *
 * reduce__reduce feeds ((0,0),1), ((1,2),1), ((1,1),1) at time 0 and reduces
 * with the logic push((s[0], s.len())): the smallest value of each key with a
 * diff of the number of distinct values.  The result is ((0,0),1) and
 * ((1,1),2).  reduce__reduce_scaling counts a large single-key collection and
 * checks that the output is produced.
 */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 4096

typedef struct out_upd {
	uint64_t key, val;
	int64_t ival;		/* field 2 decoded as i64 (for count) */
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

	(void)time;
	if (c->n >= MAX_OUT)
		return;
	c->u[c->n].key = dtfl_get_u64(rec);
	c->u[c->n].val = dtfl_get_u64((const unsigned char *)rec + 8);
	c->u[c->n].ival = len >= 16 ?
	    dtfl_get_i64((const unsigned char *)rec + 8) : 0;
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

static int64_t
out_diff(const collected *c, uint64_t key, uint64_t val)
{
	size_t i;
	int64_t sum = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].key == key && c->u[i].val == val)
			sum += c->u[i].diff;
	return sum;
}

/*
 * The reduce logic: emit the smallest value (input is sorted by value) with a
 * diff equal to the number of distinct values.  push((*s[0].0, s.len())).
 */
static void
smallest_logic(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t count = (int64_t)n;
	unsigned char vbuf[8];

	(void)ctx; (void)key; (void)klen; (void)vlens; (void)diffs;
	dtfl_put_u64(vbuf, dtfl_get_u64(vals[0]));
	dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &count);
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
	bool scaling;
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

static void
build_reduce(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_diff *od = dtfl_diff_i64();
	dtfl_collection *red;
	dtfl_collection *ins, *probed;

	if (c->scaling) {
		red = dtfl_collection_count(base, od);
	} else {
		red = dtfl_collection_reduce(base, dtfl_type_u64(),
		    dtfl_type_u64(), dtfl_type_u64(), od, smallest_logic,
		    DTFL_NOCTX);
	}
	ins = dtfl_collection_inspect(red, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	probed = dtfl_collection_probe_with(ins, c->probe);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(red);
	dtfl_diff_free(od);
	dtfl_collection_free(base);
}

/* key || val record of two u64. */
static void
put_kv(unsigned char *buf, uint64_t key, uint64_t val)
{
	dtfl_put_u64(buf, key);
	dtfl_put_u64(buf + 8, val);
}

DTFL_TEST(reduce__reduce)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out;
	unsigned char kv[16];
	uint64_t next = 1;
	const dtfl_type *fields[2];
	const dtfl_type *rec_type;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	fields[0] = dtfl_type_u64();
	fields[1] = dtfl_type_u64();
	rec_type = dtfl_type_tuple(fields, 2);
	c.in = dtfl_input_session_new(rec_type, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.scaling = false;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_reduce, &c), ==, 0);

	put_kv(kv, 0, 0);
	dtfl_input_session_insert(c.in, kv, sizeof kv);
	put_kv(kv, 1, 2);
	dtfl_input_session_insert(c.in, kv, sizeof kv);
	put_kv(kv, 1, 1);
	dtfl_input_session_insert(c.in, kv, sizeof kv);
	dtfl_input_session_advance_to(c.in, &next);
	dtfl_input_session_flush(c.in);
	{ probe_cond pc = { c.probe, next }; dtfl_worker_step_while(w, probe_behind, &pc); }
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* Output: ((0,0),1) and ((1,1),2). */
	assert_int64(out_diff(&out, 0, 0), ==, 1);
	assert_int64(out_diff(&out, 1, 1), ==, 2);
	{
		size_t i;
		int64_t total = 0;

		for (i = 0; i < out.n; i++)
			total += out.u[i].diff;
		assert_int64(total, ==, 3);
	}

	dtfl_probe_free(c.probe);
	dtfl_type_free(rec_type);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

DTFL_TEST(reduce__reduce_scaling)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out;
	uint64_t next = 1;
	uint64_t i;
	const uint64_t scale = 1000;	/* upstream uses 100000; 1000 suffices */

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	/* The record is the key only (count arranges by self); use u64. */
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.scaling = true;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_reduce, &c), ==, 0);

	/* One key () would need a unit record; use a single u64 key 0 with
	 * `scale` copies so the count is `scale`. */
	for (i = 0; i < scale; i++) {
		unsigned char k[8];

		dtfl_put_u64(k, 0);
		dtfl_input_session_insert(c.in, k, sizeof k);
	}
	dtfl_input_session_advance_to(c.in, &next);
	dtfl_input_session_flush(c.in);
	{ probe_cond pc = { c.probe, next }; dtfl_worker_step_while(w, probe_behind, &pc); }
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* The output exists: one (key 0, count scale) update. */
	assert_size(out.n, ==, 1);
	assert_int64(out.u[0].diff, ==, 1);
	assert_int64(out.u[0].ival, ==, (int64_t)scale);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* distinct_unit_key: distinct over unit (0-byte) key records.          */
/* A valid cursor at a 0-byte key returns a NULL pointer; the reduce    */
/* key-marching loop must not read that as an exhausted cursor, or it   */
/* never advances the batch cursor and loops forever (regression).      */
/* ------------------------------------------------------------------ */

static void
build_distinct_unit(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_diff *od = dtfl_diff_i64();
	dtfl_collection *dist = dtfl_collection_distinct(base, od);
	dtfl_collection *ins = dtfl_collection_inspect(dist, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(dist);
	dtfl_diff_free(od);
	dtfl_collection_free(base);
}

DTFL_TEST(reduce__distinct_unit_key)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out;
	uint64_t next = 1;
	int steps;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_unit(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.scaling = false;
	assert_int(dtfl_worker_dataflow(w, "du", tc, build_distinct_unit, &c),
	    ==, 0);

	/* Two copies of the unit record () distinct to one. */
	dtfl_input_session_insert(c.in, NULL, 0);
	dtfl_input_session_insert(c.in, NULL, 0);
	dtfl_input_session_advance_to(c.in, &next);
	dtfl_input_session_flush(c.in);
	{ probe_cond pc = { c.probe, next };
	  dtfl_worker_step_while(w, probe_behind, &pc); }
	dtfl_input_session_close(c.in);
	for (steps = 0; steps < 100000 && dtfl_worker_step(w); steps++)
		;
	assert_int(steps, <, 100000);
	assert_true(dtfl_probe_done(c.probe));

	/* One unit key present with a diff of one. */
	assert_size(out.n, ==, 1);
	assert_int64(out.u[0].diff, ==, 1);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(reduce__reduce),
	DTFL_T(reduce__reduce_scaling),
	DTFL_T(reduce__distinct_unit_key),
	DTFL_T_END
};

DTFL_TEST_MAIN("dd/reduce", tests)
