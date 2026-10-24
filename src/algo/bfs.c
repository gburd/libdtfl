/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/graphs/bfs.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/algo/bfs.c: breadth-first distance labeling.
 *
 * bfs seeds each root at distance 0, then iterates to a fixed point:
 * join each labelled node against the outgoing edges to reach each
 * destination at distance + 1, concatenate the roots back in, and reduce
 * to keep the minimum distance per node.
 *
 * The distance is a u32 counter, as upstream keeps it: distance + 1 is
 * computed in u32 and wraps at 2^32, matching bfs.rs's u32 distance.
 * The loop record is the natural (u64 node, u32 dist) pair, twelve
 * bytes, so the update's embedded time lands at a byte offset that is
 * not a multiple of eight; the time accessor copies the words into an
 * aligned buffer, so the odd-sized record is read safely.
 *
 * Upstream bfs arranges the edges once and uses Collection::join_core to
 * reuse that index each round.  The public C surface arranges by key
 * through join_map, which arranges both operands by key internally; the
 * result is identical, matching the join_map form the upstream bfs test
 * compares against the sequential oracle.
 */
#include "dtfl/algo.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"

#include <string.h>

/* The (u64 node, u32 dist) record type, built once per bfs call. */
static const dtfl_type *
node_dist_type(void)
{
	return dtfl_type_pair(dtfl_type_u64(), dtfl_type_u32());
}

/* map a bare u64 node to (node, 0): the root at distance 0. */
static void
seed_root(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[12];

	(void)ctx; (void)len;
	memcpy(buf, rec, 8);		/* node */
	dtfl_put_u32(buf + 8, 0);	/* distance 0 */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/*
 * join_map result: key is the node, val1 is the inner value (the u32
 * distance), val2 is the edge destination (u64).  Emit (dst, dist + 1);
 * the add is performed in u32 and wraps at 2^32, as upstream's u32
 * counter does.
 */
static void
step_edge(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[12];
	uint32_t dist = dtfl_get_u32(v1);

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 8);			/* destination node */
	dtfl_put_u32(buf + 8, dist + 1);	/* distance + 1 (u32 wrap) */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * reduce logic: input is the multiset of distances for one node, sorted
 * by value, so the smallest distance is first.  Keep it with diff 1.
 */
static void
keep_min(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_reduce_emit_pair(out, vals[0], 4, &one);
}

typedef struct bfs_ctx {
	dtfl_collection *edges;	/* (src, dst) edges, outer scope */
	dtfl_collection *nodes;	/* seeded (node, 0) roots, outer scope */
	const dtfl_type *nd;	/* (u64, u64) record type */
} bfs_ctx;

static dtfl_collection *
bfs_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	bfs_ctx *c = vctx;
	dtfl_collection *edges = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *stepped = dtfl_collection_join_map(var, edges,
	    dtfl_type_u64(), dtfl_type_u32(), dtfl_type_u64(), c->nd,
	    step_edge, DTFL_NOCTX);
	dtfl_collection *nodes = dtfl_collection_enter(c->nodes, inner);
	dtfl_collection *merged = dtfl_collection_concat(stepped, nodes);
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *reduced = dtfl_collection_reduce(merged,
	    dtfl_type_u64(), dtfl_type_u32(), dtfl_type_u32(), i64, keep_min,
	    DTFL_NOCTX);

	dtfl_diff_free(i64);
	dtfl_collection_free(merged);
	dtfl_collection_free(nodes);
	dtfl_collection_free(stepped);
	dtfl_collection_free(edges);
	return reduced;
}

dtfl_collection *
dtfl_algo_bfs(dtfl_collection *edges, dtfl_collection *roots)
{
	const dtfl_type *nd = node_dist_type();
	dtfl_collection *nodes = dtfl_collection_map(roots, nd, seed_root,
	    DTFL_NOCTX);
	bfs_ctx c = { edges, nodes, nd };
	dtfl_collection *result = dtfl_collection_iterate(nodes, bfs_logic,
	    DTFL_CTX(&c, NULL));

	dtfl_collection_free(nodes);
	dtfl_type_free(nd);
	return result;
}
