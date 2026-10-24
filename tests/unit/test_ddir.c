/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ddir.c: the DDIR core (ir, parse, lower, backend).
 *
 * The IR layer: the Value data model's structural ordering and the Term
 * interpreter (dd_eval).  Later layers add parse/lower/build/evaluate.
 */
#include "dtfl_test.h"

#include "dtfl/ddir.h"
#include "ddir/ddir_int.h"
#include "ddir/parse_int.h"
#include "base/base_int.h"
#include "progress/time_int.h"
#include "dtfl/time.h"

#include <string.h>

/* Evaluate a term against an environment of owned values. */
static dd_value *
eval_env(const dd_term *t, dd_value **env0, size_t n)
{
	dd_value **env = NULL;
	size_t len = 0, cap = 0, i;
	dd_value *r;

	for (i = 0; i < n; i++) {
		if (len == cap) {
			cap = cap ? cap * 2 : 8;
			env = __dtfl_realloc(env, cap * sizeof(dd_value *));
		}
		env[len++] = env0[i];
	}
	r = dd_eval(t, &env, &len, &cap);
	__dtfl_free(env);
	return r;
}

/* ------------------------------------------------------------------ */
/* Value ordering                                                      */
/* ------------------------------------------------------------------ */

DTFL_TEST(ddir__value_cmp)
{
	dd_value *i1 = dd_value_int(1), *i2 = dd_value_int(2);
	dd_value *t = dd_value_unit();
	dd_value *v = dd_value_variant(0, dd_value_int(0));

	(void)params; (void)data;
	/* Int < Int by value. */
	assert_int(dd_value_cmp(i1, i2), <, 0);
	assert_int(dd_value_cmp(i2, i1), >, 0);
	assert_int(dd_value_cmp(i1, i1), ==, 0);
	/* Discriminant order: Int < Tuple < Variant < List. */
	assert_int(dd_value_cmp(i1, t), <, 0);
	assert_int(dd_value_cmp(t, v), <, 0);
	dd_value_free(i1);
	dd_value_free(i2);
	dd_value_free(t);
	dd_value_free(v);
	return MUNIT_OK;
}

DTFL_TEST(ddir__value_clone_roundtrip)
{
	dd_value **items = __dtfl_alloc(2 * sizeof(dd_value *));
	dd_value *tup, *dup;

	(void)params; (void)data;
	items[0] = dd_value_int(7);
	items[1] = dd_value_variant(3, dd_value_int(-1));
	tup = dd_value_tuple(items, 2);
	dup = dd_value_clone(tup);
	assert_int(dd_value_cmp(tup, dup), ==, 0);
	dd_value_free(tup);
	dd_value_free(dup);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Term interpreter                                                    */
/* ------------------------------------------------------------------ */

/* $0 + $1 over env {3, 4} = 7. */
DTFL_TEST(ddir__eval_arith)
{
	dd_term *v0 = dd_term_new(DD_T_VAR);
	dd_term *v1 = dd_term_new(DD_T_VAR);
	dd_term *add = dd_term_new(DD_T_BINARY);
	dd_value *env[2] = { dd_value_int(3), dd_value_int(4) };
	dd_value *r;

	(void)params; (void)data;
	v0->u.var = 0;
	v1->u.var = 1;
	add->u.binary.op = DD_BIN_ADD;
	add->u.binary.l = v0;
	add->u.binary.r = v1;
	r = eval_env(add, env, 2);
	assert_int64(dd_value_as_int(r), ==, 7);
	dd_value_free(r);
	dd_value_free(env[0]);
	dd_value_free(env[1]);
	dd_term_free(add);
	return MUNIT_OK;
}

/* if(0, 1, 2) short-circuits to the else branch. */
DTFL_TEST(ddir__eval_if)
{
	dd_term *cond = dd_term_new(DD_T_INT);
	dd_term *then_ = dd_term_new(DD_T_INT);
	dd_term *els = dd_term_new(DD_T_INT);
	dd_term *iff = dd_term_new(DD_T_IF);
	dd_value *r;

	(void)params; (void)data;
	cond->u.int_lit = 0;
	then_->u.int_lit = 1;
	els->u.int_lit = 2;
	iff->u.if_.cond = cond;
	iff->u.if_.then_ = then_;
	iff->u.if_.els = els;
	r = eval_env(iff, NULL, 0);
	assert_int64(dd_value_as_int(r), ==, 2);
	dd_value_free(r);
	dd_term_free(iff);
	return MUNIT_OK;
}

/* proj($0, 1) over env {tuple(10, 20)} = 20. */
DTFL_TEST(ddir__eval_proj)
{
	dd_value **items = __dtfl_alloc(2 * sizeof(dd_value *));
	dd_value *env[1];
	dd_term *base = dd_term_new(DD_T_VAR);
	dd_term *proj = dd_term_new(DD_T_PROJ);
	dd_value *r;

	(void)params; (void)data;
	items[0] = dd_value_int(10);
	items[1] = dd_value_int(20);
	env[0] = dd_value_tuple(items, 2);
	base->u.var = 0;
	proj->u.proj.base = base;
	proj->u.proj.index = 1;
	r = eval_env(proj, env, 1);
	assert_int64(dd_value_as_int(r), ==, 20);
	dd_value_free(r);
	dd_value_free(env[0]);
	dd_term_free(proj);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

/* A small pipe program parses to the expected statement shape. */
DTFL_TEST(ddir__parse_pipe_basic)
{
	const char *src =
	    "let edges = input 0 | key($0[0] ; $0[1]);\n"
	    "export \"result\" = edges;\n";
	dd_vec stmts = { 0 };
	dd_parse_ctx pc;
	dd_stmt *s0, *s1;

	(void)params; (void)data;
	assert_true(dd_parse_pipe(src, &stmts, &pc));
	assert_size(stmts.len, ==, 2);
	s0 = stmts.data[0];
	s1 = stmts.data[1];
	assert_int(s0->kind, ==, DD_S_LET);
	assert_string_equal(s0->name, "edges");
	/* edges = Map(Input 0, proj) */
	assert_int(s0->expr->kind, ==, DD_E_MAP);
	assert_int(s0->expr->u.map.e->kind, ==, DD_E_INPUT);
	assert_int(s1->kind, ==, DD_S_EXPORT);
	assert_string_equal(s1->name, "result");
	__dtfl_free(stmts.data);
	dd_arena_free_all(&pc);
	return MUNIT_OK;
}

/* The same program in applicative syntax parses to the same shape. */
DTFL_TEST(ddir__parse_applicative_basic)
{
	const char *src =
	    "let edges = MAP(INPUT 0, ($0[0] ; $0[1]));\n"
	    "export \"result\" = JOIN(edges, edges, ($1 ;));\n";
	dd_vec stmts = { 0 };
	dd_parse_ctx pc;
	dd_stmt *s1;

	(void)params; (void)data;
	assert_true(dd_parse_applicative(src, &stmts, &pc));
	assert_size(stmts.len, ==, 2);
	s1 = stmts.data[1];
	assert_int(s1->kind, ==, DD_S_EXPORT);
	assert_int(s1->expr->kind, ==, DD_E_JOIN);
	__dtfl_free(stmts.data);
	dd_arena_free_all(&pc);
	return MUNIT_OK;
}

/* A malformed program returns an error, not a crash, with a message. */
DTFL_TEST(ddir__parse_error_no_crash)
{
	dd_vec stmts = { 0 };
	dd_parse_ctx pc;

	(void)params; (void)data;
	/* missing semicolon / RHS */
	assert_false(dd_parse_pipe("let x = ;", &stmts, &pc));
	assert_true(pc.error);
	assert_size(strlen(pc.msg), >, 0);
	/* unknown builtin in a term */
	assert_false(dd_parse_pipe("let x = input 0 | filter(bogus(1));",
	    &stmts, &pc));
	assert_true(pc.error);
	/* unterminated string */
	assert_false(dd_parse_pipe("export \"oops = input 0;", &stmts, &pc));
	assert_true(pc.error);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Build + evaluate                                                    */
/* ------------------------------------------------------------------ */

/* The int in a single-element tuple value (the projection wraps each field
 * in a tuple: key(k ; v) builds key = tuple(k), val = tuple(v)). */
static int64_t
tup1(const dtfl_ddir_value *v)
{
	if (v->kind == DD_V_TUPLE && v->u.seq.len == 1 &&
	    v->u.seq.items[0]->kind == DD_V_INT)
		return v->u.seq.items[0]->u.int_v;
	return INT64_MIN;
}

/* Find the net diff for a (key_int, val_int) row, keys/vals being the
 * single-element tuples the projection produces. */
static int64_t
find_row(const dtfl_ddir_output *o, int64_t key, int64_t val)
{
	size_t i;

	for (i = 0; i < o->n; i++) {
		const dtfl_ddir_row *r = &o->rows[i];

		if (tup1(r->key) == key && tup1(r->val) == val)
			return r->diff;
	}
	return 0;
}

/* An input row whose key is tuple(a, b) and value is unit (the DDIR input
 * convention: a record's fields live in the key tuple). */
static dtfl_ddir_row
row_edge(int64_t a, int64_t b)
{
	dtfl_ddir_row r;
	dtfl_ddir_value **f = __dtfl_alloc(2 * sizeof(dtfl_ddir_value *));

	f[0] = dtfl_ddir_value_int(a);
	f[1] = dtfl_ddir_value_int(b);
	r.key = dtfl_ddir_value_tuple(f, 2);
	__dtfl_free(f);
	r.val = dtfl_ddir_value_unit();
	r.diff = 1;
	return r;
}

static void
free_rows(dtfl_ddir_row *rows, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_ddir_value_free(rows[i].key);
		dtfl_ddir_value_free(rows[i].val);
	}
}

/*
 * A join of one input against itself: edges keyed (src -> dst); joined on
 * dst == src' to produce two-hop paths (src -> dst').  With edges
 * {0->1, 1->2, 2->3} the two-hop set is {0->2, 1->3}.
 */
DTFL_TEST(ddir__build_join)
{
	const char *src =
	    "let fwd = input 0 | key($0[0] ; $0[1]);\n"
	    "let bwd = input 0 | key($0[1] ; $0[0]);\n"
	    "export \"two_hop\" = bwd | join(fwd, ($2 ; $1));\n";
	dtfl_ddir_program *p = NULL;
	dtfl_ddir_row in[3];
	const dtfl_ddir_row *inputs[1];
	size_t input_lens[1];
	dtfl_ddir_output *out = NULL;
	size_t n_out = 0;

	(void)params; (void)data;
	assert_int(dtfl_ddir_parse(src, DTFL_DDIR_PIPE, &p), ==, 0);
	assert_size(dtfl_ddir_program_input_count(p), ==, 1);
	assert_size(dtfl_ddir_program_export_count(p), ==, 1);
	/* edges keyed by dst (bwd) joined with edges keyed by src (fwd):
	 * key = dst of first = src of second; emit (first_src, second_dst). */
	in[0] = row_edge(0, 1);
	in[1] = row_edge(1, 2);
	in[2] = row_edge(2, 3);
	inputs[0] = in;
	input_lens[0] = 3;
	assert_int(dtfl_ddir_evaluate(p, inputs, input_lens, 1, &out, &n_out),
	    ==, 0);
	assert_size(n_out, ==, 1);
	assert_string_equal(out[0].name, "two_hop");
	/* projection ($2 ; $1): key = right val (fwd dst), val = left val
	 * (bwd val = original src).  For 1->2 (bwd key 2? no): walk it:
	 * bwd rows: (1,0),(2,1),(3,2); fwd rows: (0,1),(1,2),(2,3).
	 * join on key: key 1 matches bwd(1,0) and fwd(1,2) -> ($2=2 ; $1=0).
	 * key 2 matches bwd(2,1) and fwd(2,3) -> (3 ; 1).  So {2->0, 3->1}. */
	assert_int64(find_row(&out[0], 2, 0), ==, 1);
	assert_int64(find_row(&out[0], 3, 1), ==, 1);
	assert_size(out[0].n, ==, 2);

	free_rows(in, 3);
	dtfl_ddir_outputs_free(out, n_out);
	dtfl_ddir_program_free(p);
	return MUNIT_OK;
}

/*
 * Transitive closure (reachability) via a feedback var: reach = roots join
 * step over edges, until a fixed point.  Graph 0->1,1->2,2->3,3->1 and 4->5;
 * reachable from {0} is {0,1,2,3}.  The lowered recursive plan must
 * converge (a non-terminating loop is a bug caught by the step bound).
 */
DTFL_TEST(ddir__build_transitive_closure)
{
	const char *src =
	    "let edges = input 0 | key($0[0] ; $0[1]);\n"
	    "let roots = input 1 | key($0[0] ;);\n"
	    "reach: {\n"
	    "    let step = reach | join(edges, ($2 ;));\n"
	    "    var reach = roots + step | distinct;\n"
	    "}\n"
	    "export \"reach\" = reach::reach;\n";
	dtfl_ddir_program *p = NULL;
	dtfl_ddir_row edges[5], roots[1];
	const dtfl_ddir_row *inputs[2];
	size_t input_lens[2];
	dtfl_ddir_output *out = NULL;
	size_t n_out = 0;

	(void)params; (void)data;
	assert_int(dtfl_ddir_parse(src, DTFL_DDIR_PIPE, &p), ==, 0);
	assert_size(dtfl_ddir_program_input_count(p), ==, 2);
	edges[0] = row_edge(0, 1);
	edges[1] = row_edge(1, 2);
	edges[2] = row_edge(2, 3);
	edges[3] = row_edge(3, 1);
	edges[4] = row_edge(4, 5);
	/* roots keyed by node, value unit; input is (node, _) with key($0[0];) */
	roots[0] = row_edge(0, 0);
	inputs[0] = edges;
	inputs[1] = roots;
	input_lens[0] = 5;
	input_lens[1] = 1;
	assert_int(dtfl_ddir_evaluate(p, inputs, input_lens, 2, &out, &n_out),
	    ==, 0);
	assert_size(n_out, ==, 1);
	/* reach is keyed by node, value unit (empty tuple).  Reachable from 0:
	 * {0,1,2,3}; 4 and 5 not reachable. */
	{
		size_t i;
		int64_t seen[6] = { 0, 0, 0, 0, 0, 0 };

		for (i = 0; i < out[0].n; i++) {
			const dtfl_ddir_row *r = &out[0].rows[i];
			int64_t node = tup1(r->key);

			if (node >= 0 && node < 6)
				seen[node] += r->diff;
		}
		assert_int64(seen[0], ==, 1);
		assert_int64(seen[1], ==, 1);
		assert_int64(seen[2], ==, 1);
		assert_int64(seen[3], ==, 1);
		assert_int64(seen[4], ==, 0);
		assert_int64(seen[5], ==, 0);
	}

	free_rows(edges, 5);
	free_rows(roots, 1);
	dtfl_ddir_outputs_free(out, n_out);
	dtfl_ddir_program_free(p);
	return MUNIT_OK;
}

/*
 * The live DDIR dataflow's time is literally Product<u64, PointStamp<u64>>
 * (ir.rs:9, PLAN D3): a u64 outer and a PointStamp inner.  Inspect the
 * class dtfl_ddir_time_class hands the renderer.
 */
DTFL_TEST(ddir__time_is_product_pointstamp)
{
	dtfl_time_class *tc = dtfl_ddir_time_class();

	(void)params; (void)data;
	assert_not_null(tc);
	/* Product<outer, inner>. */
	assert_int(tc->shape, ==, DTFL_SHAPE_PRODUCT);
	/* outer = u64 (one TOTAL coordinate). */
	assert_int(tc->a->shape, ==, DTFL_SHAPE_TOTAL);
	assert_int(tc->a->depth, ==, 1);
	/* inner = PointStamp<u64>, padded to DD_POINTSTAMP_MAX coordinates. */
	assert_int(tc->b->shape, ==, DTFL_SHAPE_POINTSTAMP);
	assert_int(tc->b->depth, ==, DD_POINTSTAMP_MAX);
	/* The whole time is 1 + DD_POINTSTAMP_MAX words. */
	assert_uint(dtfl_time_depth(tc), ==, 1u + DD_POINTSTAMP_MAX);
	/* The product order is partial, not total. */
	assert_false(dtfl_time_is_total(tc));
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * enter_at places each record at a per-record inner coordinate (the
 * 256 * (64 - leading_zeros(field)) delay of backend/vec.rs render_linear)
 * yet still reaches the same fixed point.  A min-label flood over a small
 * chain, with the seed labels entered at a delay derived from the label,
 * must still settle to every node carrying the component's minimum label.
 * The delay only changes WHEN a proposal enters the loop, not the fixpoint,
 * so a wrong enter_at (missing, or the wrong coordinate) either diverges
 * (caught by the step bound) or settles to the wrong labels.
 */
DTFL_TEST(ddir__build_enter_at_label_flood)
{
	const char *src =
	    "let edges = input 0 | key($0[0] ; $0[1]);\n"
	    "let trans = edges | key($1 ; $0);\n"
	    "let sym = edges + trans;\n"
	    "flood: {\n"
	    "    let nodes = sym | key($0 ; $0) | enter_at($1[0]);\n"
	    "    let labels = proposals + nodes | min;\n"
	    "    var proposals = labels | join(sym, ($2 ; $1));\n"
	    "}\n"
	    "export \"labels\" = flood::labels;\n";
	dtfl_ddir_program *p = NULL;
	dtfl_ddir_row edges[3];
	const dtfl_ddir_row *inputs[1];
	size_t input_lens[1];
	dtfl_ddir_output *out = NULL;
	size_t n_out = 0, i;

	(void)params; (void)data;
	assert_int(dtfl_ddir_parse(src, DTFL_DDIR_PIPE, &p), ==, 0);
	/* A 3-node chain 1-2-3 (undirected via sym): one component, min label
	 * is 1, so every node settles to label 1. */
	edges[0] = row_edge(1, 2);
	edges[1] = row_edge(2, 3);
	edges[2] = row_edge(1, 3);
	inputs[0] = edges;
	input_lens[0] = 3;
	assert_int(dtfl_ddir_evaluate(p, inputs, input_lens, 1, &out, &n_out),
	    ==, 0);
	assert_size(n_out, ==, 1);
	assert_string_equal(out[0].name, "labels");
	/* labels is keyed by node; its value is the min label (= 1 for all).
	 * Three nodes, each converged (net multiplicity 1) to the min label.
	 * The exact fixpoint (every node reaches label 1) confirms enter_at
	 * only reorders loop entry, it does not change the result. */
	assert_size(out[0].n, ==, 3);
	for (i = 0; i < out[0].n; i++) {
		const dtfl_ddir_row *r = &out[0].rows[i];
		int64_t label = tup1(r->val);

		assert_int64(r->diff, ==, 1);
		assert_int64(label, ==, 1); /* the component minimum */
	}
	free_rows(edges, 3);
	dtfl_ddir_outputs_free(out, n_out);
	dtfl_ddir_program_free(p);
	return MUNIT_OK;
}

/* value_parse of a closed term yields the constant value. */
DTFL_TEST(ddir__value_parse)
{
	dtfl_ddir_value *v = dtfl_ddir_value_parse("tuple(1, 2)");
	dtfl_ddir_value **items = __dtfl_alloc(2 * sizeof(dtfl_ddir_value *));
	dtfl_ddir_value *expect;

	(void)params; (void)data;
	assert_not_null(v);
	items[0] = dtfl_ddir_value_int(1);
	items[1] = dtfl_ddir_value_int(2);
	expect = dtfl_ddir_value_tuple(items, 2);
	__dtfl_free(items);
	assert_int(dtfl_ddir_value_cmp(v, expect), ==, 0);
	assert_null(dtfl_ddir_value_parse("tuple(1,")); /* malformed */
	dtfl_ddir_value_free(v);
	dtfl_ddir_value_free(expect);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(ddir__value_cmp),
	DTFL_T(ddir__value_clone_roundtrip),
	DTFL_T(ddir__eval_arith),
	DTFL_T(ddir__eval_if),
	DTFL_T(ddir__eval_proj),
	DTFL_T(ddir__parse_pipe_basic),
	DTFL_T(ddir__parse_applicative_basic),
	DTFL_T(ddir__parse_error_no_crash),
	DTFL_T(ddir__build_join),
	DTFL_T(ddir__build_transitive_closure),
	DTFL_T(ddir__time_is_product_pointstamp),
	DTFL_T(ddir__build_enter_at_label_flood),
	DTFL_T(ddir__value_parse),
	DTFL_T_END
};

DTFL_TEST_MAIN("ddir", tests)
