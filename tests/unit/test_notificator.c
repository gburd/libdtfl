/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_notificator.c: the FrontierNotificator delivery order.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/generic/notificator.rs
 *     (notificator_delivers_notifications_in_topo_order)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Builds a FrontierNotificator over a single Product(u64, u64) frontier,
 * requests notifications at nine times, and checks that advancing the
 * frontier makes exactly the passed times available, least first,
 * including a request added mid-session.  This exercises the notificator
 * directly, so it uses the library-internal struct.
 */
#include "dtfl_test.h"

#include "timely/ops_notify_int.h"
#include "progress/progress_int.h"
#include "progress/time_int.h"

#include "dtfl/time.h"

/* A Product(a, b) time as two words. */
static void
prod(uint64_t *t, uint64_t a, uint64_t b)
{
	t[0] = a;
	t[1] = b;
}

/* Requests a notification at Product(a, b) off the root capability. */
static void
notify_at(__dtfl_fnotificator *n, const dtfl_cap *root, uint64_t a, uint64_t b)
{
	uint64_t t[2];

	prod(t, a, b);
	__dtfl_fnote_notify_at(n, dtfl_cap_delayed(root, t));
}

/* The next delivered time's two words, or fails the test if none. */
static void
expect_next(__dtfl_fnotificator *n, uint64_t a, uint64_t b)
{
	dtfl_cap *cap = NULL;
	uint64_t count = 0;
	const uint64_t *t;

	assert_true(__dtfl_fnote_next(n, &cap, &count));
	t = dtfl_cap_time(cap);
	assert_uint64(t[0], ==, a);
	assert_uint64(t[1], ==, b);
	dtfl_cap_drop(cap);
}

static void
expect_none(__dtfl_fnotificator *n)
{
	dtfl_cap *cap = NULL;
	uint64_t count = 0;

	assert_false(__dtfl_fnote_next(n, &cap, &count));
}

DTFL_TEST(dataflow_operators_generic_notificator__notificator_delivers_notifications_in_topo_order)
{
	dtfl_time_class *u64a = dtfl_time_u64();
	dtfl_time_class *u64b = dtfl_time_u64();
	dtfl_time_class *tc = dtfl_time_product(u64a, u64b);
	__dtfl_mutable_antichain frontier;
	__dtfl_change_batch internal;
	__dtfl_fnotificator note;
	dtfl_cap *root;
	uint64_t t[2];

	(void)params; (void)data;
	/* product retains its children; drop our own references. */
	dtfl_time_class_free(u64a);
	dtfl_time_class_free(u64b);

	/* The frontier starts at Product(0, 0); the cap holds its own batch. */
	prod(t, 0, 0);
	__dtfl_mantichain_init_elem(&frontier, tc, t);
	__dtfl_cb_init(&internal, dtfl_time_depth(tc));
	root = __dtfl_cap_new(tc, t, &internal, 0);

	__dtfl_fnote_init(&note, tc, 1);
	__dtfl_fnote_bind(&note, &frontier);

	/* Request notifications at the nine upstream times (two are dups). */
	notify_at(&note, root, 3, 5);
	notify_at(&note, root, 5, 4);
	notify_at(&note, root, 1, 2);
	notify_at(&note, root, 1, 1);
	notify_at(&note, root, 1, 1);
	notify_at(&note, root, 5, 4);
	notify_at(&note, root, 6, 0);
	notify_at(&note, root, 6, 2);
	notify_at(&note, root, 5, 8);

	/* At frontier (0,0) nothing is available. */
	expect_none(&note);

	/* Advance the frontier to [(5,7), (6,1)]: open up some notifications. */
	prod(t, 0, 0);
	__dtfl_mantichain_update(&frontier, t, -1);
	prod(t, 5, 7);
	__dtfl_mantichain_update(&frontier, t, 1);
	prod(t, 6, 1);
	__dtfl_mantichain_update(&frontier, t, 1);

	expect_next(&note, 1, 1);
	expect_next(&note, 1, 2);
	expect_next(&note, 3, 5);
	expect_next(&note, 5, 4);
	expect_next(&note, 6, 0);
	expect_none(&note);

	/* Advance to [(6,10)], opening the rest. */
	prod(t, 5, 7);
	__dtfl_mantichain_update(&frontier, t, -1);
	prod(t, 6, 1);
	__dtfl_mantichain_update(&frontier, t, -1);
	prod(t, 6, 10);
	__dtfl_mantichain_update(&frontier, t, 1);

	/* (5,8) comes first: before (6,0) in the total order but after here. */
	expect_next(&note, 5, 8);

	/* Add a notification mid-session; it is delivered before (6,2). */
	notify_at(&note, root, 5, 9);
	expect_next(&note, 5, 9);
	expect_next(&note, 6, 2);
	expect_none(&note);

	dtfl_cap_drop(root);
	__dtfl_fnote_free(&note);
	__dtfl_cb_free(&internal);
	__dtfl_mantichain_free(&frontier);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_operators_generic_notificator__notificator_delivers_notifications_in_topo_order),
	DTFL_T_END
};

DTFL_TEST_MAIN("notificator", tests)
