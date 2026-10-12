/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_agent.c: the TraceAgent / TraceBox compaction-frontier
 * logic (agent.rs), tested directly against the internal API.
 *
 * The subtle invariant is reference-counted compaction: the box compacts the
 * trace only to the meet of every live reader's hold, so compaction never
 * advances past a reader that still needs older times; and when a reader
 * releases its hold, compaction advances to the next reader's hold.  These
 * tests exercise the box's accumulated frontier through the agent's set,
 * clone and free, with no dataflow in the loop.
 */
#include "dtfl_test.h"
#include "dd/arrange_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

/* The box's accumulated logical compaction frontier (single coordinate). */
static uint64_t
logical_frontier(const __dtfl_trace_agent *a, size_t *n)
{
	const __dtfl_mutable_antichain *m = &a->box->logical_compaction;

	*n = __dtfl_mantichain_frontier_len(m);
	return *n > 0 ? __dtfl_mantichain_frontier(m)[0] : 0;
}

/* Builds a fresh agent + writer over an empty u64->u64 i64 trace. */
static void
make_agent(dtfl_time_class *tc, __dtfl_trace_agent *a, __dtfl_trace_writer *w)
{
	__dtfl_trace *tr = __dtfl_trace_new(false, dtfl_type_u64(),
	    dtfl_type_u64(), tc, I64);

	__dtfl_trace_agent_new(tr, false, dtfl_type_u64(), dtfl_type_u64(), tc,
	    I64, a, w);
}

/* A single reader's hold drives the box's compaction frontier. */
DTFL_TEST(agent__single_hold)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_trace_agent a;
	__dtfl_trace_writer w;
	uint64_t five = 5, f;
	size_t n;

	(void)params;
	(void)data;
	make_agent(tc, &a, &w);

	/* Fresh box: frontier is the minimum [0]. */
	f = logical_frontier(&a, &n);
	assert_size(n, ==, 1);
	assert_uint64(f, ==, 0);

	/* Setting the hold to [5] advances the box frontier to [5]. */
	__dtfl_trace_agent_set_logical(&a, &five, 1);
	f = logical_frontier(&a, &n);
	assert_size(n, ==, 1);
	assert_uint64(f, ==, 5);

	/* Freeing the only reader drops the hold: frontier becomes empty. */
	__dtfl_trace_agent_free(&a);
	__dtfl_trace_writer_free(&w);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * Two readers: compaction advances only to the meet (minimum) of their
 * holds, never past the one that holds older times.
 */
DTFL_TEST(agent__meet_of_holds)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_trace_agent a, b;
	__dtfl_trace_writer w;
	uint64_t t, f;
	size_t n;

	(void)params;
	(void)data;
	make_agent(tc, &a, &w);
	__dtfl_trace_agent_clone(&a, &b);

	/* a holds [5]; b holds [3].  Frontier is the meet, [3]. */
	t = 5;
	__dtfl_trace_agent_set_logical(&a, &t, 1);
	t = 3;
	__dtfl_trace_agent_set_logical(&b, &t, 1);
	f = logical_frontier(&a, &n);
	assert_size(n, ==, 1);
	assert_uint64(f, ==, 3);		/* not 5: b still needs [3,5) */

	/*
	 * b releases its hold: compaction may now advance to a's hold [5].
	 * This is the "a released reader lets compaction advance" property.
	 */
	__dtfl_trace_agent_free(&b);
	f = logical_frontier(&a, &n);
	assert_size(n, ==, 1);
	assert_uint64(f, ==, 5);

	__dtfl_trace_agent_free(&a);
	__dtfl_trace_writer_free(&w);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * A reader that never advances its hold holds compaction at the minimum,
 * even as another reader advances far ahead: compaction cannot run past the
 * live reader's frontier.
 */
DTFL_TEST(agent__stuck_reader_blocks)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_trace_agent a, b;
	__dtfl_trace_writer w;
	uint64_t t, f;
	size_t n;

	(void)params;
	(void)data;
	make_agent(tc, &a, &w);
	__dtfl_trace_agent_clone(&a, &b);

	/* b never advances (hold stays at the minimum [0]); a advances to [9]. */
	t = 9;
	__dtfl_trace_agent_set_logical(&a, &t, 1);
	f = logical_frontier(&a, &n);
	assert_size(n, ==, 1);
	assert_uint64(f, ==, 0);		/* b pins it at the minimum */

	/* Only once b is gone does compaction advance to a's [9]. */
	__dtfl_trace_agent_free(&b);
	f = logical_frontier(&a, &n);
	assert_uint64(f, ==, 9);

	__dtfl_trace_agent_free(&a);
	__dtfl_trace_writer_free(&w);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* Physical compaction tracks its own, independent accumulated hold. */
DTFL_TEST(agent__physical_independent)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_trace_agent a;
	__dtfl_trace_writer w;
	uint64_t t;

	(void)params;
	(void)data;
	make_agent(tc, &a, &w);

	t = 7;
	__dtfl_trace_agent_set_physical(&a, &t, 1);
	/* Logical hold is untouched: still the minimum. */
	assert_size(__dtfl_mantichain_frontier_len(&a.box->logical_compaction), ==, 1);
	assert_uint64(__dtfl_mantichain_frontier(&a.box->logical_compaction)[0], ==, 0);
	/* Physical frontier advanced to [7]. */
	assert_uint64(__dtfl_mantichain_frontier(&a.box->physical_compaction)[0], ==, 7);

	__dtfl_trace_agent_free(&a);
	__dtfl_trace_writer_free(&w);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(agent__single_hold),
	DTFL_T(agent__meet_of_holds),
	DTFL_T(agent__stuck_reader_blocks),
	DTFL_T(agent__physical_independent),
	DTFL_T_END
};

DTFL_TEST_MAIN("agent", tests)
