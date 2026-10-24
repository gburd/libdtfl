/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_ops_data.c: multiset properties of the record operators.
 *
 * For random u64 inputs fed to a standalone worker:
 *   map    (flat_map +k): the output is the input multiset shifted by k.
 *   filter (even):        the output is the sub-multiset satisfying the
 *                         predicate, order aside.
 *   concat:               the output multiset is the union of the two
 *                         input multisets.
 * Each property builds the dataflow, drains the worker, and compares
 * sorted output values against the expected multiset.
 */
#include "pbt.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>

#define MAX_RECS 128

/* ------------------------------------------------------------------ */
/* Feeder and sink (standalone worker)                                */
/* ------------------------------------------------------------------ */

typedef struct feeder_ctx {
	uint64_t vals[MAX_RECS];
	size_t n;
} feeder_ctx;

typedef struct collected {
	uint64_t *vals;		/* heap; freed by the owning scenario */
	size_t n, cap;
} collected;

static void
collect_push(collected *c, uint64_t v)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 16;
		c->vals = realloc(c->vals, c->cap * sizeof(uint64_t));
	}
	c->vals[c->n++] = v;
}

static bool
feeder_logic(dtfl_op *op, void *vctx)
{
	feeder_ctx *f = vctx;
	dtfl_cap *cap = dtfl_op_initial_cap(op, 0);
	size_t i;
	uint64_t t = 0;

	if (cap == NULL)
		return false;
	for (i = 0; i < f->n; i++) {
		dtfl_cap *at = dtfl_cap_delayed(cap, &t);
		dtfl_session *s = dtfl_op_session(op, 0, at);
		unsigned char buf[8];

		dtfl_put_u64(buf, f->vals[i]);
		dtfl_session_give(s, buf, sizeof buf);
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
	dtfl_stream *out = dtfl_op_builder_output(b, dtfl_type_u64());

	dtfl_op_builder_build(b, feeder_logic, f, NULL);
	return out;
}

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	collected *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			collect_push(c, dtfl_get_u64(rec));
		}
	}
	return false;
}

static void
make_sink(dtfl_stream *in, collected *c)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(in), "Sink");

	dtfl_op_builder_input(b, in, dtfl_pact_pipeline());
	dtfl_op_builder_build(b, sink_logic, c, NULL);
}

/* ------------------------------------------------------------------ */
/* Scenario: a worker plus its result buffers, torn down on case end  */
/* ------------------------------------------------------------------ */

typedef struct scenario {
	dtfl_worker *w;
	dtfl_time_class *tc;
	collected a, b;
} scenario;

static void
scenario_free(void *vp)
{
	scenario *s = vp;

	dtfl_worker_free(s->w);
	dtfl_time_class_free(s->tc);
	free(s->a.vals);
	free(s->b.vals);
	free(s);
}

static scenario *
scenario_new(pbt_tc *tc)
{
	scenario *s = calloc(1, sizeof *s);

	pbt_defer(tc, scenario_free, s);
	s->w = dtfl_worker_new_standalone();
	s->tc = dtfl_time_u64();
	return s;
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void
draw_recs(pbt_tc *tc, feeder_ctx *f, size_t n)
{
	size_t i;

	f->n = n;
	for (i = 0; i < n; i++)
		f->vals[i] = (uint64_t)pbt_int(tc, 0, 1000000);
}

/* ------------------------------------------------------------------ */
/* map: flat_map(+k) shifts the input multiset by k                   */
/* ------------------------------------------------------------------ */

static uint64_t g_shift;	/* read-only during a case */

static void
shift_by(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)time; (void)len;
	dtfl_put_u64(buf, dtfl_get_u64(rec) + g_shift);
	dtfl_emit_record(out, buf, sizeof buf);
}

typedef struct build1 {
	feeder_ctx *feeder;
	collected *out;
} build1;

static void
build_map(dtfl_scope *scope, void *arg)
{
	build1 *c = arg;
	dtfl_stream *s = make_feeder(scope, c->feeder);

	make_sink(dtfl_stream_flat_map(s, dtfl_type_u64(), shift_by, DTFL_NOCTX),
	    c->out);
}

static void
prop_map_shift(pbt_tc *tc)
{
	size_t n = (size_t)pbt_int(tc, 0, MAX_RECS);
	static feeder_ctx feeder;	/* captured by the build callback */
	scenario *s;
	size_t i;

	g_shift = (uint64_t)pbt_int(tc, 0, 1000);
	draw_recs(tc, &feeder, n);
	s = scenario_new(tc);
	{
		build1 c = { &feeder, &s->a };

		PBT_ASSERT(dtfl_worker_dataflow(s->w, "map", s->tc, build_map, &c)
		    == 0);
		while (dtfl_worker_step(s->w))
			;
	}

	PBT_ASSERT(s->a.n == n);
	if (n > 0) {
		qsort(s->a.vals, s->a.n, sizeof(uint64_t), cmp_u64);
		qsort(feeder.vals, feeder.n, sizeof(uint64_t), cmp_u64);
	}
	for (i = 0; i < n; i++)
		PBT_ASSERT(s->a.vals[i] == feeder.vals[i] + g_shift);
}

/* ------------------------------------------------------------------ */
/* filter: output is the even sub-multiset                            */
/* ------------------------------------------------------------------ */

static bool
is_even(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)len;
	return dtfl_get_u64(rec) % 2 == 0;
}

static void
build_filter(dtfl_scope *scope, void *arg)
{
	build1 *c = arg;
	dtfl_stream *s = make_feeder(scope, c->feeder);

	make_sink(dtfl_stream_filter(s, is_even, DTFL_NOCTX), c->out);
}

static void
prop_filter_even(pbt_tc *tc)
{
	size_t n = (size_t)pbt_int(tc, 0, MAX_RECS);
	static feeder_ctx feeder;
	scenario *s;
	size_t i, want = 0;

	draw_recs(tc, &feeder, n);
	for (i = 0; i < n; i++)
		if (feeder.vals[i] % 2 == 0)
			want++;
	s = scenario_new(tc);
	{
		build1 c = { &feeder, &s->a };

		PBT_ASSERT(dtfl_worker_dataflow(s->w, "filter", s->tc, build_filter,
		    &c) == 0);
		while (dtfl_worker_step(s->w))
			;
	}

	PBT_ASSERT(s->a.n == want);
	for (i = 0; i < s->a.n; i++)
		PBT_ASSERT(s->a.vals[i] % 2 == 0);
}

/* ------------------------------------------------------------------ */
/* concat: output multiset is the union of the two inputs             */
/* ------------------------------------------------------------------ */

typedef struct build2 {
	feeder_ctx *fa, *fb;
	collected *out;
} build2;

static void
build_concat(dtfl_scope *scope, void *arg)
{
	build2 *c = arg;
	dtfl_stream *sa = make_feeder(scope, c->fa);
	dtfl_stream *sb = make_feeder(scope, c->fb);

	make_sink(dtfl_stream_concat(sa, sb), c->out);
}

static void
prop_concat_union(pbt_tc *tc)
{
	size_t na = (size_t)pbt_int(tc, 0, MAX_RECS / 2);
	size_t nb = (size_t)pbt_int(tc, 0, MAX_RECS / 2);
	static feeder_ctx fa, fb;
	uint64_t want[MAX_RECS];
	scenario *s;
	size_t i;

	draw_recs(tc, &fa, na);
	draw_recs(tc, &fb, nb);
	for (i = 0; i < na; i++)
		want[i] = fa.vals[i];
	for (i = 0; i < nb; i++)
		want[na + i] = fb.vals[i];
	s = scenario_new(tc);
	{
		build2 c = { &fa, &fb, &s->a };

		PBT_ASSERT(dtfl_worker_dataflow(s->w, "concat", s->tc, build_concat,
		    &c) == 0);
		while (dtfl_worker_step(s->w))
			;
	}

	PBT_ASSERT(s->a.n == na + nb);
	if (na + nb > 0) {
		qsort(s->a.vals, s->a.n, sizeof(uint64_t), cmp_u64);
		qsort(want, na + nb, sizeof(uint64_t), cmp_u64);
	}
	for (i = 0; i < na + nb; i++)
		PBT_ASSERT(s->a.vals[i] == want[i]);
}

static const pbt_prop props[] = {
	{ "map_shift", prop_map_shift, 0 },
	{ "filter_even", prop_filter_even, 0 },
	{ "concat_union", prop_concat_union, 0 },
};

PBT_MAIN("ops_data", props)
