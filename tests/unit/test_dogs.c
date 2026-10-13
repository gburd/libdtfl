/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_dogs.c: delta-query triangle counting, static and
 * incremental.
 *
 * A directed triangle query Q(a,b,c) := E1(a,b), E2(b,c), E3(a,c) driven
 * through the dogsdogsdogs generic join (extend -> propose/count/validate
 * -> lookup_map) in an AltNeu scope.  The static case loads a fixed graph
 * holding exactly one triangle plus a dangling edge and asserts the query
 * finds exactly that triangle.  The incremental case then adds one edge
 * that completes a second triangle and asserts the output delta is exactly
 * the new triangle, which exercises the AltNeu half-join timing: if the
 * timing were wrong the increment would double-count or miss the triangle.
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

#define MAX_OUT 512

typedef struct triple {
	uint32_t a, b, c;
	uint64_t time;
	int64_t diff;
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
/* map closures.                                                       */
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

/* ------------------------------------------------------------------ */
/* inspect sink: collect every (a,b,c) triple with time and diff.      */
/* ------------------------------------------------------------------ */

static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	const unsigned char *p = rec;
	uint64_t t;

	(void)len;
	if (c->n >= MAX_OUT)
		return;
	memcpy(&t, time, sizeof t);
	c->t[c->n].a = dtfl_get_u32(p);
	c->t[c->n].b = dtfl_get_u32(p + 4);
	c->t[c->n].c = dtfl_get_u32(p + 8);
	c->t[c->n].time = t;
	memcpy(&c->t[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* Net multiplicity of triple (a,b,c) at `time`, summed over the output. */
static int64_t
net_at(const collected *c, uint32_t a, uint32_t b, uint32_t cc, uint64_t time)
{
	size_t i;
	int64_t sum = 0;

	for (i = 0; i < c->n; i++)
		if (c->t[i].a == a && c->t[i].b == b && c->t[i].c == cc &&
		    c->t[i].time == time)
			sum += c->t[i].diff;
	return sum;
}

/* Net multiplicity of triple (a,b,c) summed over all epochs (accumulated). */
static int64_t
net_total(const collected *c, uint32_t a, uint32_t b, uint32_t cc)
{
	size_t i;
	int64_t sum = 0;

	for (i = 0; i < c->n; i++)
		if (c->t[i].a == a && c->t[i].b == b && c->t[i].c == cc)
			sum += c->t[i].diff;
	return sum;
}

/* ------------------------------------------------------------------ */
/* The scoped triangle build (same shape as the regression test).      */
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
edge(dtfl_input_session *in, uint32_t x, uint32_t y, int insert)
{
	unsigned char rec[8];

	dtfl_put_u32(rec, x);
	dtfl_put_u32(rec + 4, y);
	if (insert)
		dtfl_input_session_insert(in, rec, 8);
	else
		dtfl_input_session_remove(in, rec, 8);
}

static void
run_to(dtfl_worker *w, build_ctx *c, uint64_t next)
{
	probe_cond pc;

	dtfl_input_session_advance_to(c->in, &next);
	dtfl_input_session_flush(c->in);
	pc.probe = c->probe;
	pc.target = next;
	dtfl_worker_step_while(w, probe_behind, &pc);
}

/* ------------------------------------------------------------------ */
/* Tests.                                                              */
/* ------------------------------------------------------------------ */

/* A fixed graph with one triangle (0,1,2) plus a dangling edge 0->3. */
DTFL_TEST(dogs__triangle_static)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	const dtfl_type *tf[3] = { dtfl_type_u32(), dtfl_type_u32(),
	    dtfl_type_u32() };
	const dtfl_type *tri = dtfl_type_tuple(tf, 3);
	build_ctx c;
	tri_ctx tctx;
	collected out;
	size_t i, found = 0;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	memset(&tctx, 0, sizeof tctx);
	tctx.pair = pair; tctx.tri = tri; tctx.u32 = u32;
	c.in = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.tc = &tctx;
	assert_int(dtfl_worker_dataflow(w, "triangles", tc, build_dataflow, &c),
	    ==, 0);

	edge(c.in, 0, 1, 1);
	edge(c.in, 1, 2, 1);
	edge(c.in, 0, 2, 1);
	edge(c.in, 0, 3, 1);	/* dangling: forms no triangle */
	run_to(w, &c, 1);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* Exactly the triangle (0,1,2), net +1, and nothing else accumulated. */
	assert_int64(net_total(&out, 0, 1, 2), ==, 1);
	for (i = 0; i < out.n; i++) {
		int64_t net = net_total(&out, out.t[i].a, out.t[i].b,
		    out.t[i].c);

		if (net != 0) {
			found++;
			assert_uint32(out.t[i].a, ==, 0);
			assert_uint32(out.t[i].b, ==, 1);
			assert_uint32(out.t[i].c, ==, 2);
		}
	}
	assert_size(found, >=, 1);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_type_free(tri);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/*
 * Incremental: epoch 0 loads {0->1, 1->2, 0->2, 0->3} (triangle (0,1,2)).
 * Epoch 1 adds 1->3, which completes a second triangle (0,1,3) via the
 * existing 0->1 and 0->3.  The output delta at time 1 must be exactly the
 * new triangle: a positive multiplicity for (0,1,3) and no spurious change
 * to (0,1,2).  A wrong AltNeu order would double-count or miss it.
 */
DTFL_TEST(dogs__triangle_incremental)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *u32 = dtfl_type_u32();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u32(),
	    dtfl_type_u32());
	const dtfl_type *tf[3] = { dtfl_type_u32(), dtfl_type_u32(),
	    dtfl_type_u32() };
	const dtfl_type *tri = dtfl_type_tuple(tf, 3);
	build_ctx c;
	tri_ctx tctx;
	collected out;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	memset(&tctx, 0, sizeof tctx);
	tctx.pair = pair; tctx.tri = tri; tctx.u32 = u32;
	c.in = dtfl_input_session_new(pair, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.tc = &tctx;
	assert_int(dtfl_worker_dataflow(w, "triangles", tc, build_dataflow, &c),
	    ==, 0);

	/* Epoch 0: one triangle (0,1,2) plus a dangling edge 0->3. */
	edge(c.in, 0, 1, 1);
	edge(c.in, 1, 2, 1);
	edge(c.in, 0, 2, 1);
	edge(c.in, 0, 3, 1);
	run_to(w, &c, 1);

	/* Epoch 1: add 1->3, completing triangle (0,1,3). */
	edge(c.in, 1, 3, 1);
	run_to(w, &c, 2);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* At time 0, the first triangle appears (net +1). */
	assert_int64(net_at(&out, 0, 1, 2, 0), ==, 1);
	/* At time 1, exactly the new triangle appears (net +1). */
	assert_int64(net_at(&out, 0, 1, 3, 1), ==, 1);
	/* The first triangle is not disturbed at time 1 (no double-count). */
	assert_int64(net_at(&out, 0, 1, 2, 1), ==, 0);
	/* Accumulated totals: both triangles present once, nothing else. */
	assert_int64(net_total(&out, 0, 1, 2), ==, 1);
	assert_int64(net_total(&out, 0, 1, 3), ==, 1);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_type_free(tri);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(dogs__triangle_static),
	DTFL_T(dogs__triangle_incremental),
	DTFL_T_END
};

DTFL_TEST_MAIN("dogs", tests)
