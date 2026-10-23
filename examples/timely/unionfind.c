/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/unionfind.c: mirrors timely/examples/unionfind.rs.
 *
 * Arguments: [nodes edges batch] [timely flags].  Edges flow through two
 * union-find operators (the second behind an exchange that gathers them
 * on worker 0); each operator keeps roots and ranks and emits (x, y) only
 * for edges that join two distinct components, which are the edges of a
 * spanning forest.  With no positional arguments a small fixed graph is
 * used so the spanning-forest edge set is exact and checkable; with
 * nodes/edges/batch a random graph is generated as upstream.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A fixed graph on 6 nodes: a path 0-1-2, a path 3-4, and a redundant edge. */
static const uint64_t fixed_edges[][2] = {
	{ 0, 1 }, { 1, 2 }, { 0, 2 }, { 3, 4 }, { 4, 3 }, { 2, 1 },
};
#define FIXED_N (sizeof fixed_edges / sizeof fixed_edges[0])

/* ------------------------------------------------------------------ */
/* The union-find operator (unary Pipeline, keeps roots and ranks)     */
/* ------------------------------------------------------------------ */

typedef struct uf_ctx {
	uint64_t *roots;
	uint8_t *ranks;
	size_t len, cap;
} uf_ctx;

static void
uf_grow(uf_ctx *c, size_t m)
{
	size_t i;

	if (m + 1 <= c->len)
		return;
	if (m + 1 > c->cap) {
		c->cap = m + 1 > c->cap * 2 ? m + 1 : c->cap * 2;
		c->roots = realloc(c->roots, c->cap * sizeof *c->roots);
		c->ranks = realloc(c->ranks, c->cap * sizeof *c->ranks);
	}
	for (i = c->len; i < m + 1; i++) {
		c->roots[i] = i;
		c->ranks[i] = 0;
	}
	c->len = m + 1;
}

static bool
uf_logic(dtfl_op *op, void *vctx)
{
	uf_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			uint64_t x = dtfl_get_u64(rec);
			uint64_t y = dtfl_get_u64((const unsigned char *)rec + 8);
			uint64_t mx = x > y ? x : y;

			uf_grow(c, (size_t)mx);
			while (x != c->roots[x])
				x = c->roots[x];
			while (y != c->roots[y])
				y = c->roots[y];
			if (x != y) {
				unsigned char buf[16];

				dtfl_put_u64(buf, x);
				dtfl_put_u64(buf + 8, y);
				dtfl_session_give(s, buf, sizeof buf);
				if (c->ranks[x] < c->ranks[y]) {
					c->roots[x] = y;
				} else if (c->ranks[x] > c->ranks[y]) {
					c->roots[y] = x;
				} else {
					c->roots[y] = x;
					c->ranks[x]++;
				}
			}
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
uf_free(void *vctx)
{
	uf_ctx *c = vctx;

	free(c->roots);
	free(c->ranks);
	free(c);
}

static dtfl_stream *
union_find(dtfl_stream *in, dtfl_pact pact)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(in),
	    "UnionFind");
	uf_ctx *c = calloc(1, sizeof *c);
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	dtfl_stream *out;

	dtfl_op_builder_input(b, in, pact);
	/* As Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	out = dtfl_op_builder_output(b, pair);
	dtfl_op_builder_build(b, uf_logic, c, uf_free);
	dtfl_type_free(pair);
	return out;
}

/* ------------------------------------------------------------------ */
/* Sink: counts the spanning-forest edges (deterministic for checking) */
/* ------------------------------------------------------------------ */

static uint64_t
route_zero(const void *rec, size_t len, void *ctx)
{
	(void)rec; (void)len; (void)ctx;
	return 0;
}

static bool
count_logic(dtfl_op *op, void *vctx)
{
	size_t *n = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL)
		*n += dtfl_message_len(m);
	return false;
}

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
	size_t *count;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);
	dtfl_op_builder *b;

	s = union_find(s, dtfl_pact_pipeline());
	s = union_find(s, dtfl_pact_exchange(route_zero, NULL));
	s = dtfl_stream_probe_with(s, c->probe);
	b = dtfl_op_builder_new(scope, "Count");
	dtfl_op_builder_input(b, s, dtfl_pact_pipeline());
	dtfl_op_builder_build(b, count_logic, c->count, NULL);
}

typedef struct run_ctx {
	bool fixed;
	size_t nodes, edges, batch;
	size_t total_count[16];
} run_ctx;

static void
worker(dtfl_worker *w, void *ctx)
{
	run_ctx *r = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	size_t index = dtfl_worker_index(w);
	size_t peers = dtfl_worker_peers(w);
	build_ctx c;
	uint64_t next = 1;
	size_t i;

	c.input = dtfl_input_new(pair);
	c.probe = dtfl_probe_new();
	c.count = &r->total_count[index];
	dtfl_worker_dataflow(w, "unionfind", tc, build, &c);
	dtfl_type_free(pair);

	if (r->fixed) {
		/* Worker 0 feeds the whole fixed graph, so output is exact. */
		if (index == 0)
			for (i = 0; i < FIXED_N; i++) {
				unsigned char buf[16];

				dtfl_put_u64(buf, fixed_edges[i][0]);
				dtfl_put_u64(buf + 8, fixed_edges[i][1]);
				dtfl_input_send(c.input, buf, sizeof buf);
			}
	} else {
		/* Random graph: deterministic linear-congruential arcs. */
		uint64_t seed = 0x9e3779b97f4a7c15ull * (index + 1);
		size_t per = r->edges / (peers ? peers : 1);

		for (i = 0; i < per; i++) {
			unsigned char buf[16];
			uint64_t a, bb;

			seed = seed * 6364136223846793005ull + 1442695040888963407ull;
			a = (seed >> 33) % r->nodes;
			seed = seed * 6364136223846793005ull + 1442695040888963407ull;
			bb = (seed >> 33) % r->nodes;
			dtfl_put_u64(buf, a);
			dtfl_put_u64(buf + 8, bb);
			dtfl_input_send(c.input, buf, sizeof buf);
		}
	}
	dtfl_input_advance_to(c.input, &next);
	while (dtfl_probe_less_than(c.probe, dtfl_input_time(c.input)))
		dtfl_worker_step(w);
	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	run_ctx r;
	dtfl_config *cfg;
	int rc, skip = 0;
	size_t i;

	memset(&r, 0, sizeof r);
	/* With three leading numeric positionals, run the random graph. */
	r.fixed = argc < 4;
	if (!r.fixed) {
		r.nodes = ex_size(argc, argv, 1, 100);
		r.edges = ex_size(argc, argv, 2, 1000);
		r.batch = ex_size(argc, argv, 3, 100);
		if (r.nodes == 0)
			r.nodes = 1;
		skip = 3;
	}
	cfg = ex_config(argc, argv, skip);
	rc = dtfl_execute(cfg, worker, &r, NULL);
	dtfl_config_free(cfg);

	if (r.fixed) {
		size_t total = 0;

		for (i = 0; i < 16; i++)
			total += r.total_count[i];
		/* 6 nodes in two components (0-1-2 and 3-4): 3 forest edges. */
		printf("spanning forest edges: %zu\n", total);
	}
	return rc;
}
