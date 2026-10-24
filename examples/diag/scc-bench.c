/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/diag/scc-bench.c: mirrors diagnostics/examples/scc-bench.rs.
 *
 * scc-bench runs strongly_connected (differential-dataflow
 * algorithms::graphs::scc, dtfl_algo_scc) under the diagnostics console:
 * it registers a diagnostics session, starts the WebSocket server on
 * worker 0, builds scc over a hashed edge stream, loads an initial
 * graph, then applies rounds of edge changes.  A browser can connect to
 * the server and watch the live dataflow (the arrangements, the operator
 * graph, the per-operator timings).
 *
 * This C port keeps the same shape but runs a fixed, small, bounded
 * number of rounds and exits cleanly so it runs under the smoke-test
 * harness: a crash, a hang or a non-zero exit fails make check.  With no
 * argument it binds an ephemeral loopback port (D12) and does not block
 * waiting for a browser; pass a port as the first argument to serve on a
 * fixed ws://127.0.0.1:<port>.  Divergence: the upstream defaults
 * (100000 nodes, 200000 edges, run forever) are a benchmark; this port
 * uses a small deterministic graph so the smoke harness can check it.
 */
#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/diag.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NODES 50
#define EDGES 100
#define BATCH 10
#define ROUNDS 5

/* The same deterministic edge generator smoke.c uses: a hash so the
 * stream is reproducible without a seeded RNG (scc-bench.rs hashes the
 * index twice to pick a source and a destination). */
static uint64_t
mix(uint64_t x)
{
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return x;
}

static void
edge_for(size_t index, uint64_t *src, uint64_t *dst)
{
	uint64_t h1 = mix((uint64_t)index);
	uint64_t h2 = mix(h1);

	*src = h1 % NODES;
	*dst = h2 % NODES;
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
	dtfl_collection *probed = dtfl_collection_probe_with(scc, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(scc);
	dtfl_collection_free(edges);
}

typedef struct worker_ctx {
	uint16_t port;		/* 0 = ephemeral, no blocking */
} worker_ctx;

static void
worker(dtfl_worker *w, void *ctx)
{
	worker_ctx *wc = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_diag_session *s;
	dtfl_diag_server *srv = NULL;
	build_ctx c;
	size_t i, round;
	uint64_t t;

	s = dtfl_diag_session_new(w);
	if (dtfl_worker_index(w) == 0) {
		if (dtfl_diag_server_start(s, "127.0.0.1", wc->port, &srv) ==
		    DTFL_OK)
			fprintf(stderr,
			    "Diagnostics server on ws://127.0.0.1:%u\n",
			    (unsigned)dtfl_diag_server_port(srv));
	}

	c.edges = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "scc-bench", tc, build, &c);

	if (dtfl_worker_index(w) == 0)
		for (i = 0; i < EDGES; i++) {
			unsigned char buf[16];
			uint64_t src, dst;

			edge_for(i, &src, &dst);
			dtfl_put_u64(buf, src);
			dtfl_put_u64(buf + 8, dst);
			dtfl_input_session_insert(c.edges, buf, sizeof buf);
		}
	t = 1;
	dtfl_input_session_advance_to(c.edges, &t);
	dtfl_input_session_flush(c.edges);
	while (dtfl_probe_less_than(c.probe, dtfl_input_session_time(c.edges)))
		dtfl_worker_step(w);

	for (round = 0; round < ROUNDS; round++) {
		if (dtfl_worker_index(w) == 0)
			for (i = 0; i < BATCH; i++) {
				unsigned char buf[16];
				uint64_t src, dst;

				edge_for(round * BATCH + i, &src, &dst);
				dtfl_put_u64(buf, src);
				dtfl_put_u64(buf + 8, dst);
				dtfl_input_session_remove(c.edges, buf,
				    sizeof buf);
				edge_for(EDGES + round * BATCH + i, &src, &dst);
				dtfl_put_u64(buf, src);
				dtfl_put_u64(buf + 8, dst);
				dtfl_input_session_insert(c.edges, buf,
				    sizeof buf);
			}
		t = round + 2;
		dtfl_input_session_advance_to(c.edges, &t);
		dtfl_input_session_flush(c.edges);
		while (dtfl_probe_less_than(c.probe,
		    dtfl_input_session_time(c.edges)))
			dtfl_worker_step(w);
	}

	dtfl_input_session_close(c.edges);
	while (dtfl_worker_step(w))
		;

	if (srv != NULL)
		dtfl_diag_server_stop(srv);
	dtfl_diag_session_free(s);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);

	if (dtfl_worker_index(w) == 0)
		printf("scc-bench: %d rounds served\n", ROUNDS);
}

int
main(int argc, char **argv)
{
	worker_ctx wc;

	wc.port = (uint16_t)(argc > 1 ? strtoul(argv[1], NULL, 10) : 0);
	return dtfl_execute_directly(worker, &wc);
}
