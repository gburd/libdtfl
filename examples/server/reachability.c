/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   server/dataflows/reachability/src/lib.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * examples/server/reachability.c: the reachability plugin.
 *
 * Upstream (reachability/src/lib.rs):
 *
 *   let edges = handles.get_mut::<TraceHandle>(&args[0])?.import(dataflow);
 *   let source = args[1].parse::<usize>()?;
 *   let (_input, roots) = dataflow.new_collection_from(Some(source));
 *   roots.clone().iterate(|scope, dists| {
 *       let edges = edges.enter(scope);
 *       let roots = roots.enter(scope);
 *       dists.arrange_by_self()
 *            .join_core(edges, |_src, _, &dst| Some(dst))
 *            .concat(roots)
 *            .distinct()
 *   })
 *   .probe_with(probe);
 *
 * It imports a shared edges trace (keyed src -> dst) and iterates the set of
 * nodes reachable from a single source: each round joins the current
 * reachable set against the edges to reach their destinations, folds the
 * source back in, and takes the distinct set.  The join reads the shared
 * arrangement directly (join_core on the imported trace), so this plugin
 * and degr_dist share one index over the same edges.
 *
 * Records are fixed-width u64 (D1): a node is a bare u64; an edge is
 * (u64 src, u64 dst); dists is arranged by self (node -> unit).
 */
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dd_server.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"

#include "plugins.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* join_core(|_src, _, &dst| Some(dst)): emit the edge destination. */
static void
emit_dst(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)v1; (void)v1l; (void)v2l;
	dtfl_join_emit_record(out, v2, 8);
}

typedef struct reach_ctx {
	dtfl_arranged *edges;	/* imported shared edges trace, outer scope */
	dtfl_collection *roots;	/* the single source node, outer scope */
	dtfl_diff *i64;
} reach_ctx;

static dtfl_collection *
reach_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	reach_ctx *c = vctx;
	dtfl_arranged *edges = dtfl_arranged_enter(c->edges, inner);
	/* dists.arrange_by_self(): key is the node, value is unit. */
	dtfl_arranged *dists = dtfl_collection_arrange_by_key(var,
	    dtfl_type_u64(), dtfl_type_unit(), "reachability dists");
	dtfl_collection *reached = dtfl_arranged_join_core(dists, edges,
	    dtfl_type_u64(), emit_dst, DTFL_NOCTX);
	dtfl_collection *roots = dtfl_collection_enter(c->roots, inner);
	dtfl_collection *merged = dtfl_collection_concat(reached, roots);
	dtfl_collection *distinct = dtfl_collection_distinct(merged, c->i64);

	dtfl_collection_free(merged);
	dtfl_collection_free(roots);
	dtfl_collection_free(reached);
	dtfl_arranged_free(dists);
	dtfl_arranged_free(edges);
	return distinct;
}

static int
reachability_build(dtfl_dd_env *env)
{
	const char *graph;
	dtfl_trace *edges_trace;
	dtfl_arranged *imported;
	dtfl_input_session *root_in;
	dtfl_collection *roots, *result, *probed;
	uint64_t source;
	char *end;
	reach_ctx c;

	if (dtfl_dd_env_argc(env) != 2)
		return dtfl_plugin_argc_error("reachability", 2,
		    dtfl_dd_env_argc(env));
	graph = dtfl_dd_env_arg(env, 0);
	source = strtoull(dtfl_dd_env_arg(env, 1), &end, 10);
	if (*end != '\0') {
		fprintf(stderr, "reachability: parse error, source: \"%s\"\n",
		    dtfl_dd_env_arg(env, 1));
		return 1;
	}
	edges_trace = dtfl_trace_handler_get(dtfl_dd_env_handles(env), graph);
	if (edges_trace == NULL) {
		fprintf(stderr, "reachability: %s\n", dtfl_last_error_detail());
		return 1;
	}

	c.i64 = dtfl_diff_i64();
	imported = dtfl_trace_import(edges_trace, dtfl_dd_env_scope(env),
	    "reachability edges");

	/* new_collection_from(Some(source)): a one-row input seeded with the
	 * source node.  The server drives this input each tick. */
	root_in = dtfl_input_session_new(dtfl_type_u64(), c.i64);
	roots = dtfl_input_session_to_collection(root_in, dtfl_dd_env_scope(env));
	dtfl_input_session_insert(root_in, &source, 8);
	dtfl_dd_env_add_input(env, root_in);

	c.edges = imported;
	c.roots = roots;
	result = dtfl_collection_iterate(roots, reach_logic,
	    DTFL_CTX(&c, NULL));
	probed = dtfl_collection_probe_with(result, dtfl_dd_env_probe(env));

	/* Publish the reachable set as a shared trace, keyed by node. */
	{
		dtfl_arranged *out = dtfl_collection_arrange_by_key(result,
		    dtfl_type_u64(), dtfl_type_unit(), "reachability out");
		char name[256];

		snprintf(name, sizeof name, "%s-reach", graph);
		dtfl_trace_handler_set(dtfl_dd_env_handles(env), name,
		    dtfl_arranged_trace(out), dtfl_type_u64());
		dtfl_arranged_free(out);
	}

	dtfl_collection_free(probed);
	dtfl_collection_free(result);
	dtfl_collection_free(roots);
	dtfl_arranged_free(imported);
	dtfl_diff_free(c.i64);
	return 0;
}

const dtfl_dataflow_plugin dtfl_plugin_reachability = {
	"reachability", reachability_build
};
