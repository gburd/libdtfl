/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/upstream/dd/int_proxy.c: cross-checks for the integer-proxy
 * tactics (differential-dataflow-v0.25.1
 * differential-dataflow/src/operators/int_proxy/).  Upstream ships the
 * tactics with no tests and no consumers (the API is experimental, CHANGELOG
 * 0.25), so these are original C tests, in the style of reduce_reference.rs:
 * run the default reduce and the ProxyReduceTactic (through the u64 backend)
 * on the SAME random Pair<u64,u64> product-time input with +/-1 diffs, and
 * assert_eq their outputs at every time, for count, sum and min, and the
 * same for an iterative BFS (reduce_reference.rs's bfs shape).
 *
 * The input, the logics and assert_eq follow tests/upstream/dd/
 * reduce_reference.c: an unordered input under the (0, 0) capability viewed
 * as a collection, splitmix64 seeded per run, and assert_eq as negate +
 * concat + consolidate + inspect counting violations.  Each run also asserts
 * the default output is non-empty, so two empty outputs cannot pass.
 */
#include "dtfl_test.h"

#include "dd/arrange_int.h"
#include "dd/int_proxy_int.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* splitmix64 (reduce_reference.c's generator). */
typedef struct rng {
	uint64_t s;
} rng;

static uint64_t
rng_next(rng *r)
{
	uint64_t z = (r->s += UINT64_C(0x9e3779b97f4a7c15));

	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

static uint64_t
rng_range(rng *r, uint64_t n)
{
	return rng_next(r) % n;
}

static rng
rng_from_seed(const uint64_t seed[4])
{
	rng r = { 0x1234567ULL };
	unsigned i;

	for (i = 0; i < 4; i++) {
		r.s ^= seed[i] + UINT64_C(0x9e3779b97f4a7c15) * (i + 1);
		(void)rng_next(&r);
	}
	return r;
}

/* ------------------------------------------------------------------ */
/* The logics: count / sum / min over (u64 key, u64 val) -> i64.        */
/* ------------------------------------------------------------------ */

typedef enum logic_kind { LOGIC_COUNT, LOGIC_SUM, LOGIC_MIN } logic_kind;

static bool
logic_eval(logic_kind kind, size_t n, const void *const *vals,
    const void *diffs, int64_t *result)
{
	const unsigned char *d = diffs;
	size_t i;

	switch (kind) {
	case LOGIC_COUNT:
		*result = (int64_t)n;
		return true;
	case LOGIC_SUM:
		*result = 0;
		for (i = 0; i < n; i++) {
			int64_t di;

			memcpy(&di, d + 8 * i, 8);
			*result += (int64_t)dtfl_get_u64(vals[i]) * di;
		}
		return true;
	case LOGIC_MIN:
		if (n == 0)
			return false;
		*result = (int64_t)dtfl_get_u64(vals[0]);
		return true;
	}
	return false;
}

/* The default reduce's logic (dtfl_reduce_fn). */
static void
cursor_logic(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t result, one = 1;
	unsigned char vbuf[8];

	(void)key; (void)klen; (void)vlens;
	if (logic_eval((logic_kind)(intptr_t)ctx, n, vals, diffs, &result)) {
		dtfl_put_i64(vbuf, result);
		dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &one);
	}
}

/*
 * The same logic as a reduce_core logic for the proxy backend, with the
 * abelian wrapper: emit the new output, then retract the tentative output.
 * Output values are i64 encodings; the backend's ids are their u64
 * readings, which order the same way.
 */
static void
proxy_logic(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, __dtfl_reduce_change *ch)
{
	int64_t result, one = 1;
	unsigned char vbuf[8];
	size_t i;

	(void)key; (void)klen; (void)in_vlens;
	if (n != 0 && logic_eval((logic_kind)(intptr_t)ctx, n, in_vals,
	    in_diffs, &result)) {
		dtfl_put_i64(vbuf, result);
		__dtfl_reduce_change_push(ch, vbuf, sizeof vbuf, &one);
	}
	for (i = 0; i < m; i++) {
		int64_t d;

		memcpy(&d, (const unsigned char *)out_diffs + 8 * i, 8);
		d = -d;
		__dtfl_reduce_change_push(ch, out_vals[i], out_vlens[i], &d);
	}
}

/* as_collection(|k, v| (k, v)): key || val. */
static void
kv_concat(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	unsigned char rec[16];

	(void)ctx;
	munit_assert_size(klen + vlen, ==, sizeof rec);
	memcpy(rec, key, klen);
	memcpy(rec + klen, val, vlen);
	dtfl_arranged_emit_record(out, rec, sizeof rec);
}

static dtfl_collection *
reduce_cursor_kind(dtfl_collection *c, logic_kind kind)
{
	dtfl_diff *odiff = dtfl_diff_i64();
	dtfl_collection *r = dtfl_collection_reduce(c, dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_i64(), odiff, cursor_logic,
	    DTFL_CTX((void *)(intptr_t)kind, NULL));

	dtfl_diff_free(odiff);
	return r;
}

static dtfl_collection *
reduce_proxy_kind(dtfl_collection *c, logic_kind kind)
{
	dtfl_diff *odiff = dtfl_diff_i64();
	const dtfl_type *fields[2] = { dtfl_type_u64(), dtfl_type_i64() };
	const dtfl_type *out_t = dtfl_type_tuple(fields, 2);
	dtfl_arranged *arr = dtfl_collection_arrange_by_key(c, dtfl_type_u64(),
	    dtfl_type_u64(), "Arrange: ReduceProxy");
	__dtfl_proxy_reduce_backend be = __dtfl_proxy_reduce_u64_backend(arr,
	    dtfl_type_i64(), odiff->cls, proxy_logic, (void *)(intptr_t)kind,
	    NULL);
	dtfl_arranged *red = __dtfl_arranged_reduce_with_proxy(arr,
	    "ReduceProxy", dtfl_type_i64(), odiff->cls, be);
	dtfl_collection *out = dtfl_arranged_as_collection(red, out_t,
	    kv_concat, DTFL_NOCTX);

	dtfl_arranged_free(red);
	dtfl_arranged_free(arr);
	dtfl_type_free(out_t);
	dtfl_diff_free(odiff);
	return out;
}

/* ------------------------------------------------------------------ */
/* assert_eq: self.negate().concat(other).assert_empty().              */
/* ------------------------------------------------------------------ */

typedef struct eq_state {
	size_t violations;
	size_t seen;
	unsigned tw;
} eq_state;

static void
eq_violation(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	eq_state *s = ctx;
	int64_t d;

	memcpy(&d, diff, sizeof d);
	if (s->violations++ < 4) {
		unsigned i;

		fprintf(stderr, "assert_eq: non-empty: key %llu val %lld "
		    "diff %lld time", (unsigned long long)dtfl_get_u64(rec),
		    len >= 16 ? (long long)dtfl_get_i64(
		    (const unsigned char *)rec + 8) : 0LL, (long long)d);
		for (i = 0; i < s->tw; i++)
			fprintf(stderr, " %llu", (unsigned long long)time[i]);
		fprintf(stderr, "\n");
	}
}

static void
eq_seen(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	eq_state *s = ctx;

	(void)rec; (void)len; (void)time; (void)diff;
	s->seen++;
}

static void
assert_eq(dtfl_collection *a, dtfl_collection *b, eq_state *s)
{
	dtfl_collection *seen = dtfl_collection_inspect(a, eq_seen,
	    DTFL_CTX(s, NULL));
	dtfl_collection *neg = dtfl_collection_negate(seen);
	dtfl_collection *cat = dtfl_collection_concat(neg, b);
	dtfl_collection *cons = dtfl_collection_consolidate(cat);
	dtfl_collection *ins = dtfl_collection_inspect(cons, eq_violation,
	    DTFL_CTX(s, NULL));

	s->tw = dtfl_time_depth(dtfl_scope_time_class(
	    dtfl_collection_scope(a)));
	dtfl_collection_free(ins);
	dtfl_collection_free(cons);
	dtfl_collection_free(cat);
	dtfl_collection_free(neg);
	dtfl_collection_free(seen);
}

/* ------------------------------------------------------------------ */
/* Random Pair-timed input, fed under the initial (0, 0) capability.    */
/* ------------------------------------------------------------------ */

#define SPAN 6
#define DIFF_N 400

typedef struct in_upd {
	uint64_t key, val, t1, t2;
	int64_t diff;
} in_upd;

static void
random_input(unsigned seed, size_t n, uint64_t keys, uint64_t vals,
    in_upd *out)
{
	const uint64_t s[4] = { seed, 11, 17, 31 };
	rng r = rng_from_seed(s);
	size_t i;

	for (i = 0; i < n; i++) {
		out[i].key = rng_range(&r, keys);
		out[i].val = rng_range(&r, vals);
		out[i].t1 = rng_range(&r, SPAN);
		out[i].t2 = rng_range(&r, SPAN);
		out[i].diff = (rng_next(&r) & 1) ? 1 : -1;
	}
}

typedef struct diff_build {
	dtfl_unordered_input *in;
	dtfl_cap *cap;
	logic_kind kind;
	eq_state eq;
} diff_build;

static void
build_differential(dtfl_scope *scope, void *arg)
{
	diff_build *b = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *fields[5] = { u64, u64, u64, u64, dtfl_type_i64() };
	const dtfl_type *upd = dtfl_type_tuple(fields, 5);
	const dtfl_type *rec = dtfl_type_pair(u64, u64);
	dtfl_stream *st = dtfl_unordered_input_new(scope, upd, &b->in, &b->cap);
	dtfl_collection *coll = dtfl_stream_as_collection(st, rec,
	    dtfl_diff_i64());
	dtfl_collection *via_cursor = reduce_cursor_kind(coll, b->kind);
	dtfl_collection *via_proxy = reduce_proxy_kind(coll, b->kind);

	assert_eq(via_cursor, via_proxy, &b->eq);
	dtfl_collection_free(via_proxy);
	dtfl_collection_free(via_cursor);
	dtfl_collection_free(coll);
	dtfl_type_free(rec);
	dtfl_type_free(upd);
}

static dtfl_time_class *
pair_time(void)
{
	dtfl_time_class *u = dtfl_time_u64();
	dtfl_time_class *p = dtfl_time_product(u, u);

	dtfl_time_class_free(u);
	return p;
}

/*
 * Feed in waves along the diagonal: wave w sends the updates with
 * min(t1, t2) == w under a capability at (w, w), steps the worker, and moves
 * on.  Later retires then see accumulated input and output histories, and
 * because two in-band times such as (w, 5) and (5, w) join to (5, 5), beyond
 * the upper (w + 1, w + 1), they also carry pending interesting times
 * between retires, which a single retire of the whole input never does.
 */
static void
feed_waves(diff_build *b, dtfl_worker *w, const in_upd *u, size_t n)
{
	uint64_t wave, t[2];
	size_t i, k;

	for (wave = 0; wave < SPAN; wave++) {
		t[0] = t[1] = wave;
		dtfl_cap_downgrade(b->cap, t);
		for (i = 0; i < n; i++) {
			unsigned char buf[40];

			if (DTFL_MIN(u[i].t1, u[i].t2) != wave)
				continue;
			dtfl_put_u64(buf, u[i].key);
			dtfl_put_u64(buf + 8, u[i].val);
			memcpy(buf + 16, &u[i].t1, 8);
			memcpy(buf + 24, &u[i].t2, 8);
			memcpy(buf + 32, &u[i].diff, 8);
			dtfl_unordered_input_send(b->in, b->cap, buf,
			    sizeof buf);
		}
		dtfl_unordered_input_flush(b->in);
		for (k = 0; k < 64; k++)
			dtfl_worker_step(w);
	}
}

static void
run_differential(unsigned seed, logic_kind kind, uint64_t keys, bool waves)
{
	static in_upd u[DIFF_N];
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = pair_time();
	diff_build b;
	size_t i;

	memset(&b, 0, sizeof b);
	b.kind = kind;
	random_input(seed, DIFF_N, keys, 8, u);
	munit_assert_int(dtfl_worker_dataflow(w, "int_proxy", tc,
	    build_differential, &b), ==, 0);
	if (waves)
		feed_waves(&b, w, u, DIFF_N);
	for (i = 0; !waves && i < DIFF_N; i++) {
		unsigned char buf[40];

		dtfl_put_u64(buf, u[i].key);
		dtfl_put_u64(buf + 8, u[i].val);
		memcpy(buf + 16, &u[i].t1, 8);
		memcpy(buf + 24, &u[i].t2, 8);
		memcpy(buf + 32, &u[i].diff, 8);
		dtfl_unordered_input_send(b.in, b.cap, buf, sizeof buf);
	}
	dtfl_unordered_input_flush(b.in);
	dtfl_cap_drop(b.cap);
	dtfl_unordered_input_free(b.in);
	while (dtfl_worker_step(w))
		;
	if (b.eq.violations != 0)
		fprintf(stderr, "int_proxy: seed %u kind %d keys %llu waves %d: "
		    "%zu violations\n", seed, (int)kind,
		    (unsigned long long)keys, (int)waves, b.eq.violations);
	munit_assert_size(b.eq.violations, ==, 0);
	munit_assert_size(b.eq.seen, >, 0);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
}

/*
 * 12 keys fit one 16-key window; 40 keys span three windows.  Each seed
 * runs once with every update in one retire and once fed in waves.
 */
static void
differential(unsigned seed, logic_kind kind, uint64_t keys)
{
	run_differential(seed, kind, keys, false);
	run_differential(seed, kind, keys, true);
}

DTFL_TEST(int_proxy__reduce_count)
{
	unsigned s;

	(void)params; (void)data;
	for (s = 0; s < 25; s++) {
		differential(s, LOGIC_COUNT, 12);
		differential(s, LOGIC_COUNT, 40);
	}
	return MUNIT_OK;
}

DTFL_TEST(int_proxy__reduce_sum)
{
	unsigned s;

	(void)params; (void)data;
	for (s = 0; s < 25; s++) {
		differential(s, LOGIC_SUM, 12);
		differential(s, LOGIC_SUM, 40);
	}
	return MUNIT_OK;
}

DTFL_TEST(int_proxy__reduce_min)
{
	unsigned s;

	(void)params; (void)data;
	for (s = 0; s < 25; s++) {
		differential(s, LOGIC_MIN, 12);
		differential(s, LOGIC_MIN, 40);
	}
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Iterative BFS, default vs proxy (reduce_reference.rs bfs shape).    */
/* ------------------------------------------------------------------ */

/*
 * The Pair-time runs above retire everything at once (one capability, then
 * close), so they never exercise pending times or an output history.  BFS
 * over a graph that grows and shrinks round by round does: its reduce runs
 * at Product<u64, u64> times across many retires.
 */

#define MAX_EDGES 512

typedef struct timed_edge {
	uint64_t src, dst, round;
	int64_t diff;
} timed_edge;

static size_t
edge_list(size_t nodes, size_t edges, size_t rounds, const uint64_t seed[4],
    timed_edge *out)
{
	rng r1 = rng_from_seed(seed), r2 = rng_from_seed(seed);
	size_t n = 0, i, round;

	for (i = 0; i < edges; i++) {
		out[n].src = rng_range(&r1, nodes);
		out[n].dst = rng_range(&r1, nodes);
		out[n].round = 0;
		out[n++].diff = 1;
	}
	for (round = 1; round < rounds; round++) {
		out[n].src = rng_range(&r1, nodes);
		out[n].dst = rng_range(&r1, nodes);
		out[n].round = round;
		out[n++].diff = 1;
		out[n].src = rng_range(&r2, nodes);
		out[n].dst = rng_range(&r2, nodes);
		out[n].round = round;
		out[n++].diff = -1;
	}
	munit_assert_size(n, <=, MAX_EDGES);
	return n;
}

/* |_, s, t| t.push((*s[0].0, 1)) as a reduce_core logic. */
static void
proxy_keep_min(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, __dtfl_reduce_change *ch)
{
	int64_t one = 1;
	size_t i;

	(void)ctx; (void)key; (void)klen; (void)in_vlens; (void)in_diffs;
	if (n != 0)
		__dtfl_reduce_change_push(ch, in_vals[0], 8, &one);
	for (i = 0; i < m; i++) {
		int64_t d;

		memcpy(&d, (const unsigned char *)out_diffs + 8 * i, 8);
		d = -d;
		__dtfl_reduce_change_push(ch, out_vals[i], out_vlens[i], &d);
	}
}

static void
cursor_keep_min(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_reduce_emit_pair(out, vals[0], 8, &one);
}

static dtfl_collection *
reduce_keep_min(dtfl_collection *c, bool proxy)
{
	dtfl_diff *odiff = dtfl_diff_i64();
	dtfl_collection *r;

	if (proxy) {
		const dtfl_type *fields[2] = { dtfl_type_u64(),
		    dtfl_type_u64() };
		const dtfl_type *out_t = dtfl_type_tuple(fields, 2);
		dtfl_arranged *arr = dtfl_collection_arrange_by_key(c,
		    dtfl_type_u64(), dtfl_type_u64(), "Arrange: ReduceProxy");
		__dtfl_proxy_reduce_backend be =
		    __dtfl_proxy_reduce_u64_backend(arr, dtfl_type_u64(),
		    odiff->cls, proxy_keep_min, NULL, NULL);
		dtfl_arranged *red = __dtfl_arranged_reduce_with_proxy(arr,
		    "ReduceProxy", dtfl_type_u64(), odiff->cls, be);

		r = dtfl_arranged_as_collection(red, out_t, kv_concat,
		    DTFL_NOCTX);
		dtfl_arranged_free(red);
		dtfl_arranged_free(arr);
		dtfl_type_free(out_t);
	} else
		r = dtfl_collection_reduce(c, dtfl_type_u64(), dtfl_type_u64(),
		    dtfl_type_u64(), odiff, cursor_keep_min, DTFL_NOCTX);
	dtfl_diff_free(odiff);
	return r;
}

static void
bfs_seed(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, rec, 8);
	dtfl_put_u64(buf + 8, 0);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

static void
bfs_step(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 8);
	dtfl_put_u64(buf + 8, dtfl_get_u64(v1) + 1);
	dtfl_join_emit_record(out, buf, sizeof buf);
}

typedef struct bfs_iter {
	dtfl_collection *edges;
	dtfl_collection *nodes;
	bool proxy;
} bfs_iter;

static dtfl_collection *
bfs_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	bfs_iter *c = vctx;
	const dtfl_type *pr = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_collection *edges = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *nodes = dtfl_collection_enter(c->nodes, inner);
	dtfl_collection *stepped = dtfl_collection_join_map(var, edges,
	    dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64(), pr, bfs_step,
	    DTFL_NOCTX);
	dtfl_collection *combined = dtfl_collection_concat(stepped, nodes);
	dtfl_collection *r = reduce_keep_min(combined, c->proxy);

	dtfl_collection_free(combined);
	dtfl_collection_free(stepped);
	dtfl_collection_free(nodes);
	dtfl_collection_free(edges);
	dtfl_type_free(pr);
	return r;
}

static dtfl_collection *
bfs(dtfl_collection *edges, dtfl_collection *roots, bool proxy)
{
	const dtfl_type *pr = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_collection *nodes = dtfl_collection_map(roots, pr, bfs_seed,
	    DTFL_NOCTX);
	bfs_iter it = { edges, nodes, proxy };
	dtfl_collection *r = dtfl_collection_iterate(nodes, bfs_logic,
	    DTFL_CTX(&it, NULL));

	dtfl_collection_free(nodes);
	dtfl_type_free(pr);
	return r;
}

typedef struct graph_build {
	dtfl_input_session *roots;
	dtfl_input_session *edges;
	eq_state eq;
} graph_build;

static void
build_bfs(dtfl_scope *scope, void *arg)
{
	graph_build *b = arg;
	dtfl_collection *roots = dtfl_input_session_to_collection(b->roots,
	    scope);
	dtfl_collection *edges = dtfl_input_session_to_collection(b->edges,
	    scope);
	dtfl_collection *via_cursor = bfs(edges, roots, false);
	dtfl_collection *via_proxy = bfs(edges, roots, true);

	assert_eq(via_cursor, via_proxy, &b->eq);
	dtfl_collection_free(via_proxy);
	dtfl_collection_free(via_cursor);
	dtfl_collection_free(edges);
	dtfl_collection_free(roots);
}

static void
bfs_differential(size_t nodes, size_t nedges, size_t rounds,
    uint64_t s0, uint64_t s1, uint64_t s2, uint64_t s3)
{
	const uint64_t seed[4] = { s0, s1, s2, s3 };
	static timed_edge e[MAX_EDGES];
	size_t ne = edge_list(nodes, nedges, rounds, seed, e), i;
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pr = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	graph_build b;
	uint64_t round;

	memset(&b, 0, sizeof b);
	b.roots = dtfl_input_session_new(dtfl_type_u64(), r);
	b.edges = dtfl_input_session_new(pr, r);
	munit_assert_int(dtfl_worker_dataflow(w, "bfs", tc, build_bfs, &b),
	    ==, 0);
	for (round = 0; round < rounds; round++) {
		uint64_t next = round + 1;

		if (round == 0) {
			unsigned char nb[8];
			int64_t one = 1;

			dtfl_put_u64(nb, 1);
			dtfl_input_session_update(b.roots, nb, 8, &one);
		}
		for (i = 0; i < ne; i++)
			if (e[i].round == round) {
				unsigned char buf[16];

				dtfl_put_u64(buf, e[i].src);
				dtfl_put_u64(buf + 8, e[i].dst);
				dtfl_input_session_update(b.edges, buf, 16,
				    &e[i].diff);
			}
		dtfl_input_session_advance_to(b.roots, &next);
		dtfl_input_session_advance_to(b.edges, &next);
		dtfl_input_session_flush(b.roots);
		dtfl_input_session_flush(b.edges);
	}
	dtfl_input_session_close(b.roots);
	dtfl_input_session_close(b.edges);
	while (dtfl_worker_step(w))
		;
	if (b.eq.violations != 0)
		fprintf(stderr, "int_proxy bfs: %zu violations\n",
		    b.eq.violations);
	munit_assert_size(b.eq.violations, ==, 0);
	munit_assert_size(b.eq.seen, >, 0);
	dtfl_type_free(pr);
	dtfl_diff_free(r);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
}

DTFL_TEST(int_proxy__reduce_bfs)
{
	uint64_t s;

	(void)params; (void)data;
	bfs_differential(6, 10, 4, 1, 2, 3, 4);
	bfs_differential(30, 60, 20, 1, 2, 3, 4);
	for (s = 0; s < 8; s++)
		bfs_differential(40, 120, 30, s, s + 7, 13, 29);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(int_proxy__reduce_count),
	DTFL_T(int_proxy__reduce_sum),
	DTFL_T(int_proxy__reduce_min),
	DTFL_T(int_proxy__reduce_bfs),
	DTFL_T_END
};

DTFL_TEST_MAIN("int_proxy", tests)
