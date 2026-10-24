/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dogs/delta_query.c: mirrors
 * dogsdogsdogs/examples/delta_query.rs.
 *
 * The triangle delta query Q(a,b,c) := E1(a,b), E2(b,c), E3(a,c) in an
 * AltNeu scope.  Each relation drives a delta rule that extends the changed
 * edge with the two matching endpoints; a relation after the delta stream
 * in the join order is delayed alt -> neu so each pair of concurrent updates
 * interacts exactly once.  Upstream reads a GraphMMap file and reports
 * timings; this port feeds a small fixed graph, so the triangle set is exact
 * and printed as upstream's inspect does ("\tTriangle: (a, b, c)").
 *
 * Divergence from the Rust.  (1) Upstream computes the triangles two ways --
 * the "prior technology" propose/validate path and the "new ideas" join_core
 * path -- and asserts them equal.  The propose/validate path is already
 * demonstrated by examples/dogs/delta_query_wcoj.c (the generic extend()
 * join), so this port carries only the join_core path, the distinct
 * contribution of this example.  (2) Upstream arranges forward/reverse,
 * forward_self/reverse_self in the OUTER scope and brings them in with
 * arrangement.enter_at(inner, alt/neu, |t| t - 1).  The public API has no
 * arranged enter_at, so this port enters the edge COLLECTION into the AltNeu
 * scope, delays it alt -> neu for the neu relations (dtfl_collection_delay_neu),
 * and arranges INSIDE the scope.  Arranging a delay_neu'd collection inside
 * the scope is observationally identical to enter_at(neu) on an outer
 * arrangement -- the same delta-query result, only the trace sharing (a
 * performance detail) differs.  (3) The GraphMMap input is replaced by the
 * same small fixed graph delta_query_wcoj.c uses, so the triangle set
 * cross-checks against that example exactly.
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
 * A fixed directed graph (the same one delta_query_wcoj.c uses).  Directed
 * triangles (a,b,c) with edges a->b, b->c, a->c:
 *   (0,1,2): 0->1, 1->2, 0->2
 *   (0,1,3): 0->1, 1->3, 0->3
 * Edge 4->5 forms no triangle.
 */
static const uint32_t fixed_edges[][2] = {
	{ 0, 1 }, { 1, 2 }, { 0, 2 }, { 1, 3 }, { 0, 3 }, { 4, 5 },
};
#define N_EDGES (sizeof fixed_edges / sizeof fixed_edges[0])

/* ------------------------------------------------------------------ */
/* map closures.                                                       */
/* ------------------------------------------------------------------ */

/* map |(x,y)| (y,x): reverse an edge. */
static void
map_reverse(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 4, 4);
	memcpy(buf + 4, rec, 4);
	dtfl_collection_emit_record(out, buf, 8);
}

/* ------------------------------------------------------------------ */
/* join_core result closures.                                          */
/* ------------------------------------------------------------------ */

/*
 * changes1 first join: d_edges mapped (b,a) joined with forward_key_neu
 * (b -> c), as |b,a,c| Some(((a,c),b)).  Key is b; val1 is a (the mapped
 * value); val2 is c (forward value under b).  Output record ((a,c),b).
 */
static void
join_c1a(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[12]; /* (a, c, b) */

	(void)ctx; (void)v1len; (void)v2len;
	memcpy(buf, val1, 4);		/* a (mapped (b,a) value) */
	memcpy(buf + 4, val2, 4);	/* c (forward value under b) */
	memcpy(buf + 8, key, 4);	/* b (the shared key) */
	dtfl_join_emit_record(out, buf, sizeof buf);
	(void)klen;
}

/*
 * changes2 first join: d_edges (b,c) joined with reverse_key_alt (b -> a),
 * as |b,c,a| Some(((a,c),b)).  Key is b; val1 is c (d_edges value); val2 is
 * a (reverse value under b).  Output record ((a,c),b).
 */
static void
join_c2a(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[12]; /* (a, c, b) */

	(void)ctx; (void)v1len; (void)v2len;
	memcpy(buf, val2, 4);		/* a (reverse value) */
	memcpy(buf + 4, val1, 4);	/* c (d_edges value) */
	memcpy(buf + 8, key, 4);	/* b (the shared key) */
	dtfl_join_emit_record(out, buf, sizeof buf);
	(void)klen;
}

/*
 * changes3 first join: d_edges (a,c) joined with forward_key_alt (a -> b),
 * as |a,c,b| Some(((c,b),a)).  Key is a; val1 is c (d_edges value); val2 is
 * b (forward value under a).  Output record ((c,b),a).
 */
static void
join_c3a(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[12]; /* (c, b, a) */

	(void)ctx; (void)v1len; (void)v2len;
	memcpy(buf, val1, 4);		/* c (d_edges value) */
	memcpy(buf + 4, val2, 4);	/* b (forward value) */
	memcpy(buf + 8, key, 4);	/* a (the shared key) */
	dtfl_join_emit_record(out, buf, sizeof buf);
	(void)klen;
}

/*
 * changes1 second join: ((a,c),b) validated against forward_self_neu (the
 * edge a->c), as |(a,c),b,&()| Some((a,b,c)).  Key is (a,c); val1 is b;
 * val2 is unit.  Output record (a,b,c).
 */
static void
join_c1b(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	const unsigned char *p = key;
	unsigned char buf[12]; /* (a, b, c) */

	(void)ctx; (void)klen; (void)v1len; (void)val2; (void)v2len;
	memcpy(buf, p, 4);		/* a */
	memcpy(buf + 4, val1, 4);	/* b */
	memcpy(buf + 8, p + 4, 4);	/* c */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * changes2 second join: ((a,c),b) validated against forward_self_neu (edge
 * a->c), as |(a,c),b,&()| Some((a,b,c)).  Identical shape to changes1's.
 */
static void
join_c2b(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	const unsigned char *p = key;
	unsigned char buf[12]; /* (a, b, c) */

	(void)ctx; (void)klen; (void)v1len; (void)val2; (void)v2len;
	memcpy(buf, p, 4);		/* a */
	memcpy(buf + 4, val1, 4);	/* b */
	memcpy(buf + 8, p + 4, 4);	/* c */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * changes3 second join: ((c,b),a) validated against reverse_self_alt (the
 * reverse edge (c,b), i.e. the original edge b->c), as |(c,b),a,&()|
 * Some((a,b,c)).  Key is (c,b); val1 is a; val2 is unit.  Output (a,b,c).
 */
static void
join_c3b(void *ctx, const void *key, size_t klen, const void *val1,
    size_t v1len, const void *val2, size_t v2len, dtfl_join_emit *out)
{
	const unsigned char *p = key;
	unsigned char buf[12]; /* (a, b, c) */

	(void)ctx; (void)klen; (void)v1len; (void)val2; (void)v2len;
	memcpy(buf, val1, 4);		/* a */
	memcpy(buf + 4, p + 4, 4);	/* b */
	memcpy(buf + 8, p, 4);		/* c */
	dtfl_join_emit_record(out, buf, sizeof buf);
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
/* The scoped triangle build (the join_core path).                     */
/* ------------------------------------------------------------------ */

typedef struct tri_ctx {
	dtfl_collection *edges;	/* the outer edge collection */
	dtfl_collection *result;
	const dtfl_type *u32;
	const dtfl_type *unit;
	const dtfl_type *pair;	/* (u32, u32), the (a,c) / (c,b) key */
	const dtfl_type *acb;	/* (u32, u32, u32), the ((a,c),b) record */
	const dtfl_type *tri;	/* (u32, u32, u32), the output (a,b,c) */
} tri_ctx;

static void
build_triangles(dtfl_scope *inner, void *arg)
{
	tri_ctx *c = arg;
	const dtfl_type *u32 = c->u32, *unit = c->unit, *pair = c->pair;
	const dtfl_type *acb = c->acb, *tri = c->tri;

	/* The edge relations: entered (alt), reversed, and delayed to neu. */
	dtfl_collection *forward = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *reverse_outer = dtfl_collection_map(c->edges, pair,
	    map_reverse, DTFL_NOCTX);
	dtfl_collection *reverse = dtfl_collection_enter(reverse_outer, inner);
	dtfl_collection *forward_neu = dtfl_collection_delay_neu(forward);

	/* The instantaneous change stream: d_edges = differentiate(edges). */
	dtfl_collection *d_edges = dtfl_collection_differentiate(c->edges,
	    inner);
	dtfl_collection *d_rev = dtfl_collection_map(d_edges, pair, map_reverse,
	    DTFL_NOCTX);

	/*
	 * The arranged relations, built INSIDE the scope (arrange-inside-scope
	 * in place of enter_at on an outer arrangement; see the file header):
	 *   forward_key_alt  = arrange_by_key(forward)        src -> dst
	 *   forward_key_neu  = arrange_by_key(forward_neu)    src -> dst (neu)
	 *   reverse_key_alt  = arrange_by_key(reverse)        dst -> src
	 *   forward_self_neu = arrange_by_self(forward_neu)   (a,c) edge (neu)
	 *   reverse_self_alt = arrange_by_self(reverse)       (c,b) edge
	 */
	dtfl_arranged *forward_key_alt = dtfl_collection_arrange_by_key(forward,
	    u32, u32, "forward_key_alt");
	dtfl_arranged *forward_key_neu = dtfl_collection_arrange_by_key(
	    forward_neu, u32, u32, "forward_key_neu");
	dtfl_arranged *reverse_key_alt = dtfl_collection_arrange_by_key(reverse,
	    u32, u32, "reverse_key_alt");
	dtfl_arranged *forward_self_neu = dtfl_collection_arrange(forward_neu,
	    "forward_self_neu");
	dtfl_arranged *reverse_self_alt = dtfl_collection_arrange(reverse,
	    "reverse_self_alt");

	/* The delta-stream sides arranged by their join key. */
	dtfl_arranged *d_rev_key = dtfl_collection_arrange_by_key(d_rev, u32,
	    u32, "d_rev_key");	/* (b,a) keyed by b, for changes1 */
	dtfl_arranged *d_fwd_key = dtfl_collection_arrange_by_key(d_edges, u32,
	    u32, "d_fwd_key");	/* (b,c)/(a,c) keyed by first, for 2 and 3 */

	dtfl_collection *c1a, *c2a, *c3a;
	dtfl_arranged *c1a_arr, *c2a_arr, *c3a_arr;
	dtfl_collection *ch1, *ch2, *ch3, *cat12, *cat;

	(void)unit;

	/* dQ/dE1 := dE1(a,b), E2(b,c), E3(a,c): neu, neu. */
	c1a = dtfl_arranged_join_core(d_rev_key, forward_key_neu, acb, join_c1a,
	    DTFL_NOCTX);
	c1a_arr = dtfl_collection_arrange_by_key(c1a, pair, u32, "c1a");
	ch1 = dtfl_arranged_join_core(c1a_arr, forward_self_neu, tri, join_c1b,
	    DTFL_NOCTX);

	/* dQ/dE2 := dE2(b,c), E1(a,b), E3(a,c): alt, neu. */
	c2a = dtfl_arranged_join_core(d_fwd_key, reverse_key_alt, acb, join_c2a,
	    DTFL_NOCTX);
	c2a_arr = dtfl_collection_arrange_by_key(c2a, pair, u32, "c2a");
	ch2 = dtfl_arranged_join_core(c2a_arr, forward_self_neu, tri, join_c2b,
	    DTFL_NOCTX);

	/* dQ/dE3 := dE3(a,c), E1(a,b), E2(b,c): alt, alt. */
	c3a = dtfl_arranged_join_core(d_fwd_key, forward_key_alt, acb, join_c3a,
	    DTFL_NOCTX);
	c3a_arr = dtfl_collection_arrange_by_key(c3a, pair, u32, "c3a");
	ch3 = dtfl_arranged_join_core(c3a_arr, reverse_self_alt, tri, join_c3b,
	    DTFL_NOCTX);

	cat12 = dtfl_collection_concat(ch1, ch2);
	cat = dtfl_collection_concat(cat12, ch3);
	c->result = dtfl_collection_integrate(cat);

	dtfl_collection_free(cat);
	dtfl_collection_free(cat12);
	dtfl_collection_free(ch1);
	dtfl_collection_free(ch2);
	dtfl_collection_free(ch3);
	dtfl_arranged_free(c1a_arr);
	dtfl_arranged_free(c2a_arr);
	dtfl_arranged_free(c3a_arr);
	dtfl_collection_free(c1a);
	dtfl_collection_free(c2a);
	dtfl_collection_free(c3a);
	dtfl_arranged_free(d_fwd_key);
	dtfl_arranged_free(d_rev_key);
	dtfl_arranged_free(reverse_self_alt);
	dtfl_arranged_free(forward_self_neu);
	dtfl_arranged_free(reverse_key_alt);
	dtfl_arranged_free(forward_key_neu);
	dtfl_arranged_free(forward_key_alt);
	dtfl_collection_free(d_rev);
	dtfl_collection_free(d_edges);
	dtfl_collection_free(forward_neu);
	dtfl_collection_free(reverse);
	dtfl_collection_free(reverse_outer);
	dtfl_collection_free(forward);
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
	const dtfl_type *unit = dtfl_type_unit();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	const dtfl_type *tf[3] = { dtfl_type_u32(), dtfl_type_u32(),
	    dtfl_type_u32() };
	const dtfl_type *acb = dtfl_type_tuple(tf, 3);
	const dtfl_type *tri = dtfl_type_tuple(tf, 3);
	build_ctx c;
	tri_ctx tctx;
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	memset(&tctx, 0, sizeof tctx);
	tctx.u32 = u32; tctx.unit = unit; tctx.pair = pair;
	tctx.acb = acb; tctx.tri = tri;
	c.in = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	c.tc = &tctx;
	dtfl_worker_dataflow(w, "delta_query", tc, build_dataflow, &c);

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
	dtfl_type_free(acb);
	dtfl_type_free(tri);
	dtfl_type_free(unit);
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
