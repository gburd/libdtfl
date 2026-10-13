/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_dogs.c: the delta-query triangle join equals a reference
 * nested-loop triangle enumeration.
 *
 * A small random directed graph (at most 8 nodes, each edge present at
 * most once) is fed to the dogsdogsdogs triangle query Q(a,b,c) :=
 * E1(a,b), E2(b,c), E3(a,c), driven through the generic join in an AltNeu
 * scope, over two epochs so the incremental (delta) path is exercised: some
 * edges arrive at time 0 and the rest at time 1.  The reference model
 * enumerates every (a,b,c) with edges (a,b), (b,c) and (a,c) present by the
 * end.  The property asserts the accumulated output multiset equals the
 * reference, and that no triple is ever reported more than once (a wrong
 * AltNeu order would double-count across the delta boundary).
 *
 * It is deliberately small (at most 28 ordered edges over 8 nodes) so each
 * case runs well under a second.
 */
#include "pbt.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dogs.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define NODES 8
#define MAX_OUT 2048

typedef struct triple {
	uint32_t a, b, c;
	int64_t diff;
} triple;

typedef struct collected {
	triple t[MAX_OUT];
	size_t n;
} collected;

/* ------------------------------------------------------------------ */
/* Build machinery, shared with the unit test's triangle dataflow.     */
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

static void
map_reverse(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 4, 4);
	memcpy(buf + 4, rec, 4);
	dtfl_collection_emit_record(out, buf, 8);
}

static void
map_abc_1(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 12);
}

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
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	const unsigned char *p = rec;

	(void)len; (void)time;
	if (c->n >= MAX_OUT)
		return;
	c->t[c->n].a = dtfl_get_u32(p);
	c->t[c->n].b = dtfl_get_u32(p + 4);
	c->t[c->n].c = dtfl_get_u32(p + 8);
	memcpy(&c->t[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

static int64_t
net_total(const collected *c, uint32_t a, uint32_t b, uint32_t cc)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->t[i].a == a && c->t[i].b == b && c->t[i].c == cc)
			s += c->t[i].diff;
	return s;
}

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
	dtfl_collection *ch1, *ch2, *ch3, *m1, *m2, *m3, *cat12, *cat, *left;

	e1a = dtfl_collection_index_extend_using(neu_forward, c->pair,
	    key_second, DTFL_NOCTX);
	e1b = dtfl_collection_index_extend_using(neu_forward, c->pair,
	    key_first, DTFL_NOCTX);
	ext1[0] = e1a; ext1[1] = e1b;
	ch1 = dtfl_collection_extend(forward, ext1, 2, c->tri);
	m1 = dtfl_collection_map(ch1, c->tri, map_abc_1, DTFL_NOCTX);

	e2a = dtfl_collection_index_extend_using(alt_reverse, c->pair,
	    key_first, DTFL_NOCTX);
	e2b = dtfl_collection_index_extend_using(neu_reverse, c->pair,
	    key_second, DTFL_NOCTX);
	ext2[0] = e2a; ext2[1] = e2b;
	ch2 = dtfl_collection_extend(forward, ext2, 2, c->tri);
	m2 = dtfl_collection_map(ch2, c->tri, map_abc_2, DTFL_NOCTX);

	e3a = dtfl_collection_index_extend_using(alt_forward, c->pair,
	    key_first, DTFL_NOCTX);
	e3b = dtfl_collection_index_extend_using(alt_reverse, c->pair,
	    key_second, DTFL_NOCTX);
	ext3[0] = e3a; ext3[1] = e3b;
	ch3 = dtfl_collection_extend(forward, ext3, 2, c->tri);
	m3 = dtfl_collection_map(ch3, c->tri, map_abc_3, DTFL_NOCTX);

	cat12 = dtfl_collection_concat(m1, m2);
	cat = dtfl_collection_concat(cat12, m3);
	left = dtfl_collection_leave(cat);
	c->result = left;

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
	collected *out;
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
	dtfl_scope_scoped(scope, "Triangles", altneu, build_triangles, c->tc);
	dtfl_time_class_free(altneu);
	dtfl_time_class_free(base);

	ins = dtfl_collection_inspect(c->tc->result, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(c->tc->result);
	dtfl_collection_free(edges);
}

typedef struct probe_cond {
	dtfl_probe *probe;
	uint64_t target;
} probe_cond;

static bool
probe_behind(void *vctx)
{
	probe_cond *pc = vctx;

	return dtfl_probe_less_than(pc->probe, &pc->target);
}

/* ------------------------------------------------------------------ */
/* The runtime state torn down by pbt_defer however the case ends.     */
/* ------------------------------------------------------------------ */

typedef struct run_state {
	dtfl_worker *w;
	dtfl_time_class *tc;
	dtfl_diff *r;
	const dtfl_type *pair;
	const dtfl_type *tri;
	dtfl_probe *probe;
	dtfl_input_session *in;
	bool closed;
	collected *out;
} run_state;

static void
run_cleanup(void *arg)
{
	run_state *s = arg;

	if (!s->closed && s->in != NULL)
		dtfl_input_session_close(s->in);
	if (s->probe != NULL)
		dtfl_probe_free(s->probe);
	if (s->pair != NULL)
		dtfl_type_free(s->pair);
	if (s->tri != NULL)
		dtfl_type_free(s->tri);
	if (s->tc != NULL)
		dtfl_time_class_free(s->tc);
	if (s->r != NULL)
		dtfl_diff_free(s->r);
	if (s->w != NULL)
		dtfl_worker_free(s->w);
	free(s->out);
	free(s);
}

/* ------------------------------------------------------------------ */
/* The property.                                                       */
/* ------------------------------------------------------------------ */

static void
prop_triangle_model(pbt_tc *tc)
{
	run_state *s = calloc(1, sizeof *s);
	collected *out = calloc(1, sizeof *out);
	build_ctx c;
	tri_ctx tctx;
	/* edges[x][y]: 0 absent, 1 at epoch 0, 2 at epoch 1. */
	unsigned char present[NODES][NODES];
	const dtfl_type *tf[3];
	uint32_t x, y, a, b, cc;
	probe_cond pc;
	uint64_t next;

	s->out = out;
	pbt_defer(tc, run_cleanup, s);

	s->w = dtfl_worker_new_standalone();
	s->tc = dtfl_time_u64();
	s->r = dtfl_diff_i64();
	s->pair = dtfl_type_pair(dtfl_type_u32(), dtfl_type_u32());
	tf[0] = dtfl_type_u32(); tf[1] = dtfl_type_u32(); tf[2] = dtfl_type_u32();
	s->tri = dtfl_type_tuple(tf, 3);
	s->probe = dtfl_probe_new();
	s->in = dtfl_input_session_new(s->pair, s->r);

	memset(present, 0, sizeof present);

	/* Draw a sparse random graph (no self loops), split across 2 epochs. */
	for (x = 0; x < NODES; x++)
		for (y = 0; y < NODES; y++) {
			if (x == y)
				continue;
			/* 1 in 4 edges present, to keep triangles plentiful but
			 * the graph small. */
			if (pbt_int(tc, 0, 3) == 0)
				present[x][y] = (unsigned char)(pbt_bool(tc)
				    ? 2 : 1);
		}

	c.in = s->in;
	c.probe = s->probe;
	c.out = out;
	c.tc = &tctx;
	memset(&tctx, 0, sizeof tctx);
	tctx.pair = s->pair;
	tctx.tri = s->tri;
	tctx.u32 = dtfl_type_u32();
	PBT_ASSERT(dtfl_worker_dataflow(s->w, "triangles", s->tc,
	    build_dataflow, &c) == 0);

	/* Epoch 0: insert the epoch-0 edges. */
	for (x = 0; x < NODES; x++)
		for (y = 0; y < NODES; y++)
			if (present[x][y] == 1) {
				unsigned char rec[8];

				dtfl_put_u32(rec, x);
				dtfl_put_u32(rec + 4, y);
				dtfl_input_session_insert(s->in, rec, 8);
			}
	next = 1;
	dtfl_input_session_advance_to(s->in, &next);
	dtfl_input_session_flush(s->in);
	pc.probe = s->probe;
	pc.target = next;
	dtfl_worker_step_while(s->w, probe_behind, &pc);

	/* Epoch 1: insert the epoch-1 edges (the delta path). */
	for (x = 0; x < NODES; x++)
		for (y = 0; y < NODES; y++)
			if (present[x][y] == 2) {
				unsigned char rec[8];

				dtfl_put_u32(rec, x);
				dtfl_put_u32(rec + 4, y);
				dtfl_input_session_insert(s->in, rec, 8);
			}
	next = 2;
	dtfl_input_session_advance_to(s->in, &next);
	dtfl_input_session_flush(s->in);
	pc.target = next;
	dtfl_worker_step_while(s->w, probe_behind, &pc);

	dtfl_input_session_close(s->in);
	s->closed = true;
	while (dtfl_worker_step(s->w))
		;

	/*
	 * Reference: enumerate every (a,b,c) with edges (a,b), (b,c), (a,c)
	 * all present by the end.  The accumulated net multiplicity of each
	 * such triple must be exactly 1, and every other triple exactly 0.
	 */
	for (a = 0; a < NODES; a++)
		for (b = 0; b < NODES; b++)
			for (cc = 0; cc < NODES; cc++) {
				int ref = (present[a][b] && present[b][cc] &&
				    present[a][cc]) ? 1 : 0;

				PBT_ASSERT(net_total(out, a, b, cc) == ref);
			}

	/*
	 * No spurious triple outside the reference: every collected triple
	 * that nets non-zero is one the reference also found (checked above,
	 * since the triple loop covers all nodes).  Guard the output count so
	 * a double-count that overflows MAX_OUT does not silently pass.
	 */
	PBT_ASSERT(out->n < MAX_OUT);
}

static const pbt_prop props[] = {
	{ "triangle_model", prop_triangle_model, 100 },
};

PBT_MAIN("dogs", props)
