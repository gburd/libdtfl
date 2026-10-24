/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/stackoverflow.c: mirrors
 * differential-dataflow/examples/stackoverflow.rs.
 *
 * Incremental breadth-first search over a graph whose edges arrive over time,
 * as the Stack Overflow temporal interaction graph does upstream.  Each edge
 * is introduced at its own timestamp; bfs (dtfl_algo_bfs) maintains the
 * distance of every node from the root, mapped to the distance histogram and
 * consolidated.  Upstream reads the edges from a SNAP file and reports
 * timings; a C runtime has no such file, so this port feeds a small fixed
 * list of (time, src, dst) edges, which makes the final distance histogram
 * exact and checkable.  The computation, operators and output shape are
 * upstream's.
 */
#include "example_args.h"

#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

/*
 * A fixed temporal edge list: (time, src, dst).  Root is node 1.  The edges
 * build the graph incrementally; the final reachable set from node 1 and its
 * distances are:
 *   1:0  2:1  3:1  4:2  5:2  6:3.  Node 0 and 7 are not reached from 1.
 * Final distance histogram: 0 -> 1, 1 -> 2, 2 -> 2, 3 -> 1.
 */
static const uint64_t timed_edges[][3] = {
	{ 1, 1, 2 }, { 1, 1, 3 }, { 2, 2, 4 }, { 2, 3, 5 }, { 3, 4, 6 },
	{ 3, 5, 6 }, { 4, 0, 7 }, { 5, 7, 0 },
};
#define N_EDGES (sizeof timed_edges / sizeof timed_edges[0])
#define ROOT 1

/* map (node, dist) -> dist (the u32 distance in the second field). */
static void
label_to_dist(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[4];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 8, 4);
	dtfl_collection_emit_record(out, buf, 4);
}

static void
inspect_dist(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	printf("\t(%llu, %lld)\n", (unsigned long long)dtfl_get_u32(rec),
	    (long long)d);
}

typedef struct build_ctx {
	dtfl_input_session *roots;
	dtfl_input_session *graph;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u32 = dtfl_type_u32();
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	dtfl_collection *graph = dtfl_input_session_to_collection(c->graph,
	    scope);
	dtfl_collection *result = dtfl_algo_bfs(graph, roots);
	dtfl_collection *dists = dtfl_collection_map(result, u32, label_to_dist,
	    DTFL_NOCTX);
	dtfl_collection *con = dtfl_collection_consolidate(dists);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_dist,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(dists);
	dtfl_collection_free(result);
	dtfl_collection_free(graph);
	dtfl_collection_free(roots);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	build_ctx c;
	uint64_t root = ROOT, final_time;
	size_t i;

	(void)ctx;
	c.roots = dtfl_input_session_new(dtfl_type_u64(), i64);
	c.graph = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "stackoverflow", tc, build, &c);

	/* Seed the root at distance 0 and close the root input. */
	{
		unsigned char buf[8];

		dtfl_put_u64(buf, root);
		dtfl_input_session_insert(c.roots, buf, 8);
	}
	dtfl_input_session_close(c.roots);

	/* Introduce each edge at its own time, in time order (already sorted). */
	for (i = 0; i < N_EDGES; i++) {
		unsigned char buf[16];
		uint64_t t = timed_edges[i][0];

		dtfl_input_session_advance_to(c.graph, &t);
		dtfl_put_u64(buf, timed_edges[i][1]);
		dtfl_put_u64(buf + 8, timed_edges[i][2]);
		dtfl_input_session_insert(c.graph, buf, sizeof buf);
	}
	final_time = timed_edges[N_EDGES - 1][0] + 1;
	dtfl_input_session_advance_to(c.graph, &final_time);
	dtfl_input_session_flush(c.graph);
	while (dtfl_probe_less_than(c.probe, &final_time))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.graph);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
