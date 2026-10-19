/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/arrange.c: mirrors differential-dataflow/examples/arrange.rs.
 *
 * Arguments: nodes edges batch pre inspect [rounds] [timely flags].
 *
 * One dataflow holds an ever-changing edge collection: a raw source
 * operator ("RandomGraph", timely's generic::source) emits explicit
 * ((src, dst), time, diff) updates.  At each time t it adds edge number
 * t + edges - 1 and deletes edge number t - 1, so the live graph at time
 * t is the window of edges [t, t + edges) drawn from a seeded generator.
 * It emits `batch` times per activation, but only once the shared probe
 * has caught up with its capability, and then downgrades its capability
 * past them.  The stream is viewed as a collection (AsCollection) and
 * arranged by source node; the arrangement's trace is shared.
 *
 * Two query dataflows import that trace: an iterative distance labelling
 * from a `roots` input (upstream never inserts a root, so it prints
 * nothing, as upstream), and a three-hop walk from a `query` input whose
 * consolidated result, the query node with multiplicity equal to the
 * number of three-edge walks from it, is printed when `inspect` is given.
 * The driver inserts node round % nodes, advances both inputs by `batch`,
 * removes the node again, and steps until the probe passes.
 *
 * Divergences, each to make the run finite and checkable: upstream loops
 * over rounds forever and the source never releases its capability; this
 * port stops after `rounds` rounds (default 4) and then tells the source
 * to drop its capability so the worker drains.  Upstream draws edges from
 * a seeded StdRng; a C runtime cannot reproduce that stream, so this port
 * uses a per-worker seeded xorshift64*.  The elapsed-time column of the
 * progress lines is omitted so the output is deterministic.  With no
 * positional arguments it runs nodes=5 edges=6 batch=2 pre=0 inspect.
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct params {
	uint32_t nodes;
	uint64_t edges;
	uint64_t batch;
	uint64_t pre;
	bool inspect;
	uint64_t rounds;
} params;

/* A deterministic xorshift64* generator (upstream: a seeded StdRng). */
static uint64_t
rng_next(uint64_t *s)
{
	*s ^= *s >> 12;
	*s ^= *s << 25;
	*s ^= *s >> 27;
	return *s * 0x2545F4914F6CDD1DULL;
}

/* gen_range(0, nodes). */
static uint32_t
rng_range(uint64_t *s, uint32_t nodes)
{
	return (uint32_t)(rng_next(s) % nodes);
}

/* ------------------------------------------------------------------ */
/* The RandomGraph source                                              */
/* ------------------------------------------------------------------ */

/*
 * Owned by the source operator and freed with it.  `cap` and `act` are
 * taken on the first schedule and released when the driver sets *stop.
 */
typedef struct source_ctx {
	const params *p;
	dtfl_probe *probe;	/* shared with every query dataflow */
	const bool *stop;	/* set by the driver once its rounds are done */
	size_t index, peers;
	uint64_t rng1, rng2;	/* additions, deletions: the same sequence */
	uint64_t additions, deletions;
	dtfl_cap *cap;
	dtfl_activator *act;
	bool started;
} source_ctx;

/* ((src, dst), time, diff): the update layout dtfl_stream_as_collection reads. */
static void
give_update(dtfl_session *s, uint32_t src, uint32_t dst, uint64_t t,
    int64_t diff)
{
	unsigned char buf[24];

	dtfl_put_u32(buf, src);
	dtfl_put_u32(buf + 4, dst);
	memcpy(buf + 8, &t, sizeof t);
	memcpy(buf + 16, &diff, sizeof diff);
	dtfl_session_give(s, buf, sizeof buf);
}

static bool
source_logic(dtfl_op *op, void *vctx)
{
	source_ctx *c = vctx;
	const params *p = c->p;

	if (!c->started) {
		c->cap = dtfl_op_initial_cap(op, 0);
		c->act = dtfl_op_activator(op);
		c->started = true;
	}
	if (c->cap == NULL)
		return false;
	if (*c->stop) {
		/* Releasing the capability lets the worker drain. */
		dtfl_cap_drop(c->cap);
		dtfl_activator_free(c->act);
		c->cap = NULL;
		c->act = NULL;
		return false;
	}

	/* Do nothing if the probe is not caught up to us. */
	if (!dtfl_probe_less_than(c->probe, dtfl_cap_time(c->cap))) {
		uint64_t time = dtfl_cap_time(c->cap)[0];
		dtfl_session *s = dtfl_op_session(op, 0, c->cap);
		uint64_t b;

		/* We want to send at times.inner + (0 .. batch). */
		for (b = 0; b < p->batch; b++) {
			while (c->additions < time + p->edges) {
				if (c->additions % c->peers == c->index) {
					uint32_t src = rng_range(&c->rng1, p->nodes);
					uint32_t dst = rng_range(&c->rng1, p->nodes);

					give_update(s, src, dst, time, 1);
				}
				c->additions++;
			}
			while (c->deletions < time) {
				if (c->deletions % c->peers == c->index) {
					uint32_t src = rng_range(&c->rng2, p->nodes);
					uint32_t dst = rng_range(&c->rng2, p->nodes);

					give_update(s, src, dst, time, -1);
				}
				c->deletions++;
			}
			time++;
		}
		dtfl_session_close(s);
		dtfl_cap_downgrade(c->cap, &time);
	}
	dtfl_activator_activate(c->act);
	return false;
}

static void
source_ctx_free(void *vctx)
{
	source_ctx *c = vctx;

	if (c->cap != NULL)
		dtfl_cap_drop(c->cap);
	if (c->act != NULL)
		dtfl_activator_free(c->act);
	free(c);
}

/* ------------------------------------------------------------------ */
/* Dataflows                                                           */
/* ------------------------------------------------------------------ */

typedef struct worker_ctx {
	const params *p;
	dtfl_probe *probe;
	bool stop;
	dtfl_trace *graph;
	dtfl_input_session *roots;
	dtfl_input_session *query;
	dtfl_diff *i64;
	const dtfl_type *pair;	/* (u32, u32) */
	const dtfl_time_class *tc;
} worker_ctx;

static void
build_graph(dtfl_scope *scope, void *arg)
{
	worker_ctx *w = arg;
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *fields[4] = { u32, u32, dtfl_type_u64(),
	    dtfl_type_i64() };
	const dtfl_type *upd = dtfl_type_tuple(fields, 4);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "RandomGraph");
	dtfl_stream *out = dtfl_op_builder_output(b, upd);
	source_ctx *c = calloc(1, sizeof *c);
	dtfl_stream *probed;
	dtfl_collection *coll;
	dtfl_arranged *arr;
	uint64_t seed = 0x0102030000000000ULL | dtfl_scope_index(scope);

	c->p = w->p;
	c->probe = w->probe;
	c->stop = &w->stop;
	c->index = dtfl_scope_index(scope);
	c->peers = dtfl_scope_peers(scope);
	c->rng1 = c->rng2 = seed;
	dtfl_op_builder_build(b, source_logic, c, source_ctx_free);

	probed = dtfl_stream_probe_with(out, w->probe);
	coll = dtfl_stream_as_collection(probed, w->pair,
	    dtfl_diff_retain(w->i64));
	arr = dtfl_collection_arrange_by_key(coll, u32, u32, "ArrangeByKey");
	w->graph = dtfl_arranged_trace(arr);

	dtfl_arranged_free(arr);
	dtfl_collection_free(coll);
	dtfl_type_free(upd);
}

/* inspect: print upstream's {:?} of (data, Product((), t), diff). */
static void
print_update(const char *prefix, uint32_t v, const dtfl_time_class *tc,
    const uint64_t *time, const void *diff)
{
	char tb[64];
	int64_t d;

	memcpy(&d, diff, sizeof d);
	dtfl_time_format(tc, time, tb, sizeof tb);
	printf("%s(%u, %s, %lld)\n", prefix, (unsigned)v, tb, (long long)d);
}

/* roots.map(|x| (x, 0)) */
static void
root_at_zero(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	memcpy(buf, rec, 4);
	dtfl_put_u32(buf + 4, 0);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* join_core(edges, |_k, l, d| Some((*d, l + 1))) */
static void
step_dist(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 4);
	dtfl_put_u32(buf + 4, dtfl_get_u32(v1) + 1);
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* reduce(|_, s, t| t.push((*s[0].0, 1))): the least distance. */
static void
keep_min(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_reduce_emit_pair(out, vals[0], 4, &one);
}

/* map(|(_node, dist)| dist) */
static void
take_second(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 4, 4);
}

static void
inspect_dist(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	(void)len;
	print_update("distance update: ", dtfl_get_u32(rec), ctx, time, diff);
}

typedef struct roots_ctx {
	worker_ctx *w;
	dtfl_arranged *edges;	/* imported, outer scope */
	dtfl_collection *roots;	/* (node, 0), outer scope */
} roots_ctx;

static dtfl_collection *
roots_logic(void *vctx, dtfl_scope *inner, dtfl_collection *dists)
{
	roots_ctx *r = vctx;
	const dtfl_type *u32 = dtfl_type_u32();
	dtfl_arranged *edges = dtfl_arranged_enter(r->edges, inner);
	dtfl_collection *roots = dtfl_collection_enter(r->roots, inner);
	dtfl_arranged *arr = dtfl_collection_arrange_by_key(dists, u32, u32,
	    "ArrangeByKey");
	dtfl_collection *joined = dtfl_arranged_join_core(arr, edges,
	    r->w->pair, step_dist, DTFL_NOCTX);
	dtfl_collection *merged = dtfl_collection_concat(joined, roots);
	dtfl_collection *reduced = dtfl_collection_reduce(merged, u32, u32,
	    u32, r->w->i64, keep_min, DTFL_NOCTX);

	dtfl_collection_free(merged);
	dtfl_collection_free(joined);
	dtfl_arranged_free(arr);
	dtfl_collection_free(roots);
	dtfl_arranged_free(edges);
	return reduced;
}

static void
build_roots(dtfl_scope *scope, void *arg)
{
	worker_ctx *w = arg;
	dtfl_arranged *edges = dtfl_trace_import(w->graph, scope, "ImportGraph");
	dtfl_collection *roots = dtfl_input_session_to_collection(w->roots,
	    scope);
	dtfl_collection *seeded = dtfl_collection_map(roots, w->pair,
	    root_at_zero, DTFL_NOCTX);
	roots_ctx r = { w, edges, seeded };
	dtfl_collection *it = dtfl_collection_iterate(seeded, roots_logic,
	    DTFL_CTX(&r, NULL));
	dtfl_collection *dist = dtfl_collection_map(it, dtfl_type_u32(),
	    take_second, DTFL_NOCTX);
	dtfl_collection *con = dtfl_collection_consolidate(dist);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_dist,
	    DTFL_CTX((void *)w->tc, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, w->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(dist);
	dtfl_collection_free(it);
	dtfl_collection_free(seeded);
	dtfl_collection_free(roots);
	dtfl_arranged_free(edges);
}

/* query.map(|x| (x, x)) */
static void
self_pair(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	memcpy(buf, rec, 4);
	memcpy(buf + 4, rec, 4);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* join_core(edges, |_n, &q, &d| Some((d, q))) */
static void
hop(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 4);
	memcpy(buf + 4, v1, 4);
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* filter(move |_| inspect) */
static bool
keep_if_inspect(void *ctx, const void *rec, size_t len)
{
	(void)rec; (void)len;
	return *(const bool *)ctx;
}

static void
inspect_query(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	(void)len;
	print_update("", dtfl_get_u32(rec), ctx, time, diff);
}

static void
build_query(dtfl_scope *scope, void *arg)
{
	worker_ctx *w = arg;
	const dtfl_type *u32 = dtfl_type_u32();
	dtfl_arranged *edges = dtfl_trace_import(w->graph, scope, "ImportGraph");
	dtfl_collection *query = dtfl_input_session_to_collection(w->query,
	    scope);
	dtfl_collection *cur = dtfl_collection_map(query, w->pair, self_pair,
	    DTFL_NOCTX);
	dtfl_collection *filt, *qs, *con, *ins, *probed;
	int i;

	for (i = 0; i < 3; i++) {
		dtfl_arranged *a = dtfl_collection_arrange_by_key(cur, u32, u32,
		    "ArrangeByKey");
		dtfl_collection *next = dtfl_arranged_join_core(a, edges,
		    w->pair, hop, DTFL_NOCTX);

		dtfl_arranged_free(a);
		dtfl_collection_free(cur);
		cur = next;
	}
	filt = dtfl_collection_filter(cur, keep_if_inspect,
	    DTFL_CTX((void *)&w->p->inspect, NULL));
	qs = dtfl_collection_map(filt, u32, take_second, DTFL_NOCTX);
	con = dtfl_collection_consolidate(qs);
	ins = dtfl_collection_inspect(con, inspect_query,
	    DTFL_CTX((void *)w->tc, NULL));
	probed = dtfl_collection_probe_with(ins, w->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(qs);
	dtfl_collection_free(filt);
	dtfl_collection_free(cur);
	dtfl_collection_free(query);
	dtfl_arranged_free(edges);
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

typedef struct probe_cond {
	dtfl_probe *probe;
	const uint64_t *time;
} probe_cond;

static bool
probe_behind(void *vctx)
{
	probe_cond *pc = vctx;

	return dtfl_probe_less_than(pc->probe, pc->time);
}

static void
worker(dtfl_worker *wk, void *vctx)
{
	const params *p = vctx;
	dtfl_time_class *unit = dtfl_time_unit();
	dtfl_time_class *inner = dtfl_time_u64();
	/* Product<(), usize>: one word, totally ordered. */
	dtfl_time_class *tc = dtfl_time_product(unit, inner);
	worker_ctx w;
	uint64_t i, round;

	memset(&w, 0, sizeof w);
	w.p = p;
	w.tc = tc;
	w.i64 = dtfl_diff_i64();
	w.pair = dtfl_type_pair(dtfl_type_u32(), dtfl_type_u32());
	w.probe = dtfl_probe_new();
	w.roots = dtfl_input_session_new(dtfl_type_u32(), w.i64);
	w.query = dtfl_input_session_new(dtfl_type_u32(), w.i64);

	dtfl_worker_dataflow(wk, "graph", tc, build_graph, &w);
	printf("loading edges\n");
	for (i = 0; i < p->pre; i++)
		dtfl_worker_step(wk);
	printf("edges loaded; building query dataflows\n");
	dtfl_worker_dataflow(wk, "roots", tc, build_roots, &w);
	dtfl_worker_dataflow(wk, "query", tc, build_query, &w);
	printf("query dataflows built; querying\n");

	/* The trace will not compact unless we release capabilities. */
	dtfl_trace_free(w.graph);
	w.graph = NULL;

	if (p->batch > 0) {
		for (round = 0; round < p->rounds; round++) {
			uint64_t time = dtfl_input_session_time(w.query)[0];
			unsigned char q[4];
			probe_cond pc;

			dtfl_put_u32(q, (uint32_t)(round % p->nodes));
			if (dtfl_worker_index(wk) == 0)
				dtfl_input_session_insert(w.query, q, 4);
			time += p->batch;
			dtfl_input_session_advance_to(w.roots, &time);
			dtfl_input_session_flush(w.roots);
			dtfl_input_session_advance_to(w.query, &time);
			dtfl_input_session_flush(w.query);
			if (dtfl_worker_index(wk) == 0)
				dtfl_input_session_remove(w.query, q, 4);
			pc.probe = w.probe;
			pc.time = &time;
			dtfl_worker_step_while(wk, probe_behind, &pc);
			printf("query round %llu complete\n",
			    (unsigned long long)round);
		}
	}

	/* Finite run: release the inputs and the source, then drain. */
	dtfl_input_session_close(w.roots);
	dtfl_input_session_close(w.query);
	w.stop = true;
	while (dtfl_worker_step(wk))
		;

	dtfl_probe_free(w.probe);
	dtfl_type_free(w.pair);
	dtfl_diff_free(w.i64);
	dtfl_time_class_free(tc);
	dtfl_time_class_free(inner);
	dtfl_time_class_free(unit);
}

int
main(int argc, char **argv)
{
	params p = { 5, 6, 2, 0, true, 4 };
	int skip = 0;
	dtfl_config *cfg;
	int rc;

	if (argc > 5) {
		p.nodes = (uint32_t)ex_u64(argc, argv, 1, 5);
		p.edges = ex_u64(argc, argv, 2, 6);
		p.batch = ex_u64(argc, argv, 3, 2);
		p.pre = ex_u64(argc, argv, 4, 0);
		p.inspect = strcmp(argv[5], "inspect") == 0;
		skip = 5;
		if (argc > 6 && argv[6][0] != '-') {
			p.rounds = ex_u64(argc, argv, 6, 4);
			skip = 6;
		}
	}
	if (p.nodes == 0)
		p.nodes = 1;
	cfg = ex_config(argc, argv, skip);
	rc = dtfl_execute(cfg, worker, &p, NULL);
	dtfl_config_free(cfg);
	return rc;
}
