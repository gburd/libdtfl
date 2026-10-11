/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_record.c: record descriptors against Rust Ord models.
 *
 * The models are written from Rust's definitions, not from record.c:
 * integers compare natively, Vec<u8>/String compare lexicographically
 * then by length, tuples field by field, Option with None < Some, and
 * Vec element by element then by count.  Values and types are drawn
 * from small alphabets so that ties and shared prefixes are common.
 */
#include "pbt.h"
#include "base/batch_int.h"
#include "dtfl/record.h"

static int
sgn64(int64_t a, int64_t b)
{
	return (a > b) - (a < b);
}

static int
sgnu64(uint64_t a, uint64_t b)
{
	return (a > b) - (a < b);
}

/* Signed integers: encodings sort as the native values and round-trip. */
static void
prop_int_signed(pbt_tc *tc)
{
	static const int64_t lo[4] = { INT8_MIN, INT16_MIN, INT32_MIN, INT64_MIN };
	static const int64_t hi[4] = { INT8_MAX, INT16_MAX, INT32_MAX, INT64_MAX };
	const dtfl_type *ts[4] = { dtfl_type_i8(), dtfl_type_i16(),
	    dtfl_type_i32(), dtfl_type_i64() };
	int w = (int)pbt_int(tc, 0, 3);
	int64_t a = pbt_int(tc, lo[w], hi[w]), b = pbt_int(tc, lo[w], hi[w]);
	unsigned char ea[8], eb[8];
	size_t n = 0;
	int64_t ra = 0;

	switch (w) {
	case 0:
		n = dtfl_put_i8(ea, (int8_t)a);
		(void)dtfl_put_i8(eb, (int8_t)b);
		ra = dtfl_get_i8(ea);
		break;
	case 1:
		n = dtfl_put_i16(ea, (int16_t)a);
		(void)dtfl_put_i16(eb, (int16_t)b);
		ra = dtfl_get_i16(ea);
		break;
	case 2:
		n = dtfl_put_i32(ea, (int32_t)a);
		(void)dtfl_put_i32(eb, (int32_t)b);
		ra = dtfl_get_i32(ea);
		break;
	default:
		n = dtfl_put_i64(ea, a);
		(void)dtfl_put_i64(eb, b);
		ra = dtfl_get_i64(ea);
		break;
	}
	PBT_ASSERT(n == ts[w]->fixed_size);
	PBT_ASSERT(ra == a);
	PBT_ASSERT(dtfl_type_cmp(ts[w], ea, n, eb, n) == sgn64(a, b));
	PBT_ASSERT(dtfl_type_value_len(ts[w], ea, n) == n);
}

/* Unsigned integers: as above. */
static void
prop_int_unsigned(pbt_tc *tc)
{
	static const uint64_t mx[4] = { UINT8_MAX, UINT16_MAX, UINT32_MAX,
	    UINT64_MAX };
	const dtfl_type *ts[4] = { dtfl_type_u8(), dtfl_type_u16(),
	    dtfl_type_u32(), dtfl_type_u64() };
	int w = (int)pbt_int(tc, 0, 3);
	uint64_t a = (uint64_t)pbt_int(tc, INT64_MIN, INT64_MAX) & mx[w];
	uint64_t b = (uint64_t)pbt_int(tc, INT64_MIN, INT64_MAX) & mx[w];
	unsigned char ea[8], eb[8];
	size_t n = 0;
	uint64_t ra = 0;

	switch (w) {
	case 0:
		n = dtfl_put_u8(ea, (uint8_t)a);
		(void)dtfl_put_u8(eb, (uint8_t)b);
		ra = dtfl_get_u8(ea);
		break;
	case 1:
		n = dtfl_put_u16(ea, (uint16_t)a);
		(void)dtfl_put_u16(eb, (uint16_t)b);
		ra = dtfl_get_u16(ea);
		break;
	case 2:
		n = dtfl_put_u32(ea, (uint32_t)a);
		(void)dtfl_put_u32(eb, (uint32_t)b);
		ra = dtfl_get_u32(ea);
		break;
	default:
		n = dtfl_put_u64(ea, a);
		(void)dtfl_put_u64(eb, b);
		ra = dtfl_get_u64(ea);
		break;
	}
	PBT_ASSERT(n == ts[w]->fixed_size);
	PBT_ASSERT(ra == a);
	PBT_ASSERT(dtfl_type_cmp(ts[w], ea, n, eb, n) == sgnu64(a, b));
}

/* ------------------------------------------------------------------ */
/* Composite model                                                     */
/* ------------------------------------------------------------------ */

enum { M_U8, M_I16, M_STR, M_UNIT, M_TUPLE, M_OPT, M_VEC };

typedef struct mtype {
	int kind;
	size_t n;			/* tuple arity */
	struct mtype *kid[3];
	const dtfl_type *dt;		/* owned reference */
} mtype;

typedef struct mval {
	int64_t i;			/* integer, option tag, vec count */
	unsigned char s[6];
	size_t slen;
	struct mval *kid[3];
} mval;

/* Per-case pools; a case uses far fewer than these. */
static mtype tpool[64];
static size_t ntp;
static mval vpool[2048];
static size_t nvp;

/* Draws a type tree; build_type() makes its descriptors after all draws. */
static mtype *
gen_type(pbt_tc *tc, int depth)
{
	mtype *t;
	size_t i;

	PBT_ASSUME(ntp < DTFL_ARRAY_LEN(tpool));
	t = &tpool[ntp++];
	memset(t, 0, sizeof *t);
	t->kind = (int)pbt_int(tc, 0, depth > 0 ? M_VEC : M_UNIT);
	if (t->kind == M_TUPLE)
		t->n = (size_t)pbt_int(tc, 0, 3);
	else if (t->kind == M_OPT || t->kind == M_VEC)
		t->n = 1;
	for (i = 0; i < t->n; i++)
		t->kid[i] = gen_type(tc, depth - 1);
	return t;
}

/*
 * Builds the descriptors of a type tree, children first.  Called only
 * after the case's last draw, so that a draw that unwinds (overrun,
 * assume) cannot leak them.
 */
static void
build_type(mtype *t)
{
	const dtfl_type *f[3];
	size_t i;

	for (i = 0; i < t->n; i++) {
		build_type(t->kid[i]);
		f[i] = t->kid[i]->dt;
	}
	switch (t->kind) {
	case M_U8: t->dt = dtfl_type_u8(); break;
	case M_I16: t->dt = dtfl_type_i16(); break;
	case M_STR: t->dt = dtfl_type_string(); break;
	case M_UNIT: t->dt = dtfl_type_unit(); break;
	case M_TUPLE: t->dt = dtfl_type_tuple(f, t->n); break;
	case M_OPT: t->dt = dtfl_type_option(f[0]); break;
	default: t->dt = dtfl_type_vec(f[0]); break;
	}
}

/* Frees the descriptors of a type tree; each holds its children. */
static void
free_type(mtype *t)
{
	size_t i;

	for (i = 0; i < t->n; i++)
		free_type(t->kid[i]);
	dtfl_type_free(t->dt);
}

static mval *
gen_val(pbt_tc *tc, const mtype *t)
{
	mval *v;
	size_t i, n = 0;

	PBT_ASSUME(nvp < DTFL_ARRAY_LEN(vpool));
	v = &vpool[nvp++];
	memset(v, 0, sizeof *v);
	switch (t->kind) {
	case M_U8: v->i = pbt_int(tc, 0, 3); break;
	case M_I16: v->i = pbt_bool(tc) ? pbt_int(tc, -2, 2) :
	    pbt_int(tc, INT16_MIN, INT16_MAX); break;
	case M_STR:
		v->slen = (size_t)pbt_int(tc, 0, sizeof v->s);
		for (i = 0; i < v->slen; i++)
			v->s[i] = (unsigned char)pbt_int(tc, 0, 2) * 0x7f;
		break;
	case M_UNIT: break;
	case M_TUPLE: n = t->n; break;
	case M_OPT: v->i = pbt_int(tc, 0, 1); n = (size_t)v->i; break;
	default: v->i = pbt_int(tc, 0, 3); n = (size_t)v->i; break;
	}
	for (i = 0; i < n; i++)
		v->kid[i] = gen_val(tc, t->kind == M_TUPLE ? t->kid[i] : t->kid[0]);
	return v;
}

/* Encodes v at p through the public put functions; returns bytes written. */
static size_t
encode(const mtype *t, const mval *v, unsigned char *p)
{
	size_t n = 0, i, k = 0;

	switch (t->kind) {
	case M_U8: return dtfl_put_u8(p, (uint8_t)v->i);
	case M_I16: return dtfl_put_i16(p, (int16_t)v->i);
	case M_STR: return dtfl_put_bytes(p, v->s, v->slen);
	case M_UNIT: return 0;
	case M_TUPLE: k = t->n; break;
	case M_OPT: p[n++] = (unsigned char)v->i; k = (size_t)v->i; break;
	default: n = dtfl_put_u64(p, (uint64_t)v->i); k = (size_t)v->i; break;
	}
	for (i = 0; i < k; i++)
		n += encode(t->kind == M_TUPLE ? t->kid[i] : t->kid[0],
		    v->kid[i], p + n);
	return n;
}

/* Rust's derived Ord for the modelled type. */
static int
model_cmp(const mtype *t, const mval *a, const mval *b)
{
	size_t i, m;
	int c;

	switch (t->kind) {
	case M_U8:
	case M_I16:
		return sgn64(a->i, b->i);
	case M_STR:
		m = a->slen < b->slen ? a->slen : b->slen;
		for (i = 0; i < m; i++)
			if (a->s[i] != b->s[i])
				return a->s[i] < b->s[i] ? -1 : 1;
		return (a->slen > b->slen) - (a->slen < b->slen);
	case M_UNIT:
		return 0;
	case M_TUPLE:
		for (i = 0; i < t->n; i++)
			if ((c = model_cmp(t->kid[i], a->kid[i], b->kid[i])) != 0)
				return c;
		return 0;
	case M_OPT:
		if (a->i != b->i)
			return a->i < b->i ? -1 : 1;
		return a->i == 0 ? 0 : model_cmp(t->kid[0], a->kid[0], b->kid[0]);
	default:
		for (i = 0; i < (size_t)a->i && i < (size_t)b->i; i++)
			if ((c = model_cmp(t->kid[0], a->kid[i], b->kid[i])) != 0)
				return c;
		return sgn64(a->i, b->i);
	}
}

/*
 * (T1, T2) for random T1, T2: dtfl_type_cmp equals the model, is
 * antisymmetric, value_len recovers the encoded length, and
 * tuple_offset finds both fields.
 */
static void
prop_composite_cmp(pbt_tc *tc)
{
	mtype top;
	mval va, vb, *a = &va, *b = &vb;
	unsigned char ea[4096], eb[4096];
	size_t na, nb, a0, off, flen;
	bool ok[7];
	int c;

	ntp = nvp = 0;
	memset(&top, 0, sizeof top);
	top.kind = M_TUPLE;
	top.n = 2;
	top.kid[0] = gen_type(tc, 2);
	top.kid[1] = gen_type(tc, 2);
	memset(a, 0, sizeof *a);
	memset(b, 0, sizeof *b);
	a->kid[0] = gen_val(tc, top.kid[0]);
	a->kid[1] = gen_val(tc, top.kid[1]);
	b->kid[0] = pbt_bool(tc) ? a->kid[0] : gen_val(tc, top.kid[0]);
	b->kid[1] = gen_val(tc, top.kid[1]);
	build_type(&top);
	a0 = encode(top.kid[0], a->kid[0], ea);
	na = a0 + encode(top.kid[1], a->kid[1], ea + a0);
	nb = encode(&top, b, eb);
	PBT_ASSERT(na <= sizeof ea && nb <= sizeof eb);

	c = dtfl_type_cmp(top.dt, ea, na, eb, nb);
	ok[0] = c == model_cmp(&top, a, b);
	ok[1] = dtfl_type_cmp(top.dt, eb, nb, ea, na) == -c;
	ok[2] = dtfl_type_cmp(top.dt, ea, na, ea, na) == 0;
	ok[3] = dtfl_type_value_len(top.dt, ea, na) == na;
	ok[4] = dtfl_type_value_len(top.kid[0]->dt, ea, na) == a0;
	off = dtfl_type_tuple_offset(top.dt, ea, na, 1, &flen);
	ok[5] = off == a0 && flen == na - a0;
	ok[6] = dtfl_type_hash(top.dt, ea, na) == __dtfl_fnv1a(ea, na);
	free_type(&top);
	PBT_ASSERT(ok[0]);	/* cmp == Rust Ord model */
	PBT_ASSERT(ok[1]);
	PBT_ASSERT(ok[2]);
	PBT_ASSERT(ok[3]);
	PBT_ASSERT(ok[4]);
	PBT_ASSERT(ok[5]);
	PBT_ASSERT(ok[6]);
}

/*
 * Random bytes into every parser: value_len, cmp, fmt and tuple_offset
 * on a random type, and batch decode.  The only check is that they stay
 * in bounds (ASan) and that a reported length fits the input.
 */
static void
prop_parse_robust(pbt_tc *tc)
{
	static FILE *devnull;
	static unsigned char draw[2][64];
	mtype *t;
	unsigned char *buf, *buf2;
	size_t n, n2, i, len, flen, off;
	__dtfl_batch *b;

	if (devnull == NULL)
		devnull = fopen("/dev/null", "w");
	ntp = nvp = 0;
	t = gen_type(tc, 3);
	n = (size_t)pbt_int(tc, 0, 64);
	n2 = (size_t)pbt_int(tc, 0, 64);
	for (i = 0; i < n; i++)
		draw[0][i] = (unsigned char)(pbt_bool(tc) ? pbt_int(tc, 0, 2) :
		    pbt_int(tc, 0, 255));
	for (i = 0; i < n2; i++)
		draw[1][i] = (unsigned char)pbt_int(tc, 0, 255);
	/* Exact-size heap copies so ASan sees any overread. */
	buf = __dtfl_memdup(draw[0], n);
	buf2 = __dtfl_memdup(draw[1], n2);
	build_type(t);
	len = dtfl_type_value_len(t->dt, buf, n);
	(void)dtfl_type_cmp(t->dt, buf, n, buf2, n2);
	(void)dtfl_type_cmp(t->dt, buf2, n2, buf, n);
	if (devnull != NULL)
		(void)dtfl_type_fmt(t->dt, devnull, buf, n);
	off = 0;
	flen = 0;
	for (i = 0; t->kind == M_TUPLE && i < t->n && off != SIZE_MAX; i++)
		off = dtfl_type_tuple_offset(t->dt, buf, n, i, &flen);
	b = __dtfl_batch_decode(buf, n);
	n2 = b != NULL ? __dtfl_batch_encoded_len(b) : n;
	__dtfl_batch_release(b);
	i = (size_t)dtfl_type_cmp(t->dt, buf, n, buf, n);
	__dtfl_free(buf);
	__dtfl_free(buf2);
	free_type(t);
	PBT_ASSERT(len == SIZE_MAX || len <= n);
	PBT_ASSERT(off == SIZE_MAX || off + flen <= n);
	PBT_ASSERT(n2 == n);
	PBT_ASSERT(i == 0);
}

static const pbt_prop props[] = {
	{ "int_signed", prop_int_signed, 1000 },
	{ "int_unsigned", prop_int_unsigned, 1000 },
	{ "composite_cmp", prop_composite_cmp, 1000 },
	{ "parse_robust", prop_parse_robust, 1000 },
};

PBT_MAIN("record", props)
