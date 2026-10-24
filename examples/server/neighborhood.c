/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   server/dataflows/neighborhood/src/lib.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * examples/server/neighborhood.c: the three-hop neighborhood plugin.
 *
 * Upstream (neighborhood/src/lib.rs):
 *
 *   let edges = handles.get_mut::<TraceHandle>(&args[0])?.import(dataflow);
 *   let source = args[1].parse::<usize>()?;
 *   let (_input, query) = dataflow.new_collection_from(Some(source));
 *   query.map(|x| (x, x))
 *        .join_core(edges.clone(), |_n, &q, &d| Some((d, q)))  // one hop
 *        .join_core(edges.clone(), |_n, &q, &d| Some((d, q)))  // two hops
 *        .join_core(edges,         |_n, &q, &d| Some((d, q)))  // three hops
 *        .map(|x| x.1)
 *        .consolidate()
 *        .inspect(...)
 *        .probe_with(probe);
 *
 * It walks three edge hops out from a query node and reports the originating
 * query for each node reached in exactly three hops.  The record carried
 * through the hops is (current-node, query); each hop joins against the
 * shared edges arrangement by current-node, re-keying by the destination.
 * All three hops read the same imported trace, so this plugin shares the one
 * edges index with degr_dist and reachability.
 *
 * Records are fixed-width u64 (D1): a node is a bare u64; the hop record is
 * (u64 current, u64 query); an edge is (u64 src, u64 dst).
 */
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dd_server.h"
#include "dtfl/record.h"
#include "dtfl/time.h"

#include "plugins.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* map(|x| (x, x)): a bare node becomes (node, node) = (current, query). */
static void
dup_node(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, rec, 8);		/* current node */
	memcpy(buf + 8, rec, 8);	/* query (the originating node) */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/*
 * join_core(|_n, &q, &d| Some((d, q))): the hop record is keyed by its
 * current node; v1 is the carried query, v2 is the edge destination.  Emit
 * (destination, query): advance to the destination, keep the query.
 */
static void
hop(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 8);		/* destination becomes the new current */
	memcpy(buf + 8, v1, 8);		/* carry the query */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* map(|x| x.1): keep only the query field (the second u64). */
static void
keep_query(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 8, 8);
}

/* inspect: report each reached node (its originating query). */
static void
report(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	FILE *out = ctx;

	(void)len;
	fprintf(out, "neighborhood: node %" PRIu64 " @ %" PRIu64 " : %"
	    PRId64 "\n", dtfl_get_u64(rec), time[0], *(const int64_t *)diff);
}

/* One hop against the imported edges arrangement.  Frees `in`, returns the
 * next hop's collection. */
static dtfl_collection *
one_hop(dtfl_collection *in, dtfl_arranged *edges, const dtfl_type *pair)
{
	dtfl_arranged *keyed = dtfl_collection_arrange_by_key(in,
	    dtfl_type_u64(), dtfl_type_u64(), "neighborhood hop");
	dtfl_collection *out = dtfl_arranged_join_core(keyed, edges, pair, hop,
	    DTFL_NOCTX);

	dtfl_arranged_free(keyed);
	dtfl_collection_free(in);
	return out;
}

static int
neighborhood_build(dtfl_dd_env *env)
{
	const char *graph;
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_trace *edges_trace;
	dtfl_arranged *edges;
	dtfl_input_session *query_in;
	dtfl_collection *query, *h0, *h1, *h2, *h3, *nodes, *cons, *ins, *probed;
	dtfl_diff *i64 = dtfl_diff_i64();
	uint64_t source;
	char *end;

	if (dtfl_dd_env_argc(env) != 2) {
		dtfl_type_free(pair);
		dtfl_diff_free(i64);
		return dtfl_plugin_argc_error("neighborhood", 2,
		    dtfl_dd_env_argc(env));
	}
	graph = dtfl_dd_env_arg(env, 0);
	source = strtoull(dtfl_dd_env_arg(env, 1), &end, 10);
	if (*end != '\0') {
		fprintf(stderr, "neighborhood: parse error, source: \"%s\"\n",
		    dtfl_dd_env_arg(env, 1));
		dtfl_type_free(pair);
		dtfl_diff_free(i64);
		return 1;
	}
	edges_trace = dtfl_trace_handler_get(dtfl_dd_env_handles(env), graph);
	if (edges_trace == NULL) {
		fprintf(stderr, "neighborhood: %s\n", dtfl_last_error_detail());
		dtfl_type_free(pair);
		dtfl_diff_free(i64);
		return 1;
	}

	edges = dtfl_trace_import(edges_trace, dtfl_dd_env_scope(env),
	    "neighborhood edges");
	query_in = dtfl_input_session_new(dtfl_type_u64(), i64);
	query = dtfl_input_session_to_collection(query_in,
	    dtfl_dd_env_scope(env));
	dtfl_input_session_insert(query_in, &source, 8);
	dtfl_dd_env_add_input(env, query_in);

	h0 = dtfl_collection_map(query, pair, dup_node, DTFL_NOCTX);
	h1 = one_hop(h0, edges, pair);
	h2 = one_hop(h1, edges, pair);
	h3 = one_hop(h2, edges, pair);
	nodes = dtfl_collection_map(h3, dtfl_type_u64(), keep_query,
	    DTFL_NOCTX);
	cons = dtfl_collection_consolidate(nodes);
	ins = dtfl_collection_inspect(cons, report, DTFL_CTX(stdout, NULL));
	probed = dtfl_collection_probe_with(ins, dtfl_dd_env_probe(env));

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(cons);
	dtfl_collection_free(nodes);
	dtfl_collection_free(h3);
	dtfl_collection_free(query);
	dtfl_arranged_free(edges);
	dtfl_type_free(pair);
	dtfl_diff_free(i64);
	return 0;
}

const dtfl_dataflow_plugin dtfl_plugin_neighborhood = {
	"neighborhood", neighborhood_build
};
