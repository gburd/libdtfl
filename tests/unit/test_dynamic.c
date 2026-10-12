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

#include "dtfl/iterate.h"
#include "dtfl/time.h"

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

static MunitTest tests[] = {
	DTFL_T(dynamic__feedback_summary_bare),
	DTFL_T(dynamic__feedback_summary_product),
	DTFL_T(dynamic__feedback_summary_offset),
	DTFL_T(dynamic__feedback_summary_level_panics),
	DTFL_T_END
};

DTFL_TEST_MAIN("dynamic", tests)
