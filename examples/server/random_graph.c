/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   server/dataflows/random_graph/src/lib.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * examples/server/random_graph.c: the random-graph generator plugin.
 *
 * Upstream (random_graph/src/lib.rs) is an open-loop source: a timely
 * `source` operator that plays out <edges> random edges and then a stream of
 * timed additions and deletions at a target <rate> per second, timestamping
 * each change by the system timer, maintaining an HDRHist latency summary,
 * and registering a droppable capability so the stream runs until the
 * capability is dropped.  It arranges the edges by key and publishes the
 * trace so the query plugins import it.
 *
 * Divergence: this port produces only the finite initial graph -- <edges>
 * deterministic edges at time 0 -- and publishes its arranged-by-key trace.
 * The open-loop machinery (timed additions/deletions keyed to wall-clock
 * rate, the HDRHist CCDF, the self-cancelling capability) is dropped: it is
 * non-deterministic by construction (it timestamps by the real clock), so it
 * cannot drive a replayable session or a hand-checked test, which is the
 * P14 exit criterion.  The <rate> argument is accepted and ignored.  The
 * generator is deterministic in <seed>, so degr_dist and reachability over
 * it have an exact, hand-computable answer.
 *
 * Arguments: <graph_name> <nodes> <edges> <rate>.  Records are fixed-width
 * u64 (D1): an edge is (u64 src, u64 dst).  The edges are inserted at time 0
 * and the input is handed to the server, which seals it on the first tick.
 */
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dd_server.h"
#include "dtfl/record.h"
#include "dtfl/time.h"

#include "plugins.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* splitmix64: a deterministic, well-mixed 64-bit generator seeded per draw,
 * matching the hash-derived style of the DDIR random source (ir.c). */
static uint64_t
mix(uint64_t x)
{
	x += UINT64_C(0x9e3779b97f4a7c15);
	x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
	return x ^ (x >> 31);
}

static int
random_graph_build(dtfl_dd_env *env)
{
	const char *name;
	uint64_t nodes, edges, e;
	dtfl_input_session *in;
	dtfl_collection *coll, *probed, *probed2;
	dtfl_arranged *arr;
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_diff *i64 = dtfl_diff_i64();
	char *end;
	uint64_t seed;

	if (dtfl_dd_env_argc(env) != 4) {
		dtfl_type_free(pair);
		dtfl_diff_free(i64);
		return dtfl_plugin_argc_error("random_graph", 4,
		    dtfl_dd_env_argc(env));
	}
	name = dtfl_dd_env_arg(env, 0);
	nodes = strtoull(dtfl_dd_env_arg(env, 1), &end, 10);
	if (*end != '\0' || nodes == 0) {
		fprintf(stderr, "random_graph: parse error, nodes: \"%s\"\n",
		    dtfl_dd_env_arg(env, 1));
		dtfl_type_free(pair);
		dtfl_diff_free(i64);
		return 1;
	}
	edges = strtoull(dtfl_dd_env_arg(env, 2), &end, 10);
	if (*end != '\0') {
		fprintf(stderr, "random_graph: parse error, edges: \"%s\"\n",
		    dtfl_dd_env_arg(env, 2));
		dtfl_type_free(pair);
		dtfl_diff_free(i64);
		return 1;
	}
	/* <rate> is accepted and ignored (see the divergence note above). */

	in = dtfl_input_session_new(pair, i64);
	coll = dtfl_input_session_to_collection(in, dtfl_dd_env_scope(env));

	/* The deterministic initial graph: <edges> edges at time 0. */
	seed = mix((uint64_t)nodes * 31 + edges);
	for (e = 0; e < edges; e++) {
		unsigned char buf[16];
		uint64_t src = mix(seed + 2 * e) % nodes;
		uint64_t dst = mix(seed + 2 * e + 1) % nodes;

		dtfl_put_u64(buf, src);
		dtfl_put_u64(buf + 8, dst);
		dtfl_input_session_insert(in, buf, sizeof buf);
	}
	dtfl_dd_env_add_input(env, in);

	/* arrange_by_key: index src -> dst, publish the shared trace. */
	arr = dtfl_collection_arrange_by_key(coll, dtfl_type_u64(),
	    dtfl_type_u64(), "random_graph edges");
	dtfl_trace_handler_set(dtfl_dd_env_handles(env), name,
	    dtfl_arranged_trace(arr), pair);

	/*
	 * Probe the arrangement read back as a collection, as upstream probes
	 * the source (source(...).probe_with(probe)).  Probing the arrangement
	 * output (not just the input stream) makes the server wait on tick for
	 * the trace to seal its empty batches up to each epoch; otherwise a
	 * later importer seeds a stale frontier from the last data batch
	 * (agent.c new_listener seeds from the last batch upper).
	 */
	probed = dtfl_arranged_as_collection(arr, pair, NULL, DTFL_NOCTX);
	probed2 = dtfl_collection_probe_with(probed, dtfl_dd_env_probe(env));

	dtfl_collection_free(probed2);
	dtfl_collection_free(probed);
	dtfl_arranged_free(arr);
	dtfl_collection_free(coll);
	dtfl_type_free(pair);
	dtfl_diff_free(i64);
	return 0;
}

const dtfl_dataflow_plugin dtfl_plugin_random_graph = {
	"random_graph", random_graph_build
};
