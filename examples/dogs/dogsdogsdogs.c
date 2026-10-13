/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dogs/dogsdogsdogs.c: mirrors
 * dogsdogsdogs/examples/dogsdogsdogs.rs.
 *
 * Counts directed triangles with the generic worst-case-optimal join over a
 * single CollectionIndex of the edge relation.  Each edge (x, y) is a prefix
 * extended with the node z such that both (x, z) and (y, z) are edges; the
 * two extenders read the index at key x and at key y, the join routes each
 * prefix to whichever offers fewer extensions (via count), proposes from it
 * and validates against the other.  Upstream drives the generic
 * count/propose/validate operators by hand; dtfl_collection_extend performs
 * exactly that routing internally, so this port expresses the same join.
 * Upstream reads the graph from a GraphMMap file and prints the running count
 * of extended tuples; this port feeds a fixed graph and prints the triangle
 * records found (one per (edge, z) extension).
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dogs.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

/*
 * A fixed directed graph.  An edge prefix (x, y) extends to z when (x, z)
 * and (y, z) are both edges.  With these edges the extensions are:
 *   (0,1) -> 2 (0->2, 1->2) and -> 3 (0->3, 1->3)
 *   (0,2) -> none; (1,2) -> none; (0,3) -> none; (1,3) -> none; (4,5) -> none
 * so two triangles result: ((0,1),2) and ((0,1),3).
 */
static const uint32_t fixed_edges[][2] = {
	{ 0, 1 }, { 1, 2 }, { 0, 2 }, { 1, 3 }, { 0, 3 }, { 4, 5 },
};
#define N_EDGES (sizeof fixed_edges / sizeof fixed_edges[0])

/* Key selectors on a pair(u32, u32) prefix. */
static void
key_first(void *ctx, const void *prefix, size_t plen, void *key, size_t cap,
    size_t *klen)
{
	(void)ctx; (void)plen; (void)cap;
	memcpy(key, prefix, sizeof(uint32_t));
	*klen = sizeof(uint32_t);
}

static void
key_second(void *ctx, const void *prefix, size_t plen, void *key, size_t cap,
    size_t *klen)
{
	(void)ctx; (void)plen; (void)cap;
	memcpy(key, (const unsigned char *)prefix + sizeof(uint32_t),
	    sizeof(uint32_t));
	*klen = sizeof(uint32_t);
}

/* inspect: print each extended triple (x, y, z) with positive multiplicity. */
static void
inspect_triple(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	const unsigned char *p = rec;
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	if (d <= 0)
		return;
	printf("(%llu, %llu, %llu)\n", (unsigned long long)dtfl_get_u32(p),
	    (unsigned long long)dtfl_get_u32(p + 4),
	    (unsigned long long)dtfl_get_u32(p + 8));
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	const dtfl_type *pair;
	const dtfl_type *tri;
	const dtfl_type *u32;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *edges = dtfl_input_session_to_collection(c->in, scope);
	/* One index of the edge relation, keyed by source. */
	dtfl_collection_index *idx = dtfl_collection_index_new(edges, c->u32,
	    c->u32);
	/* Two extenders: extend prefix (x, y) at key x and at key y. */
	dtfl_prefix_extender *ext_xz = dtfl_collection_index_extend_using(idx,
	    c->pair, key_first, DTFL_NOCTX);
	dtfl_prefix_extender *ext_yz = dtfl_collection_index_extend_using(idx,
	    c->pair, key_second, DTFL_NOCTX);
	dtfl_prefix_extender *exts[2] = { ext_xz, ext_yz };
	dtfl_collection *tris = dtfl_collection_extend(edges, exts, 2, c->tri);
	dtfl_collection *ins = dtfl_collection_inspect(tris, inspect_triple,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(tris);
	dtfl_prefix_extender_free(ext_xz);
	dtfl_prefix_extender_free(ext_yz);
	dtfl_collection_index_free(idx);
	dtfl_collection_free(edges);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	const dtfl_type *tf[3] = { dtfl_type_u32(), dtfl_type_u32(),
	    dtfl_type_u32() };
	const dtfl_type *tri = dtfl_type_tuple(tf, 3);
	build_ctx c;
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	c.in = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	c.pair = pair; c.tri = tri; c.u32 = u32;
	dtfl_worker_dataflow(w, "dogsdogsdogs", tc, build, &c);

	for (i = 0; i < N_EDGES; i++) {
		unsigned char rec[8];

		dtfl_put_u32(rec, fixed_edges[i][0]);
		dtfl_put_u32(rec + 4, fixed_edges[i][1]);
		dtfl_input_session_insert(c.in, rec, sizeof rec);
	}
	dtfl_input_session_advance_to(c.in, &one);
	dtfl_input_session_flush(c.in);
	while (dtfl_probe_less_than(c.probe, &one))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_type_free(tri);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc;

	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
