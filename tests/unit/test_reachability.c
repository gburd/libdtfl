/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_reachability.c: the reachability builder and tracker.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/progress/reachability.rs (module documentation example)
 * Copyright (c) 2014 Frank McSherry and contributors.
 */
#include "dtfl_test.h"
#include "progress/operate_int.h"

#include <unistd.h>

static __dtfl_location
src_loc(size_t node, size_t port)
{
	__dtfl_location l = { node, port, true };
	return l;
}

static __dtfl_location
tgt_loc(size_t node, size_t port)
{
	__dtfl_location l = { node, port, false };
	return l;
}

/* The u64 class every test uses; created and freed per test. */
static dtfl_time_class *u64;

/* A one-input one-output node whose summary adds `inc`. */
static void
add_simple_node(__dtfl_reach_builder *b, size_t index, uint64_t inc)
{
	__dtfl_connectivity c;

	__dtfl_connectivity_init(&c, u64, 1);
	__dtfl_port_conn_insert(&c.ports[0], 0, &inc);
	__dtfl_reach_builder_add_node(b, index, 1, 1, &c);
}

typedef struct pushed {
	size_t node;
	size_t port;
	uint64_t time;
	int64_t diff;
} pushed;

/* Collects pushed target changes (filtering sources), sorted by Ord. */
static size_t
collect_targets(__dtfl_tracker *t, pushed *out, size_t max)
{
	__dtfl_change_batch *cb = __dtfl_tracker_pushed(t);
	size_t n = __dtfl_cb_iter(cb), i, k = 0;

	for (i = 0; i < n; i++) {
		const uint64_t *key = __dtfl_cb_key(cb, i);
		__dtfl_location l = __dtfl_location_from_key(key);

		if (l.source || k == max)
			continue;
		out[k].node = l.node;
		out[k].port = l.port;
		out[k].time = key[2];
		out[k].diff = __dtfl_cb_delta(cb, i);
		k++;
	}
	__dtfl_cb_clear(cb);
	return k;
}

/*
 * reachability.rs doc example: three nodes in a cycle, the last adding
 * 1.  A pointstamp at node 0's output at 17 implies 17 at nodes 1 and 2
 * and 18 back at node 0.  Retracting it retracts all three.
 */
DTFL_TEST(progress_reachability__doc_example)
{
	__dtfl_reach_builder *b = __dtfl_reach_builder_new(u64);
	__dtfl_connectivity scope;
	__dtfl_tracker *t;
	pushed r[8];
	uint64_t t17 = 17;
	size_t n;

	(void)params; (void)data;
	add_simple_node(b, 0, 0);
	add_simple_node(b, 1, 0);
	add_simple_node(b, 2, 1);
	__dtfl_reach_builder_add_edge(b, src_loc(0, 0), tgt_loc(1, 0));
	__dtfl_reach_builder_add_edge(b, src_loc(1, 0), tgt_loc(2, 0));
	__dtfl_reach_builder_add_edge(b, src_loc(2, 0), tgt_loc(0, 0));
	assert_true(__dtfl_reach_builder_is_acyclic(b));
	t = __dtfl_reach_builder_build(b, &scope);

	__dtfl_tracker_update(t, src_loc(0, 0), &t17, 1);
	__dtfl_tracker_propagate_all(t);
	n = collect_targets(t, r, 8);
	assert_size(n, ==, 3);
	assert_size(r[0].node, ==, 0); assert_uint64(r[0].time, ==, 18); assert_int64(r[0].diff, ==, 1);
	assert_size(r[1].node, ==, 1); assert_uint64(r[1].time, ==, 17); assert_int64(r[1].diff, ==, 1);
	assert_size(r[2].node, ==, 2); assert_uint64(r[2].time, ==, 17); assert_int64(r[2].diff, ==, 1);

	__dtfl_tracker_update(t, src_loc(0, 0), &t17, -1);
	__dtfl_tracker_propagate_all(t);
	n = collect_targets(t, r, 8);
	assert_size(n, ==, 3);
	assert_size(r[0].node, ==, 0); assert_uint64(r[0].time, ==, 18); assert_int64(r[0].diff, ==, -1);
	assert_size(r[1].node, ==, 1); assert_uint64(r[1].time, ==, 17); assert_int64(r[1].diff, ==, -1);
	assert_size(r[2].node, ==, 2); assert_uint64(r[2].time, ==, 17); assert_int64(r[2].diff, ==, -1);
	assert_false(__dtfl_tracker_tracking_anything(t));

	__dtfl_connectivity_free(&scope);
	__dtfl_tracker_free(t);
	return MUNIT_OK;
}

/*
 * The worklist is a min-heap on (time, location), as upstream's
 * BinaryHeap<Reverse<(T, Location, i64)>>: whatever order entries are
 * pushed in, they pop sorted by time then location, with entries for one
 * (time, location) adjacent so propagate_all can sum them.  A heap that
 * pops out of order still reaches the same frontiers, the counts being
 * order-free, so only this test sees it; propagation itself just takes
 * longer, exponentially so in nested scopes.
 */
DTFL_TEST(worklist_pops_in_time_location_order)
{
	__dtfl_reach_builder *b = __dtfl_reach_builder_new(u64);
	__dtfl_connectivity scope;
	__dtfl_tracker *t;
	enum { N = 300 };
	uint64_t times[N], out[N * 4], state = 12345;
	__dtfl_location locs[N];
	int64_t diffs[N], sum;
	size_t i, round;

	(void)params; (void)data;
	add_simple_node(b, 0, 0);
	add_simple_node(b, 1, 1);
	__dtfl_reach_builder_add_edge(b, src_loc(0, 0), tgt_loc(1, 0));
	__dtfl_reach_builder_add_edge(b, src_loc(1, 0), tgt_loc(0, 0));
	t = __dtfl_reach_builder_build(b, &scope);
	for (round = 1; round <= N; round += 37) {
		for (i = 0; i < round; i++) {
			state = state * 6364136223846793005ULL +
			    1442695040888963407ULL;
			times[i] = (state >> 33) % 17;
			locs[i] = (state >> 20) & 1 ?
			    src_loc((state >> 21) & 1, 0) :
			    tgt_loc((state >> 21) & 1, 0);
			diffs[i] = (int64_t)i + 1;
		}
		__dtfl_tracker_worklist_roundtrip(t, times, locs, diffs, round,
		    out);
		for (i = 1; i < round; i++)
			assert_int(__dtfl_words_cmp(out + (i - 1) * 4,
			    out + i * 4, 3), <=, 0);
		/* Every diff comes back once: the pops are a permutation. */
		sum = 0;
		for (i = 0; i < round; i++)
			sum += (int64_t)out[i * 4 + 3];
		assert_int64(sum, ==, (int64_t)(round * (round + 1) / 2));
	}
	__dtfl_connectivity_free(&scope);
	__dtfl_tracker_free(t);
	return MUNIT_OK;
}

DTFL_TEST(cycle_without_increment_is_detected)
{
	__dtfl_reach_builder *b = __dtfl_reach_builder_new(u64);

	(void)params; (void)data;
	add_simple_node(b, 0, 0);
	add_simple_node(b, 1, 0);
	__dtfl_reach_builder_add_edge(b, src_loc(0, 0), tgt_loc(1, 0));
	__dtfl_reach_builder_add_edge(b, src_loc(1, 0), tgt_loc(0, 0));
	assert_false(__dtfl_reach_builder_is_acyclic(b));
	__dtfl_reach_builder_free(b);
	return MUNIT_OK;
}

/*
 * Scope summary: node 0 has one output (scope input) feeding a chain
 * 1 -> 2 (adds 3) -> node 0 input (scope output).  The scope's summary
 * from input 0 to output 0 is 3; a capability at node 1's output at 5
 * appears as 8 at scope output 0.
 */
DTFL_TEST(scope_summary_and_outputs)
{
	__dtfl_reach_builder *b = __dtfl_reach_builder_new(u64);
	__dtfl_connectivity scope, c0;
	__dtfl_tracker *t;
	const uint64_t *s;
	size_t ns, n;
	uint64_t t5 = 5;
	__dtfl_change_batch *out;

	(void)params; (void)data;
	__dtfl_connectivity_init(&c0, u64, 1);
	__dtfl_reach_builder_add_node(b, 0, 1, 1, &c0);	/* boundary, no internal summary */
	add_simple_node(b, 1, 0);
	add_simple_node(b, 2, 3);
	__dtfl_reach_builder_add_edge(b, src_loc(0, 0), tgt_loc(1, 0));
	__dtfl_reach_builder_add_edge(b, src_loc(1, 0), tgt_loc(2, 0));
	__dtfl_reach_builder_add_edge(b, src_loc(2, 0), tgt_loc(0, 0));
	t = __dtfl_reach_builder_build(b, &scope);
	assert_size(scope.len, ==, 1);
	s = __dtfl_port_conn_get(&scope.ports[0], 0, &ns);
	assert_not_null(s);
	assert_size(ns, ==, 1);
	assert_uint64(s[0], ==, 3);

	__dtfl_tracker_update(t, src_loc(1, 0), &t5, 1);
	__dtfl_tracker_propagate_all(t);
	out = __dtfl_tracker_pushed_output(t, 0);
	n = __dtfl_cb_iter(out);
	assert_size(n, ==, 1);
	assert_uint64(__dtfl_cb_key(out, 0)[0], ==, 8);
	assert_int64(__dtfl_cb_delta(out, 0), ==, 1);
	assert_int64(__dtfl_tracker_node_cap_counts(t, 1), ==, 1);
	assert_true(__dtfl_tracker_is_global(t, src_loc(1, 0), &t5));
	__dtfl_connectivity_free(&scope);
	__dtfl_tracker_free(t);
	return MUNIT_OK;
}

/*
 * A wide operator whose reverse maps arrive in reverse order: node 1 has
 * WIDE inputs and outputs, input i connected to output WIDE-1-i, scope
 * input i feeding input i and output o feeding scope output o.  Built in
 * node order, reverse_internal is sorted descending and every scope edge
 * sorts ahead of every node-1 edge, so summarize_outputs must sort as
 * upstream does (sort_unstable) and accumulate through BinaryRuns; a
 * quadratic sort or sorted-vector insert takes minutes here, and the
 * alarm turns that into a failure.
 */
#define WIDE 200000

DTFL_TEST(wide_reversed_operator_builds)
{
	__dtfl_reach_builder *b = __dtfl_reach_builder_new(u64);
	__dtfl_connectivity scope, c0, c1;
	__dtfl_tracker *t;
	__dtfl_change_batch *out;
	uint64_t zero = 0, t5 = 5, t9 = 9;
	size_t i;

	(void)params; (void)data;
	alarm(60);
	__dtfl_connectivity_init(&c0, u64, WIDE);
	__dtfl_reach_builder_add_node(b, 0, WIDE, WIDE, &c0);
	__dtfl_connectivity_init(&c1, u64, WIDE);
	for (i = 0; i < WIDE; i++)
		__dtfl_port_conn_insert(&c1.ports[i], WIDE - 1 - i, &zero);
	__dtfl_reach_builder_add_node(b, 1, WIDE, WIDE, &c1);
	for (i = 0; i < WIDE; i++)
		__dtfl_reach_builder_add_edge(b, src_loc(0, i), tgt_loc(1, i));
	for (i = 0; i < WIDE; i++)
		__dtfl_reach_builder_add_edge(b, src_loc(1, i), tgt_loc(0, i));
	t = __dtfl_reach_builder_build(b, &scope);

	/* Scope input i reaches exactly scope output WIDE-1-i, unchanged. */
	assert_size(scope.len, ==, WIDE);
	for (i = 0; i < WIDE; i++) {
		const __dtfl_port_conn *pc = &scope.ports[i];
		const __dtfl_port_conn_entry *e;

		assert_size(pc->entries.len, ==, 1);
		e = DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, 0);
		assert_size(e->port, ==, WIDE - 1 - i);
		assert_size(e->summaries.len, ==, 1);
		assert_uint64(*(const uint64_t *)(void *)e->summaries.data, ==, 0);
	}

	/* Source summaries: a capability at output 7 surfaces at scope output 7. */
	__dtfl_tracker_update(t, src_loc(1, 7), &t5, 1);
	/* Target summaries: a message at input 3 surfaces at scope output WIDE-4. */
	__dtfl_tracker_update(t, tgt_loc(1, 3), &t9, 1);
	__dtfl_tracker_propagate_all(t);
	out = __dtfl_tracker_pushed_output(t, 7);
	assert_size(__dtfl_cb_iter(out), ==, 1);
	assert_uint64(__dtfl_cb_key(out, 0)[0], ==, 5);
	out = __dtfl_tracker_pushed_output(t, WIDE - 4);
	assert_size(__dtfl_cb_iter(out), ==, 1);
	assert_uint64(__dtfl_cb_key(out, 0)[0], ==, 9);
	assert_size(__dtfl_cb_iter(__dtfl_tracker_pushed_output(t, 3)), ==, 0);
	alarm(0);

	__dtfl_connectivity_free(&scope);
	__dtfl_tracker_free(t);
	return MUNIT_OK;
}

static void *
setup(const MunitParameter params[], void *data)
{
	(void)params; (void)data;
	u64 = dtfl_time_u64();
	return NULL;
}

static void
teardown(void *fixture)
{
	(void)fixture;
	dtfl_time_class_free(u64);
	u64 = NULL;
}

#define T(name) { "/" #name, name, setup, teardown, MUNIT_TEST_OPTION_NONE, NULL }

static MunitTest tests[] = {
	T(progress_reachability__doc_example),
	T(worklist_pops_in_time_location_order),
	T(cycle_without_increment_is_detected),
	T(scope_summary_and_outputs),
	T(wide_reversed_operator_builds),
	DTFL_T_END
};

DTFL_TEST_MAIN("reachability", tests)
