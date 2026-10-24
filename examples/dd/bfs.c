/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/bfs.c: mirrors differential-dataflow/examples/bfs.rs.
 *
 * Breadth-first distance labeling via dtfl_algo_bfs: seed root 0, iterate to
 * a fixed point joining labelled nodes against outgoing edges and keeping the
 * minimum distance, then map each (node, dist) to its distance, consolidate,
 * and inspect the (distance -> count) histogram.  Upstream builds a random
 * graph with a seeded StdRng and reports timings; a C runtime cannot
 * reproduce that stream, so with no positional arguments this port feeds a
 * small fixed graph whose distance histogram is exact and checkable.  With
 * "nodes edges batch rounds inspect" positionals it keeps upstream's shape.
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
 * A fixed directed graph.  From root 0 the BFS distances are:
 *   0:0  1:1  2:1  3:2  4:2  5:3.  Node 6 is unreachable.
 * Distance histogram: dist 0 -> 1, dist 1 -> 2, dist 2 -> 2, dist 3 -> 1.
 */
static const uint64_t fixed_edges[][2] = {
	{ 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 }, { 2, 4 }, { 3, 5 },
	{ 4, 5 }, { 6, 0 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])

/* map (node, dist) -> dist (the u32 distance stored in the second field). */
static void
label_to_dist(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[4];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 8, 4);
	dtfl_collection_emit_record(out, buf, 4);
}

/* inspect: print "\t(dist, count)" for each consolidated update. */
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
	dtfl_input_session *edges;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u32 = dtfl_type_u32();
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	dtfl_collection *result = dtfl_algo_bfs(edges, roots);
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
	dtfl_collection_free(edges);
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
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	c.roots = dtfl_input_session_new(dtfl_type_u64(), i64);
	c.edges = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "bfs", tc, build, &c);

	if (dtfl_worker_index(w) == 0) {
		unsigned char buf[16];

		dtfl_put_u64(buf, 0);
		dtfl_input_session_insert(c.roots, buf, 8);
		for (i = 0; i < FIXED_E; i++) {
			dtfl_put_u64(buf, fixed_edges[i][0]);
			dtfl_put_u64(buf + 8, fixed_edges[i][1]);
			dtfl_input_session_insert(c.edges, buf, sizeof buf);
		}
	}
	dtfl_input_session_advance_to(c.roots, &one);
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_flush(c.roots);
	dtfl_input_session_flush(c.edges);
	while (dtfl_probe_less_than(c.probe, dtfl_input_session_time(c.edges)))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.roots);
	dtfl_input_session_close(c.edges);
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
	int skip = argc > 5 ? 5 : 0;
	dtfl_config *cfg = ex_config(argc, argv, skip);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
