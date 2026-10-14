/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   server/dataflows/degr_dist/src/lib.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * examples/server/degr_dist.c: the degree-distribution plugin.
 *
 * Upstream (degr_dist/src/lib.rs):
 *
 *   handles.get_mut::<TraceHandle>(&args[0])?.import(dataflow)
 *       .as_collection(|&src,_dst| src)   // keep the source node
 *       .count_total()                    // (src, out-degree)
 *       .map(|(_deg, cnt)| cnt as usize)  // keep the degree
 *       .count_total()                    // (degree, how-many-nodes)
 *       .probe_with(probe);
 *
 * It imports a shared edges trace (keyed src -> dst) another plugin
 * published, counts each source's out-degree, then counts how many nodes
 * share each degree.  That is the degree distribution of the graph, and it
 * reads the one shared arrangement, not its own copy of the edges.
 *
 * The port registers through the static plugin table (dd_server.h): one
 * dtfl_dataflow_plugin named "degr_dist" with a build callback.  Records are
 * fixed-width u64 (D1): an edge is (u64 src, u64 dst), a count is appended as
 * an i64.
 */
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dd_server.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"

#include "plugins.h"

#include <stdint.h>
#include <stdio.h>

/* as_collection(|&src, _dst| src): keep the key (source node). */
static void
edge_to_src(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	(void)ctx; (void)klen; (void)val; (void)vlen;
	dtfl_arranged_emit_record(out, key, 8);
}

/* map(|(_deg, cnt)| cnt): the count_total record is (node, degree); keep the
 * degree field.  count_total renders the count in bytes 8..16 as an i64
 * record field; re-emit it in record encoding (not raw native bytes) so the
 * second count_total keys on the degree correctly. */
static void
keep_degree(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	int64_t degree;
	unsigned char buf[8];

	(void)ctx; (void)len;
	degree = dtfl_get_i64((const unsigned char *)rec + 8);
	dtfl_put_i64(buf, degree);
	dtfl_collection_emit_record(out, buf, 8);
}

static int
degr_dist_build(dtfl_dd_env *env)
{
	const char *graph;
	dtfl_trace *edges;
	dtfl_arranged *imported;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *srcs, *per_node, *degrees, *dist, *probed;
	int rc = 0;

	if (dtfl_dd_env_argc(env) != 1) {
		dtfl_diff_free(i64);
		return dtfl_plugin_argc_error("degr_dist", 1,
		    dtfl_dd_env_argc(env));
	}
	graph = dtfl_dd_env_arg(env, 0);
	edges = dtfl_trace_handler_get(dtfl_dd_env_handles(env), graph);
	if (edges == NULL) {
		fprintf(stderr, "degr_dist: %s\n", dtfl_last_error_detail());
		dtfl_diff_free(i64);
		return 1; /* DTFL_EINVAL: unknown shared trace */
	}

	imported = dtfl_trace_import(edges, dtfl_dd_env_scope(env),
	    "degr_dist edges");
	srcs = dtfl_arranged_as_collection(imported, dtfl_type_u64(),
	    edge_to_src, DTFL_NOCTX);
	per_node = dtfl_collection_count_total(srcs, i64);
	degrees = dtfl_collection_map(per_node, dtfl_type_i64(), keep_degree,
	    DTFL_NOCTX);
	dist = dtfl_collection_count_total(degrees, i64);
	probed = dtfl_collection_probe_with(dist, dtfl_dd_env_probe(env));

	/* Publish the distribution as a shared trace (a port addition over
	 * upstream, which only probes): keyed by degree -> count, so a peek
	 * or a downstream plugin can read the hand-checkable result. */
	{
		dtfl_arranged *out = dtfl_collection_arrange_by_key(dist,
		    dtfl_type_i64(), dtfl_type_i64(), "degr_dist out");
		const dtfl_type *rec = dtfl_type_pair(dtfl_type_i64(),
		    dtfl_type_i64());
		char name[256];

		snprintf(name, sizeof name, "%s-degrees", graph);
		dtfl_trace_handler_set(dtfl_dd_env_handles(env), name,
		    dtfl_arranged_trace(out), rec);
		dtfl_type_free(rec);
		dtfl_arranged_free(out);
	}

	dtfl_collection_free(probed);
	dtfl_collection_free(dist);
	dtfl_collection_free(degrees);
	dtfl_collection_free(per_node);
	dtfl_collection_free(srcs);
	dtfl_arranged_free(imported);
	dtfl_diff_free(i64);
	return rc;
}

const dtfl_dataflow_plugin dtfl_plugin_degr_dist = {
	"degr_dist", degr_dist_build
};
