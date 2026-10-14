/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_dd_server.c: the shared-dataflow plugin server (P14).
 *
 * Drives the server through a short session over a fixed, hand-computable
 * graph and checks three things:
 *
 *   - the degr_dist plugin's degree distribution matches the hand count;
 *   - the reachability plugin's reachable set matches the hand walk;
 *   - a SHARED arrangement is genuinely shared: two plugins that import the
 *     same base trace receive the one trace handle, which is the reason the
 *     crate exists (one arranged graph feeds many queries).
 *
 * The server runs a single in-process worker and dispatches commands on the
 * calling thread, so there is no accept loop; a hang would be a plugin
 * dataflow that does not converge, which the harness timeout would surface
 * as a wedged run.
 */
#define _GNU_SOURCE	/* open_memstream */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dd_server.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"

#include "../../examples/server/plugins.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* A fixed-graph source plugin for the test.                           */
/* ------------------------------------------------------------------ */

/* The fixed test graph, as (src, dst) edges with no duplicates:
 *   0 -> 1, 1 -> 2, 2 -> 0, 2 -> 3, 3 -> 4, 4 -> 2
 * Out-degrees: 0:1, 1:1, 2:2, 3:1, 4:1  ->  distribution {1:4, 2:1}.
 * Reachable from 0: {0,1,2,3,4} (all five).
 * Reachable from 3: {3,4,2,0,1} (all five, via 3->4->2->0->1->2...). */
static const uint64_t test_edges[][2] = {
	{ 0, 1 }, { 1, 2 }, { 2, 0 }, { 2, 3 }, { 3, 4 }, { 4, 2 },
};
#define N_EDGES (sizeof test_edges / sizeof test_edges[0])

static int
fixed_graph_build(dtfl_dd_env *env)
{
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_input_session *in = dtfl_input_session_new(pair, i64);
	dtfl_collection *coll = dtfl_input_session_to_collection(in,
	    dtfl_dd_env_scope(env));
	dtfl_arranged *arr;
	dtfl_collection *probed;
	size_t e;

	for (e = 0; e < N_EDGES; e++) {
		unsigned char buf[16];

		dtfl_put_u64(buf, test_edges[e][0]);
		dtfl_put_u64(buf + 8, test_edges[e][1]);
		dtfl_input_session_insert(in, buf, sizeof buf);
	}
	dtfl_dd_env_add_input(env, in);

	arr = dtfl_collection_arrange_by_key(coll, dtfl_type_u64(),
	    dtfl_type_u64(), "fixed edges");
	dtfl_trace_handler_set(dtfl_dd_env_handles(env),
	    dtfl_dd_env_arg(env, 0), dtfl_arranged_trace(arr), pair);
	/* Probe the arrangement so the server drives it to seal each epoch. */
	probed = dtfl_arranged_as_collection(arr, pair, NULL, DTFL_NOCTX);
	{
		dtfl_collection *p = dtfl_collection_probe_with(probed,
		    dtfl_dd_env_probe(env));

		dtfl_collection_free(p);
	}
	dtfl_collection_free(probed);
	dtfl_arranged_free(arr);
	dtfl_collection_free(coll);
	dtfl_type_free(pair);
	dtfl_diff_free(i64);
	return 0;
}

static const dtfl_dataflow_plugin fixed_graph = { "fixed_graph",
	fixed_graph_build };

/* ------------------------------------------------------------------ */
/* Shared-trace witnesses: two plugins that record the handle they got. */
/* ------------------------------------------------------------------ */

static dtfl_trace *witness_a;
static dtfl_trace *witness_b;

static int
witness_a_build(dtfl_dd_env *env)
{
	witness_a = dtfl_trace_handler_get(dtfl_dd_env_handles(env),
	    dtfl_dd_env_arg(env, 0));
	return witness_a != NULL ? 0 : 1;
}

static int
witness_b_build(dtfl_dd_env *env)
{
	witness_b = dtfl_trace_handler_get(dtfl_dd_env_handles(env),
	    dtfl_dd_env_arg(env, 0));
	return witness_b != NULL ? 0 : 1;
}

static const dtfl_dataflow_plugin witness_a_plugin = { "witness_a",
	witness_a_build };
static const dtfl_dataflow_plugin witness_b_plugin = { "witness_b",
	witness_b_build };

/* ------------------------------------------------------------------ */
/* Tests.                                                              */
/* ------------------------------------------------------------------ */

/*
 * degr_dist and reachability over the fixed graph, read back through the
 * server's peek into a memory stream, matched against the hand count.
 */
DTFL_TEST(dd_server__plugins_match)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *out = open_memstream(&buf, &len);
	dtfl_dd_server *s;
	int rc;

	(void)params; (void)data;
	munit_assert_not_null(out);
	s = dtfl_dd_server_new(out);
	dtfl_dd_server_register(s, &fixed_graph);
	dtfl_dd_server_register(s, &dtfl_plugin_degr_dist);
	dtfl_dd_server_register(s, &dtfl_plugin_reachability);

	rc = dtfl_dd_server_load(s, "g", "fixed_graph",
	    1, (const char *const[]){ "edges" });
	munit_assert_int(rc, ==, 0);
	rc = dtfl_dd_server_load(s, "dd", "degr_dist",
	    1, (const char *const[]){ "edges" });
	munit_assert_int(rc, ==, 0);
	rc = dtfl_dd_server_load(s, "rr", "reachability",
	    2, (const char *const[]){ "edges", "0" });
	munit_assert_int(rc, ==, 0);
	dtfl_dd_server_tick(s);

	/* Degree distribution: four nodes have out-degree 1, one has 2. */
	rc = dtfl_dd_server_peek(s, "edges-degrees");
	munit_assert_int(rc, ==, 0);
	/* Reachability from 0: all five nodes. */
	rc = dtfl_dd_server_peek(s, "edges-reach");
	munit_assert_int(rc, ==, 0);
	dtfl_dd_server_free(s);
	fclose(out);

	munit_assert_not_null(buf);
	/* The degree distribution: (1, 4) and (2, 1). */
	munit_assert_not_null(strstr(buf, "(1, 4) : 1"));
	munit_assert_not_null(strstr(buf, "(2, 1) : 1"));
	/* Reachability from 0 reaches every node exactly once. */
	munit_assert_not_null(strstr(buf, "0 : 1"));
	munit_assert_not_null(strstr(buf, "1 : 1"));
	munit_assert_not_null(strstr(buf, "2 : 1"));
	munit_assert_not_null(strstr(buf, "3 : 1"));
	munit_assert_not_null(strstr(buf, "4 : 1"));
	free(buf);
	return MUNIT_OK;
}

/*
 * A shared arrangement is genuinely shared: two importing plugins receive
 * the one trace handle from the handler, not two independent copies.  This
 * is the crate's reason to exist.
 */
DTFL_TEST(dd_server__arrangement_shared)
{
	dtfl_dd_server *s = dtfl_dd_server_new(NULL);
	int rc;

	(void)params; (void)data;
	witness_a = NULL;
	witness_b = NULL;
	dtfl_dd_server_register(s, &fixed_graph);
	dtfl_dd_server_register(s, &witness_a_plugin);
	dtfl_dd_server_register(s, &witness_b_plugin);

	rc = dtfl_dd_server_load(s, "g", "fixed_graph",
	    1, (const char *const[]){ "edges" });
	munit_assert_int(rc, ==, 0);
	munit_assert_true(dtfl_trace_handler_has(dtfl_dd_server_handles(s),
	    "edges"));

	rc = dtfl_dd_server_load(s, "wa", "witness_a",
	    1, (const char *const[]){ "edges" });
	munit_assert_int(rc, ==, 0);
	rc = dtfl_dd_server_load(s, "wb", "witness_b",
	    1, (const char *const[]){ "edges" });
	munit_assert_int(rc, ==, 0);

	/* Both plugins got the one registered handle: a single shared trace. */
	munit_assert_not_null(witness_a);
	munit_assert_ptr_equal(witness_a, witness_b);
	dtfl_dd_server_free(s);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dd_server__plugins_match),
	DTFL_T(dd_server__arrangement_shared),
	DTFL_T_END
};

DTFL_TEST_MAIN("dd_server", tests)
