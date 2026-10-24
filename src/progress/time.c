/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/order.rs
 *   timely/src/progress/timestamp.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/lattice.rs
 *   differential-dataflow/src/dynamic/pointstamp.rs (order, lattice, summaries)
 *   dogsdogsdogs/src/altneu.rs (order, lattice, Refines)
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/progress/time.c: time classes (PLAN D3).
 *
 * Every operation dispatches on the class shape and recurses into the
 * children of PRODUCT and LEX.  A time of a PRODUCT or LEX class is the
 * first child's words followed by the second child's, and so is a
 * summary, so a child always operates on a sub-slice.
 *
 * PointStamp is stored padded with the minimum (word 0) to max_len
 * coordinates; upstream trims instead.  PLAN D3 "PointStamp padding"
 * explains why every operation agrees.  Summary actions are padded with
 * the identity increment, which is also word 0 because PointStamp
 * coordinates are UNSIGNED or DURATION (SIGNED is refused at
 * construction: its identity increment is not word 0 and its minimum
 * is not the identity, so upstream's length rules would not survive
 * padding).
 */
#include "progress/time_int.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>

#define SIGN_BIT (UINT64_C(1) << 63)
#define NANOS_PER_SEC UINT64_C(1000000000)
#define PS_NONE UINT64_MAX	/* PointStamp summary retain: None */

/* ------------------------------------------------------------------ */
/* Coordinates                                                         */
/* ------------------------------------------------------------------ */

/* Signed values are biased so unsigned word order is signed order. */
uint64_t
dtfl_coord_from_i64(int64_t v)
{
	return (uint64_t)v ^ SIGN_BIT;
}

int64_t
dtfl_coord_to_i64(uint64_t w)
{
	uint64_t x = w ^ SIGN_BIT;

	/* Portable two's complement reinterpretation of x. */
	if (x <= (uint64_t)INT64_MAX)
		return (int64_t)x;
	return -(int64_t)(~x) - 1;
}

static bool
coord_valid(dtfl_coord c)
{
	switch (c.kind) {
	case DTFL_COORD_UNSIGNED:
	case DTFL_COORD_SIGNED:
		return c.bits == 8 || c.bits == 16 || c.bits == 32 || c.bits == 64;
	case DTFL_COORD_DURATION:
		return c.bits == 64;
	}
	return false;
}

/* Rust T::MIN for the coordinate (Timestamp::minimum). */
static uint64_t
coord_min(dtfl_coord c)
{
	if (c.kind == DTFL_COORD_SIGNED)
		return dtfl_coord_from_i64(c.bits == 64 ? INT64_MIN :
		    -(INT64_C(1) << (c.bits - 1)));
	return 0;
}

/* Rust T::MAX for the coordinate (lattice.rs Maximum). */
static uint64_t
coord_max(dtfl_coord c)
{
	if (c.kind == DTFL_COORD_SIGNED)
		return dtfl_coord_from_i64(c.bits == 64 ? INT64_MAX :
		    (INT64_C(1) << (c.bits - 1)) - 1);
	if (c.kind == DTFL_COORD_UNSIGNED && c.bits < 64)
		return (UINT64_C(1) << c.bits) - 1;
	return UINT64_MAX;
}

/* The identity increment (Rust Default for the summary type). */
static uint64_t
coord_zero(dtfl_coord c)
{
	return c.kind == DTFL_COORD_SIGNED ? SIGN_BIT : 0;
}

/*
 * checked_add at the declared width (timestamp.rs implement_timestamp_add).
 * Returns false where Rust returns None; *out is written only on success.
 */
static bool
coord_add(dtfl_coord c, uint64_t a, uint64_t s, uint64_t *out)
{
	uint64_t r;

	if (c.kind == DTFL_COORD_SIGNED) {
		int64_t x = dtfl_coord_to_i64(a), y = dtfl_coord_to_i64(s);

		if ((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y))
			return false;
		r = dtfl_coord_from_i64(x + y);
		if (r < coord_min(c) || r > coord_max(c))
			return false;
	} else {
		r = a + s;
		if (r < a || r > coord_max(c))
			return false;
	}
	*out = r;
	return true;
}

/* ------------------------------------------------------------------ */
/* Classes                                                             */
/* ------------------------------------------------------------------ */

/* Allocates a class with one reference and no children. */
static dtfl_time_class *
class_new(__dtfl_time_shape shape, unsigned depth, unsigned sdepth, bool total)
{
	dtfl_time_class *tc = __dtfl_calloc(1, sizeof *tc);

	__dtfl_arc_init(&tc->ref);
	tc->shape = shape;
	tc->depth = depth;
	tc->sdepth = sdepth;
	tc->total = total;
	return tc;
}

dtfl_time_class *
dtfl_time_unit(void)
{
	return class_new(DTFL_SHAPE_UNIT, 0, 0, true);
}

dtfl_time_class *
dtfl_time_total(dtfl_coord coord)
{
	dtfl_time_class *tc;

	if (!coord_valid(coord)) {
		(void)__dtfl_err(DTFL_EINVAL, "invalid coordinate (kind %d, %u bits)",
		    (int)coord.kind, coord.bits);
		return NULL;
	}
	tc = class_new(DTFL_SHAPE_TOTAL, 1, 1, true);
	tc->coord = coord;
	return tc;
}

dtfl_time_class *
dtfl_time_u64(void)
{
	return dtfl_time_total((dtfl_coord){ DTFL_COORD_UNSIGNED, 64 });
}

dtfl_time_class *
dtfl_time_u32(void)
{
	return dtfl_time_total((dtfl_coord){ DTFL_COORD_UNSIGNED, 32 });
}

dtfl_time_class *
dtfl_time_duration(void)
{
	return dtfl_time_total((dtfl_coord){ DTFL_COORD_DURATION, 64 });
}

/*
 * Common part of PRODUCT and LEX: checks the combined widths and
 * retains both children.  Returns NULL with the error detail set.
 */
static dtfl_time_class *
class_pair(__dtfl_time_shape shape, const dtfl_time_class *a,
    const dtfl_time_class *b, bool total)
{
	dtfl_time_class *tc;

	if (a == NULL || b == NULL) {
		(void)__dtfl_err(DTFL_EINVAL, "NULL child time class");
		return NULL;
	}
	if (a->depth + b->depth > DTFL_TIME_MAX ||
	    a->sdepth + b->sdepth > DTFL_TIME_MAX) {
		(void)__dtfl_err(DTFL_ERANGE,
		    "time class needs %u time and %u summary words; max is %d",
		    a->depth + b->depth, a->sdepth + b->sdepth, DTFL_TIME_MAX);
		return NULL;
	}
	tc = class_new(shape, a->depth + b->depth, a->sdepth + b->sdepth, total);
	tc->a = dtfl_time_class_retain(a);
	tc->b = dtfl_time_class_retain(b);
	return tc;
}

/*
 * Product<T1, T2> is TotalOrder when T1 is Empty (a composition of `()`,
 * that is depth 0) and T2 is TotalOrder (order.rs).
 */
dtfl_time_class *
dtfl_time_product(const dtfl_time_class *outer, const dtfl_time_class *inner)
{
	bool total = outer != NULL && inner != NULL && outer->depth == 0 &&
	    inner->total;

	return class_pair(DTFL_SHAPE_PRODUCT, outer, inner, total);
}

dtfl_time_class *
dtfl_time_lex(const dtfl_time_class *first, const dtfl_time_class *second)
{
	bool total = first != NULL && second != NULL && first->total &&
	    second->total;

	return class_pair(DTFL_SHAPE_LEX, first, second, total);
}

/*
 * AltNeu<T> as 2t + neu.  The encoding is exact only for a totally
 * ordered T (PLAN D3), and 2t must fit a word, so the base is one
 * unsigned or Duration coordinate (a biased signed word would not
 * double).  A 64-bit base (AltNeu<usize>, the only pinned use) holds
 * t < 2^63; larger t is refused where times enter (to_inner panics,
 * decode returns DTFL_ERANGE).
 */
dtfl_time_class *
dtfl_time_altneu(const dtfl_time_class *base)
{
	dtfl_time_class *tc;

	if (base == NULL || base->shape != DTFL_SHAPE_TOTAL ||
	    base->coord.kind == DTFL_COORD_SIGNED) {
		(void)__dtfl_err(DTFL_EINVAL,
		    "AltNeu needs a one-coordinate unsigned or Duration base");
		return NULL;
	}
	tc = class_new(DTFL_SHAPE_ALTNEU, 1, 0, true);
	tc->coord = base->coord;
	tc->a = dtfl_time_class_retain(base);
	return tc;
}

/*
 * PointStamp<coord> padded to max_len.  The summary takes 1 + max_len
 * words and must fit DTFL_TIME_MAX, so max_len < DTFL_TIME_MAX.
 */
dtfl_time_class *
dtfl_time_pointstamp(dtfl_coord coord, unsigned max_len)
{
	dtfl_time_class *tc;

	if (!coord_valid(coord) || coord.kind == DTFL_COORD_SIGNED) {
		(void)__dtfl_err(DTFL_EINVAL,
		    "PointStamp needs an unsigned or Duration coordinate");
		return NULL;
	}
	if (max_len == 0 || max_len >= DTFL_TIME_MAX) {
		(void)__dtfl_err(DTFL_ERANGE,
		    "PointStamp max_len %u not in 1..%d", max_len,
		    DTFL_TIME_MAX - 1);
		return NULL;
	}
	tc = class_new(DTFL_SHAPE_POINTSTAMP, max_len, 1 + max_len, false);
	tc->coord = coord;
	return tc;
}

dtfl_time_class *
dtfl_time_class_retain(const dtfl_time_class *tc)
{
	/* Only the atomic count of an immutable class changes. */
	dtfl_time_class *m = (dtfl_time_class *)(uintptr_t)tc;

	if (m != NULL)
		__dtfl_arc_retain(&m->ref);
	return m;
}

/* Drops one reference; the last one releases the children too. */
void
dtfl_time_class_free(const dtfl_time_class *tc)
{
	dtfl_time_class *m = (dtfl_time_class *)(uintptr_t)tc;

	if (m == NULL || !__dtfl_arc_release(&m->ref))
		return;
	dtfl_time_class_free(m->a);
	dtfl_time_class_free(m->b);
	__dtfl_free(m);
}

unsigned
dtfl_time_depth(const dtfl_time_class *tc)
{
	return tc->depth;
}

unsigned
dtfl_summary_depth(const dtfl_time_class *tc)
{
	return tc->sdepth;
}

bool
dtfl_time_is_total(const dtfl_time_class *tc)
{
	return tc->total;
}

/* Structural equality: two classes give words the same meaning. */
static bool
class_eq(const dtfl_time_class *x, const dtfl_time_class *y)
{
	if (x == y)
		return true;
	if (x == NULL || y == NULL || x->shape != y->shape ||
	    x->depth != y->depth || x->sdepth != y->sdepth ||
	    x->coord.kind != y->coord.kind || x->coord.bits != y->coord.bits)
		return false;
	return class_eq(x->a, y->a) && class_eq(x->b, y->b);
}

/* ------------------------------------------------------------------ */
/* Order (order.rs PartialOrder; Ord is word order)                    */
/* ------------------------------------------------------------------ */

int
dtfl_time_cmp(const dtfl_time_class *tc, const uint64_t *a, const uint64_t *b)
{
	return __dtfl_words_cmp(a, b, tc->depth);
}

bool
dtfl_time_less_equal(const dtfl_time_class *tc, const uint64_t *a,
    const uint64_t *b)
{
	unsigned i, d;

	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
		return true;
	case DTFL_SHAPE_TOTAL:
	case DTFL_SHAPE_ALTNEU:
		return a[0] <= b[0];
	case DTFL_SHAPE_PRODUCT:
		d = tc->a->depth;
		return dtfl_time_less_equal(tc->a, a, b) &&
		    dtfl_time_less_equal(tc->b, a + d, b + d);
	case DTFL_SHAPE_LEX:
		/* order.rs tuple: a.0 < b.0 || (a.0 == b.0 && a.1 <= b.1). */
		d = tc->a->depth;
		if (__dtfl_time_eq(tc->a, a, b))
			return dtfl_time_less_equal(tc->b, a + d, b + d);
		return dtfl_time_less_equal(tc->a, a, b);
	case DTFL_SHAPE_POINTSTAMP:
		for (i = 0; i < tc->depth; i++) {
			if (a[i] > b[i])
				return false;
		}
		return true;
	}
	return false;
}

bool
dtfl_time_less_than(const dtfl_time_class *tc, const uint64_t *a,
    const uint64_t *b)
{
	return dtfl_time_less_equal(tc, a, b) && !__dtfl_time_eq(tc, a, b);
}

/* ------------------------------------------------------------------ */
/* Lattice (lattice.rs)                                                */
/* ------------------------------------------------------------------ */

void
dtfl_time_minimum(const dtfl_time_class *tc, uint64_t *out)
{
	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
		break;
	case DTFL_SHAPE_TOTAL:
		out[0] = coord_min(tc->coord);
		break;
	case DTFL_SHAPE_ALTNEU:
	case DTFL_SHAPE_POINTSTAMP:
		memset(out, 0, tc->depth * sizeof(uint64_t));
		break;
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		dtfl_time_minimum(tc->a, out);
		dtfl_time_minimum(tc->b, out + tc->a->depth);
		break;
	}
}

/*
 * Rust has Maximum only for integers, Duration and `()`; the composite
 * shapes get their top element so LEX may nest any class second.
 */
void
__dtfl_time_maximum(const dtfl_time_class *tc, uint64_t *out)
{
	unsigned i;

	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
		break;
	case DTFL_SHAPE_TOTAL:
		out[0] = coord_max(tc->coord);
		break;
	case DTFL_SHAPE_ALTNEU:
		out[0] = 2 * DTFL_MIN(coord_max(tc->coord), SIGN_BIT - 1) + 1;
		break;
	case DTFL_SHAPE_POINTSTAMP:
		for (i = 0; i < tc->depth; i++)
			out[i] = coord_max(tc->coord);
		break;
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		__dtfl_time_maximum(tc->a, out);
		__dtfl_time_maximum(tc->b, out + tc->a->depth);
		break;
	}
}

static void time_lattice(const dtfl_time_class *tc, const uint64_t *a,
    const uint64_t *b, uint64_t *out, bool join);

/*
 * lattice.rs:204-230, Lattice for (T1, T2).  Incomparable first
 * components join to (a.0 join b.0, minimum) and meet to
 * (a.0 meet b.0, maximum).  `out` may alias a or b.
 */
static void
lex_lattice(const dtfl_time_class *tc, const uint64_t *a, const uint64_t *b,
    uint64_t *out, bool join)
{
	const dtfl_time_class *f = tc->a, *s = tc->b;
	unsigned d = f->depth;
	uint64_t tmp[DTFL_TIME_MAX];

	if (__dtfl_time_eq(f, a, b)) {
		__dtfl_time_copy(f, tmp, a);
		time_lattice(s, a + d, b + d, tmp + d, join);
	} else if (dtfl_time_less_than(f, a, b)) {
		__dtfl_time_copy(tc, tmp, join ? b : a);
	} else if (dtfl_time_less_than(f, b, a)) {
		__dtfl_time_copy(tc, tmp, join ? a : b);
	} else {
		time_lattice(f, a, b, tmp, join);
		if (join)
			dtfl_time_minimum(s, tmp + d);
		else
			__dtfl_time_maximum(s, tmp + d);
	}
	__dtfl_time_copy(tc, out, tmp);
}

/*
 * Join (join == true) or meet.  Totals take max/min, products and
 * padded PointStamps work pointwise (pointstamp.rs Lattice: missing
 * coordinates are the minimum, so max copies the longer tail and min
 * truncates), AltNeu is max/min of 2t + neu (exact for total T, PLAN
 * D3).  Each word is read before it is written, so `out` may alias.
 */
static void
time_lattice(const dtfl_time_class *tc, const uint64_t *a, const uint64_t *b,
    uint64_t *out, bool join)
{
	unsigned i, d;

	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
		break;
	case DTFL_SHAPE_TOTAL:
	case DTFL_SHAPE_ALTNEU:
	case DTFL_SHAPE_POINTSTAMP:
		for (i = 0; i < tc->depth; i++)
			out[i] = join ? DTFL_MAX(a[i], b[i]) : DTFL_MIN(a[i], b[i]);
		break;
	case DTFL_SHAPE_PRODUCT:
		d = tc->a->depth;
		time_lattice(tc->a, a, b, out, join);
		time_lattice(tc->b, a + d, b + d, out + d, join);
		break;
	case DTFL_SHAPE_LEX:
		lex_lattice(tc, a, b, out, join);
		break;
	}
}

void
dtfl_time_join(const dtfl_time_class *tc, const uint64_t *a, const uint64_t *b,
    uint64_t *out)
{
	time_lattice(tc, a, b, out, true);
}

void
dtfl_time_meet(const dtfl_time_class *tc, const uint64_t *a, const uint64_t *b,
    uint64_t *out)
{
	time_lattice(tc, a, b, out, false);
}

/* lattice.rs:138-147: the meet over the frontier of t join f. */
void
__dtfl_time_advance_by(const dtfl_time_class *tc, uint64_t *t,
    const uint64_t *frontier, size_t n)
{
	uint64_t result[DTFL_TIME_MAX], j[DTFL_TIME_MAX];
	size_t i;

	if (n == 0)
		return;
	dtfl_time_join(tc, t, frontier, result);
	for (i = 1; i < n; i++) {
		dtfl_time_join(tc, t, frontier + i * tc->depth, j);
		dtfl_time_meet(tc, result, j, result);
	}
	__dtfl_time_copy(tc, t, result);
}

/* ------------------------------------------------------------------ */
/* Summaries (timestamp.rs PathSummary)                                */
/* ------------------------------------------------------------------ */

void
dtfl_summary_default(const dtfl_time_class *tc, uint64_t *out)
{
	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
	case DTFL_SHAPE_ALTNEU:
		break;
	case DTFL_SHAPE_TOTAL:
		out[0] = coord_zero(tc->coord);
		break;
	case DTFL_SHAPE_POINTSTAMP:
		out[0] = PS_NONE;
		memset(out + 1, 0, tc->depth * sizeof(uint64_t));
		break;
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		dtfl_summary_default(tc->a, out);
		dtfl_summary_default(tc->b, out + tc->a->sdepth);
		break;
	}
}

/*
 * PointStampSummary::results_in: coordinates at or past `retain` become
 * the minimum (truncation), then every coordinate takes its action
 * (identity where upstream has none).
 */
static bool
ps_results_in(const dtfl_time_class *tc, const uint64_t *s, const uint64_t *t,
    uint64_t *out)
{
	unsigned i;

	for (i = 0; i < tc->depth; i++) {
		if (!coord_add(tc->coord, i < s[0] ? t[i] : 0, s[1 + i], &out[i]))
			return false;
	}
	return true;
}

/* Product and tuple results_in: both halves, None if either is. */
static bool
results_in(const dtfl_time_class *tc, const uint64_t *s, const uint64_t *t,
    uint64_t *out)
{
	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
		return true;
	case DTFL_SHAPE_TOTAL:
		return coord_add(tc->coord, t[0], s[0], out);
	case DTFL_SHAPE_ALTNEU:
		out[0] = t[0];
		return true;
	case DTFL_SHAPE_POINTSTAMP:
		return ps_results_in(tc, s, t, out);
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		return results_in(tc->a, s, t, out) &&
		    results_in(tc->b, s + tc->a->sdepth, t + tc->a->depth,
		    out + tc->a->depth);
	}
	return false;
}

/* `out` may alias `time`; it is left unchanged when this returns false. */
bool
dtfl_summary_results_in(const dtfl_time_class *tc, const uint64_t *summary,
    const uint64_t *time, uint64_t *out)
{
	uint64_t tmp[DTFL_TIME_MAX];

	if (!results_in(tc, summary, time, tmp))
		return false;
	__dtfl_time_copy(tc, out, tmp);
	return true;
}

/*
 * PointStampSummary::followed_by: retain is the smaller one; a's actions
 * at or past b's retain are dropped, then the actions compose pairwise.
 */
static bool
ps_followed_by(const dtfl_time_class *tc, const uint64_t *a, const uint64_t *b,
    uint64_t *out)
{
	unsigned i;

	out[0] = DTFL_MIN(a[0], b[0]);
	for (i = 0; i < tc->depth; i++) {
		if (!coord_add(tc->coord, i < b[0] ? a[1 + i] : 0, b[1 + i],
		    &out[1 + i]))
			return false;
	}
	return true;
}

static bool
followed_by(const dtfl_time_class *tc, const uint64_t *a, const uint64_t *b,
    uint64_t *out)
{
	unsigned d;

	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
	case DTFL_SHAPE_ALTNEU:
		return true;
	case DTFL_SHAPE_TOTAL:
		return coord_add(tc->coord, a[0], b[0], out);
	case DTFL_SHAPE_POINTSTAMP:
		return ps_followed_by(tc, a, b, out);
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		d = tc->a->sdepth;
		return followed_by(tc->a, a, b, out) &&
		    followed_by(tc->b, a + d, b + d, out + d);
	}
	return false;
}

/* `out` may alias a or b; it is left unchanged when this returns false. */
bool
dtfl_summary_followed_by(const dtfl_time_class *tc, const uint64_t *a,
    const uint64_t *b, uint64_t *out)
{
	uint64_t tmp[DTFL_TIME_MAX];

	if (!followed_by(tc, a, b, tmp))
		return false;
	__dtfl_summary_copy(tc, out, tmp);
	return true;
}

static bool
summary_eq(const dtfl_time_class *tc, const uint64_t *a, const uint64_t *b)
{
	return __dtfl_words_cmp(a, b, tc->sdepth) == 0;
}

/*
 * The summary order: integers by value, Product pointwise, tuples
 * lexicographically (with less_than = less_equal && !=), PointStamp
 * summaries by equal retain then pointwise actions (pointstamp.rs:
 * 199-213; with padding, trailing identity actions compare equal).
 */
bool
dtfl_summary_less_equal(const dtfl_time_class *tc, const uint64_t *a,
    const uint64_t *b)
{
	unsigned i, d;

	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
	case DTFL_SHAPE_ALTNEU:
		return true;
	case DTFL_SHAPE_TOTAL:
		return a[0] <= b[0];
	case DTFL_SHAPE_POINTSTAMP:
		if (a[0] != b[0])
			return false;
		for (i = 1; i < tc->sdepth; i++) {
			if (a[i] > b[i])
				return false;
		}
		return true;
	case DTFL_SHAPE_PRODUCT:
		d = tc->a->sdepth;
		return dtfl_summary_less_equal(tc->a, a, b) &&
		    dtfl_summary_less_equal(tc->b, a + d, b + d);
	case DTFL_SHAPE_LEX:
		d = tc->a->sdepth;
		if (summary_eq(tc->a, a, b))
			return dtfl_summary_less_equal(tc->b, a + d, b + d);
		return dtfl_summary_less_equal(tc->a, a, b);
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Refines (timestamp.rs, order.rs, altneu.rs)                         */
/* ------------------------------------------------------------------ */

typedef enum refine_kind {
	REFINE_NONE, REFINE_IDENT, REFINE_PRODUCT, REFINE_ALTNEU, REFINE_UNIT
} refine_kind;

static refine_kind
refine_kind_of(const dtfl_time_class *inner, const dtfl_time_class *outer)
{
	if (class_eq(inner, outer))
		return REFINE_IDENT;
	if (inner->shape == DTFL_SHAPE_PRODUCT && class_eq(inner->a, outer))
		return REFINE_PRODUCT;
	if (inner->shape == DTFL_SHAPE_ALTNEU && class_eq(inner->a, outer))
		return REFINE_ALTNEU;
	if (outer->shape == DTFL_SHAPE_UNIT)
		return REFINE_UNIT;
	return REFINE_NONE;
}

static refine_kind
refine_check(const dtfl_time_class *inner, const dtfl_time_class *outer)
{
	refine_kind k = refine_kind_of(inner, outer);

	if (k == REFINE_NONE)
		__dtfl_panic("time class does not refine the outer class");
	return k;
}

bool
__dtfl_refines(const dtfl_time_class *inner, const dtfl_time_class *outer)
{
	return refine_kind_of(inner, outer) != REFINE_NONE;
}

/*
 * Refines::to_inner.  Upstream's Refines<()> for integers and Duration
 * returns Default::default(), which for a signed integer is 0, not MIN
 * (timestamp.rs implement_refines_empty); PointStamp's returns the
 * empty vector, the minimum.  Classes upstream gives no Refines<()>
 * get the minimum.
 */
void
__dtfl_time_to_inner(const dtfl_time_class *inner,
    const dtfl_time_class *outer, const uint64_t *t_outer, uint64_t *t_inner)
{
	switch (refine_check(inner, outer)) {
	case REFINE_IDENT:
		__dtfl_time_copy(inner, t_inner, t_outer);
		break;
	case REFINE_PRODUCT:
		__dtfl_time_copy(outer, t_inner, t_outer);
		dtfl_time_minimum(inner->b, t_inner + outer->depth);
		break;
	case REFINE_ALTNEU:
		if (t_outer[0] >= SIGN_BIT)
			__dtfl_panic("AltNeu time %" PRIu64 " >= 2^63",
			    t_outer[0]);
		t_inner[0] = 2 * t_outer[0];
		break;
	case REFINE_UNIT:
	case REFINE_NONE:
		if (inner->shape == DTFL_SHAPE_TOTAL)
			t_inner[0] = coord_zero(inner->coord);
		else
			dtfl_time_minimum(inner, t_inner);
		break;
	}
}

void
__dtfl_time_to_outer(const dtfl_time_class *inner,
    const dtfl_time_class *outer, const uint64_t *t_inner, uint64_t *t_outer)
{
	switch (refine_check(inner, outer)) {
	case REFINE_IDENT:
	case REFINE_PRODUCT:
		__dtfl_time_copy(outer, t_outer, t_inner);
		break;
	case REFINE_ALTNEU:
		t_outer[0] = t_inner[0] / 2;
		break;
	case REFINE_UNIT:
	case REFINE_NONE:
		break;
	}
}

void
__dtfl_summary_to_outer(const dtfl_time_class *inner,
    const dtfl_time_class *outer, const uint64_t *s_inner, uint64_t *s_outer)
{
	switch (refine_check(inner, outer)) {
	case REFINE_IDENT:
	case REFINE_PRODUCT:
		__dtfl_summary_copy(outer, s_outer, s_inner);
		break;
	case REFINE_ALTNEU:
		dtfl_summary_default(outer, s_outer);
		break;
	case REFINE_UNIT:
	case REFINE_NONE:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* bincode-1 fixint little-endian codec (PLAN D9)                      */
/* ------------------------------------------------------------------ */

static void
put_le(__dtfl_vec *out, uint64_t v, unsigned n)
{
	unsigned char b[8];
	unsigned i;

	for (i = 0; i < n; i++)
		b[i] = (unsigned char)(v >> (8 * i));
	__dtfl_vec_push_n(out, b, n);
}

static uint64_t
get_le(const unsigned char *p, unsigned n)
{
	uint64_t v = 0;
	unsigned i;

	for (i = 0; i < n; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static size_t
coord_len(dtfl_coord c)
{
	return c.kind == DTFL_COORD_DURATION ? 12 : c.bits / 8;
}

/* Integers at their width (signed as two's complement); Duration as (secs, nanos). */
static void
coord_encode(dtfl_coord c, uint64_t w, __dtfl_vec *out)
{
	switch (c.kind) {
	case DTFL_COORD_UNSIGNED:
		put_le(out, w, c.bits / 8);
		break;
	case DTFL_COORD_SIGNED:
		put_le(out, w ^ SIGN_BIT, c.bits / 8);
		break;
	case DTFL_COORD_DURATION:
		put_le(out, w / NANOS_PER_SEC, 8);
		put_le(out, w % NANOS_PER_SEC, 4);
		break;
	}
}

/*
 * Decoding cursor: `p[*off..len)` is unread.  Each decoder returns 0,
 * DTFL_EPROTO for truncated or malformed bytes, or DTFL_ERANGE for a
 * valid Rust value this class cannot represent.
 */
static int
coord_decode(dtfl_coord c, const unsigned char *p, size_t len, size_t *off,
    uint64_t *w)
{
	size_t n = coord_len(c);
	uint64_t v, secs, nanos;

	if (len - *off < n)
		return DTFL_EPROTO;
	p += *off;
	*off += n;
	switch (c.kind) {
	case DTFL_COORD_UNSIGNED:
		*w = get_le(p, (unsigned)n);
		return 0;
	case DTFL_COORD_SIGNED:
		v = get_le(p, (unsigned)n);
		if (c.bits < 64 && (v >> (c.bits - 1)) != 0)
			v |= ~((UINT64_C(1) << c.bits) - 1);	/* sign-extend */
		*w = v ^ SIGN_BIT;
		return 0;
	case DTFL_COORD_DURATION:
		/* serde accepts nanos >= 10^9 and carries them into secs. */
		secs = get_le(p, 8);
		nanos = get_le(p + 8, 4);
		if (secs > (UINT64_MAX - nanos) / NANOS_PER_SEC)
			return DTFL_ERANGE;
		*w = secs * NANOS_PER_SEC + nanos;
		return 0;
	}
	return DTFL_EPROTO;
}

/* Length of a padded run once trailing zero words are trimmed. */
static unsigned
trimmed_len(const uint64_t *v, unsigned n)
{
	while (n > 0 && v[n - 1] == 0)
		n--;
	return n;
}

size_t
__dtfl_time_encoded_len(const dtfl_time_class *tc, const uint64_t *t)
{
	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
		return 0;
	case DTFL_SHAPE_TOTAL:
		return coord_len(tc->coord);
	case DTFL_SHAPE_ALTNEU:
		return coord_len(tc->coord) + 1;
	case DTFL_SHAPE_POINTSTAMP:
		return 8 + trimmed_len(t, tc->depth) * coord_len(tc->coord);
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		return __dtfl_time_encoded_len(tc->a, t) +
		    __dtfl_time_encoded_len(tc->b, t + tc->a->depth);
	}
	return 0;
}

/* Appends a u64 length and the first n coordinates (a SmallVec / Vec). */
static void
seq_encode(dtfl_coord c, const uint64_t *v, unsigned n, __dtfl_vec *out)
{
	unsigned i;

	put_le(out, n, 8);
	for (i = 0; i < n; i++)
		coord_encode(c, v[i], out);
}

/*
 * Reads a u64 length and that many coordinates into v[0..max), padded
 * with zero words.  Coordinates past max must be zero (trailing minima
 * or identity actions); anything else is DTFL_ERANGE.
 */
static int
seq_decode(dtfl_coord c, unsigned max, const unsigned char *p, size_t len,
    size_t *off, uint64_t *v)
{
	uint64_t n, i, w;
	int err;

	if (len - *off < 8)
		return DTFL_EPROTO;
	n = get_le(p + *off, 8);
	*off += 8;
	if (n > (len - *off) / coord_len(c))
		return DTFL_EPROTO;
	memset(v, 0, max * sizeof(uint64_t));
	for (i = 0; i < n; i++) {
		if ((err = coord_decode(c, p, len, off, &w)) != 0)
			return err;
		if (i < max)
			v[i] = w;
		else if (w != 0)
			return DTFL_ERANGE;
	}
	return 0;
}

static void
time_encode(const dtfl_time_class *tc, const uint64_t *t, __dtfl_vec *out)
{
	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
		break;
	case DTFL_SHAPE_TOTAL:
		coord_encode(tc->coord, t[0], out);
		break;
	case DTFL_SHAPE_ALTNEU:
		/* AltNeu { time, neu }: the base time, then a bool byte. */
		coord_encode(tc->coord, t[0] >> 1, out);
		put_le(out, t[0] & 1, 1);
		break;
	case DTFL_SHAPE_POINTSTAMP:
		seq_encode(tc->coord, t, trimmed_len(t, tc->depth), out);
		break;
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		time_encode(tc->a, t, out);
		time_encode(tc->b, t + tc->a->depth, out);
		break;
	}
}

size_t
__dtfl_time_encode(const dtfl_time_class *tc, const uint64_t *t, __dtfl_vec *out)
{
	size_t before = out->len;

	DTFL_ASSERT(out->elem == 1);
	time_encode(tc, t, out);
	return out->len - before;
}

static int
time_decode(const dtfl_time_class *tc, const unsigned char *p, size_t len,
    size_t *off, uint64_t *t)
{
	int err;

	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
		return 0;
	case DTFL_SHAPE_TOTAL:
		return coord_decode(tc->coord, p, len, off, t);
	case DTFL_SHAPE_ALTNEU:
		if ((err = coord_decode(tc->coord, p, len, off, t)) != 0)
			return err;
		if (len - *off < 1 || p[*off] > 1)
			return DTFL_EPROTO;
		if (t[0] >= SIGN_BIT)
			return DTFL_ERANGE;
		t[0] = 2 * t[0] + p[(*off)++];
		return 0;
	case DTFL_SHAPE_POINTSTAMP:
		/* A non-canonical (trailing minimum) input is accepted. */
		return seq_decode(tc->coord, tc->depth, p, len, off, t);
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		if ((err = time_decode(tc->a, p, len, off, t)) != 0)
			return err;
		return time_decode(tc->b, p, len, off, t + tc->a->depth);
	}
	return DTFL_EPROTO;
}

int
__dtfl_time_decode(const dtfl_time_class *tc, const unsigned char *p,
    size_t len, uint64_t *t, size_t *used)
{
	uint64_t tmp[DTFL_TIME_MAX];
	size_t off = 0;
	int err;

	if ((err = time_decode(tc, p, len, &off, tmp)) != 0)
		return err;
	__dtfl_time_copy(tc, t, tmp);
	*used = off;
	return 0;
}

static void
summary_encode(const dtfl_time_class *tc, const uint64_t *s, __dtfl_vec *out)
{
	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
	case DTFL_SHAPE_ALTNEU:
		break;
	case DTFL_SHAPE_TOTAL:
		coord_encode(tc->coord, s[0], out);
		break;
	case DTFL_SHAPE_POINTSTAMP:
		/* PointStampSummary { retain: Option<usize>, actions: Vec<_> } */
		put_le(out, s[0] != PS_NONE, 1);
		if (s[0] != PS_NONE)
			put_le(out, s[0], 8);
		seq_encode(tc->coord, s + 1, trimmed_len(s + 1, tc->depth), out);
		break;
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		summary_encode(tc->a, s, out);
		summary_encode(tc->b, s + tc->a->sdepth, out);
		break;
	}
}

size_t
__dtfl_summary_encode(const dtfl_time_class *tc, const uint64_t *s,
    __dtfl_vec *out)
{
	size_t before = out->len;

	DTFL_ASSERT(out->elem == 1);
	summary_encode(tc, s, out);
	return out->len - before;
}

static int
summary_decode(const dtfl_time_class *tc, const unsigned char *p, size_t len,
    size_t *off, uint64_t *s)
{
	int err;

	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
	case DTFL_SHAPE_ALTNEU:
		return 0;
	case DTFL_SHAPE_TOTAL:
		return coord_decode(tc->coord, p, len, off, s);
	case DTFL_SHAPE_POINTSTAMP:
		if (len - *off < 1 || p[*off] > 1)
			return DTFL_EPROTO;
		s[0] = PS_NONE;
		if (p[(*off)++] == 1) {
			if (len - *off < 8)
				return DTFL_EPROTO;
			s[0] = get_le(p + *off, 8);
			*off += 8;
			if (s[0] == PS_NONE)	/* Some(usize::MAX) */
				return DTFL_ERANGE;
		}
		return seq_decode(tc->coord, tc->depth, p, len, off, s + 1);
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		if ((err = summary_decode(tc->a, p, len, off, s)) != 0)
			return err;
		return summary_decode(tc->b, p, len, off, s + tc->a->sdepth);
	}
	return DTFL_EPROTO;
}

int
__dtfl_summary_decode(const dtfl_time_class *tc, const unsigned char *p,
    size_t len, uint64_t *s, size_t *used)
{
	uint64_t tmp[DTFL_TIME_MAX];
	size_t off = 0;
	int err;

	if ((err = summary_decode(tc, p, len, &off, tmp)) != 0)
		return err;
	__dtfl_summary_copy(tc, s, tmp);
	*used = off;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Formatting: Rust's Debug, except Duration prints as secs.nnnnnnnnns  */
/* ------------------------------------------------------------------ */

/* snprintf that accumulates: `*pos` counts every byte, written or not. */
static void DTFL_PRINTF(4, 5)
fmt_put(char *buf, size_t len, size_t *pos, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(*pos < len ? buf + *pos : NULL, *pos < len ? len - *pos : 0,
	    fmt, ap);
	va_end(ap);
	if (n > 0)
		*pos += (size_t)n;
}

static void
coord_format(dtfl_coord c, uint64_t w, char *buf, size_t len, size_t *pos)
{
	switch (c.kind) {
	case DTFL_COORD_UNSIGNED:
		fmt_put(buf, len, pos, "%" PRIu64, w);
		break;
	case DTFL_COORD_SIGNED:
		fmt_put(buf, len, pos, "%" PRId64, dtfl_coord_to_i64(w));
		break;
	case DTFL_COORD_DURATION:
		fmt_put(buf, len, pos, "%" PRIu64 ".%09" PRIu64 "s",
		    w / NANOS_PER_SEC, w % NANOS_PER_SEC);
		break;
	}
}

static void
time_format(const dtfl_time_class *tc, const uint64_t *t, char *buf, size_t len,
    size_t *pos)
{
	unsigned i, n;

	switch (tc->shape) {
	case DTFL_SHAPE_UNIT:
		fmt_put(buf, len, pos, "()");
		break;
	case DTFL_SHAPE_TOTAL:
		coord_format(tc->coord, t[0], buf, len, pos);
		break;
	case DTFL_SHAPE_ALTNEU:
		fmt_put(buf, len, pos, "AltNeu { time: ");
		coord_format(tc->coord, t[0] >> 1, buf, len, pos);
		fmt_put(buf, len, pos, ", neu: %s }", (t[0] & 1) ? "true" : "false");
		break;
	case DTFL_SHAPE_POINTSTAMP:
		fmt_put(buf, len, pos, "PointStamp { vector: [");
		n = trimmed_len(t, tc->depth);
		for (i = 0; i < n; i++) {
			fmt_put(buf, len, pos, "%s", i == 0 ? "" : ", ");
			coord_format(tc->coord, t[i], buf, len, pos);
		}
		fmt_put(buf, len, pos, "] }");
		break;
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		fmt_put(buf, len, pos, "(");
		time_format(tc->a, t, buf, len, pos);
		fmt_put(buf, len, pos, ", ");
		time_format(tc->b, t + tc->a->depth, buf, len, pos);
		fmt_put(buf, len, pos, ")");
		break;
	}
}

int
dtfl_time_format(const dtfl_time_class *tc, const uint64_t *t, char *buf,
    size_t len)
{
	size_t pos = 0;

	if (len > 0)
		buf[0] = '\0';
	time_format(tc, t, buf, len, &pos);
	return pos > INT_MAX ? -1 : (int)pos;
}
