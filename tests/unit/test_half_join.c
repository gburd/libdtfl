/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_half_join.c: the half-join and its total-order timing.
 *
 * A stream of (key, val1, time1) updates is half-joined against an arranged
 * (key -> val2) trace in an AltNeu scope.  The streamed record carries its
 * own time as the time1 field, as upstream's half_join expects (the time is
 * hoisted into the record).  The test checks that the join finds the
 * matching value, and that the strict and non-strict comparisons differ on
 * a trace entry at a time equal to the update's initial time: strict
 * ignores the concurrent entry, non-strict sees it.  That difference is the
 * AltNeu timing the delta query relies on.
 */
#include "dtfl_test.h"

#include "dd/arrange_int.h"
#include "dd/collection_int.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dogs.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 256

typedef struct out_rec {
	uint32_t k, v, w;
	uint64_t jt;		/* joined time (encoded AltNeu word) */
	int64_t diff;
} out_rec;

typedef struct collected {
	out_rec r[MAX_OUT];
	size_t n;
} collected;

/* The streamed record is k||v||time1; the output record is k||v||w. */
static void
out_kvw(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, void *dst, size_t cap,
    size_t *out_len)
{
	unsigned char *p = dst;

	(void)ctx; (void)klen; (void)v1len; (void)v2len; (void)cap;
	memcpy(p, key, 4);		/* k */
	memcpy(p + 4, val1, 4);		/* v1 */
	memcpy(p + 8, val2, 4);		/* v2 (w) */
	*out_len = 12;
}

static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	const unsigned char *p = rec;
	uint64_t jt;

	(void)len;
	if (c->n >= MAX_OUT)
		return;
	/* The output record is k||v||w followed by the joined time word. */
	c->r[c->n].k = dtfl_get_u32(p);
	c->r[c->n].v = dtfl_get_u32(p + 4);
	c->r[c->n].w = dtfl_get_u32(p + 8);
	memcpy(&jt, p + 12, sizeof jt);		/* joined time in the record */
	c->r[c->n].jt = jt;
	(void)time;
	memcpy(&c->r[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

typedef struct hj_ctx {
	dtfl_collection *stream;	/* (k, v, time1) outer records */
	dtfl_collection *base;		/* (k, w) outer records */
	dtfl_collection *result;
	const dtfl_type *kt;		/* u32 */
	const dtfl_type *v1t;		/* u32 */
	const dtfl_type *out_type;	/* tuple(u32,u32,u32) */
	bool strict;
} hj_ctx;

static void
build_inner(dtfl_scope *inner, void *arg)
{
	hj_ctx *c = arg;
	dtfl_collection *stream = dtfl_collection_enter(c->stream, inner);
	dtfl_collection *base = dtfl_collection_enter(c->base, inner);
	dtfl_arranged *arr = __dtfl_collection_arrange_by_key(base, c->kt,
	    c->v1t, "ByKey");
	dtfl_collection *joined = dtfl_dogs_half_join(stream, arr, c->kt,
	    c->v1t, c->out_type, c->strict, out_kvw, DTFL_NOCTX);

	c->result = dtfl_collection_leave(joined);

	dtfl_arranged_free(arr);
	dtfl_collection_free(joined);
	dtfl_collection_free(stream);
	dtfl_collection_free(base);
}

typedef struct build_ctx {
	dtfl_input_session *in_stream;	/* k||v||time1 records */
	dtfl_input_session *in_base;	/* k||w records */
	dtfl_probe *probe;
	collected *out;
	hj_ctx *hc;
} build_ctx;

static void
build_dataflow(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *stream = dtfl_input_session_to_collection(c->in_stream,
	    scope);
	dtfl_collection *base = dtfl_input_session_to_collection(c->in_base,
	    scope);
	dtfl_time_class *base_tc = dtfl_time_u64();
	dtfl_time_class *altneu = dtfl_time_altneu(base_tc);
	dtfl_collection *ins, *probed;

	c->hc->stream = stream;
	c->hc->base = base;
	c->hc->result = NULL;
	dtfl_scope_scoped(scope, "Inner", altneu, build_inner, c->hc);
	dtfl_time_class_free(altneu);
	dtfl_time_class_free(base_tc);

	ins = dtfl_collection_inspect(c->hc->result, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(c->hc->result);
	dtfl_collection_free(stream);
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

/*
 * Both the stream (k,v,time1) and the base (k,w) carry time1 = 0, so the
 * matching trace entry is at a time equal to the update's initial time.
 * The joined time is the AltNeu join of the two alt(0) times, which is
 * alt(0) encoded as 0.  Strict ignores the concurrent entry (no output);
 * non-strict sees it (one output).
 */
static size_t
run(bool strict)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *trip[3] = { dtfl_type_u32(), dtfl_type_u32(),
	    dtfl_type_u32() };
	const dtfl_type *streamf[3] = { dtfl_type_u32(), dtfl_type_u32(),
	    dtfl_type_u64() };
	const dtfl_type *streamt = dtfl_type_tuple(streamf, 3); /* k,v,time1 */
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());		/* k, w */
	const dtfl_type *out_type = dtfl_type_tuple(trip, 3); /* k, v, w */
	build_ctx c;
	hj_ctx hc;
	collected out;
	unsigned char rec[16];
	uint64_t next;
	probe_cond pc;
	size_t count;

	memset(&out, 0, sizeof out);
	memset(&hc, 0, sizeof hc);
	hc.kt = u32;
	hc.v1t = u32;
	hc.out_type = out_type;
	hc.strict = strict;
	/* The stream record is k||v||time1, time1 an 8-byte AltNeu word (0). */
	c.in_stream = dtfl_input_session_new(streamt, r);
	c.in_base = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.hc = &hc;
	assert_int(dtfl_worker_dataflow(w, "hj", tc, build_dataflow, &c), ==, 0);

	/* stream: (k=1, v=100, time1=alt(0)=0). */
	dtfl_put_u32(rec, 1);
	dtfl_put_u32(rec + 4, 100);
	memset(rec + 8, 0, 8);
	dtfl_input_session_insert(c.in_stream, rec, 16);
	/* base: (k=1, w=200). */
	dtfl_put_u32(rec, 1);
	dtfl_put_u32(rec + 4, 200);
	dtfl_input_session_insert(c.in_base, rec, 8);

	next = 1;
	dtfl_input_session_advance_to(c.in_stream, &next);
	dtfl_input_session_advance_to(c.in_base, &next);
	dtfl_input_session_flush(c.in_stream);
	dtfl_input_session_flush(c.in_base);
	pc.probe = c.probe;
	pc.target = next;
	dtfl_worker_step_while(w, probe_behind, &pc);
	dtfl_input_session_close(c.in_stream);
	dtfl_input_session_close(c.in_base);
	while (dtfl_worker_step(w))
		;

	count = 0;
	{
		size_t i;

		for (i = 0; i < out.n; i++)
			if (out.r[i].diff > 0) {
				count++;
				/* The matched pair is (1, 100, 200). */
				assert_uint32(out.r[i].k, ==, 1);
				assert_uint32(out.r[i].v, ==, 100);
				assert_uint32(out.r[i].w, ==, 200);
			}
	}

	dtfl_probe_free(c.probe);
	dtfl_type_free(streamt);
	dtfl_type_free(pair);
	dtfl_type_free(out_type);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return count;
}

/* Non-strict sees the concurrent (equal-time) trace entry: one output. */
DTFL_TEST(half_join__nonstrict_sees_concurrent)
{
	(void)params; (void)data;
	assert_size(run(false), ==, 1);
	return MUNIT_OK;
}

/* Strict ignores the concurrent entry: no output. */
DTFL_TEST(half_join__strict_ignores_concurrent)
{
	(void)params; (void)data;
	assert_size(run(true), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(half_join__nonstrict_sees_concurrent),
	DTFL_T(half_join__strict_ignores_concurrent),
	DTFL_T_END
};

DTFL_TEST_MAIN("half_join", tests)
