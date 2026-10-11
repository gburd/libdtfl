/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_consolidate.c: __dtfl_consolidate_batch against a naive
 * model, and the semigroup laws of every built-in diff class.
 *
 * The model accumulates each distinct (record, time) by a quadratic
 * scan.  The consolidated batch must be strictly sorted by (record,
 * time), hold no zero diffs, and hold exactly the model's non-zero
 * sums.  Records are u64 or strings and times u64 or Product<u64, u64>,
 * drawn from small ranges so that duplicates are common.
 */
#include "pbt.h"
#include "dd/dd_int.h"

#define MAXN 64

typedef struct upd {
	unsigned char rec[16];
	size_t len;
	uint64_t t[2];
	int64_t r;
} upd;

typedef struct ucase {
	upd u[MAXN];
	size_t n;
	unsigned tw;
	bool str;
} ucase;

static void
release_batch(void *b)
{
	__dtfl_batch_release(b);
}

static bool
same_key(const ucase *c, const upd *a, const void *rec, size_t len,
    const uint64_t *t)
{
	return a->len == len && memcmp(a->rec, rec, len) == 0 &&
	    __dtfl_words_cmp(a->t, t, c->tw) == 0;
}

/* The model: sum of r over the updates with this (record, time). */
static int64_t
model_sum(const ucase *c, const void *rec, size_t len, const uint64_t *t)
{
	uint64_t s = 0;
	size_t i;

	for (i = 0; i < c->n; i++) {
		if (same_key(c, &c->u[i], rec, len, t))
			s += (uint64_t)c->u[i].r;
	}
	return (int64_t)s;
}

/* Distinct keys of the input whose model sum is non-zero. */
static size_t
model_count(const ucase *c)
{
	size_t i, j, n = 0;

	for (i = 0; i < c->n; i++) {
		for (j = 0; j < i; j++) {
			if (same_key(c, &c->u[j], c->u[i].rec, c->u[i].len,
			    c->u[i].t))
				break;
		}
		if (j == i && model_sum(c, c->u[i].rec, c->u[i].len,
		    c->u[i].t) != 0)
			n++;
	}
	return n;
}

static void
prop_consolidate_model(pbt_tc *tc)
{
	static ucase c;
	const dtfl_type *rt;
	const __dtfl_diff_class *dc = __dtfl_diff_builtin(DTFL_DIFF_I64);
	__dtfl_batch *b;
	size_t i, k, len, plen = 0, slen;
	const void *p, *prev = NULL;
	int64_t r;
	char s[4];

	c.str = pbt_bool(tc);
	c.tw = pbt_bool(tc) ? 2 : 1;
	c.n = (size_t)pbt_int(tc, 0, MAXN);
	for (i = 0; i < c.n; i++) {
		upd *u = &c.u[i];

		if (c.str) {
			slen = (size_t)pbt_int(tc, 0, 3);
			for (k = 0; k < slen; k++)
				s[k] = (char)('a' + pbt_int(tc, 0, 2));
			u->len = dtfl_put_bytes(u->rec, s, slen);
		} else {
			u->len = dtfl_put_u64(u->rec,
			    (uint64_t)pbt_int(tc, 0, 5) << 60 |
			    (uint64_t)pbt_int(tc, 0, 3));
		}
		u->t[0] = (uint64_t)pbt_int(tc, 0, 2);
		u->t[1] = (uint64_t)pbt_int(tc, 0, 2);
		u->r = pbt_bool(tc) ? pbt_int(tc, -2, 2) :
		    pbt_int(tc, INT64_MIN, INT64_MAX);
	}
	rt = c.str ? dtfl_type_string() : dtfl_type_u64();
	b = __dtfl_batch_new(c.tw, 8);
	pbt_defer(tc, release_batch, b);
	for (i = 0; i < c.n; i++)
		__dtfl_batch_push(b, c.u[i].rec, c.u[i].len, c.u[i].t,
		    &c.u[i].r);
	__dtfl_consolidate_batch(b, rt, dc);

	PBT_ASSERT(__dtfl_batch_len(b) == model_count(&c));
	for (i = 0; i < __dtfl_batch_len(b); i++) {
		p = __dtfl_batch_rec(b, i, &len);
		memcpy(&r, __dtfl_batch_diff(b, i), 8);
		PBT_ASSERT(r != 0);
		PBT_ASSERT(r == model_sum(&c, p, len, __dtfl_batch_time(b, i)));
		if (prev != NULL) {
			int o = dtfl_type_cmp(rt, prev, plen, p, len);

			PBT_ASSERT(o < 0 || (o == 0 && __dtfl_words_cmp(
			    __dtfl_batch_time(b, i - 1),
			    __dtfl_batch_time(b, i), c.tw) < 0));
		}
		prev = p;
		plen = len;
	}
}

static void
free_class(void *c)
{
	__dtfl_diff_class_free(c);
}

/* Draws a value of class dc: small or arbitrary bytes. */
static void
draw_value(pbt_tc *tc, const __dtfl_diff_class *dc, unsigned char *v)
{
	size_t i;

	for (i = 0; i < dc->width; i++)
		v[i] = (unsigned char)(pbt_bool(tc) ? 0 : pbt_int(tc, 0, 255));
}

/*
 * plus_equals is associative and commutative; zero (if any) is an
 * identity and is_zero; negate (if any) gives an inverse.  Covers each
 * built-in and a random tuple of up to four of them.
 */
static void
prop_semigroup_laws(pbt_tc *tc)
{
	const __dtfl_diff_class *dc, *f[DTFL_DIFF_TUPLE_MAX];
	unsigned char a[64], b[64], c[64], x[64], y[64];
	size_t i, n;
	int id = (int)pbt_int(tc, 0, DTFL_DIFF_NBUILTIN);

	if (id < DTFL_DIFF_NBUILTIN) {
		dc = __dtfl_diff_builtin(id);
	} else {
		n = (size_t)pbt_int(tc, 1, DTFL_DIFF_TUPLE_MAX);
		for (i = 0; i < n; i++)
			f[i] = __dtfl_diff_builtin(pbt_int(tc, 0,
			    DTFL_DIFF_NBUILTIN - 1));
		dc = __dtfl_diff_tuple(f, n);
		pbt_defer(tc, free_class, (void *)dc);
	}
	draw_value(tc, dc, a);
	draw_value(tc, dc, b);
	draw_value(tc, dc, c);

	/* (a + b) + c == a + (b + c) */
	memcpy(x, a, dc->width);
	dc->plus_equals(dc, x, b);
	dc->plus_equals(dc, x, c);
	memcpy(y, b, dc->width);
	dc->plus_equals(dc, y, c);
	memcpy(c, a, dc->width);
	dc->plus_equals(dc, c, y);
	PBT_ASSERT(dc->width == 0 || memcmp(x, c, dc->width) == 0);
	PBT_ASSERT(dc->cmp(dc, x, c) == 0);

	/* a + b == b + a */
	memcpy(x, a, dc->width);
	dc->plus_equals(dc, x, b);
	memcpy(y, b, dc->width);
	dc->plus_equals(dc, y, a);
	PBT_ASSERT(dc->width == 0 || memcmp(x, y, dc->width) == 0);

	if (dc->zero != NULL) {
		dc->zero(dc, x);
		PBT_ASSERT(dc->is_zero(dc, x));
		dc->plus_equals(dc, x, a);
		PBT_ASSERT(dc->cmp(dc, x, a) == 0);
	} else {
		PBT_ASSERT(dc->negate == NULL);
	}
	if (dc->negate != NULL) {
		memcpy(x, a, dc->width);
		dc->negate(dc, x);
		dc->plus_equals(dc, x, a);
		PBT_ASSERT(dc->is_zero(dc, x));
	}
	/* cmp is antisymmetric. */
	PBT_ASSERT(dc->cmp(dc, a, b) == -dc->cmp(dc, b, a));
}

static const pbt_prop props[] = {
	{ "consolidate_model", prop_consolidate_model, 0 },
	{ "semigroup_laws", prop_semigroup_laws, 0 },
};

PBT_MAIN("consolidate", props)
