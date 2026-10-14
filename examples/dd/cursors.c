/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/cursors.c: mirrors differential-dataflow/examples/cursors.rs.
 *
 * Arranges a graph of (u32, u32) edges by the first coordinate: map
 * (x, y) -> (x, (x, y)) then arrange_by_key, keeping the trace so it can be
 * enumerated at runtime.  At round i (1..=rounds) it inserts edges (i, i+1)
 * and (i+1, i) and, for i > 1, removes (i-1, i), probing the arrangement
 * so a round's updates have all landed before the next.  After the last
 * round it dumps the whole trace with dtfl_trace_to_vec (CursorDebug::to_vec)
 * and validates the accumulated (edge -> net diff) multiset against the
 * hand-computed final graph.  Upstream splits the dump across workers and
 * prefixes each line with "round N, wX"; this single-worker port verifies
 * the accumulated content, which is worker-count independent.
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed round count; matches the 3-round invocation in cursors.rs's doc. */
#define ROUNDS 3

/* map (x, y) -> (x, (x, y)): key is x (u32), value is the edge pair. */
static void
edge_by_src(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[12];

	(void)ctx; (void)len;
	memcpy(buf, rec, 4);		/* key:   x        */
	memcpy(buf + 4, rec, 8);	/* value: (x, y)   */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* One dumped (edge -> net diff) entry, accumulated across (time, diff). */
typedef struct edge_sum {
	uint32_t a;
	uint32_t b;
	int64_t diff;
} edge_sum;

typedef struct dump {
	edge_sum *v;
	size_t n;
	size_t cap;
} dump;

/* Fold one (key, value, time, diff) tuple into the per-edge net diff. */
static void
collect(void *ctx, const void *key, size_t klen, const void *val, size_t vlen,
    const uint64_t *time, const void *diff)
{
	dump *d = ctx;
	uint32_t a, b;
	int64_t dd;
	size_t i;

	(void)key; (void)klen; (void)time;
	/* The stored value is the edge pair (x, y). */
	(void)vlen;
	a = dtfl_get_u32((const unsigned char *)val);
	b = dtfl_get_u32((const unsigned char *)val + 4);
	memcpy(&dd, diff, sizeof dd);
	for (i = 0; i < d->n; i++)
		if (d->v[i].a == a && d->v[i].b == b) {
			d->v[i].diff += dd;
			return;
		}
	if (d->n == d->cap) {
		d->cap = d->cap ? d->cap * 2 : 8;
		d->v = realloc(d->v, d->cap * sizeof *d->v);
	}
	d->v[d->n].a = a;
	d->v[d->n].b = b;
	d->v[d->n].diff = dd;
	d->n++;
}

static int
cmp_edge(const void *x, const void *y)
{
	const edge_sum *p = x, *q = y;

	if (p->a != q->a)
		return p->a < q->a ? -1 : 1;
	if (p->b != q->b)
		return p->b < q->b ? -1 : 1;
	return 0;
}

/* The trace survives the build scope so the worker can enumerate it. */
typedef struct build_ctx {
	dtfl_input_session *graph;
	dtfl_probe *probe;
	dtfl_trace *trace;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *pair = dtfl_type_pair(u32, u32);
	const dtfl_type *kv = dtfl_type_pair(u32, pair);
	dtfl_collection *edges = dtfl_input_session_to_collection(c->graph,
	    scope);
	dtfl_collection *keyed = dtfl_collection_map(edges, kv, edge_by_src,
	    DTFL_NOCTX);
	dtfl_arranged *arr = dtfl_collection_arrange_by_key(keyed, u32, pair,
	    "graph");
	/* Read the arrangement back so a probe tells us a round has landed. */
	dtfl_collection *as_coll = dtfl_arranged_as_collection(arr, kv, NULL,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(as_coll, c->probe);

	c->trace = dtfl_arranged_trace(arr);

	dtfl_type_free(pair);
	dtfl_type_free(kv);
	dtfl_collection_free(probed);
	dtfl_collection_free(as_coll);
	dtfl_arranged_free(arr);
	dtfl_collection_free(keyed);
	dtfl_collection_free(edges);
}

static void
insert_edge(dtfl_input_session *s, uint32_t a, uint32_t b)
{
	unsigned char buf[8];

	dtfl_put_u32(buf, a);
	dtfl_put_u32(buf + 4, b);
	dtfl_input_session_insert(s, buf, sizeof buf);
}

static void
remove_edge(dtfl_input_session *s, uint32_t a, uint32_t b)
{
	unsigned char buf[8];

	dtfl_put_u32(buf, a);
	dtfl_put_u32(buf + 4, b);
	dtfl_input_session_remove(s, buf, sizeof buf);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u32();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *edge_type = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	build_ctx c;
	dump d = { NULL, 0, 0 };
	size_t i;
	uint32_t round;
	int ok;

	(void)ctx;
	c.graph = dtfl_input_session_new(edge_type, i64);
	c.probe = dtfl_probe_new();
	c.trace = NULL;
	dtfl_worker_dataflow(w, "cursors", tc, build, &c);

	/* At round i: insert (i, i+1) and (i+1, i); for i > 1 remove (i-1, i). */
	for (round = 1; round <= ROUNDS; round++) {
		uint64_t t = round;

		insert_edge(c.graph, round, round + 1);
		insert_edge(c.graph, round + 1, round);
		if (round > 1)
			remove_edge(c.graph, round - 1, round);
		dtfl_input_session_advance_to(c.graph, &t);
		dtfl_input_session_flush(c.graph);
		while (dtfl_probe_less_than(c.probe, &t))
			dtfl_worker_step(w);
	}
	dtfl_input_session_close(c.graph);
	while (dtfl_worker_step(w))
		;

	/* Dump the entire trace content, then validate the net multiset. */
	dtfl_trace_to_vec(c.trace, collect, DTFL_CTX(&d, NULL));

	/* Drop the zero-diff edges, as upstream's summary aggregation does. */
	{
		size_t k = 0;

		for (i = 0; i < d.n; i++)
			if (d.v[i].diff != 0)
				d.v[k++] = d.v[i];
		d.n = k;
	}
	if (d.n > 1)
		qsort(d.v, d.n, sizeof *d.v, cmp_edge);

	printf("Final graph:\n");
	for (i = 0; i < d.n; i++)
		printf("  (%lu, %lu): %lld\n", (unsigned long)d.v[i].a,
		    (unsigned long)d.v[i].b, (long long)d.v[i].diff);

	/*
	 * Expected: (i+1, i): 1 for i in 1..=ROUNDS, plus (ROUNDS, ROUNDS+1): 1.
	 * For ROUNDS = 3 that is {(2,1):1, (3,2):1, (3,4):1, (4,3):1}.
	 */
	ok = 1;
	{
		edge_sum exp[ROUNDS + 1];
		size_t ne = 0;

		for (round = 1; round <= ROUNDS; round++) {
			exp[ne].a = round + 1;
			exp[ne].b = round;
			exp[ne].diff = 1;
			ne++;
		}
		exp[ne].a = ROUNDS;
		exp[ne].b = ROUNDS + 1;
		exp[ne].diff = 1;
		ne++;
		if (ne > 1)
			qsort(exp, ne, sizeof *exp, cmp_edge);
		if (ne != d.n)
			ok = 0;
		for (i = 0; ok && i < ne; i++)
			if (exp[i].a != d.v[i].a || exp[i].b != d.v[i].b ||
			    exp[i].diff != d.v[i].diff)
				ok = 0;
	}
	printf("validation: %s\n", ok ? "ok" : "FAILED");

	free(d.v);
	dtfl_trace_free(c.trace);
	dtfl_probe_free(c.probe);
	dtfl_type_free(edge_type);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
	if (!ok)
		exit(1);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc;

	/* A single worker so the trace dump is the whole, unsplit graph. */
	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
