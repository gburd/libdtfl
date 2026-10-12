/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_event.c: the event link drop test (P3/P4).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/capture/event.rs (link, link_sync)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * avoid_stack_overflow_in_drop: a link with a very long chain must drop
 * iteratively, not recurse once per node.  Pushing a million empty
 * Progress events and freeing the link must not overflow the stack.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely_ops.h"

#define LIMIT 1000000

static void
push_many(dtfl_event_link *l)
{
	dtfl_event_pusher p = dtfl_event_link_pusher(l);
	int i;

	/* Empty Progress events, as upstream pushes Event::Progress(vec![]). */
	for (i = 0; i < LIMIT; i++)
		p.progress(p.ctx, NULL, NULL, 0);
}

DTFL_TEST(dataflow_operators_core_capture_event_link__avoid_stack_overflow_in_drop)
{
	dtfl_time_class *tc = dtfl_time_unit();
	dtfl_event_link *l = dtfl_event_link_new(tc, dtfl_type_unit());

	(void)params; (void)data;
	push_many(l);
	dtfl_event_link_free(l);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(dataflow_operators_core_capture_event_link_sync__avoid_stack_overflow_in_drop)
{
	dtfl_time_class *tc = dtfl_time_unit();
	dtfl_event_link *l = dtfl_event_link_new_sync(tc, dtfl_type_unit());

	(void)params; (void)data;
	push_many(l);
	dtfl_event_link_free(l);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dataflow_operators_core_capture_event_link__avoid_stack_overflow_in_drop),
	DTFL_T(dataflow_operators_core_capture_event_link_sync__avoid_stack_overflow_in_drop),
	DTFL_T_END
};

DTFL_TEST_MAIN("event", tests)
