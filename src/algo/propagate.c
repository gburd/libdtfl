/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/graphs/propagate.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/algo/propagate.c: directed label reachability.
 *
 * Each node starts with a label; labels flow forward along directed
 * edges and every node keeps the minimum label it can be reached from.
 *
 * This ports propagate_core's rotated loop: a monotonic iteration
 * variable (Variable::new, seeded empty) rather than the morally
 * equivalent filter-then-iterate form.  The monotonic variable is what
 * lets propagate compose inside another iteration: the quoted
 * filter-then-iterate form "pretends to perform non-monotonic
 * computation" and does not converge when nested in a feedback loop,
 * whereas the empty monotonic variable does.  Upstream stages label
 * introduction by round; this port introduces every label at the loop
 * start (the default logic's zero delay), which is a plain enter.
 */
#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"

#include <string.h>

/* The (u64 node, u64 label) record type. */
static const dtfl_type *
node_label_type(void)
{
	return dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
}

/*
 * join_map result: key is the node, val1 is the current label (u64),
 * val2 is the edge destination (u64).  Emit (dst, label): the label
 * flows forward along the edge.
 */
static void
flow_label(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 8);		/* destination node */
	memcpy(buf + 8, v1, 8);		/* label */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * reduce logic: values are the labels proposed for one node, sorted by
 * value, so the smallest is first.  Keep it with diff 1.
 */
static void
keep_min_label(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_reduce_emit_pair(out, vals[0], 8, &one);
}

typedef struct prop_ctx {
	dtfl_collection *edges;	/* (src, dst) edges, outer scope */
	dtfl_collection *nodes;	/* (node, label) seeds, outer scope */
	const dtfl_type *nl;	/* (u64, u64) record type */
	dtfl_collection *result;	/* labels, left into the outer scope */
} prop_ctx;

/*
 * The Propagate subscope: a monotonic Variable seeded empty, concatenated
 * with the entered nodes, reduced to the minimum label per node, then
 * joined with the edges to flow labels forward.  The flowed labels feed
 * the variable; the reduced labels leave the scope.
 */
static void
prop_build(dtfl_scope *inner, void *arg)
{
	prop_ctx *c = arg;
	dtfl_collection *edges = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *nodes = dtfl_collection_enter(c->nodes, inner);
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *proposals;
	dtfl_variable *var = dtfl_variable_new(inner, c->nl, i64, &proposals);
	dtfl_collection *merged = dtfl_collection_concat(proposals, nodes);
	dtfl_collection *labels = dtfl_collection_reduce(merged, dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_u64(), i64, keep_min_label, DTFL_NOCTX);
	dtfl_collection *flowed = dtfl_collection_join_map(labels, edges,
	    dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64(), c->nl,
	    flow_label, DTFL_NOCTX);

	c->result = dtfl_collection_leave(labels);
	dtfl_variable_set(var, flowed);

	dtfl_collection_free(labels);
	dtfl_collection_free(merged);
	dtfl_collection_free(proposals);
	dtfl_collection_free(nodes);
	dtfl_collection_free(edges);
	dtfl_diff_free(i64);
}

dtfl_collection *
dtfl_algo_propagate(dtfl_collection *edges, dtfl_collection *nodes)
{
	const dtfl_type *nl = node_label_type();
	dtfl_scope *scope = dtfl_collection_scope(nodes);
	prop_ctx c;

	c.edges = edges;
	c.nodes = nodes;
	c.nl = nl;
	c.result = NULL;
	dtfl_scope_iterative(scope, "Propagate", prop_build, &c);
	dtfl_type_free(nl);
	return c.result;
}
