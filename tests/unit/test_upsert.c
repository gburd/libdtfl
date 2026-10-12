/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_upsert.c: arrange_from_upsert, last-writer-wins per key.
 *
 * A worker feeds a stream of keyed upserts (key, Option<value>, time) and
 * reads the resulting arrangement back as a collection.  The operator must
 * turn the upsert sequence into the differences a collection needs: each
 * change in a key's value retracts the old value and installs the new, a
 * None uninstalls the key, and a repeated value produces nothing.  Ties in
 * time take the greatest Option<value> (None < Some).  This ports the
 * sequence from upsert.rs's doc example (one key overwritten and removed
 * across epochs) plus a same-time tie.
 */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 256

/* An output update: (key, value, time, diff). */
typedef struct out_upd {
	uint64_t key, val, time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAX_OUT];
	size_t n;
} collected;

/* The arrangement's records are key||value (both u64). */
static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;

	(void)len;
	if (c->n >= MAX_OUT)
		return;
	c->u[c->n].key = dtfl_get_u64(rec);
	c->u[c->n].val = dtfl_get_u64((const unsigned char *)rec + 8);
	c->u[c->n].time = time[0];
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

static int64_t
out_diff(const collected *c, uint64_t key, uint64_t val, uint64_t time)
{
	size_t i;

	for (i = 0; i < c->n; i++)
		if (c->u[i].key == key && c->u[i].val == val &&
		    c->u[i].time == time)
			return c->u[i].diff;
	return 0;
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
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

/* The upsert record type: key(u64) || option(u64). */
static const dtfl_type *
upsert_record_type(void)
{
	static const dtfl_type *rt;

	if (rt == NULL) {
		const dtfl_type *fields[2];

		fields[0] = dtfl_type_u64();
		fields[1] = dtfl_type_option(dtfl_type_u64());
		rt = dtfl_type_tuple(fields, 2);
	}
	return rt;
}

/* The arrangement reads as key||value. */
static const dtfl_type *
kv_record_type(void)
{
	static const dtfl_type *rt;

	if (rt == NULL)
		rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	return rt;
}

/* Build one upsert record key||option(val) into buf; returns its length. */
static size_t
make_upsert(unsigned char *buf, uint64_t key, bool some, uint64_t val)
{
	size_t n = 0;

	dtfl_put_u64(buf, key);
	n = 8;
	buf[n++] = some ? 1 : 0;
	if (some) {
		dtfl_put_u64(buf + n, val);
		n += 8;
	}
	return n;
}

/* Emits the stored key||value as the output record (key and val are u64). */
static void
kv_emit(void *ctx, const void *key, size_t klen, const void *val, size_t vlen,
    dtfl_arranged_emit *out)
{
	unsigned char buf[16];

	(void)ctx;
	memcpy(buf, key, klen);
	memcpy(buf + klen, val, vlen);
	dtfl_arranged_emit_record(out, buf, klen + vlen);
}

static void
build_upsert(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_arranged *arr = dtfl_arrange_from_upsert(base, dtfl_type_u64(),
	    dtfl_type_u64(), "Upsert");
	dtfl_collection *coll = dtfl_arranged_as_collection(arr,
	    kv_record_type(), kv_emit, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(coll, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(coll);
	dtfl_arranged_free(arr);
	dtfl_collection_free(base);
}

/* Sends one upsert at the session's current epoch. */
static void
send_upsert(dtfl_input_session *in, uint64_t key, bool some, uint64_t val)
{
	unsigned char buf[32];
	size_t len = make_upsert(buf, key, some, val);

	dtfl_input_session_update(in, buf, len, &(int64_t){ 1 });
}

static void
advance(dtfl_worker *w, build_ctx *c, uint64_t to)
{
	probe_cond pc = { c->probe, to };

	dtfl_input_session_advance_to(c->in, &to);
	dtfl_input_session_flush(c->in);
	dtfl_worker_step_while(w, probe_behind, &pc);
}

/*
 * One key overwritten and removed over time:
 *   t3: Some(100)   install 100
 *   t4: Some(200)   retract 100, install 200
 *   t5: None        retract 200
 *   t9: Some(300)   install 300
 *   t11: Some(300)  repeat, no change
 *   t15: None       retract 300
 */
DTFL_TEST(upsert__overwrite_and_remove)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(upsert_record_type(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_upsert, &c), ==, 0);

	/* The session epoch starts at 0; place each upsert at its time. */
	{
		uint64_t t;

		t = 3; dtfl_input_session_advance_to(c.in, &t);
		send_upsert(c.in, 1, true, 100);
		t = 4; dtfl_input_session_advance_to(c.in, &t);
		send_upsert(c.in, 1, true, 200);
		t = 5; dtfl_input_session_advance_to(c.in, &t);
		send_upsert(c.in, 1, false, 0);
		t = 9; dtfl_input_session_advance_to(c.in, &t);
		send_upsert(c.in, 1, true, 300);
		t = 11; dtfl_input_session_advance_to(c.in, &t);
		send_upsert(c.in, 1, true, 300);
		t = 15; dtfl_input_session_advance_to(c.in, &t);
		send_upsert(c.in, 1, false, 0);
	}
	advance(w, &c, 16);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* Install 100 at 3, swap to 200 at 4, remove at 5. */
	assert_int64(out_diff(&out, 1, 100, 3), ==, 1);
	assert_int64(out_diff(&out, 1, 100, 4), ==, -1);
	assert_int64(out_diff(&out, 1, 200, 4), ==, 1);
	assert_int64(out_diff(&out, 1, 200, 5), ==, -1);
	/* Install 300 at 9, no change at 11, remove at 15. */
	assert_int64(out_diff(&out, 1, 300, 9), ==, 1);
	assert_int64(out_diff(&out, 1, 300, 11), ==, 0);
	assert_int64(out_diff(&out, 1, 300, 15), ==, -1);
	/* No zero diffs recorded. */
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

/*
 * Tie rule: two upserts to one key at the same time take the greatest
 * Option<value>.  At t2, Some(5) and Some(9) tie; the greatest, 9, wins.
 */
DTFL_TEST(upsert__tie_takes_greatest)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	collected out;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(upsert_record_type(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build_upsert, &c), ==, 0);

	{
		uint64_t t = 2;

		dtfl_input_session_advance_to(c.in, &t);
		send_upsert(c.in, 7, true, 5);
		send_upsert(c.in, 7, true, 9);	/* same time: greatest wins */
	}
	advance(w, &c, 3);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* Only value 9 is installed at t2; 5 never appears. */
	assert_int64(out_diff(&out, 7, 9, 2), ==, 1);
	assert_int64(out_diff(&out, 7, 5, 2), ==, 0);
	assert_size(out.n, ==, 1);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(upsert__overwrite_and_remove),
	DTFL_T(upsert__tie_takes_greatest),
	DTFL_T_END
};

DTFL_TEST_MAIN("upsert", tests)
