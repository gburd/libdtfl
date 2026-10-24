/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/examples/monoid-bfs.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * examples/dd/monoid-bfs.c: breadth-first distances carried in the diff.
 *
 * Upstream monoid-bfs stores each node's distance from the roots not in
 * the record but in a user-defined difference, MinSum { u32 value }:
 *   is_zero     always false (a semigroup with no zero element)
 *   plus_equals value = min(value, rhs.value)   (keep the shorter path)
 *   multiply    value = self.value + rhs.value  (min-plus product)
 * MinSum has no negate, so it is a Semigroup and not Abelian.  This is
 * exactly the user-defined-semigroup diff dtfl_diff_new builds, with a
 * Multiply<Self> callback so the join can multiply a node distance by an
 * edge weight.
 *
 * bfs() iterates to a fixed point: map each labelled node to a keyed
 * (node, ()) pair, join it against the outgoing edges so each
 * destination inherits distance + weight (the min-plus multiply),
 * concatenate the roots back in, and reduce_core to keep the minimum
 * distance per node.  reduce_core's output trace consolidates by MinSum
 * plus_equals (min), so the keep-min logic simply emits the new minimum
 * when it improves; no retraction is needed, as upstream relies on the
 * same semigroup consolidation.
 *
 * Upstream builds a random graph with a seeded StdRng, which a C runtime
 * cannot reproduce, and ends in count().map(|(_,l)| l).consolidate(),
 * which discards the distances and reports only how many nodes are
 * reachable.  This port instead runs over a fixed small graph and
 * inspects the bfs() result directly, printing each (node, distance) so
 * the min-plus distances are exact and hand-checkable.
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * A fixed directed graph (src, dst, weight).  From root 0, the min-plus
 * (shortest-path) distances are:
 *   0:0  1:2  2:3 (0->1->2 = 2+1 beats 0->2 = 5)  3:4 (0->1->3 = 2+2)
 *   4:7 (3->4 = 4+3).  Node 5 is unreachable and does not appear.
 */
static const struct {
	uint32_t src, dst, weight;
} fixed_edges[] = {
	{ 0, 1, 2 }, { 0, 2, 5 }, { 1, 2, 1 }, { 1, 3, 2 },
	{ 2, 3, 1 }, { 3, 4, 3 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])

/* ------------------------------------------------------------------ */
/* MinSum: the min-plus user diff (difference.rs in monoid-bfs.rs).    */
/* ------------------------------------------------------------------ */

/* plus_equals: self.value = min(self.value, rhs.value). */
static void
minsum_plus_equals(void *ctx, void *acc, const void *x)
{
	uint32_t a, b;

	(void)ctx;
	memcpy(&a, acc, 4);
	memcpy(&b, x, 4);
	if (b < a)
		memcpy(acc, &b, 4);
}

/* is_zero: always false (MinSum has no zero element). */
static bool
minsum_is_zero(void *ctx, const void *x)
{
	(void)ctx; (void)x;
	return false;
}

/* cmp: Ord by the u32 value. */
static int
minsum_cmp(void *ctx, const void *a, const void *b)
{
	uint32_t x, y;

	(void)ctx;
	memcpy(&x, a, 4);
	memcpy(&y, b, 4);
	return (x > y) - (x < y);
}

/* multiply: self.value = self.value + rhs.value (saturating at U32_MAX). */
static void
minsum_multiply(void *ctx, void *acc, const void *x)
{
	uint32_t a, b, s;

	(void)ctx;
	memcpy(&a, acc, 4);
	memcpy(&b, x, 4);
	s = a > UINT32_MAX - b ? UINT32_MAX : a + b;
	memcpy(acc, &s, 4);
}

static dtfl_diff *
minsum_new(void)
{
	dtfl_diff_ops ops = { 0 };

	ops.name = "MinSum";
	ops.width = 4;
	ops.plus_equals = minsum_plus_equals;
	ops.is_zero = minsum_is_zero;
	ops.cmp = minsum_cmp;
	ops.multiply = minsum_multiply;	/* no negate: a Semigroup, not Abelian */
	ops.type = dtfl_type_u32();
	return dtfl_diff_new(&ops);
}

/* ------------------------------------------------------------------ */
/* bfs: iterate node distances to a fixed point.                       */
/* ------------------------------------------------------------------ */

/* map n -> (n, ()): the keyed form.  () is 0 bytes, so the record is n. */
static void
key_by_node(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 4);	/* node; () appends 0 */
}

/*
 * join_map result (|_k, &(), d| *d): key is the node, val1 is the node's
 * () value (0 bytes), val2 is the edge destination (u32).  Emit the
 * destination; the operator carries the min-plus product of the two diffs
 * (node distance + edge weight) as the output diff.
 */
static void
relax_edge(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)v1; (void)v1l; (void)v2l;
	dtfl_join_emit_record(out, v2, 4);		/* destination node */
}

/*
 * reduce_core keep-min (|_key, input, output, updates|): the input is the
 * node's distances consolidated by MinSum plus_equals, so input[0] is the
 * single surviving entry carrying the minimum distance in its diff.  Emit
 * the minimum when the output is empty or the input improves on it; the
 * output trace consolidates by min, so no retraction is needed.
 */
static void
keep_min(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, dtfl_reduce_emit *out)
{
	uint32_t in_min, out_min;

	(void)ctx; (void)key; (void)klen; (void)n; (void)in_vals;
	(void)in_vlens; (void)m; (void)out_vals; (void)out_vlens;
	memcpy(&in_min, in_diffs, 4);		/* input[0].1 (consolidated) */
	if (m == 0) {
		dtfl_reduce_emit_pair(out, NULL, 0, &in_min);	/* ((), min) */
		return;
	}
	memcpy(&out_min, out_diffs, 4);		/* output[0].1 */
	if (in_min < out_min)
		dtfl_reduce_emit_pair(out, NULL, 0, &in_min);
}

/* as_collection(|k, ()| *k): the output record is the key (the node). */
static void
unkey_node(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	(void)ctx; (void)klen; (void)val; (void)vlen;
	dtfl_arranged_emit_record(out, key, 4);
}

typedef struct bfs_ctx {
	dtfl_collection *edges;		/* (src, dst) edges, outer scope */
	dtfl_collection *roots;		/* root nodes, outer scope */
	const dtfl_type *node;		/* u32 node record */
	dtfl_diff *minsum;		/* the MinSum diff */
	dtfl_collection *result;	/* node distances, left to the outer scope */
} bfs_ctx;

static void
bfs_build(dtfl_scope *inner, void *arg)
{
	bfs_ctx *c = arg;
	const dtfl_type *u32 = dtfl_type_u32();
	dtfl_collection *collection;
	dtfl_variable *var = dtfl_variable_new(inner, c->node, c->minsum,
	    &collection);
	dtfl_collection *edges = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *roots = dtfl_collection_enter(c->roots, inner);
	/* collection.map(|n| (n,())).join_map(edges, |_k,&(),d| *d) */
	dtfl_collection *keyed = dtfl_collection_map(collection, u32,
	    key_by_node, DTFL_NOCTX);
	dtfl_collection *stepped = dtfl_collection_join_map(keyed, edges, u32,
	    dtfl_type_unit(), u32, u32, relax_edge, DTFL_NOCTX);
	/* .concat(roots).map(|x| (x,())) */
	dtfl_collection *merged = dtfl_collection_concat(stepped, roots);
	dtfl_collection *remkeyed = dtfl_collection_map(merged, u32,
	    key_by_node, DTFL_NOCTX);
	/* .reduce_core("Reduce", keep_min).as_collection(|k,()| *k) */
	dtfl_arranged *arr = dtfl_collection_arrange_by_key(remkeyed, u32,
	    dtfl_type_unit(), "Arrange");
	dtfl_arranged *reduced = dtfl_arranged_reduce_core(arr, "Reduce",
	    dtfl_type_unit(), c->minsum, keep_min, DTFL_NOCTX);
	dtfl_collection *result = dtfl_arranged_as_collection(reduced, u32,
	    unkey_node, DTFL_NOCTX);

	c->result = dtfl_collection_leave(result);
	dtfl_variable_set(var, result);

	dtfl_arranged_free(reduced);
	dtfl_arranged_free(arr);
	dtfl_collection_free(remkeyed);
	dtfl_collection_free(merged);
	dtfl_collection_free(stepped);
	dtfl_collection_free(keyed);
	dtfl_collection_free(roots);
	dtfl_collection_free(edges);
	dtfl_collection_free(collection);
}

/* bfs(edges, roots): node distances from the roots, carried in the diff. */
static dtfl_collection *
bfs(dtfl_collection *edges, dtfl_collection *roots, dtfl_diff *minsum)
{
	bfs_ctx c;

	c.edges = edges;
	c.roots = roots;
	c.node = dtfl_type_u32();
	c.minsum = minsum;
	c.result = NULL;
	dtfl_scope_iterative(dtfl_collection_scope(roots), "BFS", bfs_build, &c);
	return c.result;
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */

/* inspect: print "\t(node, distance)" for each node's min-plus distance. */
static void
inspect_dist(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	uint32_t dist;

	(void)ctx; (void)len; (void)time;
	memcpy(&dist, diff, 4);
	printf("\t(%u, %u)\n", dtfl_get_u32(rec), dist);
}

typedef struct build_ctx {
	dtfl_input_session *roots;
	dtfl_input_session *edges;
	dtfl_diff *minsum;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *roots = dtfl_input_session_to_collection(c->roots,
	    scope);
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	dtfl_collection *result = bfs(edges, roots, c->minsum);
	dtfl_collection *con = dtfl_collection_consolidate(result);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_dist,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(result);
	dtfl_collection_free(edges);
	dtfl_collection_free(roots);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *edge = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	build_ctx c;
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	c.minsum = minsum_new();
	c.roots = dtfl_input_session_new(dtfl_type_u32(), c.minsum);
	c.edges = dtfl_input_session_new(edge, c.minsum);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "monoid-bfs", tc, build, &c);

	if (dtfl_worker_index(w) == 0) {
		unsigned char rec[8];
		uint32_t zero = 0;

		/* roots.update_at(0, 0, MinSum { value: 0 }) */
		dtfl_put_u32(rec, 0);
		dtfl_input_session_update(c.roots, rec, 4, &zero);
		for (i = 0; i < FIXED_E; i++) {
			dtfl_put_u32(rec, fixed_edges[i].src);
			dtfl_put_u32(rec + 4, fixed_edges[i].dst);
			dtfl_input_session_update(c.edges, rec, 8,
			    &fixed_edges[i].weight);
		}
	}
	dtfl_input_session_advance_to(c.roots, &one);
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_flush(c.roots);
	dtfl_input_session_flush(c.edges);
	while (dtfl_probe_less_than(c.probe, dtfl_input_session_time(c.edges)))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.roots);
	dtfl_input_session_close(c.edges);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_diff_free(c.minsum);
	dtfl_type_free(edge);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
