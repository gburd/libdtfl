/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/degrees.c: mirrors differential-dataflow/examples/degrees.rs.
 *
 * Counts the out-degree distribution of a directed graph: map each edge to
 * its source, count_total the sources (so each source has its out-degree),
 * and inspect the (source, degree) pairs.  Upstream builds a random graph
 * with a seeded StdRng and reports throughput; a C runtime cannot reproduce
 * that exact stream, so with no positional arguments this port feeds a small
 * fixed graph and prints the degree of every source, which is exact and
 * checkable.  With "nodes edges batch" positionals it keeps upstream's shape
 * (the remaining arguments are timely flags) but still uses the fixed graph.
 */
#include "example_args.h"

#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

/*
 * A fixed directed graph on a few nodes.  Out-degrees:
 *   0:3  1:2  2:1  3:1  4:0 ...
 */
static const uint64_t fixed_edges[][2] = {
	{ 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 4 }, { 2, 3 },
	{ 3, 0 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])

/* map (src, dst) -> src. */
static void
edge_to_src(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 8);
}

/* inspect: print "observed: (src, degree)" for each positive update. */
static void
inspect_degree(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	if (d <= 0)
		return;
	printf("observed: (%llu, %lld)\n",
	    (unsigned long long)dtfl_get_u64(rec),
	    (long long)dtfl_get_i64((const unsigned char *)rec + 8));
}

typedef struct build_ctx {
	dtfl_input_session *edges;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	dtfl_collection *srcs = dtfl_collection_map(edges, u64, edge_to_src,
	    DTFL_NOCTX);
	dtfl_collection *degrs = dtfl_collection_count_total(srcs, i64);
	dtfl_collection *ins = dtfl_collection_inspect(degrs, inspect_degree,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_diff_free(i64);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(degrs);
	dtfl_collection_free(srcs);
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
	dtfl_worker_dataflow(w, "degrees", tc, build, &c);

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
	/* Positional args (nodes edges batch) are kept for upstream parity. */
	int skip = argc > 3 ? 3 : 0;
	dtfl_config *cfg = ex_config(argc, argv, skip);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
