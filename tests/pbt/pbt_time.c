/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_time.c: properties of the time classes (PLAN section 6,
 * tier 4) over randomly composed classes.
 *
 *   - lattice laws: join and meet are commutative, associative,
 *     idempotent and absorbing, and are the least upper and greatest
 *     lower bounds;
 *   - less_equal is a partial order, total where the class says so, and
 *     consistent with Ord (timestamp.rs: a.less_equal(b) implies a <= b);
 *   - advance_by soundness and canonicity, the statements of
 *     advance_le_iff and advance_eq_iff in formal/Differential/
 *     Compaction.lean at differential-dataflow-v0.25.1;
 *   - results_in is monotone in the time and in the summary, and
 *     followed_by composes like sequential results_in (timestamp.rs
 *     PathSummary docs);
 *   - the codec round-trips, and dtfl_time_cmp equals Rust's derived Ord
 *     of the decoded values, computed here from the bincode bytes
 *     (signed, Duration, AltNeu and PointStamp included).
 */
#include "pbt.h"
#include "progress/time_int.h"

/* ------------------------------------------------------------------ */
/* Generators                                                          */
/* ------------------------------------------------------------------ */

static const dtfl_coord C_U8 = { DTFL_COORD_UNSIGNED, 8 };
static const dtfl_coord C_U16 = { DTFL_COORD_UNSIGNED, 16 };
static const dtfl_coord C_U64 = { DTFL_COORD_UNSIGNED, 64 };
static const dtfl_coord C_I8 = { DTFL_COORD_SIGNED, 8 };
static const dtfl_coord C_I64 = { DTFL_COORD_SIGNED, 64 };
static const dtfl_coord C_DUR = { DTFL_COORD_DURATION, 64 };

static dtfl_coord
draw_coord(pbt_tc *tc, bool allow_signed)
{
	switch (pbt_int(tc, 0, allow_signed ? 5 : 3)) {
	case 0: return C_U8;
	case 1: return C_U16;
	case 2: return C_U64;
	case 3: return C_DUR;
	case 4: return C_I8;
	default: return C_I64;
	}
}

/* A class of at most `levels` composite levels; never NULL. */
static dtfl_time_class *draw_class(pbt_tc *tc, int levels);

static void
class_release(void *c)
{
	dtfl_time_class_free(c);
}

/*
 * Draws a random class.  Every class returned (and every intermediate)
 * is registered with pbt_defer, so it is released when the case ends
 * even if a later draw or assertion unwinds; callers must not free it.
 */
static dtfl_time_class *
draw_class_raw(pbt_tc *tc, int levels)
{
	dtfl_time_class *a, *b, *r, *base;
	int pick = pbt_int(tc, 0, levels > 0 ? 6 : 4);
	dtfl_coord c;

	switch (pick) {
	case 0:
		return dtfl_time_unit();
	case 1:
	case 2:
		c = draw_coord(tc, true);
		return dtfl_time_total(c);
	case 3:
		c = draw_coord(tc, false);
		base = dtfl_time_total(c);
		pbt_defer(tc, class_release, base);
		r = dtfl_time_altneu(base);
		return r;
	case 4:
		c = draw_coord(tc, false);
		return dtfl_time_pointstamp(c, (unsigned)pbt_int(tc, 1, 3));
	default:
		a = draw_class(tc, levels - 1);
		b = draw_class(tc, levels - 1);
		r = pick == 5 ? dtfl_time_product(a, b) : dtfl_time_lex(a, b);
		return r != NULL ? r : dtfl_time_u64();
	}
}

static dtfl_time_class *
draw_class(pbt_tc *tc, int levels)
{
	dtfl_time_class *r = draw_class_raw(tc, levels);

	pbt_defer(tc, class_release, r);
	return r;
}

/*
 * A word for coordinate `c`, biased toward small values (so draws
 * collide and orders are exercised) and toward the extremes (so
 * overflow is).
 */
static uint64_t
draw_word(pbt_tc *tc, dtfl_coord c)
{
	int64_t lo, hi;
	uint64_t umax;

	if (c.kind == DTFL_COORD_SIGNED) {
		hi = c.bits == 64 ? INT64_MAX : (INT64_C(1) << (c.bits - 1)) - 1;
		lo = -hi - 1;
		switch (pbt_int(tc, 0, 3)) {
		case 0: return dtfl_coord_from_i64(lo + pbt_int(tc, 0, 2));
		case 1: return dtfl_coord_from_i64(hi - pbt_int(tc, 0, 2));
		case 2: return dtfl_coord_from_i64(pbt_int(tc, lo, hi));
		default: return dtfl_coord_from_i64(pbt_int(tc, -3, 3));
		}
	}
	umax = c.kind == DTFL_COORD_UNSIGNED && c.bits < 64 ?
	    (UINT64_C(1) << c.bits) - 1 : UINT64_MAX;
	switch (pbt_int(tc, 0, 4)) {
	case 0: return umax - (uint64_t)pbt_int(tc, 0, 2);
	case 1: return (uint64_t)pbt_int(tc, INT64_MIN, INT64_MAX) & umax;
	default: return (uint64_t)pbt_int(tc, 0, 3);
	}
}

static void
draw_time(pbt_tc *tc, const dtfl_time_class *k, uint64_t *t)
{
	unsigned i;
	uint64_t w;

	switch (k->shape) {
	case DTFL_SHAPE_UNIT:
		break;
	case DTFL_SHAPE_TOTAL:
		t[0] = draw_word(tc, k->coord);
		break;
	case DTFL_SHAPE_ALTNEU:
		w = draw_word(tc, k->coord) & (UINT64_MAX >> 1);
		t[0] = 2 * w + (pbt_bool(tc) ? 1 : 0);
		break;
	case DTFL_SHAPE_POINTSTAMP:
		for (i = 0; i < k->depth; i++)
			t[i] = pbt_int(tc, 0, 2) == 0 ? 0 : draw_word(tc, k->coord);
		break;
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		draw_time(tc, k->a, t);
		draw_time(tc, k->b, t + k->a->depth);
		break;
	}
}

static void
draw_summary(pbt_tc *tc, const dtfl_time_class *k, uint64_t *s)
{
	unsigned i;

	switch (k->shape) {
	case DTFL_SHAPE_UNIT:
	case DTFL_SHAPE_ALTNEU:
		break;
	case DTFL_SHAPE_TOTAL:
		s[0] = draw_word(tc, k->coord);
		break;
	case DTFL_SHAPE_POINTSTAMP:
		s[0] = pbt_bool(tc) ? UINT64_MAX :
		    (uint64_t)pbt_int(tc, 0, k->depth + 1);
		for (i = 1; i < k->sdepth; i++)
			s[i] = pbt_int(tc, 0, 2) == 0 ? 0 : draw_word(tc, k->coord);
		break;
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		draw_summary(tc, k->a, s);
		draw_summary(tc, k->b, s + k->a->sdepth);
		break;
	}
}

static bool
has_signed(const dtfl_time_class *k)
{
	if (k == NULL)
		return false;
	if ((k->shape == DTFL_SHAPE_TOTAL || k->shape == DTFL_SHAPE_POINTSTAMP) &&
	    k->coord.kind == DTFL_COORD_SIGNED)
		return true;
	return has_signed(k->a) || has_signed(k->b);
}

/*
 * True if some LEX node has a PointStamp inside its first component.
 * PointStamp's results_in is monotone but not strictly (retain can map
 * distinct times to one), so a tuple with it first is not monotone in
 * the time, upstream as here (order.rs tuple PathSummary).
 */
static bool
lex_over_pointstamp(const dtfl_time_class *k, bool in_first)
{
	if (k == NULL)
		return false;
	if (k->shape == DTFL_SHAPE_POINTSTAMP)
		return in_first;
	if (k->shape == DTFL_SHAPE_LEX)
		return lex_over_pointstamp(k->a, true) ||
		    lex_over_pointstamp(k->b, in_first);
	return lex_over_pointstamp(k->a, in_first) ||
	    lex_over_pointstamp(k->b, in_first);
}

static bool
has_pointstamp(const dtfl_time_class *k)
{
	return k != NULL && (k->shape == DTFL_SHAPE_POINTSTAMP ||
	    has_pointstamp(k->a) || has_pointstamp(k->b));
}

#define LE(k, a, b) dtfl_time_less_equal((k), (a), (b))
#define EQ(k, a, b) __dtfl_time_eq((k), (a), (b))

/* ------------------------------------------------------------------ */
/* Lattice and order                                                   */
/* ------------------------------------------------------------------ */

/*
 * x = t with one word taken from y.  Both are valid times of k and the
 * word keeps its position, so x is valid too except for AltNeu, whose
 * single word is copied whole, and PointStamp, which accepts any word.
 */
static void
neighbor(pbt_tc *tc, const dtfl_time_class *k, const uint64_t *t,
    const uint64_t *y, uint64_t *x)
{
	unsigned w;

	__dtfl_time_copy(k, x, t);
	if (k->depth > 0) {
		w = (unsigned)pbt_int(tc, 0, k->depth - 1);
		x[w] = y[w];
	}
}

static void
prop_lattice_laws(pbt_tc *tc)
{
	dtfl_time_class *k = draw_class(tc, 2);
	uint64_t a[DTFL_TIME_MAX], b[DTFL_TIME_MAX], c[DTFL_TIME_MAX];
	uint64_t x[DTFL_TIME_MAX], y[DTFL_TIME_MAX], j[DTFL_TIME_MAX],
	    m[DTFL_TIME_MAX];

	draw_time(tc, k, a);
	draw_time(tc, k, b);
	draw_time(tc, k, c);
	dtfl_time_join(k, a, b, j);
	dtfl_time_meet(k, a, b, m);
	/* Commutative. */
	dtfl_time_join(k, b, a, x);
	PBT_ASSERT(EQ(k, x, j));
	dtfl_time_meet(k, b, a, x);
	PBT_ASSERT(EQ(k, x, m));
	/* Associative. */
	dtfl_time_join(k, j, c, x);
	dtfl_time_join(k, b, c, y);
	dtfl_time_join(k, a, y, y);
	PBT_ASSERT(EQ(k, x, y));
	dtfl_time_meet(k, m, c, x);
	dtfl_time_meet(k, b, c, y);
	dtfl_time_meet(k, a, y, y);
	PBT_ASSERT(EQ(k, x, y));
	/* Idempotent. */
	dtfl_time_join(k, a, a, x);
	PBT_ASSERT(EQ(k, x, a));
	dtfl_time_meet(k, a, a, x);
	PBT_ASSERT(EQ(k, x, a));
	/* Absorption. */
	dtfl_time_join(k, a, m, x);
	PBT_ASSERT(EQ(k, x, a));
	dtfl_time_meet(k, a, j, x);
	PBT_ASSERT(EQ(k, x, a));
	/* Bounds. */
	PBT_ASSERT(LE(k, a, j) && LE(k, b, j));
	PBT_ASSERT(LE(k, m, a) && LE(k, m, b));
	if (LE(k, a, c) && LE(k, b, c))
		PBT_ASSERT(LE(k, j, c));
	if (LE(k, c, a) && LE(k, c, b))
		PBT_ASSERT(LE(k, c, m));
	/* Least upper bound against an upper bound built to exist. */
	dtfl_time_join(k, j, c, x);
	PBT_ASSERT(LE(k, j, x));
	/*
	 * The defining property of a least upper (greatest lower) bound,
	 * at points next to the result: x is j (or m) with one word
	 * replaced by a drawn value for that coordinate.  Random points
	 * almost never test the bound; neighbors do.
	 */
	draw_time(tc, k, y);
	neighbor(tc, k, j, y, x);
	PBT_ASSERT((LE(k, a, x) && LE(k, b, x)) == LE(k, j, x));
	neighbor(tc, k, m, y, x);
	PBT_ASSERT((LE(k, x, a) && LE(k, x, b)) == LE(k, x, m));
	/* The lattice order is the partial order. */
	PBT_ASSERT(LE(k, a, b) == EQ(k, j, b));
	PBT_ASSERT(LE(k, a, b) == EQ(k, m, a));
	/* minimum is below everything. */
	dtfl_time_minimum(k, x);
	PBT_ASSERT(LE(k, x, a));
}

static void
prop_partial_order(pbt_tc *tc)
{
	dtfl_time_class *k = draw_class(tc, 2);
	uint64_t a[DTFL_TIME_MAX], b[DTFL_TIME_MAX], c[DTFL_TIME_MAX],
	    x[DTFL_TIME_MAX];

	draw_time(tc, k, a);
	draw_time(tc, k, x);
	draw_time(tc, k, c);
	PBT_ASSERT(LE(k, a, a));
	PBT_ASSERT(!dtfl_time_less_than(k, a, a));
	/* Antisymmetric, and consistent with Ord. */
	if (LE(k, a, x) && LE(k, x, a))
		PBT_ASSERT(EQ(k, a, x));
	if (LE(k, a, x))
		PBT_ASSERT(dtfl_time_cmp(k, a, x) <= 0);
	PBT_ASSERT(dtfl_time_less_than(k, a, x) == (LE(k, a, x) && !EQ(k, a, x)));
	PBT_ASSERT(dtfl_time_cmp(k, a, x) == -dtfl_time_cmp(k, x, a));
	PBT_ASSERT((dtfl_time_cmp(k, a, x) == 0) == EQ(k, a, x));
	if (dtfl_time_is_total(k))
		PBT_ASSERT(LE(k, a, x) || LE(k, x, a));
	/* Transitive along a chain a <= b <= c built with join. */
	dtfl_time_join(k, a, x, b);
	dtfl_time_join(k, b, c, c);
	PBT_ASSERT(LE(k, a, b) && LE(k, b, c) && LE(k, a, c));
	PBT_ASSERT(dtfl_time_cmp(k, a, b) <= 0 && dtfl_time_cmp(k, b, c) <= 0);
}

/* ------------------------------------------------------------------ */
/* advance_by (formal/Differential/Compaction.lean)                    */
/* ------------------------------------------------------------------ */

#define FMAX 4

static void
prop_advance_by(pbt_tc *tc)
{
	dtfl_time_class *k = draw_class(tc, 1);
	unsigned d = k->depth;
	uint64_t f[FMAX * DTFL_TIME_MAX], t1[DTFL_TIME_MAX], t2[DTFL_TIME_MAX];
	uint64_t a1[DTFL_TIME_MAX], a2[DTFL_TIME_MAX], s[DTFL_TIME_MAX],
	    x[DTFL_TIME_MAX];
	size_t n = (size_t)pbt_int(tc, 1, FMAX), i;
	bool differs;

	for (i = 0; i < n; i++)
		draw_time(tc, k, f + i * d);
	draw_time(tc, k, t1);
	draw_time(tc, k, t2);
	__dtfl_time_copy(k, a1, t1);
	__dtfl_time_advance_by(k, a1, f, n);
	__dtfl_time_copy(k, a2, t2);
	__dtfl_time_advance_by(k, a2, f, n);

	/* le_advance: t <= advance t F. */
	PBT_ASSERT(LE(k, t1, a1));
	/* advance_le_iff: s beyond F (s = x join f_i for a drawn x). */
	draw_time(tc, k, x);
	i = (size_t)pbt_int(tc, 0, (int64_t)n - 1);
	dtfl_time_join(k, x, f + i * d, s);
	PBT_ASSERT(LE(k, a1, s) == LE(k, t1, s));
	/* Idempotent: a canonical form. */
	__dtfl_time_copy(k, x, a1);
	__dtfl_time_advance_by(k, x, f, n);
	PBT_ASSERT(EQ(k, x, a1));
	/*
	 * advance_eq_iff.  Equal representatives compare alike beyond F
	 * (checked on s).  Different representatives are told apart by
	 * some t_j join f, which is beyond F (the Lean proof's witness).
	 */
	if (EQ(k, a1, a2)) {
		PBT_ASSERT(LE(k, t1, s) == LE(k, t2, s));
	} else {
		differs = false;
		for (i = 0; i < n && !differs; i++) {
			dtfl_time_join(k, t1, f + i * d, x);
			differs = LE(k, t1, x) != LE(k, t2, x);
			dtfl_time_join(k, t2, f + i * d, x);
			differs = differs || LE(k, t1, x) != LE(k, t2, x);
		}
		PBT_ASSERT(differs);
	}
	/* Empty frontier: unchanged. */
	__dtfl_time_copy(k, x, t1);
	__dtfl_time_advance_by(k, x, f, 0);
	PBT_ASSERT(EQ(k, x, t1));
}

/* ------------------------------------------------------------------ */
/* Summaries                                                           */
/* ------------------------------------------------------------------ */

static void
prop_results_in_monotone(pbt_tc *tc)
{
	dtfl_time_class *k = draw_class(tc, 2);
	uint64_t t1[DTFL_TIME_MAX], t2[DTFL_TIME_MAX], x[DTFL_TIME_MAX];
	uint64_t s1[DTFL_TIME_MAX], s2[DTFL_TIME_MAX];
	uint64_t r1[DTFL_TIME_MAX], r2[DTFL_TIME_MAX];

	draw_time(tc, k, t1);
	draw_time(tc, k, x);
	dtfl_time_join(k, t1, x, t2);	/* t1 <= t2 */
	draw_summary(tc, k, s1);
	draw_summary(tc, k, s2);
	/* The default summary is the identity. */
	dtfl_summary_default(k, r1);
	PBT_ASSERT(dtfl_summary_results_in(k, r1, t1, r2));
	PBT_ASSERT(EQ(k, r2, t1));
	PBT_ASSERT(dtfl_summary_less_equal(k, r1, r1));
	/* Monotone in the time. */
	if (!lex_over_pointstamp(k, false) &&
	    dtfl_summary_results_in(k, s1, t1, r1) &&
	    dtfl_summary_results_in(k, s1, t2, r2))
		PBT_ASSERT(LE(k, r1, r2));
	/* Monotone in the summary. */
	if (dtfl_summary_less_equal(k, s1, s2) &&
	    dtfl_summary_results_in(k, s1, t1, r1) &&
	    dtfl_summary_results_in(k, s2, t1, r2))
		PBT_ASSERT(LE(k, r1, r2));
	PBT_ASSERT(dtfl_summary_less_equal(k, s1, s1));
	if (dtfl_summary_less_equal(k, s1, s2) &&
	    dtfl_summary_less_equal(k, s2, s1))
		PBT_ASSERT(__dtfl_words_cmp(s1, s2, k->sdepth) == 0);
}

static void
prop_followed_by(pbt_tc *tc)
{
	dtfl_time_class *k = draw_class(tc, 2);
	uint64_t t[DTFL_TIME_MAX], a[DTFL_TIME_MAX], b[DTFL_TIME_MAX],
	    ab[DTFL_TIME_MAX];
	uint64_t r1[DTFL_TIME_MAX], r2[DTFL_TIME_MAX];
	bool composed, seq;

	draw_time(tc, k, t);
	draw_summary(tc, k, a);
	draw_summary(tc, k, b);
	if (!dtfl_summary_followed_by(k, a, b, ab))
		return;
	composed = dtfl_summary_results_in(k, ab, t, r1);
	seq = dtfl_summary_results_in(k, a, t, r2) &&
	    dtfl_summary_results_in(k, b, r2, r2);
	/* Whatever survives both paths arrives at the same time. */
	if (seq)
		PBT_ASSERT(composed && EQ(k, r1, r2));
	/*
	 * The converse needs exact overflow agreement.  With signed
	 * coordinates a + b can cancel where t + a overflows.  With a
	 * PointStamp, `a` may overflow a coordinate that `b` then truncates
	 * away, which the composed summary never applies (pointstamp.rs
	 * followed_by, as upstream).
	 */
	if (!has_signed(k) && !has_pointstamp(k))
		PBT_ASSERT(composed == seq);
	/* Composing with the identity changes nothing. */
	dtfl_summary_default(k, r1);
	PBT_ASSERT(dtfl_summary_followed_by(k, a, r1, r2));
	PBT_ASSERT(__dtfl_words_cmp(r2, a, k->sdepth) == 0);
}

/* ------------------------------------------------------------------ */
/* Codec and Ord                                                       */
/* ------------------------------------------------------------------ */

static void
prop_codec_roundtrip(pbt_tc *tc)
{
	dtfl_time_class *k = draw_class(tc, 2);
	uint64_t t[DTFL_TIME_MAX], back[DTFL_TIME_MAX];
	__dtfl_vec v;
	size_t used, n, cut;

	__dtfl_vec_init(&v, 1);
	draw_time(tc, k, t);
	n = __dtfl_time_encode(k, t, &v);
	PBT_ASSERT(n == v.len && n == __dtfl_time_encoded_len(k, t));
	PBT_ASSERT(__dtfl_time_decode(k, v.data, v.len, back, &used) == 0);
	PBT_ASSERT(used == n && EQ(k, back, t));
	if (n > 0) {
		cut = (size_t)pbt_int(tc, 0, (int64_t)n - 1);
		PBT_ASSERT(__dtfl_time_decode(k, v.data, cut, back, &used) ==
		    DTFL_EPROTO);
	}
	__dtfl_vec_clear(&v);
	draw_summary(tc, k, t);
	n = __dtfl_summary_encode(k, t, &v);
	PBT_ASSERT(__dtfl_summary_decode(k, v.data, v.len, back, &used) == 0);
	PBT_ASSERT(used == n && __dtfl_words_cmp(back, t, k->sdepth) == 0);
	__dtfl_vec_free(&v);
}

/* Decoding random bytes never reads past `len` and never crashes. */
static void
prop_decode_robust(pbt_tc *tc)
{
	dtfl_time_class *k = draw_class(tc, 2);
	unsigned char src[64], *buf;
	uint64_t t[DTFL_TIME_MAX];
	size_t len = (size_t)pbt_int(tc, 0, sizeof src), i, used = 0;
	int err;

	for (i = 0; i < len; i++)
		src[i] = (unsigned char)(pbt_int(tc, 0, 3) == 0 ? 0 :
		    pbt_int(tc, 0, 255));
	/*
	 * Decode from a heap copy of exactly `len` bytes, so a read past
	 * `len` leaves the allocation and ASan reports it.
	 */
	buf = __dtfl_alloc(len);
	pbt_defer(tc, __dtfl_free, buf);
	if (len > 0)
		memcpy(buf, src, len);
	err = __dtfl_time_decode(k, buf, len, t, &used);
	PBT_ASSERT(err == 0 || err == DTFL_EPROTO || err == DTFL_ERANGE);
	if (err == 0)
		PBT_ASSERT(used <= len);
	err = __dtfl_summary_decode(k, buf, len, t, &used);
	PBT_ASSERT(err == 0 || err == DTFL_EPROTO || err == DTFL_ERANGE);
}

static uint64_t
rd(const unsigned char **p, unsigned n)
{
	uint64_t v = 0;
	unsigned i;

	for (i = 0; i < n; i++)
		v |= (uint64_t)(*p)[i] << (8 * i);
	*p += n;
	return v;
}

#define CMP3(x, y) ((x) < (y) ? -1 : (x) > (y) ? 1 : 0)

/* Rust's Ord on one native coordinate value read from bincode. */
static int
ref_coord(dtfl_coord c, const unsigned char **pa, const unsigned char **pb)
{
	uint64_t x, y, xn, yn;
	int64_t sx, sy;

	switch (c.kind) {
	case DTFL_COORD_SIGNED:
		x = rd(pa, c.bits / 8);
		y = rd(pb, c.bits / 8);
		sx = c.bits == 64 ? (int64_t)x : (int64_t)(x << (64 - c.bits)) >> (64 - c.bits);
		sy = c.bits == 64 ? (int64_t)y : (int64_t)(y << (64 - c.bits)) >> (64 - c.bits);
		return CMP3(sx, sy);
	case DTFL_COORD_DURATION:
		x = rd(pa, 8);
		xn = rd(pa, 4);
		y = rd(pb, 8);
		yn = rd(pb, 4);
		return x != y ? CMP3(x, y) : CMP3(xn, yn);
	default:
		x = rd(pa, c.bits / 8);
		y = rd(pb, c.bits / 8);
		return CMP3(x, y);
	}
}

/*
 * Rust's derived Ord of two values of class k, read from their bincode
 * bytes: structs and tuples field by field, AltNeu as (time, neu),
 * PointStamp as a slice (elements, then length).  Both cursors advance
 * past the whole value.
 */
static int
ref_cmp(const dtfl_time_class *k, const unsigned char **pa,
    const unsigned char **pb)
{
	uint64_t la, lb, i;
	int r = 0, r2;

	switch (k->shape) {
	case DTFL_SHAPE_UNIT:
		return 0;
	case DTFL_SHAPE_TOTAL:
		return ref_coord(k->coord, pa, pb);
	case DTFL_SHAPE_ALTNEU:
		r = ref_coord(k->coord, pa, pb);
		r2 = CMP3(**pa, **pb);
		(*pa)++;
		(*pb)++;
		return r != 0 ? r : r2;
	case DTFL_SHAPE_POINTSTAMP:
		la = rd(pa, 8);
		lb = rd(pb, 8);
		for (i = 0; i < la || i < lb; i++) {
			if (i < la && i < lb) {
				r2 = ref_coord(k->coord, pa, pb);
				r = r != 0 ? r : r2;
			} else if (i < la) {
				*pa += k->coord.kind == DTFL_COORD_DURATION ? 12 : k->coord.bits / 8;
			} else {
				*pb += k->coord.kind == DTFL_COORD_DURATION ? 12 : k->coord.bits / 8;
			}
		}
		return r != 0 ? r : CMP3(la, lb);
	case DTFL_SHAPE_PRODUCT:
	case DTFL_SHAPE_LEX:
		r = ref_cmp(k->a, pa, pb);
		r2 = ref_cmp(k->b, pa, pb);
		return r != 0 ? r : r2;
	}
	return 0;
}

static void
prop_cmp_is_rust_ord(pbt_tc *tc)
{
	dtfl_time_class *k = draw_class(tc, 2);
	uint64_t a[DTFL_TIME_MAX], b[DTFL_TIME_MAX];
	__dtfl_vec va, vb;
	const unsigned char *pa, *pb;
	int want, got;

	draw_time(tc, k, a);
	if (pbt_bool(tc))
		__dtfl_time_copy(k, b, a);
	else
		draw_time(tc, k, b);
	if (pbt_bool(tc) && k->depth > 0)	/* a near miss */
		b[pbt_int(tc, 0, k->depth - 1)] ^= (uint64_t)pbt_int(tc, 0, 1);
	/* The perturbed b must still be a valid time; re-encode to check. */
	__dtfl_vec_init(&va, 1);
	__dtfl_vec_init(&vb, 1);
	__dtfl_time_encode(k, a, &va);
	__dtfl_time_encode(k, b, &vb);
	{
		uint64_t back[DTFL_TIME_MAX];
		size_t used;

		PBT_ASSUME(__dtfl_time_decode(k, vb.data, vb.len, back, &used) ==
		    0 && EQ(k, back, b));
	}
	pa = va.data;
	pb = vb.data;
	want = ref_cmp(k, &pa, &pb);
	got = dtfl_time_cmp(k, a, b);
	PBT_ASSERT(CMP3(got, 0) == want);
	__dtfl_vec_free(&va);
	__dtfl_vec_free(&vb);
}


/*
 * Reference model of upstream PointStamp summaries over u64 coordinates
 * (pointstamp.rs:135-196): variable-length vectors, trailing zeros
 * trimmed.  Used to check the padded implementation in time.c.
 */
typedef struct ps_vec {
	size_t n;
	uint64_t v[DTFL_TIME_MAX];
} ps_vec;

static void
ps_trim(ps_vec *x)
{
	while (x->n > 0 && x->v[x->n - 1] == 0)
		x->n--;
}

/* PointStampSummary::results_in.  Returns false on overflow (None). */
static bool
ps_results_in(bool has_retain, size_t retain, const ps_vec *act,
    const ps_vec *t, ps_vec *out)
{
	size_t tn = t->n, i, m;

	if (has_retain && retain < tn)
		tn = retain;
	m = DTFL_MIN(tn, act->n);
	out->n = 0;
	for (i = 0; i < m; i++) {
		if (act->v[i] > UINT64_MAX - t->v[i])
			return false;
		out->v[out->n++] = t->v[i] + act->v[i];
	}
	for (i = m; i < tn; i++)
		out->v[out->n++] = t->v[i];
	for (i = m; i < act->n; i++)
		out->v[out->n++] = act->v[i];	/* applied to the minimum, 0 */
	ps_trim(out);
	return true;
}

/*
 * The padded implementation agrees with upstream's variable-length
 * results_in for PointStamp<u64> on random summaries and times,
 * including retain truncation and overflow.
 */
static void
prop_pointstamp_matches_upstream(pbt_tc *tc)
{
	unsigned k = (unsigned)pbt_int(tc, 1, 4), i;
	dtfl_time_class *ps = dtfl_time_pointstamp(C_U64, k);
	uint64_t s[DTFL_TIME_MAX + 1], t[DTFL_TIME_MAX], out[DTFL_TIME_MAX];
	ps_vec act, tv, want;
	bool has_retain = pbt_bool(tc), ok_c, ok_m;
	size_t retain = (size_t)pbt_int(tc, 0, k);

	pbt_defer(tc, class_release, ps);
	act.n = (size_t)pbt_int(tc, 0, k);
	for (i = 0; i < act.n; i++)
		act.v[i] = pbt_int(tc, 0, 4) == 0 ? UINT64_MAX - (uint64_t)pbt_int(tc, 0, 2)
		    : (uint64_t)pbt_int(tc, 0, 5);
	tv.n = (size_t)pbt_int(tc, 0, k);
	for (i = 0; i < tv.n; i++)
		tv.v[i] = pbt_int(tc, 0, 4) == 0 ? UINT64_MAX - (uint64_t)pbt_int(tc, 0, 2)
		    : (uint64_t)pbt_int(tc, 0, 5);
	ps_trim(&tv);
	/* Encode for libdtfl: retain word, then actions padded with zeros. */
	memset(s, 0, sizeof s);
	s[0] = has_retain ? (uint64_t)retain : UINT64_MAX;
	for (i = 0; i < act.n; i++)
		s[1 + i] = act.v[i];
	memset(t, 0, sizeof t);
	for (i = 0; i < tv.n; i++)
		t[i] = tv.v[i];
	ok_c = dtfl_summary_results_in(ps, s, t, out);
	ok_m = ps_results_in(has_retain, retain, &act, &tv, &want);
	PBT_ASSERT(ok_c == ok_m);
	if (ok_c) {
		for (i = 0; i < k; i++)
			PBT_ASSERT(out[i] == (i < want.n ? want.v[i] : 0));
	}
}

static const pbt_prop props[] = {
	{ "lattice_laws", prop_lattice_laws, 500 },
	{ "partial_order", prop_partial_order, 500 },
	{ "advance_by", prop_advance_by, 500 },
	{ "results_in_monotone", prop_results_in_monotone, 300 },
	{ "followed_by", prop_followed_by, 300 },
	{ "codec_roundtrip", prop_codec_roundtrip, 300 },
	{ "decode_robust", prop_decode_robust, 300 },
	{ "cmp_is_rust_ord", prop_cmp_is_rust_ord, 500 },
	{ "pointstamp_matches_upstream", prop_pointstamp_matches_upstream, 1000 },
};

PBT_MAIN("time", props)
