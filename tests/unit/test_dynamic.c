/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_dynamic.c: the dynamic-scope helpers.
 *
 * dynamic/mod.rs is thin: enter_dynamic is the identity, leave_dynamic
 * truncates PointStamp coordinates, and feedback_summary builds the
 * PointStamp path summary a dynamic feedback uses at a given coordinate.
 * No program at P8 drives a dynamic scope end to end (the dynamic.rs
 * example is P9); this test checks the one piece with real index
 * arithmetic, feedback_summary, against the layout documented in
 * src/progress/time_int.h (word 0 retain, words 1.. per-coordinate
 * actions), for both a bare PointStamp and a Product-with-PointStamp
 * class.
 */
#include "dtfl_test.h"

#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

DTFL_TEST(dynamic__feedback_summary_bare)
{
	dtfl_coord u64 = { DTFL_COORD_UNSIGNED, 64 };
	dtfl_time_class *tc = dtfl_time_pointstamp(u64, 3);
	uint64_t out[DTFL_TIME_MAX];
	unsigned sw;

	(void)params; (void)data;
	assert_not_null(tc);
	sw = dtfl_summary_depth(tc);
	assert_uint(sw, ==, 4u);		/* 1 retain + 3 actions */

	/* feedback_summary(level=2, step=5): retain None, actions [0, 5]. */
	dtfl_feedback_summary(tc, 2, 5, out);
	assert_uint64(out[0], ==, UINT64_MAX);	/* retain None */
	assert_uint64(out[1], ==, 0);		/* coordinate 0 identity */
	assert_uint64(out[2], ==, 5);		/* coordinate 1 step */
	assert_uint64(out[3], ==, 0);

	/* level=1 puts the step at coordinate 0. */
	dtfl_feedback_summary(tc, 1, 7, out);
	assert_uint64(out[0], ==, UINT64_MAX);
	assert_uint64(out[1], ==, 7);
	assert_uint64(out[2], ==, 0);

	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(dynamic__feedback_summary_product)
{
	dtfl_coord u64 = { DTFL_COORD_UNSIGNED, 64 };
	dtfl_time_class *outer = dtfl_time_u64();
	dtfl_time_class *ps = dtfl_time_pointstamp(u64, 2);
	dtfl_time_class *tc = dtfl_time_product(outer, ps);
	uint64_t out[DTFL_TIME_MAX];
	unsigned sw;

	(void)params; (void)data;
	assert_not_null(tc);
	sw = dtfl_summary_depth(tc);
	/* outer summary (1 word) + PointStamp summary (1 retain + 2 actions). */
	assert_uint(sw, ==, 4u);

	/* The PointStamp summary begins after the outer summary word. */
	dtfl_feedback_summary(tc, 1, 9, out);
	assert_uint64(out[0], ==, 0);		/* outer summary stays default */
	assert_uint64(out[1], ==, UINT64_MAX);	/* PointStamp retain None */
	assert_uint64(out[2], ==, 9);		/* coordinate 0 step */
	assert_uint64(out[3], ==, 0);

	dtfl_time_class_free(tc);
	dtfl_time_class_free(ps);
	dtfl_time_class_free(outer);
	return MUNIT_OK;
}

/*
 * A class whose outer summary width differs from its outer time width,
 * so the PointStamp summary's word offset (after the outer summary) is
 * not the PointStamp's time offset (after the outer time).  This pins
 * feedback_summary to the summary layout, not the time layout.
 */
DTFL_TEST(dynamic__feedback_summary_offset)
{
	dtfl_coord u64 = { DTFL_COORD_UNSIGNED, 64 };
	dtfl_time_class *outer = dtfl_time_pointstamp(u64, 1);
	dtfl_time_class *inner = dtfl_time_pointstamp(u64, 2);
	dtfl_time_class *tc = dtfl_time_product(outer, inner);
	uint64_t out[DTFL_TIME_MAX];
	unsigned sw;

	(void)params; (void)data;
	assert_not_null(tc);
	/* outer summary (1 retain + 1 action = 2) + inner (1 retain + 2 = 3). */
	sw = dtfl_summary_depth(tc);
	assert_uint(sw, ==, 5u);

	/* The inner PointStamp summary begins at the outer summary width (2),
	 * not at the outer time width (1): out[2] is the inner retain. */
	dtfl_feedback_summary(tc, 1, 4, out);
	assert_uint64(out[2], ==, UINT64_MAX);	/* inner retain None */
	assert_uint64(out[3], ==, 4);		/* inner coordinate 0 step */
	assert_uint64(out[4], ==, 0);

	dtfl_time_class_free(tc);
	dtfl_time_class_free(inner);
	dtfl_time_class_free(outer);
	return MUNIT_OK;
}

DTFL_TEST(dynamic__feedback_summary_level_panics)
{
	dtfl_coord u64 = { DTFL_COORD_UNSIGNED, 64 };
	dtfl_time_class *tc = dtfl_time_pointstamp(u64, 2);
	uint64_t out[DTFL_TIME_MAX];

	(void)params; (void)data;
	/* level beyond the PointStamp's maximum length is a contract error. */
	DTFL_EXPECT_PANIC(dtfl_feedback_summary(tc, 3, 1, out));
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* leave_dynamic end to end: the retain-summary path                   */
/* ------------------------------------------------------------------ */

/*
 * leave_dynamic truncates both each update's embedded time and its
 * capability to the first level - 1 PointStamp coordinates.  Truncating
 * the capability means delaying it to a time earlier than the message
 * time, which the input-output summary check rejects unless the input
 * connection carries PointStampSummary { retain: Some(level - 1) }
 * (dynamic/mod.rs).  This drives a dynamic scope end to end -- enter a
 * record, seed a Variable at the PointStamp coordinate, iterate to a
 * fixed point, leave_dynamic(1) -- which aborts if that retain summary is
 * missing.  The fixpoint of distinct over a self-feeding Variable is just
 * the seed, so the one leaving record is the seed.
 */
static size_t g_dyn_count;
static int64_t g_dyn_max;

static void
dyn_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t v;

	(void)ctx; (void)len; (void)time; (void)diff;
	g_dyn_count++;
	v = (int64_t)dtfl_get_u64((const unsigned char *)rec);
	if (v > g_dyn_max)
		g_dyn_max = v;
}

/* x -> x + 1 over a u64 record. */
static void
dyn_incr(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	dtfl_put_u64(buf, dtfl_get_u64((const unsigned char *)rec) + 1);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* keep x < 4. */
static bool
dyn_lt4(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)len;
	return dtfl_get_u64((const unsigned char *)rec) < 4;
}

typedef struct dyn_build {
	dtfl_input_session *in;
	dtfl_probe *probe;
	dtfl_collection *leaving; /* set by the inner build */
} dyn_build;

static void
dyn_iter_build(dtfl_scope *inner, void *arg)
{
	dyn_build *db = arg;
	dtfl_scope *outer = dtfl_scope_parent(inner);
	dtfl_collection *src = dtfl_input_session_to_collection(db->in, outer);
	dtfl_collection *entered = dtfl_collection_enter(src, inner);
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *col;
	dtfl_variable *var;
	dtfl_collection *stepped, *grown, *body, *dist, *result;

	/* A Variable at PointStamp coordinate 0 (level 1), seeded from the
	 * entered source: col = feedback.concat(source).  The body advances
	 * the counter each round (x -> x + 1, keep x < 4), so the loop runs
	 * several iterations and the capability advances past coordinate 0 --
	 * leave_dynamic(1) then truncates a strictly later capability, the
	 * case the retain summary exists for. */
	var = dtfl_variable_new_at(inner, dtfl_type_u64(), i64, 1, &col);
	{
		dtfl_collection *fed = col;

		col = dtfl_collection_concat(fed, entered);
		dtfl_collection_free(fed);
	}
	stepped = dtfl_collection_map(col, dtfl_type_u64(), dyn_incr,
	    DTFL_NOCTX);
	grown = dtfl_collection_filter(stepped, dyn_lt4, DTFL_NOCTX);
	body = dtfl_collection_concat(grown, entered);
	dist = dtfl_collection_distinct(body, i64);
	result = dtfl_collection_leave_dynamic(dist, 1);
	db->leaving = dtfl_collection_leave(result);
	/* Close the loop with the negated source so the Variable converges to
	 * {0, 1, 2, 3} and only the delta circulates. */
	{
		dtfl_collection *neg = dtfl_collection_negate(entered);
		dtfl_collection *fb = dtfl_collection_concat(dist, neg);

		dtfl_variable_set(var, fb);
		dtfl_collection_free(neg);
	}
	dtfl_collection_free(result);
	dtfl_collection_free(dist);
	dtfl_collection_free(body);
	dtfl_collection_free(grown);
	dtfl_collection_free(stepped);
	dtfl_collection_free(col);
	dtfl_collection_free(entered);
	dtfl_collection_free(src);
	dtfl_diff_free(i64);
}

static void
dyn_build_fn(dtfl_scope *scope, void *arg)
{
	dyn_build *db = arg;
	dtfl_coord u64c = { DTFL_COORD_UNSIGNED, 64 };
	dtfl_time_class *outer = dtfl_time_u64();
	dtfl_time_class *ps = dtfl_time_pointstamp(u64c, 4);
	dtfl_time_class *inner = dtfl_time_product(outer, ps);
	dtfl_collection *ins, *probed;

	db->leaving = NULL;
	dtfl_scope_scoped(scope, "dyn", inner, dyn_iter_build, db);
	ins = dtfl_collection_inspect(db->leaving, dyn_sink, DTFL_NOCTX);
	probed = dtfl_collection_probe_with(ins, db->probe);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(db->leaving);
	dtfl_time_class_free(inner);
	dtfl_time_class_free(ps);
	dtfl_time_class_free(outer);
}

DTFL_TEST(dynamic__leave_dynamic_retain_path)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	dyn_build db;
	uint64_t seed = 0, one = 1;
	unsigned char buf[8];
	int steps;

	(void)params; (void)data;
	g_dyn_count = 0;
	g_dyn_max = -1;
	db.in = dtfl_input_session_new(dtfl_type_u64(), i64);
	db.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "dyn", tc, dyn_build_fn, &db), ==, 0);

	dtfl_put_u64(buf, seed);
	dtfl_input_session_insert(db.in, buf, 8);
	dtfl_input_session_advance_to(db.in, &one);
	dtfl_input_session_flush(db.in);
	dtfl_input_session_close(db.in);

	/* Runs to quiescence; without the retain summary the capability
	 * truncation in leave_dynamic aborts here. */
	for (steps = 0; steps < 100000 && dtfl_worker_step(w); steps++)
		;
	assert_int(steps, <, 100000);
	assert_true(dtfl_probe_done(db.probe));
	/* The loop grows the seed 0 to {0, 1, 2, 3} (x + 1 while x < 4), and
	 * every member leaves at the truncated (host) time. */
	assert_size(g_dyn_count, ==, 4);
	assert_int64(g_dyn_max, ==, 3);

	dtfl_probe_free(db.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(i64);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dynamic__feedback_summary_bare),
	DTFL_T(dynamic__feedback_summary_product),
	DTFL_T(dynamic__feedback_summary_offset),
	DTFL_T(dynamic__feedback_summary_level_panics),
	DTFL_T(dynamic__leave_dynamic_retain_path),
	DTFL_T_END
};

DTFL_TEST_MAIN("dynamic", tests)
