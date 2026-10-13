/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dogs/delta_query_wcoj.c: mirrors
 * dogsdogsdogs/examples/delta_query_wcoj.rs.
 *
 * The worst-case-optimal triangle delta query Q(a,b,c) := E1(a,b), E2(b,c),
 * E3(a,c).  Each relation drives a delta rule that extends the changed edge
 * with the two matching endpoints, using the generic extend() join over a
 * CollectionIndex of the edge relation (forward and reverse), in an AltNeu
 * scope where relations after the delta stream are delayed alt -> neu.
 * Upstream reads the graph from a GraphMMap file and reports timings; this
 * port feeds a small fixed graph, so the set of triangles is exact and
 * printed as upstream's inspect does ("\tTriangle: (a, b, c)").
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
 * A fixed directed graph.  Directed triangles (a,b,c) with edges
 * a->b, b->c, a->c:
 *   (0,1,2): 0->1, 1->2, 0->2
 *   (0,1,3): 0->1, 1->3, 0->3
 * Edge 4->5 forms no triangle.
 */
static const uint32_t fixed_edges[][2] = {
	{ 0, 1 }, { 1, 2 }, { 0, 2 }, { 1, 3 }, { 0, 3 }, { 4, 5 },
};
#define N_EDGES (sizeof fixed_edges / sizeof fixed_edges[0])

/* ------------------------------------------------------------------ */
/* Key selectors on a pair(u32, u32) prefix.                           */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* map closures: reverse an edge, and re-order the extended prefixes.  */
/* ------------------------------------------------------------------ */

static void
map_reverse(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 4, 4);
	memcpy(buf + 4, rec, 4);
	dtfl_collection_emit_record(out, buf, 8);
}

/* dQ/dE1: ((a,b),c) -> (a,b,c). */
static void
map_abc_1(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 12);
}

/* dQ/dE2: ((b,c),a) -> (a,b,c). */
static void
map_abc_2(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[12];

	(void)ctx; (void)len;
	memcpy(buf, p + 8, 4);
	memcpy(buf + 4, p, 4);
	memcpy(buf + 8, p + 4, 4);
	dtfl_collection_emit_record(out, buf, 12);
}

/* dQ/dE3: ((a,c),b) -> (a,b,c). */
static void
map_abc_3(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[12];

	(void)ctx; (void)len;
	memcpy(buf, p, 4);
	memcpy(buf + 4, p + 8, 4);
	memcpy(buf + 8, p + 4, 4);
	dtfl_collection_emit_record(out, buf, 12);
}

static void
inspect_triangle(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	const unsigned char *p = rec;
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	if (d <= 0)
		return;
	printf("\tTriangle: (%llu, %llu, %llu)\n",
	    (unsigned long long)dtfl_get_u32(p),
	    (unsigned long long)dtfl_get_u32(p + 4),
	    (unsigned long long)dtfl_get_u32(p + 8));
}

/* ------------------------------------------------------------------ */
/* The scoped triangle build.                                          */
/* ------------------------------------------------------------------ */

typedef struct tri_ctx {
	dtfl_collection *edges;
	dtfl_collection *result;
	const dtfl_type *pair;
	const dtfl_type *tri;
	const dtfl_type *u32;
} tri_ctx;

static void
build_triangles(dtfl_scope *inner, void *arg)
{
	tri_ctx *c = arg;
	dtfl_collection *forward = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *reverse_outer = dtfl_collection_map(c->edges, c->pair,
	    map_reverse, DTFL_NOCTX);
	dtfl_collection *reverse = dtfl_collection_enter(reverse_outer, inner);
	dtfl_collection *forward_neu = dtfl_collection_delay_neu(forward);
	dtfl_collection *reverse_neu = dtfl_collection_delay_neu(reverse);
	/* Four indexes: alt (current) and neu (delayed) forward and reverse. */
	dtfl_collection_index *alt_forward = dtfl_collection_index_new(forward,
	    c->u32, c->u32);
	dtfl_collection_index *alt_reverse = dtfl_collection_index_new(reverse,
	    c->u32, c->u32);
	dtfl_collection_index *neu_forward =
	    dtfl_collection_index_new(forward_neu, c->u32, c->u32);
	dtfl_collection_index *neu_reverse =
	    dtfl_collection_index_new(reverse_neu, c->u32, c->u32);
	dtfl_prefix_extender *e1a, *e1b, *e2a, *e2b, *e3a, *e3b;
	dtfl_prefix_extender *ext1[2], *ext2[2], *ext3[2];
	dtfl_collection *ch1, *ch2, *ch3, *m1, *m2, *m3, *cat12, *cat;

	/* dQ/dE1 := dE1(a,b), E2(b,c), E3(a,c): extend forward with b then a. */
	e1a = dtfl_collection_index_extend_using(neu_forward, c->pair,
	    key_second, DTFL_NOCTX);
	e1b = dtfl_collection_index_extend_using(neu_forward, c->pair,
	    key_first, DTFL_NOCTX);
	ext1[0] = e1a; ext1[1] = e1b;
	ch1 = dtfl_collection_extend(forward, ext1, 2, c->tri);
	m1 = dtfl_collection_map(ch1, c->tri, map_abc_1, DTFL_NOCTX);

	/* dQ/dE2 := dE2(b,c), E1(a,b), E3(a,c). */
	e2a = dtfl_collection_index_extend_using(alt_reverse, c->pair,
	    key_first, DTFL_NOCTX);
	e2b = dtfl_collection_index_extend_using(neu_reverse, c->pair,
	    key_second, DTFL_NOCTX);
	ext2[0] = e2a; ext2[1] = e2b;
	ch2 = dtfl_collection_extend(forward, ext2, 2, c->tri);
	m2 = dtfl_collection_map(ch2, c->tri, map_abc_2, DTFL_NOCTX);

	/* dQ/dE3 := dE3(a,c), E1(a,b), E2(b,c). */
	e3a = dtfl_collection_index_extend_using(alt_forward, c->pair,
	    key_first, DTFL_NOCTX);
	e3b = dtfl_collection_index_extend_using(alt_reverse, c->pair,
	    key_second, DTFL_NOCTX);
	ext3[0] = e3a; ext3[1] = e3b;
	ch3 = dtfl_collection_extend(forward, ext3, 2, c->tri);
	m3 = dtfl_collection_map(ch3, c->tri, map_abc_3, DTFL_NOCTX);

	cat12 = dtfl_collection_concat(m1, m2);
	cat = dtfl_collection_concat(cat12, m3);
	c->result = dtfl_collection_leave(cat);

	dtfl_collection_free(cat);
	dtfl_collection_free(cat12);
	dtfl_collection_free(m1);
	dtfl_collection_free(m2);
	dtfl_collection_free(m3);
	dtfl_collection_free(ch1);
	dtfl_collection_free(ch2);
	dtfl_collection_free(ch3);
	dtfl_prefix_extender_free(e1a);
	dtfl_prefix_extender_free(e1b);
	dtfl_prefix_extender_free(e2a);
	dtfl_prefix_extender_free(e2b);
	dtfl_prefix_extender_free(e3a);
	dtfl_prefix_extender_free(e3b);
	dtfl_collection_index_free(alt_forward);
	dtfl_collection_index_free(alt_reverse);
	dtfl_collection_index_free(neu_forward);
	dtfl_collection_index_free(neu_reverse);
	dtfl_collection_free(forward_neu);
	dtfl_collection_free(reverse_neu);
	dtfl_collection_free(forward);
	dtfl_collection_free(reverse);
	dtfl_collection_free(reverse_outer);
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	tri_ctx *tc;
} build_ctx;

static void
build_dataflow(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *edges = dtfl_input_session_to_collection(c->in, scope);
	dtfl_time_class *base = dtfl_time_u64();
	dtfl_time_class *altneu = dtfl_time_altneu(base);
	dtfl_collection *ins, *probed;

	c->tc->edges = edges;
	c->tc->result = NULL;
	dtfl_scope_scoped(scope, "DeltaQuery (Triangles)", altneu,
	    build_triangles, c->tc);
	dtfl_time_class_free(altneu);
	dtfl_time_class_free(base);

	ins = dtfl_collection_inspect(c->tc->result, inspect_triangle,
	    DTFL_NOCTX);
	probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(c->tc->result);
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
	tri_ctx tctx;
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	memset(&tctx, 0, sizeof tctx);
	tctx.pair = pair; tctx.tri = tri; tctx.u32 = u32;
	c.in = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	c.tc = &tctx;
	dtfl_worker_dataflow(w, "wcoj", tc, build_dataflow, &c);

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
