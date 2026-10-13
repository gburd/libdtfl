/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/graphs/bijkstra.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/algo/bijkstra.c: bidirectional Dijkstra distance labeling.
 *
 * For each goal (source, target) the search evolves distances outward
 * from both the source and the target, forward and reverse, until the
 * two frontiers meet.  Each round: find the goals whose source and
 * target have reached a common node (reached), subtract those from the
 * active set, expand the forward and reverse frontiers of the active
 * goals one step, and keep the minimum distance per (node, root).  The
 * result is the shortest distance for each reachable goal.
 *
 * Records: a frontier entry is (u64 med, (u64 root, u32 dist)) carried as
 * key med with a (root, dist) value; a goal is (u64 source, u64 target);
 * the result is ((u64 source, u64 target), u32 dist).  Distances keep the
 * u32 width upstream uses.  Upstream's debug inspects are dropped.
 */
#include "dtfl/algo.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"

#include <string.h>

/* (u64 root, u32 dist) value type. */
static const dtfl_type *
rd_type(void)
{
	return dtfl_type_pair(dtfl_type_u64(), dtfl_type_u32());
}

/* (u64 med, (u64 root, u32 dist)) frontier record type. */
static const dtfl_type *
frontier_type(void)
{
	const dtfl_type *rd = rd_type();
	const dtfl_type *f = dtfl_type_pair(dtfl_type_u64(), rd);

	dtfl_type_free(rd);
	return f;
}

/* ((u64 source, u64 target), u32 dist) result record type. */
static const dtfl_type *
result_type(void)
{
	const dtfl_type *st = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	const dtfl_type *r = dtfl_type_pair(st, dtfl_type_u32());

	dtfl_type_free(st);
	return r;
}

/* map (source, target) -> (source, (source, 0)): seed the forward search. */
static void
seed_forward(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[20];

	(void)ctx; (void)len;
	memcpy(buf, p, 8);		/* med = source */
	memcpy(buf + 8, p, 8);		/* root = source */
	dtfl_put_u32(buf + 16, 0);	/* dist 0 */
	dtfl_collection_emit_record(out, buf, 20);
}

/* map (source, target) -> (target, (target, 0)): seed the reverse search. */
static void
seed_reverse(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[20];

	(void)ctx; (void)len;
	memcpy(buf, p + 8, 8);		/* med = target */
	memcpy(buf + 8, p + 8, 8);	/* root = target */
	dtfl_put_u32(buf + 16, 0);	/* dist 0 */
	dtfl_collection_emit_record(out, buf, 20);
}

/*
 * join_map (reached): forward (med -> (src, d1)) with reverse
 * (med -> (dst, d2)): emit ((src, dst), d1 + d2).
 */
static void
meet(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	const unsigned char *a = v1;	/* (src, d1) */
	const unsigned char *b = v2;	/* (dst, d2) */
	unsigned char buf[20];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, a, 8);			/* src */
	memcpy(buf + 8, b, 8);			/* dst */
	dtfl_put_u32(buf + 16, dtfl_get_u32(a + 8) + dtfl_get_u32(b + 8));
	dtfl_join_emit_record(out, buf, 20);
}

/* reduce (min): the smallest distance per (src, dst) is first; keep it. */
static void
keep_min_dist(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_reduce_emit_pair(out, vals[0], 4, &one);
}

/* map ((src, dst), dist) -> (src, dst): project the goal key. */
static void
reached_key(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 16);
}

/* map (src, dst) -> src: the forward-active sources. */
static void
goal_src(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 8);
}

/* map (src, dst) -> dst: the reverse-active targets. */
static void
goal_dst(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 8, 8);
}

/* map (src, dst) -> (dst, src): a transposed edge for the reverse search. */
static void
goal_dst_pair(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, p + 8, 8);		/* dst */
	memcpy(buf + 8, p, 8);		/* src */
	dtfl_collection_emit_record(out, buf, 16);
}

/* map (med, (root, dist)) -> (root, (med, dist)): re-key by root. */
static void
by_root(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[20];

	(void)ctx; (void)len;
	memcpy(buf, p + 8, 8);		/* root -> key */
	memcpy(buf + 8, p, 8);		/* med */
	memcpy(buf + 16, p + 16, 4);	/* dist */
	dtfl_collection_emit_record(out, buf, 20);
}

/* map (root, (med, dist)) -> (med, (root, dist)): re-key by med. */
static void
by_med(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[20];

	(void)ctx; (void)len;
	memcpy(buf, p + 8, 8);		/* med -> key */
	memcpy(buf + 8, p, 8);		/* root */
	memcpy(buf + 16, p + 16, 4);	/* dist */
	dtfl_collection_emit_record(out, buf, 20);
}

/*
 * join_map (step): (med -> (root, dist)) with edges (med -> next): emit
 * (next, (root, dist + 1)).
 */
static void
step_next(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	const unsigned char *rd = v1;	/* (root, dist) */
	unsigned char buf[20];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 8);			/* next -> key */
	memcpy(buf + 8, rd, 8);			/* root */
	dtfl_put_u32(buf + 16, dtfl_get_u32(rd + 8) + 1);
	dtfl_join_emit_record(out, buf, 20);
}

/* map (med, (root, dist)) -> ((next, root), dist): key by (med, root). */
static void
to_pair_key(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[20];

	(void)ctx; (void)len;
	memcpy(buf, p, 8);		/* med */
	memcpy(buf + 8, p + 8, 8);	/* root */
	memcpy(buf + 16, p + 16, 4);	/* dist */
	dtfl_collection_emit_record(out, buf, 20);
}

/* reduce (min) over ((med, root) -> dist): keep the smallest distance. */
static void
pair_min(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_reduce_emit_pair(out, vals[0], 4, &one);
}

/* map ((med, root), dist) -> (med, (root, dist)): back to a frontier. */
static void
from_pair_key(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[20];

	(void)ctx; (void)len;
	memcpy(buf, p, 8);		/* med -> key */
	memcpy(buf + 8, p + 8, 8);	/* root */
	memcpy(buf + 16, p + 16, 4);	/* dist */
	dtfl_collection_emit_record(out, buf, 20);
}

typedef struct bij_ctx {
	dtfl_collection *edges;		/* (src, dst) edges, outer scope */
	dtfl_collection *goals;		/* (source, target) goals */
	const dtfl_type *ft;		/* frontier record type */
	const dtfl_type *rd;		/* (root, dist) value type */
	const dtfl_type *rt;		/* result record type */
	dtfl_collection *result;	/* output, in the outer scope */
} bij_ctx;

/*
 * Expand one frontier (forward or reverse) one step, keeping the minimum
 * distance per (next, root).  `frontier` is (med -> (root, dist)),
 * `active` are the active roots, `edges` are the (med -> next) edges.
 */
static dtfl_collection *
expand(bij_ctx *c, dtfl_collection *frontier, dtfl_collection *active,
    dtfl_collection *edges)
{
	const dtfl_type *rd = c->rd;
	const dtfl_type *ft = c->ft;
	const dtfl_type *pairrec = dtfl_type_pair(
	    dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64()), dtfl_type_u32());
	dtfl_collection *rooted = dtfl_collection_map(frontier, ft, by_root,
	    DTFL_NOCTX);
	dtfl_collection *picked = dtfl_collection_semijoin(rooted, active,
	    dtfl_type_u64(), rd, ft);
	dtfl_collection *remed = dtfl_collection_map(picked, ft, by_med,
	    DTFL_NOCTX);
	dtfl_collection *stepped = dtfl_collection_join_map(remed, edges,
	    dtfl_type_u64(), rd, dtfl_type_u64(), ft, step_next, DTFL_NOCTX);
	dtfl_collection *all = dtfl_collection_concat(stepped, frontier);
	dtfl_collection *keyed = dtfl_collection_map(all, pairrec, to_pair_key,
	    DTFL_NOCTX);
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *mr = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	dtfl_collection *reduced = dtfl_collection_reduce(keyed, mr,
	    dtfl_type_u32(), dtfl_type_u32(), i64, pair_min, DTFL_NOCTX);
	dtfl_collection *result = dtfl_collection_map(reduced, ft,
	    from_pair_key, DTFL_NOCTX);

	dtfl_collection_free(reduced);
	dtfl_collection_free(keyed);
	dtfl_collection_free(all);
	dtfl_collection_free(stepped);
	dtfl_collection_free(remed);
	dtfl_collection_free(picked);
	dtfl_collection_free(rooted);
	dtfl_diff_free(i64);
	dtfl_type_free(mr);
	dtfl_type_free(pairrec);
	return result;
}

/* distinct of active sources: active.map(src).distinct(). */
static dtfl_collection *
expand_active_src(dtfl_collection *active, dtfl_diff *i64)
{
	dtfl_collection *src = dtfl_collection_map(active, dtfl_type_u64(),
	    goal_src, DTFL_NOCTX);
	dtfl_collection *d = dtfl_collection_distinct(src, i64);

	dtfl_collection_free(src);
	return d;
}

/* distinct of active targets: active.map(dst).distinct(). */
static dtfl_collection *
expand_active_dst(dtfl_collection *active, dtfl_diff *i64)
{
	dtfl_collection *dst = dtfl_collection_map(active, dtfl_type_u64(),
	    goal_dst, DTFL_NOCTX);
	dtfl_collection *d = dtfl_collection_distinct(dst, i64);

	dtfl_collection_free(dst);
	return d;
}

static void
bij_build(dtfl_scope *inner, void *arg)
{
	bij_ctx *c = arg;
	const dtfl_type *rd = c->rd;
	const dtfl_type *ft = c->ft;
	const dtfl_type *rt = c->rt;
	const dtfl_type *st = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	dtfl_diff *i64 = dtfl_diff_i64();

	/* Seed forward and reverse variables from the goals. */
	dtfl_collection *goals = dtfl_collection_enter(c->goals, inner);
	dtfl_collection *edges = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *rev_edges;
	dtfl_collection *fwd_seed = dtfl_collection_map(goals, ft, seed_forward,
	    DTFL_NOCTX);
	dtfl_collection *rev_seed = dtfl_collection_map(goals, ft, seed_reverse,
	    DTFL_NOCTX);
	dtfl_collection *fwd, *rev;
	dtfl_variable *fvar = dtfl_variable_new_from(fwd_seed, &fwd);
	dtfl_variable *rvar = dtfl_variable_new_from(rev_seed, &rev);
	dtfl_collection *met, *reduced, *reached, *neg, *neg_key, *active;
	dtfl_collection *fwd_active, *rev_active, *fwd_next, *rev_next;
	dtfl_collection *left;

	/* reverse edges: (dst, src). */
	rev_edges = dtfl_collection_map(edges, dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64()), goal_dst_pair, DTFL_NOCTX);

	/* reached: forward join reverse by med, min, semijoin goals. */
	met = dtfl_collection_join_map(fwd, rev, dtfl_type_u64(), rd, rd, rt,
	    meet, DTFL_NOCTX);
	reduced = dtfl_collection_reduce(met, st, dtfl_type_u32(),
	    dtfl_type_u32(), i64, keep_min_dist, DTFL_NOCTX);
	reached = dtfl_collection_semijoin(reduced, goals, st, dtfl_type_u32(),
	    rt);

	/* active = -reached.map(key) concat goals, consolidated. */
	neg = dtfl_collection_negate(reached);
	neg_key = dtfl_collection_map(neg, st, reached_key, DTFL_NOCTX);
	{
		dtfl_collection *cat = dtfl_collection_concat(neg_key, goals);

		active = dtfl_collection_consolidate(cat);
		dtfl_collection_free(cat);
	}

	fwd_active = expand_active_src(active, i64);
	rev_active = expand_active_dst(active, i64);

	fwd_next = expand(c, fwd, fwd_active, edges);
	rev_next = expand(c, rev, rev_active, rev_edges);

	dtfl_variable_set(fvar, fwd_next);
	dtfl_variable_set(rvar, rev_next);

	left = dtfl_collection_leave(reached);
	c->result = left;

	dtfl_collection_free(rev_active);
	dtfl_collection_free(fwd_active);
	dtfl_collection_free(active);
	dtfl_collection_free(neg_key);
	dtfl_collection_free(neg);
	dtfl_collection_free(reached);
	dtfl_collection_free(reduced);
	dtfl_collection_free(met);
	dtfl_collection_free(rev);
	dtfl_collection_free(fwd);
	dtfl_collection_free(rev_seed);
	dtfl_collection_free(fwd_seed);
	dtfl_collection_free(rev_edges);
	dtfl_collection_free(edges);
	dtfl_collection_free(goals);
	dtfl_diff_free(i64);
	dtfl_type_free(st);
}

dtfl_collection *
dtfl_algo_bidijkstra(dtfl_collection *edges, dtfl_collection *goals)
{
	bij_ctx c;
	dtfl_scope *scope = dtfl_collection_scope(goals);

	c.edges = edges;
	c.goals = goals;
	c.ft = frontier_type();
	c.rd = rd_type();
	c.rt = result_type();
	c.result = NULL;

	dtfl_scope_iterative(scope, "BiDijkstra", bij_build, &c);

	dtfl_type_free(c.rt);
	dtfl_type_free(c.rd);
	dtfl_type_free(c.ft);
	return c.result;
}
