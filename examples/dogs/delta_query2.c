/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dogs/delta_query2.c: mirrors
 * dogsdogsdogs/examples/delta_query2.rs.
 *
 * A delta join over partially ordered times built from explicit half_join
 * stages (not the extend/propose WCOJ path).  The scope time is
 * Product<usize, usize> (outer, inner), two incomparable coordinates, so a
 * pair of updates can be concurrent.  Two relations E1 and E2 are fed as
 * unordered inputs of (edge, time, diff).  For each relation the stream of
 * changes stashes its own time into the record (the ((k,v),t) -> ((k,v,t),t)
 * hoist upstream does with edges.inner.map(..).as_collection()), and a
 * half_join against the other relation's arrange_by_key index pairs edges
 * that share a source, carrying the joined time:
 *
 *   path1 = half_join(changes1, forward2, closure, time2 <  initial,
 *                     |k,v1,v2| (k,(v1,v2)))
 *   path2 = half_join(changes2, forward1, closure, time2 <= initial,
 *                     |k,v1,v2| (k,(v2,v1)))
 *
 * The strict (path1) and non-strict (path2) comparisons split a pair of
 * concurrent updates so the join fires exactly once; `closure` is the
 * frontier the arrangement must retain, time -> (outer-1, inner-1) with
 * saturating subtraction (dtfl_half_join_frontier_insert).  path1 and path2
 * are concatenated and the result is the set of (a, b, c) with E1(a,b),
 * E2(a,c) at comparable times.
 *
 * Divergence from the Rust.  (1) The nested Product scope is expressed
 * directly as the dataflow's time class (dtfl_time_product), since every
 * operator lives at the same level and nothing enters or leaves a child
 * scope.  (2) The time hoist into the stream record is done at the input:
 * each changes record is fed as (k, v, t) carrying its own time t, which is
 * exactly ((k,v,t), t, r).  (3) C's half_join already delays each output to
 * the joined lattice time (the record is (a,b,c) carried at that time), so
 * upstream's trailing .inner.map(|(((k,v),t),_,r)| ((k,v),t,r)) lower is a
 * no-op here and is dropped.  (4) Upstream feeds a two-edge demo with
 * incomparable times (0,13) and (11,0) whose join is empty; this port feeds
 * a small fixed graph at comparable times so the join is a non-empty,
 * hand-verifiable set, printed as upstream's inspect does.
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dogs.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <string.h>

/*
 * The fixed graph.  Each edge (src, dst) is inserted at a Product time
 * (outer, inner).  All times here have inner == 0, so the product order is
 * the total order on `outer`: every pair of edges sharing a source joins.
 *   E1: source 0 -> {1, 2},  source 3 -> {4}
 *   E2: source 0 -> {10, 11}, source 3 -> {5}
 * The join Q(a,b,c) := E1(a,b), E2(a,c) pairs edges sharing source a:
 *   (0,1,10) (0,1,11) (0,2,10) (0,2,11) (3,4,5)
 */
typedef struct edge {
	uint32_t src, dst;
	uint64_t outer, inner;
} edge;

static const edge E1[] = {
	{ 0, 1, 1, 0 },
	{ 0, 2, 2, 0 },
	{ 3, 4, 1, 0 },
};
#define N_E1 (sizeof E1 / sizeof E1[0])

static const edge E2[] = {
	{ 0, 10, 2, 0 },
	{ 0, 11, 1, 0 },
	{ 3, 5, 3, 0 },
};
#define N_E2 (sizeof E2 / sizeof E2[0])

/* ------------------------------------------------------------------ */
/* half_join closures.                                                 */
/* ------------------------------------------------------------------ */

/*
 * The shared frontier closure: time -> (outer-1, inner-1) with saturating
 * subtraction, inserted into the antichain the arrangement must retain.
 * This is upstream's `closure` passed to both half_joins.
 */
static void
frontier_minus_one(void *ctx, const uint64_t *initial,
    dtfl_half_join_frontier *out)
{
	uint64_t t[2];

	(void)ctx;
	t[0] = initial[0] != 0 ? initial[0] - 1 : 0;	/* outer */
	t[1] = initial[1] != 0 ? initial[1] - 1 : 0;	/* inner */
	dtfl_half_join_frontier_insert(out, t);
}

/* path2's comparison: trace time2 <= initial (sees concurrent updates). */
static bool
cmp_less_equal(void *ctx, const uint64_t *time2, const uint64_t *initial)
{
	const dtfl_time_class *tc = ctx;

	return dtfl_time_less_equal(tc, time2, initial);
}

/* path1 output: (key, (val1, val2)) -> (a, b, c). */
static void
out_abc_1(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, void *dst, size_t cap,
    size_t *out_len)
{
	unsigned char *p = dst;

	(void)ctx; (void)klen; (void)v1len; (void)v2len; (void)cap;
	memcpy(p, key, 4);		/* a */
	memcpy(p + 4, val1, 4);		/* b (E1 dst) */
	memcpy(p + 8, val2, 4);		/* c (E2 dst) */
	*out_len = 12;
}

/* path2 output: (key, (val2, val1)) -> (a, b, c), where the stream is E2. */
static void
out_abc_2(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, void *dst, size_t cap,
    size_t *out_len)
{
	unsigned char *p = dst;

	(void)ctx; (void)klen; (void)v1len; (void)v2len; (void)cap;
	memcpy(p, key, 4);		/* a */
	memcpy(p + 4, val2, 4);		/* b (E1 dst, the matched arr val) */
	memcpy(p + 8, val1, 4);		/* c (E2 dst, the stream val) */
	*out_len = 12;
}

/* Collect each produced (a, b, c) with a positive accumulated diff. */
static void
inspect_abc(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	const unsigned char *p = rec;
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);	/* diff bytes are native, not encoded */
	if (d <= 0)
		return;
	printf("(%llu, %llu, %llu)\n",
	    (unsigned long long)dtfl_get_u32(p),
	    (unsigned long long)dtfl_get_u32(p + 4),
	    (unsigned long long)dtfl_get_u32(p + 8));
}

/* ------------------------------------------------------------------ */
/* The dataflow.                                                       */
/* ------------------------------------------------------------------ */

typedef struct build_ctx {
	dtfl_unordered_input *in1;	/* E1 edges (k, v) */
	dtfl_cap *cap1;
	dtfl_unordered_input *in2;	/* E2 edges (k, v) */
	dtfl_cap *cap2;
	dtfl_unordered_input *ch1;	/* E1 changes (k, v, t) */
	dtfl_cap *chcap1;
	dtfl_unordered_input *ch2;	/* E2 changes (k, v, t) */
	dtfl_cap *chcap2;
	dtfl_diff *diff;
	dtfl_probe *probe;
	const dtfl_time_class *tc;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *u64 = dtfl_type_u64();
	/* The plain edge record (k, v). */
	const dtfl_type *pair = dtfl_type_pair(u32, u32);
	/* The hoisted change record (k, v, t1, t2): edge plus its two time
	 * words (the ((k,v),t) -> ((k,v,t)) hoist, D1 leading key). */
	const dtfl_type *chf[4] = { u32, u32, u64, u64 };
	const dtfl_type *chr = dtfl_type_tuple(chf, 4);
	/* The join output (a, b, c). */
	const dtfl_type *outf[3] = { u32, u32, u32 };
	const dtfl_type *out = dtfl_type_tuple(outf, 3);

	dtfl_stream *s1 = dtfl_unordered_input_new(scope, pair, &c->in1,
	    &c->cap1);
	dtfl_stream *s2 = dtfl_unordered_input_new(scope, pair, &c->in2,
	    &c->cap2);
	dtfl_stream *sc1 = dtfl_unordered_input_new(scope, chr, &c->ch1,
	    &c->chcap1);
	dtfl_stream *sc2 = dtfl_unordered_input_new(scope, chr, &c->ch2,
	    &c->chcap2);

	dtfl_collection *edges1 = dtfl_stream_as_collection(s1, pair,
	    dtfl_diff_retain(c->diff));
	dtfl_collection *edges2 = dtfl_stream_as_collection(s2, pair,
	    dtfl_diff_retain(c->diff));
	dtfl_collection *changes1 = dtfl_stream_as_collection(sc1, chr,
	    dtfl_diff_retain(c->diff));
	dtfl_collection *changes2 = dtfl_stream_as_collection(sc2, chr,
	    dtfl_diff_retain(c->diff));

	dtfl_arranged *forward1 = dtfl_collection_arrange_by_key(edges1, u32,
	    u32, "arrange_by_key");
	dtfl_arranged *forward2 = dtfl_collection_arrange_by_key(edges2, u32,
	    u32, "arrange_by_key");

	/* path1: changes1 (E1) half-joined with forward2 (E2), strict. */
	dtfl_collection *path1 = dtfl_dogs_half_join2(changes1, forward2, u32,
	    u32, out, NULL, frontier_minus_one, out_abc_1, DTFL_NOCTX);
	/* path2: changes2 (E2) half-joined with forward1 (E1), non-strict. */
	dtfl_collection *path2 = dtfl_dogs_half_join2(changes2, forward1, u32,
	    u32, out, cmp_less_equal, frontier_minus_one, out_abc_2,
	    DTFL_CTX((void *)c->tc, NULL));

	dtfl_collection *cat = dtfl_collection_concat(path1, path2);
	dtfl_collection *ins = dtfl_collection_inspect(cat, inspect_abc,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(cat);
	dtfl_collection_free(path1);
	dtfl_collection_free(path2);
	dtfl_arranged_free(forward1);
	dtfl_arranged_free(forward2);
	dtfl_collection_free(edges1);
	dtfl_collection_free(edges2);
	dtfl_collection_free(changes1);
	dtfl_collection_free(changes2);
	dtfl_type_free(pair);
	dtfl_type_free(chr);
	dtfl_type_free(out);
}

/* Send one plain edge (k, v) carrying its product time in the time words. */
static void
send_edge(dtfl_unordered_input *in, const dtfl_cap *cap, const edge *e)
{
	unsigned char buf[32];
	int64_t one = 1;

	dtfl_put_u32(buf, e->src);
	dtfl_put_u32(buf + 4, e->dst);
	memcpy(buf + 8, &e->outer, sizeof e->outer);	/* time word 0 */
	memcpy(buf + 16, &e->inner, sizeof e->inner);	/* time word 1 */
	memcpy(buf + 24, &one, sizeof one);		/* diff */
	dtfl_unordered_input_send(in, cap, buf, sizeof buf);
}

/*
 * Send one change (k, v, t): the record hoists the edge's time into its
 * data (bytes 8..24) and carries the same time in the update's time words
 * (bytes 24..40), which is ((k,v,t), t, r).
 */
static void
send_change(dtfl_unordered_input *in, const dtfl_cap *cap, const edge *e)
{
	unsigned char buf[48];
	int64_t one = 1;

	dtfl_put_u32(buf, e->src);
	dtfl_put_u32(buf + 4, e->dst);
	memcpy(buf + 8, &e->outer, sizeof e->outer);	/* hoisted t word 0 */
	memcpy(buf + 16, &e->inner, sizeof e->inner);	/* hoisted t word 1 */
	memcpy(buf + 24, &e->outer, sizeof e->outer);	/* time word 0 */
	memcpy(buf + 32, &e->inner, sizeof e->inner);	/* time word 1 */
	memcpy(buf + 40, &one, sizeof one);		/* diff */
	dtfl_unordered_input_send(in, cap, buf, sizeof buf);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *inner = dtfl_time_u64();
	/* Product<usize, usize>: the nested scope's time, a partial order. */
	dtfl_time_class *tc = dtfl_time_product(inner, inner);
	dtfl_diff *i64 = dtfl_diff_i64();
	build_ctx c;
	size_t i;
	/* A time past every edge time, to seal the inputs; (0,0) <= seal. */
	uint64_t seal[2] = { 1000000, 1000000 };

	(void)ctx;
	memset(&c, 0, sizeof c);
	c.diff = i64;
	c.probe = dtfl_probe_new();
	c.tc = tc;
	dtfl_worker_dataflow(w, "delta_query2", tc, build, &c);

	/* All records carry their own time, so a single (0,0) capability
	 * sends every update (multitemporal's unordered-input idiom). */
	for (i = 0; i < N_E1; i++) {
		send_edge(c.in1, c.cap1, &E1[i]);
		send_change(c.ch1, c.chcap1, &E1[i]);
	}
	for (i = 0; i < N_E2; i++) {
		send_edge(c.in2, c.cap2, &E2[i]);
		send_change(c.ch2, c.chcap2, &E2[i]);
	}
	dtfl_unordered_input_flush(c.in1);
	dtfl_unordered_input_flush(c.in2);
	dtfl_unordered_input_flush(c.ch1);
	dtfl_unordered_input_flush(c.ch2);

	/* Downgrade each capability past every edge time, then drop it, to
	 * close the inputs so the join settles. */
	{
		dtfl_cap *caps[4];
		dtfl_cap **held[4] = { &c.cap1, &c.cap2, &c.chcap1, &c.chcap2 };

		for (i = 0; i < 4; i++) {
			caps[i] = dtfl_cap_delayed(*held[i], seal);
			dtfl_cap_drop(*held[i]);
			*held[i] = caps[i];
		}
		for (i = 0; i < 4; i++)
			dtfl_cap_drop(*held[i]);
	}
	dtfl_unordered_input_free(c.in1);
	dtfl_unordered_input_free(c.in2);
	dtfl_unordered_input_free(c.ch1);
	dtfl_unordered_input_free(c.ch2);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
	dtfl_time_class_free(inner);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc;

	/* A single worker so the printed join set is the whole result. */
	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
