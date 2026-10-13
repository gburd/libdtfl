/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dogs3-v0.25.1
 *   dogsdogsdogs/tests/lookup_map_regression.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dogs/lookup_map_regression.c: the lookup_map off-by-one
 * regression test.
 *
 * It drives a triangle worst-case-optimal join through the dogsdogsdogs
 * generic join (extend -> propose/count/validate -> lookup_map) over a
 * single in-memory triangle (edges 0->1, 1->2, 0->2) and asserts the query
 * finds exactly the triangle (0, 1, 2).  Only updates present with positive
 * multiplicity are collected, so a retraction or a net-zero update cannot
 * make the test pass.
 */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dogs.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 256

typedef struct triple {
	uint32_t a, b, c;
} triple;

typedef struct collected {
	triple t[MAX_OUT];
	size_t n;
} collected;

/* ------------------------------------------------------------------ */
/* Key selectors on a pair(u32, u32) prefix.                           */
/* ------------------------------------------------------------------ */

static void
key_first(void *ctx, const void *prefix, size_t plen, void *key, size_t cap,
    size_t *klen)
{
	(void)ctx; (void)plen; (void)cap;
	memcpy(key, prefix, sizeof(uint32_t));	/* first u32 */
	*klen = sizeof(uint32_t);
}

static void
key_second(void *ctx, const void *prefix, size_t plen, void *key, size_t cap,
    size_t *klen)
{
	(void)ctx; (void)plen; (void)cap;
	memcpy(key, (const unsigned char *)prefix + sizeof(uint32_t),
	    sizeof(uint32_t));			/* second u32 */
	*klen = sizeof(uint32_t);
}

/* ------------------------------------------------------------------ */
/* map closures.                                                       */
/* ------------------------------------------------------------------ */

/* reverse: (x, y) -> (y, x). */
static void
map_reverse(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 4, 4);
	memcpy(buf + 4, rec, 4);
	dtfl_collection_emit_record(out, buf, 8);
}

/* changes1: ((a,b),c) -> (a,b,c): identity on bytes (a,b,c). */
static void
map_abc_1(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 12);
}

/* changes2: ((b,c),a) -> (a,b,c): bytes are (b,c,a), reorder to (a,b,c). */
static void
map_abc_2(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[12];

	(void)ctx; (void)len;
	memcpy(buf, p + 8, 4);		/* a */
	memcpy(buf + 4, p, 4);		/* b */
	memcpy(buf + 8, p + 4, 4);	/* c */
	dtfl_collection_emit_record(out, buf, 12);
}

/* changes3: ((a,c),b) -> (a,b,c): bytes are (a,c,b), reorder to (a,b,c). */
static void
map_abc_3(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[12];

	(void)ctx; (void)len;
	memcpy(buf, p, 4);		/* a */
	memcpy(buf + 4, p + 8, 4);	/* b */
	memcpy(buf + 8, p + 4, 4);	/* c */
	dtfl_collection_emit_record(out, buf, 12);
}

/* ------------------------------------------------------------------ */
/* inspect sink: collect positive-multiplicity (a,b,c) triples.        */
/* ------------------------------------------------------------------ */

static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	int64_t r;
	const unsigned char *p = rec;

	(void)time; (void)len;
	memcpy(&r, diff, sizeof r);
	if (r <= 0 || c->n >= MAX_OUT)
		return;
	c->t[c->n].a = dtfl_get_u32(p);
	c->t[c->n].b = dtfl_get_u32(p + 4);
	c->t[c->n].c = dtfl_get_u32(p + 8);
	c->n++;
}

/* ------------------------------------------------------------------ */
/* The scoped triangle build.                                          */
/* ------------------------------------------------------------------ */

typedef struct tri_ctx {
	dtfl_collection *edges;		/* the outer forward edges */
	dtfl_collection *result;	/* set by leave */
	const dtfl_type *pair;		/* pair(u32, u32) */
	const dtfl_type *tri;		/* tuple(u32, u32, u32) */
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
	/* neu-delayed copies for relations after the delta stream. */
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

	/* dQ/dE1 := dE1(a,b), E2(b,c), E3(a,c). */
	e1a = dtfl_collection_index_extend_using(neu_forward, c->pair,
	    key_second, DTFL_NOCTX);
	e1b = dtfl_collection_index_extend_using(neu_forward, c->pair,
	    key_first, DTFL_NOCTX);
	ext1[0] = e1a;
	ext1[1] = e1b;
	ch1 = dtfl_collection_extend(forward, ext1, 2, c->tri);
	m1 = dtfl_collection_map(ch1, c->tri, map_abc_1, DTFL_NOCTX);

	/* dQ/dE2 := dE2(b,c), E1(a,b), E3(a,c). */
	e2a = dtfl_collection_index_extend_using(alt_reverse, c->pair,
	    key_first, DTFL_NOCTX);
	e2b = dtfl_collection_index_extend_using(neu_reverse, c->pair,
	    key_second, DTFL_NOCTX);
	ext2[0] = e2a;
	ext2[1] = e2b;
	ch2 = dtfl_collection_extend(forward, ext2, 2, c->tri);
	m2 = dtfl_collection_map(ch2, c->tri, map_abc_2, DTFL_NOCTX);

	/* dQ/dE3 := dE3(a,c), E1(a,b), E2(b,c). */
	e3a = dtfl_collection_index_extend_using(alt_forward, c->pair,
	    key_first, DTFL_NOCTX);
	e3b = dtfl_collection_index_extend_using(alt_reverse, c->pair,
	    key_second, DTFL_NOCTX);
	ext3[0] = e3a;
	ext3[1] = e3b;
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

/* ------------------------------------------------------------------ */
/* The dataflow build.                                                 */
/* ------------------------------------------------------------------ */

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
	dtfl_time_class *altneu = dtfl_time_altneu(dtfl_time_u64());
	dtfl_collection *ins, *probed;

	c->tc->edges = edges;
	c->tc->result = NULL;
	dtfl_scope_scoped(scope, "Triangles", altneu, build_triangles, c->tc);
	dtfl_time_class_free(altneu);

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

static void
insert_edge(dtfl_input_session *in, uint32_t x, uint32_t y)
{
	unsigned char rec[8];

	dtfl_put_u32(rec, x);
	dtfl_put_u32(rec + 4, y);
	dtfl_input_session_insert(in, rec, 8);
}

static int
triple_cmp(const void *va, const void *vb)
{
	const triple *a = va, *b = vb;

	if (a->a != b->a)
		return a->a < b->a ? -1 : 1;
	if (a->b != b->b)
		return a->b < b->b ? -1 : 1;
	if (a->c != b->c)
		return a->c < b->c ? -1 : 1;
	return 0;
}

DTFL_TEST(lookup_map_regression__lookup_map_triangle_wcoj_finds_triangle)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	const dtfl_type *trifields[3] = { dtfl_type_u32(), dtfl_type_u32(),
	    dtfl_type_u32() };
	const dtfl_type *tri = dtfl_type_tuple(trifields, 3);
	build_ctx c;
	tri_ctx tctx;
	collected out;
	uint64_t next;
	probe_cond pc;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	memset(&tctx, 0, sizeof tctx);
	tctx.pair = pair;
	tctx.tri = tri;
	tctx.u32 = u32;
	c.in = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.tc = &tctx;
	assert_int(dtfl_worker_dataflow(w, "triangles", tc, build_dataflow, &c),
	    ==, 0);

	/* A single triangle: directed edges 0->1, 1->2, 0->2. */
	insert_edge(c.in, 0, 1);
	insert_edge(c.in, 1, 2);
	insert_edge(c.in, 0, 2);
	next = 1;
	dtfl_input_session_advance_to(c.in, &next);
	dtfl_input_session_flush(c.in);
	pc.probe = c.probe;
	pc.target = next;
	dtfl_worker_step_while(w, probe_behind, &pc);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* Expect exactly the triangle (0, 1, 2). */
	if (out.n > 1)
		qsort(out.t, out.n, sizeof(triple), triple_cmp);
	assert_size(out.n, >=, 1);
	assert_uint32(out.t[0].a, ==, 0);
	assert_uint32(out.t[0].b, ==, 1);
	assert_uint32(out.t[0].c, ==, 2);
	/* dedup check: all collected triples are the same (0,1,2). */
	{
		size_t i;
		size_t distinct = 1;

		for (i = 1; i < out.n; i++)
			if (triple_cmp(&out.t[i], &out.t[i - 1]) != 0)
				distinct++;
		assert_size(distinct, ==, 1);
	}

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_type_free(tri);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

MunitTest lookup_map_regression_tests[] = {
	DTFL_T(lookup_map_regression__lookup_map_triangle_wcoj_finds_triangle),
	DTFL_T_END
};

DTFL_TEST_MAIN("lookup_map_regression", lookup_map_regression_tests)
