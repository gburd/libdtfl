/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_explain_decouple.c: the reverse-rule by-example contract
 * (explain/decouple.rs value_contract and backstop).
 *
 * The reverse rules are run against an in-memory (key, val) dataflow (the
 * dd_df Mem backend): project/filter apply the Term interpreter (dd_eval),
 * join is a nested-loop equi-join on the key.  Each spec feeds a demand row
 * and a pair table and checks the exact contributed demand, pinning the Val
 * row model and the rules independent of the full explain pass.  The
 * backstop spec reverses flatmap by hand (the pair-table idea the FlatMap
 * rule builds on).
 *
 * The nested_contract specs (decouple.rs's second, test-only RowModel over
 * a differently nested value) are excluded: libdtfl ports the one Val model
 * the backend runs, so the trait's model-agnosticism has no second model to
 * demonstrate here.  See dev/test-ledger.tsv.
 */
#include "dtfl_test.h"

#include "dtfl/ddir.h"
#include "ddir/ddir_int.h"
#include "ddir/explain_int.h"
#include "base/base_int.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* The in-memory (key, val) dataflow backend                           */
/* ------------------------------------------------------------------ */

typedef struct row {
	dd_value *k, *v;
} row;

typedef struct coll {
	row *data;
	size_t len, cap;
} coll;

static coll *
coll_new(void)
{
	return __dtfl_calloc(1, sizeof(coll));
}

static void
coll_push(coll *c, dd_value *k, dd_value *v)
{
	if (c->len == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 8;
		c->data = __dtfl_realloc(c->data, c->cap * sizeof(row));
	}
	c->data[c->len].k = k;
	c->data[c->len].v = v;
	c->len++;
}

static void
coll_free(coll *c)
{
	size_t i;

	for (i = 0; i < c->len; i++) {
		dd_value_free(c->data[i].k);
		dd_value_free(c->data[i].v);
	}
	__dtfl_free(c->data);
	__dtfl_free(c);
}

/* Evaluate a term against an environment of borrowed values. */
static dd_value *
eval_env(const dd_term *t, dd_value **env0, size_t n)
{
	dd_value **env = __dtfl_alloc((n ? n : 1) * sizeof(dd_value *));
	size_t len = n, cap = n ? n : 1, i;
	dd_value *r;

	for (i = 0; i < n; i++)
		env[i] = env0[i];
	r = dd_eval(t, &env, &len, &cap);
	__dtfl_free(env);
	return r;
}

/* All handles the Mem backend hands out are tracked so the test frees them. */
typedef struct mem {
	coll **tracked;
	size_t n, cap;
} mem;

static coll *
mem_track(mem *m, coll *c)
{
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 16;
		m->tracked = __dtfl_realloc(m->tracked, m->cap * sizeof(coll *));
	}
	m->tracked[m->n++] = c;
	return c;
}

static void *
mem_project(void *ctx, void *ch, dd_projection p)
{
	mem *m = ctx;
	coll *c = ch;
	coll *out = coll_new();
	size_t i;

	for (i = 0; i < c->len; i++) {
		dd_value *env[2] = { c->data[i].k, c->data[i].v };

		coll_push(out, eval_env(p.key, env, 2), eval_env(p.val, env, 2));
	}
	dd_projection_free_fields(&p);
	return mem_track(m, out);
}

static void *
mem_filter(void *ctx, void *ch, dd_term *cond)
{
	mem *m = ctx;
	coll *c = ch;
	coll *out = coll_new();
	size_t i;

	for (i = 0; i < c->len; i++) {
		dd_value *env[2] = { c->data[i].k, c->data[i].v };
		dd_value *r = eval_env(cond, env, 2);

		if (dd_value_truthy(r))
			coll_push(out, dd_value_clone(c->data[i].k),
			    dd_value_clone(c->data[i].v));
		dd_value_free(r);
	}
	dd_term_free(cond);
	return mem_track(m, out);
}

static void *
mem_join(void *ctx, void *lh, void *rh, dd_projection p)
{
	mem *m = ctx;
	coll *l = lh, *r = rh;
	coll *out = coll_new();
	size_t i, j;

	for (i = 0; i < l->len; i++)
		for (j = 0; j < r->len; j++)
			if (dd_value_cmp(l->data[i].k, r->data[j].k) == 0) {
				dd_value *env[3] = { l->data[i].k,
				    l->data[i].v, r->data[j].v };

				coll_push(out, eval_env(p.key, env, 3),
				    eval_env(p.val, env, 3));
			}
	dd_projection_free_fields(&p);
	return mem_track(m, out);
}

static void *
mem_concat(void *ctx, void **cs, size_t n)
{
	mem *m = ctx;
	coll *out = coll_new();
	size_t i, j;

	for (i = 0; i < n; i++) {
		coll *c = cs[i];

		for (j = 0; j < c->len; j++)
			coll_push(out, dd_value_clone(c->data[j].k),
			    dd_value_clone(c->data[j].v));
	}
	return mem_track(m, out);
}

static dd_df
mem_df(mem *m)
{
	dd_df df;

	df.ctx = m;
	df.project = mem_project;
	df.filter = mem_filter;
	df.join = mem_join;
	df.concat = mem_concat;
	return df;
}

static void
mem_free(mem *m)
{
	size_t i;

	for (i = 0; i < m->n; i++)
		coll_free(m->tracked[i]);
	__dtfl_free(m->tracked);
}

/* ------------------------------------------------------------------ */
/* Row/value helpers                                                   */
/* ------------------------------------------------------------------ */

/* A 1-field key tuple (n). */
static dd_value *
keyv(int64_t n)
{
	dd_value **f = __dtfl_alloc(sizeof(dd_value *));

	f[0] = dd_value_int(n);
	return dd_value_tuple(f, 1);
}

/* A value tuple from a flat field list ([V... | chain... | q]). */
static dd_value *
valv(const int64_t *xs, size_t n)
{
	dd_value **f = n ? __dtfl_alloc(n * sizeof(dd_value *)) : NULL;
	size_t i;

	for (i = 0; i < n; i++)
		f[i] = dd_value_int(xs[i]);
	return dd_value_tuple(f, n);
}

#define VAL(...) ((const int64_t[]){ __VA_ARGS__ })
#define NVAL(...) (sizeof VAL(__VA_ARGS__) / sizeof(int64_t))

/* A side with the given witness collection, empty forward, shape, user_len. */
static dd_side
side_of(coll *witness, coll *empty, size_t k, size_t v, size_t user_len)
{
	dd_side s;

	s.witness = witness;
	s.forward = empty;
	s.shape.k = k;
	s.shape.v = v;
	s.user_len = user_len;
	return s;
}

/* True if `c` has exactly the rows in (keys[i], vals[i]) in some order. */
static bool
coll_matches(coll *c, dd_value **keys, dd_value **vals, size_t n)
{
	bool *used;
	size_t i, j;
	bool ok = true;

	if (c->len != n)
		return false;
	used = n ? __dtfl_calloc(n, sizeof(bool)) : NULL;
	for (i = 0; i < c->len; i++) {
		bool found = false;

		for (j = 0; j < n; j++)
			if (!used[j] &&
			    dd_value_cmp(c->data[i].k, keys[j]) == 0 &&
			    dd_value_cmp(c->data[i].v, vals[j]) == 0) {
				used[j] = true;
				found = true;
				break;
			}
		if (!found) {
			ok = false;
			break;
		}
	}
	__dtfl_free(used);
	return ok;
}

/* ------------------------------------------------------------------ */
/* value_contract specs                                                */
/* ------------------------------------------------------------------ */

/* Packing (K,V) into the join key means only the V=7 pair can match. */
DTFL_TEST(explain_decouple_value_contract__sp_depth0_keeps_only_value_matching_pair)
{
	mem m = { 0 };
	dd_df df = mem_df(&m);
	coll *pair = coll_new(), *empty = coll_new(), *dep = coll_new();
	dd_side side;
	void *out;
	dd_value *ek[1], *ev[1];

	(void)params; (void)data;
	coll_push(pair, keyv(5), valv(VAL(7), NVAL(7)));
	coll_push(pair, keyv(5), valv(VAL(8), NVAL(8)));
	coll_push(dep, keyv(5), valv(VAL(7, 9), NVAL(7, 9))); /* V=7, q=9 */
	mem_track(&m, pair);
	mem_track(&m, empty);
	mem_track(&m, dep);
	side = side_of(pair, empty, 1, 1, 0);
	out = dd_shape_preserving_lookup(&df, dep, &side, 0);
	ek[0] = keyv(5);
	ev[0] = valv(VAL(7, 9), NVAL(7, 9));
	assert_true(coll_matches(out, ek, ev, 1));
	dd_value_free(ek[0]);
	dd_value_free(ev[0]);
	mem_free(&m);
	return MUNIT_OK;
}

/* chain length 1: an input at iter 2 explains an output demanded at iter 3
 * (kept); one at iter 4 does not (dropped). */
DTFL_TEST(explain_decouple_value_contract__sp_depth1_time_filters_late_inputs)
{
	mem m = { 0 };
	dd_df df = mem_df(&m);
	coll *keep = coll_new(), *drop = coll_new(), *empty = coll_new();
	coll *dep1 = coll_new(), *dep2 = coll_new();
	dd_side skeep, sdrop;
	void *kept, *dropped;
	dd_value *ek[1], *ev[1];

	(void)params; (void)data;
	coll_push(keep, keyv(5), valv(VAL(7, 2), NVAL(7, 2)));	/* chain_in 2 */
	coll_push(drop, keyv(5), valv(VAL(7, 4), NVAL(7, 4)));	/* chain_in 4 */
	coll_push(dep1, keyv(5), valv(VAL(7, 3, 9), NVAL(7, 3, 9)));
	coll_push(dep2, keyv(5), valv(VAL(7, 3, 9), NVAL(7, 3, 9)));
	mem_track(&m, keep);
	mem_track(&m, drop);
	mem_track(&m, empty);
	mem_track(&m, dep1);
	mem_track(&m, dep2);
	skeep = side_of(keep, empty, 1, 1, 1);
	sdrop = side_of(drop, empty, 1, 1, 1);
	kept = dd_shape_preserving_lookup(&df, dep1, &skeep, 1);
	ek[0] = keyv(5);
	ev[0] = valv(VAL(7, 2, 9), NVAL(7, 2, 9));
	assert_true(coll_matches(kept, ek, ev, 1));
	dd_value_free(ek[0]);
	dd_value_free(ev[0]);
	dropped = dd_shape_preserving_lookup(&df, dep2, &sdrop, 1);
	assert_size(((coll *)dropped)->len, ==, 0);
	mem_free(&m);
	return MUNIT_OK;
}

/* Min narrows to the demanded value. */
DTFL_TEST(explain_decouple_value_contract__keyed_min_narrows_to_the_demanded_value)
{
	mem m = { 0 };
	dd_df df = mem_df(&m);
	coll *pair = coll_new(), *empty = coll_new(), *dep = coll_new();
	dd_side side;
	dd_shape os = { 1, 1 };
	void *out;
	dd_value *ek[1], *ev[1];

	(void)params; (void)data;
	coll_push(pair, keyv(5), valv(VAL(7), NVAL(7)));
	coll_push(pair, keyv(5), valv(VAL(6), NVAL(6)));
	coll_push(dep, keyv(5), valv(VAL(7, 9), NVAL(7, 9)));
	mem_track(&m, pair);
	mem_track(&m, empty);
	mem_track(&m, dep);
	side = side_of(pair, empty, 1, 1, 0);
	out = dd_keyed_lookup(&df, dep, &side, os, 0, true);
	ek[0] = keyv(5);
	ev[0] = valv(VAL(7, 9), NVAL(7, 9));
	assert_true(coll_matches(out, ek, ev, 1));
	dd_value_free(ek[0]);
	dd_value_free(ev[0]);
	mem_free(&m);
	return MUNIT_OK;
}

/* Non-min (count) demands all same-key inputs. */
DTFL_TEST(explain_decouple_value_contract__keyed_nonmin_demands_all_same_key_inputs)
{
	mem m = { 0 };
	dd_df df = mem_df(&m);
	coll *pair = coll_new(), *empty = coll_new(), *dep = coll_new();
	dd_side side;
	dd_shape os = { 1, 1 };
	void *out;
	dd_value *ek[2], *ev[2];

	(void)params; (void)data;
	coll_push(pair, keyv(5), valv(VAL(7), NVAL(7)));
	coll_push(pair, keyv(5), valv(VAL(6), NVAL(6)));
	coll_push(dep, keyv(5), valv(VAL(1, 9), NVAL(1, 9))); /* V_out a count */
	mem_track(&m, pair);
	mem_track(&m, empty);
	mem_track(&m, dep);
	side = side_of(pair, empty, 1, 1, 0);
	out = dd_keyed_lookup(&df, dep, &side, os, 0, false);
	ek[0] = keyv(5);
	ev[0] = valv(VAL(6, 9), NVAL(6, 9));
	ek[1] = keyv(5);
	ev[1] = valv(VAL(7, 9), NVAL(7, 9));
	assert_true(coll_matches(out, ek, ev, 2));
	dd_value_free(ek[0]);
	dd_value_free(ev[0]);
	dd_value_free(ek[1]);
	dd_value_free(ev[1]);
	mem_free(&m);
	return MUNIT_OK;
}

/* Lossy via the pair-table fallback recovers the input. proj: K_out = $1
 * (V_in), V_out = $0 (K_in). */
DTFL_TEST(explain_decouple_value_contract__lossy_via_fallback_recovers_input)
{
	mem m = { 0 };
	dd_df df = mem_df(&m);
	coll *pair = coll_new(), *empty = coll_new(), *dep = coll_new();
	dd_side side;
	dd_shape os = { 1, 1 };
	dd_projection p;
	dd_term *sp0, *sp1;
	void *out;
	dd_value *ek[1], *ev[1];

	(void)params; (void)data;
	/* proj.key = tuple(spread($1)); proj.val = tuple(spread($0)). */
	sp0 = dd_term_new(DD_T_SPREAD);
	sp0->u.spread = dd_val_var(1);
	sp1 = dd_term_new(DD_T_SPREAD);
	sp1->u.spread = dd_val_var(0);
	p.key = dd_val_tup((dd_term *[]){ sp0 }, 1);
	p.val = dd_val_tup((dd_term *[]){ sp1 }, 1);
	coll_push(pair, keyv(3), valv(VAL(8), NVAL(8)));	/* (K_in=3; V_in=8) */
	coll_push(dep, keyv(8), valv(VAL(3, 9), NVAL(3, 9)));	/* (K_out=8; V_out=3, q=9) */
	mem_track(&m, pair);
	mem_track(&m, empty);
	mem_track(&m, dep);
	side = side_of(pair, empty, 1, 1, 0);
	out = dd_lossy_lookup(&df, dep, &side, os, 0, &p);
	dd_projection_free_fields(&p);
	ek[0] = keyv(3);
	ev[0] = valv(VAL(8, 9), NVAL(8, 9));
	assert_true(coll_matches(out, ek, ev, 1));
	dd_value_free(ek[0]);
	dd_value_free(ev[0]);
	mem_free(&m);
	return MUNIT_OK;
}

/* Join demands both inputs. proj: K_out = $0 (K), V_out = $1 (V_L). */
DTFL_TEST(explain_decouple_value_contract__join_demands_both_inputs)
{
	mem m = { 0 };
	dd_df df = mem_df(&m);
	coll *lw = coll_new(), *rw = coll_new(), *empty1 = coll_new();
	coll *empty2 = coll_new(), *dep = coll_new();
	dd_side left, right;
	dd_shape os = { 1, 1 };
	dd_projection p;
	dd_term *sp0, *sp1;
	void *lc, *rc;
	dd_value *ek[1], *ev[1];

	(void)params; (void)data;
	sp0 = dd_term_new(DD_T_SPREAD);
	sp0->u.spread = dd_val_var(0);
	sp1 = dd_term_new(DD_T_SPREAD);
	sp1->u.spread = dd_val_var(1);
	p.key = dd_val_tup((dd_term *[]){ sp0 }, 1);
	p.val = dd_val_tup((dd_term *[]){ sp1 }, 1);
	coll_push(lw, keyv(5), valv(VAL(7), NVAL(7)));
	coll_push(rw, keyv(5), valv(VAL(9), NVAL(9)));
	coll_push(dep, keyv(5), valv(VAL(7, 1), NVAL(7, 1))); /* V_out=7, q=1 */
	mem_track(&m, lw);
	mem_track(&m, rw);
	mem_track(&m, empty1);
	mem_track(&m, empty2);
	mem_track(&m, dep);
	left = side_of(lw, empty1, 1, 1, 0);
	right = side_of(rw, empty2, 1, 1, 0);
	dd_join_lookup(&df, dep, &left, &right, os, 0, &p, &lc, &rc);
	dd_projection_free_fields(&p);
	ek[0] = keyv(5);
	ev[0] = valv(VAL(7, 1), NVAL(7, 1));
	assert_true(coll_matches(lc, ek, ev, 1));
	dd_value_free(ev[0]);
	ev[0] = valv(VAL(9, 1), NVAL(9, 1));
	assert_true(coll_matches(rc, ek, ev, 1));
	dd_value_free(ek[0]);
	dd_value_free(ev[0]);
	mem_free(&m);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(explain_decouple_value_contract__sp_depth0_keeps_only_value_matching_pair),
	DTFL_T(explain_decouple_value_contract__sp_depth1_time_filters_late_inputs),
	DTFL_T(explain_decouple_value_contract__keyed_min_narrows_to_the_demanded_value),
	DTFL_T(explain_decouple_value_contract__keyed_nonmin_demands_all_same_key_inputs),
	DTFL_T(explain_decouple_value_contract__lossy_via_fallback_recovers_input),
	DTFL_T(explain_decouple_value_contract__join_demands_both_inputs),
	DTFL_T_END
};

DTFL_TEST_MAIN("explain_decouple", tests)
