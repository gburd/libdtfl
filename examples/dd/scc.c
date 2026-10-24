/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/scc.c: mirrors differential-dataflow/examples/scc.rs.
 *
 * The subset of edges whose endpoints lie in the same strongly connected
 * component, via dtfl_algo_scc.  Upstream builds a random graph with a
 * seeded DefaultHasher and reports timings; a C runtime cannot reproduce
 * that stream, so this port feeds a small fixed graph whose SCC edge set is
 * exact and checkable, and inspects each surviving (src, dst) edge.
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
 * A fixed directed graph with two cycles and a bridge.  The cycle
 * 0->1->2->0 is one SCC; the cycle 3->4->3 is another.  Edge 2->3 bridges
 * the components and is not intra-component; 4->5 and 5 are a dead end.
 * Intra-SCC edges: (0,1) (1,2) (2,0) (3,4) (4,3).
 */
static const uint64_t fixed_edges[][2] = {
	{ 0, 1 }, { 1, 2 }, { 2, 0 }, { 2, 3 }, { 3, 4 }, { 4, 3 },
	{ 4, 5 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])

/* inspect: print "\t(src, dst)" for each surviving intra-component edge. */
static void
inspect_edge(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	if (d <= 0)
		return;
	printf("\t(%llu, %llu)\n", (unsigned long long)dtfl_get_u64(rec),
	    (unsigned long long)dtfl_get_u64((const unsigned char *)rec + 8));
}

typedef struct build_ctx {
	dtfl_input_session *edges;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	dtfl_collection *scc = dtfl_algo_scc(edges);
	dtfl_collection *con = dtfl_collection_consolidate(scc);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_edge,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(scc);
	dtfl_collection_free(edges);
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
	c.edges = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "scc", tc, build, &c);

	if (dtfl_worker_index(w) == 0)
		for (i = 0; i < FIXED_E; i++) {
			unsigned char buf[16];

			dtfl_put_u64(buf, fixed_edges[i][0]);
			dtfl_put_u64(buf + 8, fixed_edges[i][1]);
			dtfl_input_session_insert(c.edges, buf, sizeof buf);
		}
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_flush(c.edges);
	while (dtfl_probe_less_than(c.probe, dtfl_input_session_time(c.edges)))
		dtfl_worker_step(w);
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
	/* Positional args (nodes edges batch rounds) are kept for parity. */
	int skip = argc > 4 ? 4 : 0;
	dtfl_config *cfg = ex_config(argc, argv, skip);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
