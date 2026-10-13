/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/graphs/scc.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/algo/scc.c: strongly connected component edge structure.
 *
 * strongly_connected returns the subset of edges whose endpoints share a
 * strongly connected component.  It iterates a variable seeded from the
 * whole edge set, trimming the candidate edges twice each round: once by
 * forward label propagation, once on the transposed graph.  An edge
 * survives a trim when both of its endpoints carry the same minimum
 * label (they reach the same component representative).
 *
 * Upstream threads a prioritization logic (strongly_connected_at) that
 * introduces node labels in log-bucketed rounds; this port carries the
 * default (unprioritized) logic, matching strongly_connected, and relies
 * on dtfl_algo_propagate for the label step.
 */
#include "dtfl/algo.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/time.h"

#include <string.h>

/* The (u64, u64) edge / labelled-pair record type. */
static const dtfl_type *
pair_type(void)
{
	return dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
}

/* map (src, dst) -> (dst, src): transpose an edge. */
static void
transpose(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 8, 8);	/* dst -> field 0 */
	memcpy(buf + 8, rec, 8);			/* src -> field 1 */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* map (src, dst) -> (dst, dst): each destination node labelled by itself. */
static void
node_self(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 8, 8);		/* dst */
	memcpy(buf + 8, (const unsigned char *)rec + 8, 8);	/* dst */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/*
 * join_map result (first join): edges are keyed by src (e1); val1 is the
 * destination (e2); val2 is e1's label l1.  Emit a record keyed by the
 * destination e2 carrying (e1, l1), so the second join can match e2
 * against its own label.  Record layout: e2 | e1 | l1.
 */
static void
join_label1(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1l, const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[24];

	(void)ctx; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v1, 8);		/* e2 (destination) -> key */
	memcpy(buf + 8, key, 8);	/* e1 (source) */
	memcpy(buf + 16, v2, 8);	/* l1 (label of e1) */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * join_map result (second join): key is e2, val1 is (e1, l1), val2 is
 * e2's label l2.  Emit ((e1, e2), (l1, l2)) as a 32-byte record.
 */
static void
join_label2(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1l, const void *v2, size_t v2l, dtfl_join_emit *out)
{
	const unsigned char *val1 = v1;
	unsigned char buf[32];

	(void)ctx; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, val1, 8);		/* e1 */
	memcpy(buf + 8, key, 8);	/* e2 */
	memcpy(buf + 16, val1 + 8, 8);	/* l1 */
	memcpy(buf + 24, v2, 8);	/* l2 */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* filter: keep ((e1,e2),(l1,l2)) where l1 == l2 (same component). */
static bool
same_label(void *ctx, const void *rec, size_t len)
{
	const unsigned char *p = rec;

	(void)ctx; (void)len;
	return memcmp(p + 16, p + 24, 8) == 0;
}

/* map ((e1,e2),(l1,l2)) -> (e2, e1): the surviving edge, reversed. */
static void
emit_reversed(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, p + 8, 8);		/* e2 */
	memcpy(buf + 8, p, 8);		/* e1 */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/*
 * trim_edges(cycle, edges): keep the edges of `edges` whose endpoints
 * carry equal labels under forward propagation over `cycle`, returning
 * the surviving edges reversed.  Both inputs live in the same scope.
 */
static dtfl_collection *
trim_edges(dtfl_collection *cycle, dtfl_collection *edges)
{
	const dtfl_type *pr = pair_type();
	const dtfl_type *trip;
	dtfl_collection *nodes_raw = dtfl_collection_map(edges, pr, node_self,
	    DTFL_NOCTX);
	dtfl_collection *nodes = dtfl_collection_consolidate(nodes_raw);
	dtfl_collection *labels = dtfl_algo_propagate(cycle, nodes);
	dtfl_collection *j1, *j2, *filt, *result;

	/* First join: edges (dst key) with labels (node key) by dst. */
	trip = dtfl_type_tuple((const dtfl_type *[]){ dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_u64() }, 3);
	j1 = dtfl_collection_join_map(edges, labels, dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_u64(), trip, join_label1, DTFL_NOCTX);

	/* Second join: (e2, (e1, l1)) with labels by e2.  j1's value side
	 * is the pair (e1, l1). */
	{
		const dtfl_type *quad = dtfl_type_tuple((const dtfl_type *[]){
		    dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64(),
		    dtfl_type_u64() }, 4);

		j2 = dtfl_collection_join_map(j1, labels, dtfl_type_u64(),
		    pr, dtfl_type_u64(), quad, join_label2, DTFL_NOCTX);
		dtfl_type_free(quad);
	}

	filt = dtfl_collection_filter(j2, same_label, DTFL_NOCTX);
	result = dtfl_collection_map(filt, pr, emit_reversed, DTFL_NOCTX);

	dtfl_collection_free(filt);
	dtfl_collection_free(j2);
	dtfl_collection_free(j1);
	dtfl_collection_free(labels);
	dtfl_collection_free(nodes);
	dtfl_collection_free(nodes_raw);
	dtfl_type_free(trip);
	dtfl_type_free(pr);
	return result;
}

typedef struct scc_ctx {
	dtfl_collection *graph;	/* original edges, outer scope */
} scc_ctx;

static dtfl_collection *
scc_logic(void *vctx, dtfl_scope *inner, dtfl_collection *var)
{
	scc_ctx *c = vctx;
	const dtfl_type *pr = pair_type();
	dtfl_collection *edges = dtfl_collection_enter(c->graph, inner);
	dtfl_collection *trans = dtfl_collection_map(edges, pr, transpose,
	    DTFL_NOCTX);
	dtfl_collection *trim1 = trim_edges(var, edges);
	dtfl_collection *trim2 = trim_edges(trim1, trans);

	dtfl_collection_free(trim1);
	dtfl_collection_free(trans);
	dtfl_collection_free(edges);
	dtfl_type_free(pr);
	return trim2;
}

dtfl_collection *
dtfl_algo_scc(dtfl_collection *graph)
{
	scc_ctx c = { graph };

	return dtfl_collection_iterate(graph, scc_logic, DTFL_CTX(&c, NULL));
}
