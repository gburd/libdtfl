/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/record.c: record type descriptors and their encodings (PLAN D1).
 *
 * Built-in descriptors are static, carry internal == NULL, and are never
 * freed.  Composite descriptors and those made by dtfl_type_new() live
 * in a struct rtype whose public `pub.internal` points back at itself;
 * that is the only way the library tells them apart.  An rtype owns one
 * reference to each of its field descriptors.
 *
 * cmp on every built-in and composite reproduces Rust's derived Ord for
 * the type it models: integers through order-preserving big-endian
 * encodings, Vec<u8> and String lexicographically over the payload and
 * then by length, tuples field by field, Option with None < Some, and
 * Vec element by element then by count.  cmp never reads outside the
 * spans it is given, even on malformed bytes; it then falls back to
 * memcmp-then-length on the unparsed remainder so the result stays a
 * deterministic total order.
 */
#include "base_int.h"

#include "dtfl/record.h"

enum rkind { K_USER, K_TUPLE, K_OPTION, K_VEC };

typedef struct rtype {
	dtfl_type pub;		/* pub.internal == this */
	__dtfl_arc ref;
	enum rkind kind;
	size_t min;		/* smallest encoded value */
	bool zw;		/* every value is zero bytes */
	bool tail;		/* a value runs to the end of its span */
	bool mcmp;		/* byte order == logical order (memcmp works) */
	size_t n;		/* fields (tuple), 1 (option, vec), 0 (user) */
	const dtfl_type *fields[];
} rtype;

#define RT(t) ((rtype *)(t)->internal)

/* ------------------------------------------------------------------ */
/* Encodings                                                           */
/* ------------------------------------------------------------------ */

static void
be_put(unsigned char *p, uint64_t v, size_t w)
{
	while (w-- > 0) {
		p[w] = (unsigned char)v;
		v >>= 8;
	}
}

static uint64_t
be_get(const unsigned char *p, size_t w)
{
	uint64_t v = 0;
	size_t i;

	for (i = 0; i < w; i++)
		v = v << 8 | p[i];
	return v;
}

/* Sign bit of a w-byte integer; flipping it makes two's complement sort as unsigned. */
#define SIGN(w) (UINT64_C(1) << (8 * (w) - 1))

/* Decodes a w-byte sign-flipped value and sign-extends it. */
static int64_t
be_get_signed(const unsigned char *p, size_t w)
{
	uint64_t u = be_get(p, w) ^ SIGN(w);

	if (w < 8 && (u & SIGN(w)) != 0)
		u |= ~((SIGN(w) << 1) - 1);
	return (int64_t)u;
}

size_t dtfl_put_u8(void *buf, uint8_t v) { be_put(buf, v, 1); return 1; }
size_t dtfl_put_u16(void *buf, uint16_t v) { be_put(buf, v, 2); return 2; }
size_t dtfl_put_u32(void *buf, uint32_t v) { be_put(buf, v, 4); return 4; }
size_t dtfl_put_u64(void *buf, uint64_t v) { be_put(buf, v, 8); return 8; }
size_t dtfl_put_i8(void *buf, int8_t v) { be_put(buf, (uint64_t)v ^ SIGN(1), 1); return 1; }
size_t dtfl_put_i16(void *buf, int16_t v) { be_put(buf, (uint64_t)v ^ SIGN(2), 2); return 2; }
size_t dtfl_put_i32(void *buf, int32_t v) { be_put(buf, (uint64_t)v ^ SIGN(4), 4); return 4; }
size_t dtfl_put_i64(void *buf, int64_t v) { be_put(buf, (uint64_t)v ^ SIGN(8), 8); return 8; }
uint8_t dtfl_get_u8(const void *buf) { return (uint8_t)be_get(buf, 1); }
uint16_t dtfl_get_u16(const void *buf) { return (uint16_t)be_get(buf, 2); }
uint32_t dtfl_get_u32(const void *buf) { return (uint32_t)be_get(buf, 4); }
uint64_t dtfl_get_u64(const void *buf) { return be_get(buf, 8); }
int8_t dtfl_get_i8(const void *buf) { return (int8_t)be_get_signed(buf, 1); }
int16_t dtfl_get_i16(const void *buf) { return (int16_t)be_get_signed(buf, 2); }
int32_t dtfl_get_i32(const void *buf) { return (int32_t)be_get_signed(buf, 4); }
int64_t dtfl_get_i64(const void *buf) { return be_get_signed(buf, 8); }

/* Writes the u64 big-endian length and the payload; returns 8 + n. */
size_t
dtfl_put_bytes(void *buf, const void *p, size_t n)
{
	be_put(buf, (uint64_t)n, 8);
	if (n != 0)
		memcpy((unsigned char *)buf + 8, p, n);
	return 8 + n;
}

/* The caller has already validated the value (dtfl_type_value_len). */
const void *
dtfl_get_bytes(const void *buf, size_t *n)
{
	*n = (size_t)be_get(buf, 8);
	return (const unsigned char *)buf + 8;
}

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

/* memcmp over the common prefix, then length: the NULL-cmp default. */
static int
cmp_raw(const void *a, size_t alen, const void *b, size_t blen)
{
	size_t m = DTFL_MIN(alen, blen);
	int c = m != 0 ? memcmp(a, b, m) : 0;

	if (c != 0)
		return c < 0 ? -1 : 1;
	return (alen > blen) - (alen < blen);
}

static int
fmt_hex(FILE *f, const unsigned char *p, size_t len)
{
	int r, n;
	size_t i;

	if ((n = fprintf(f, "0x")) < 0)
		return n;
	for (i = 0; i < len; i++) {
		if ((r = fprintf(f, "%02x", p[i])) < 0)
			return r;
		n += r;
	}
	return n;
}

/* Payload span of a bytes/string value, clamped to what is readable. */
static const unsigned char *
payload(const unsigned char *p, size_t len, size_t *n)
{
	uint64_t m;

	if (len < 8) {
		*n = 0;
		return p + len;
	}
	m = be_get(p, 8);
	*n = m > len - 8 ? len - 8 : (size_t)m;
	return p + 8;
}

static bool
is_tail(const dtfl_type *t)
{
	if (t->internal != NULL)
		return RT(t)->tail;
	return t->fixed_size == 0 && t->len == NULL;
}

/* ------------------------------------------------------------------ */
/* Built-ins                                                           */
/* ------------------------------------------------------------------ */

static size_t
unit_len(const dtfl_type *t, const void *p, size_t avail)
{
	return 0;
}

static int
unit_fmt(const dtfl_type *t, FILE *f, const void *p, size_t len)
{
	return len == 0 ? fprintf(f, "()") : fmt_hex(f, p, len);
}

static int
uint_fmt(const dtfl_type *t, FILE *f, const void *p, size_t len)
{
	if (len != t->fixed_size)
		return fmt_hex(f, p, len);
	return fprintf(f, "%llu", (unsigned long long)be_get(p, len));
}

static int
sint_fmt(const dtfl_type *t, FILE *f, const void *p, size_t len)
{
	if (len != t->fixed_size)
		return fmt_hex(f, p, len);
	return fprintf(f, "%lld", (long long)be_get_signed(p, len));
}

/* A u64 big-endian length, then that many bytes. */
static size_t
bytes_len(const dtfl_type *t, const void *p, size_t avail)
{
	uint64_t n;

	if (avail < 8)
		return SIZE_MAX;
	n = be_get(p, 8);
	if (n > avail - 8)
		return SIZE_MAX;
	return 8 + (size_t)n;
}

/* Rust Vec<u8> and String Ord: lexicographic over the bytes, then length. */
static int
bytes_cmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	const unsigned char *pa, *pb;
	size_t na, nb;

	pa = payload(a, alen, &na);
	pb = payload(b, blen, &nb);
	return cmp_raw(pa, na, pb, nb);
}

static int
bytes_fmt(const dtfl_type *t, FILE *f, const void *p, size_t len)
{
	if (bytes_len(t, p, len) != len)
		return fmt_hex(f, p, len);
	return fmt_hex(f, (const unsigned char *)p + 8, len - 8);
}

/* Quoted, with ", \ and every byte outside printable ASCII escaped. */
static int
string_fmt(const dtfl_type *t, FILE *f, const void *p, size_t len)
{
	const unsigned char *s;
	size_t n, i;
	int r, out;

	if (bytes_len(t, p, len) != len)
		return fmt_hex(f, p, len);
	s = payload(p, len, &n);
	if ((out = fprintf(f, "\"")) < 0)
		return out;
	for (i = 0; i < n; i++) {
		unsigned c = s[i];

		if (c == '"' || c == '\\')
			r = fprintf(f, "\\%c", c);
		else if (c == '\n')
			r = fprintf(f, "\\n");
		else if (c == '\t')
			r = fprintf(f, "\\t");
		else if (c == '\r')
			r = fprintf(f, "\\r");
		else if (c < 0x20 || c > 0x7e)
			r = fprintf(f, "\\x%02x", c);
		else
			r = fprintf(f, "%c", c);
		if (r < 0)
			return r;
		out += r;
	}
	if ((r = fprintf(f, "\"")) < 0)
		return r;
	return out + r;
}

#define UINT_T(nm, w) { nm, w, NULL, NULL, NULL, uint_fmt, NULL, NULL, NULL }
#define SINT_T(nm, w) { nm, w, NULL, NULL, NULL, sint_fmt, NULL, NULL, NULL }

static const dtfl_type T_UNIT =
    { "unit", 0, unit_len, NULL, NULL, unit_fmt, NULL, NULL, NULL };
static const dtfl_type T_U8 = UINT_T("u8", 1);
static const dtfl_type T_U16 = UINT_T("u16", 2);
static const dtfl_type T_U32 = UINT_T("u32", 4);
static const dtfl_type T_U64 = UINT_T("u64", 8);
static const dtfl_type T_I8 = SINT_T("i8", 1);
static const dtfl_type T_I16 = SINT_T("i16", 2);
static const dtfl_type T_I32 = SINT_T("i32", 4);
static const dtfl_type T_I64 = SINT_T("i64", 8);
static const dtfl_type T_BYTES =
    { "bytes", 0, bytes_len, bytes_cmp, NULL, bytes_fmt, NULL, NULL, NULL };
static const dtfl_type T_STRING =
    { "string", 0, bytes_len, bytes_cmp, NULL, string_fmt, NULL, NULL, NULL };

const dtfl_type *dtfl_type_unit(void) { return &T_UNIT; }
const dtfl_type *dtfl_type_u8(void) { return &T_U8; }
const dtfl_type *dtfl_type_u16(void) { return &T_U16; }
const dtfl_type *dtfl_type_u32(void) { return &T_U32; }
const dtfl_type *dtfl_type_u64(void) { return &T_U64; }
const dtfl_type *dtfl_type_i8(void) { return &T_I8; }
const dtfl_type *dtfl_type_i16(void) { return &T_I16; }
const dtfl_type *dtfl_type_i32(void) { return &T_I32; }
const dtfl_type *dtfl_type_i64(void) { return &T_I64; }
const dtfl_type *dtfl_type_bytes(void) { return &T_BYTES; }
const dtfl_type *dtfl_type_string(void) { return &T_STRING; }

/* Smallest encoded value of t; bounds element counts on parse. */
static size_t
type_min(const dtfl_type *t)
{
	if (t->fixed_size != 0)
		return t->fixed_size;
	if (t == &T_BYTES || t == &T_STRING)
		return 8;
	if (t->internal != NULL)
		return RT(t)->min;
	return 0;
}

static bool
type_zw(const dtfl_type *t)
{
	return t == &T_UNIT || (t->internal != NULL && RT(t)->zw);
}

/*
 * True when the type's byte encoding sorts identically to its logical
 * order, so a plain memcmp (cmp_raw) reproduces dtfl_type_cmp.  Holds for
 * unit (zero bytes), the big-endian / sign-flipped integers, and tuples
 * and options built only from such types (every tuple field but the last
 * must be fixed width, so an equal prefix means an equal byte span).  It
 * does not hold for bytes/string or vec (length/count prefixed, so the
 * encoding would order by length first) or for user types (their cmp is
 * authoritative).  Setting cmp = NULL on a memcmp-ordered composite lets
 * dtfl_type_cmp take the cmp_raw fast path instead of recursing.
 */
static bool
type_mcmp(const dtfl_type *t)
{
	if (t == &T_UNIT || t == &T_U8 || t == &T_U16 || t == &T_U32 ||
	    t == &T_U64 || t == &T_I8 || t == &T_I16 || t == &T_I32 ||
	    t == &T_I64)
		return true;
	if (t->internal != NULL)
		return RT(t)->mcmp;
	return false;
}

/* ------------------------------------------------------------------ */
/* Operations                                                          */
/* ------------------------------------------------------------------ */

size_t
dtfl_type_value_len(const dtfl_type *t, const void *p, size_t avail)
{
	size_t n;

	if (t->fixed_size != 0)
		return t->fixed_size <= avail ? t->fixed_size : SIZE_MAX;
	if (t->len == NULL)
		return avail;
	n = t->len(t, p, avail);
	return n <= avail ? n : SIZE_MAX;
}

int
dtfl_type_cmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	int c;

	if (t->cmp == NULL)
		return cmp_raw(a, alen, b, blen);
	c = t->cmp(t, a, alen, b, blen);
	return (c > 0) - (c < 0);
}

int
dtfl_type_fmt(const dtfl_type *t, FILE *f, const void *p, size_t len)
{
	if (t->fmt == NULL)
		return fmt_hex(f, p, len);
	return t->fmt(t, f, p, len);
}

/* ------------------------------------------------------------------ */
/* Composites                                                          */
/* ------------------------------------------------------------------ */

/* Sum of the field lengths, or SIZE_MAX if a field does not parse. */
static size_t
tuple_len(const dtfl_type *t, const void *p, size_t avail)
{
	const rtype *r = RT(t);
	size_t off = 0, i, n;

	for (i = 0; i < r->n; i++) {
		n = dtfl_type_value_len(r->fields[i],
		    (const unsigned char *)p + off, avail - off);
		if (n == SIZE_MAX)
			return SIZE_MAX;
		off += n;
	}
	return off;
}

static int
tuple_cmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	const rtype *r = RT(t);
	const unsigned char *pa = a, *pb = b;
	size_t i, na, nb;
	int c;

	for (i = 0; i < r->n; i++) {
		na = dtfl_type_value_len(r->fields[i], pa, alen);
		nb = dtfl_type_value_len(r->fields[i], pb, blen);
		if (na == SIZE_MAX || nb == SIZE_MAX)
			break;
		if ((c = dtfl_type_cmp(r->fields[i], pa, na, pb, nb)) != 0)
			return c;
		pa += na;
		alen -= na;
		pb += nb;
		blen -= nb;
	}
	return cmp_raw(pa, alen, pb, blen);
}

static int
tuple_fmt(const dtfl_type *t, FILE *f, const void *p, size_t len)
{
	const rtype *r = RT(t);
	size_t i, off = 0, n;
	int out, w;

	if (tuple_len(t, p, len) != len)
		return fmt_hex(f, p, len);
	if ((out = fprintf(f, "(")) < 0)
		return out;
	for (i = 0; i < r->n; i++) {
		n = dtfl_type_value_len(r->fields[i],
		    (const unsigned char *)p + off, len - off);
		if (i > 0) {
			if ((w = fprintf(f, ", ")) < 0)
				return w;
			out += w;
		}
		if ((w = dtfl_type_fmt(r->fields[i], f,
		    (const unsigned char *)p + off, n)) < 0)
			return w;
		out += w;
		off += n;
	}
	/* Rust prints a one-tuple as (x,). */
	if (r->n == 1 && (w = fprintf(f, ",")) >= 0)
		out += w;
	if ((w = fprintf(f, ")")) < 0)
		return w;
	return out + w;
}

static size_t
option_len(const dtfl_type *t, const void *p, size_t avail)
{
	const unsigned char *s = p;
	size_t n;

	if (avail == 0 || s[0] > 1)
		return SIZE_MAX;
	if (s[0] == 0)
		return 1;
	n = dtfl_type_value_len(RT(t)->fields[0], s + 1, avail - 1);
	return n == SIZE_MAX ? SIZE_MAX : 1 + n;
}

/* Rust's derived Ord on Option: None < Some, then the values. */
static int
option_cmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	const unsigned char *pa = a, *pb = b;

	if (alen == 0 || blen == 0 || pa[0] != pb[0] || pa[0] != 1)
		return cmp_raw(a, alen, b, blen);
	return dtfl_type_cmp(RT(t)->fields[0], pa + 1, alen - 1, pb + 1,
	    blen - 1);
}

static int
option_fmt(const dtfl_type *t, FILE *f, const void *p, size_t len)
{
	const unsigned char *s = p;
	int out, w;

	if (option_len(t, p, len) != len)
		return fmt_hex(f, p, len);
	if (s[0] == 0)
		return fprintf(f, "None");
	if ((out = fprintf(f, "Some(")) < 0)
		return out;
	if ((w = dtfl_type_fmt(RT(t)->fields[0], f, s + 1, len - 1)) < 0)
		return w;
	out += w;
	if ((w = fprintf(f, ")")) < 0)
		return w;
	return out + w;
}

/*
 * Element count of a vec value, or false if the count cannot fit in
 * avail.  A zero-width element type (unit) holds any count in 8 bytes.
 * For element types whose values may be zero bytes but are not
 * zero-width, the count is capped at the payload byte count to bound
 * the parse loop; a dedicated min-size callback would lift that cap.
 */
static bool
vec_count(const dtfl_type *t, const unsigned char *p, size_t avail,
    uint64_t *count)
{
	const dtfl_type *e = RT(t)->fields[0];
	size_t m = type_min(e);

	if (avail < 8)
		return false;
	*count = be_get(p, 8);
	if (type_zw(e))
		return true;
	return *count <= (uint64_t)(avail - 8) / (m != 0 ? m : 1);
}

static size_t
vec_len(const dtfl_type *t, const void *p, size_t avail)
{
	const dtfl_type *e = RT(t)->fields[0];
	const unsigned char *s = p;
	uint64_t count, i;
	size_t off = 8, n;

	if (!vec_count(t, s, avail, &count))
		return SIZE_MAX;
	if (type_zw(e))
		return 8;
	for (i = 0; i < count; i++) {
		n = dtfl_type_value_len(e, s + off, avail - off);
		if (n == SIZE_MAX)
			return SIZE_MAX;
		off += n;
	}
	return off;
}

/* Rust's Vec Ord: element by element, then by count. */
static int
vec_cmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	const dtfl_type *e = RT(t)->fields[0];
	const unsigned char *pa = a, *pb = b;
	uint64_t ca, cb, i;
	size_t na, nb;
	int c;

	if (!vec_count(t, pa, alen, &ca) || !vec_count(t, pb, blen, &cb))
		return cmp_raw(a, alen, b, blen);
	pa += 8;
	alen -= 8;
	pb += 8;
	blen -= 8;
	if (!type_zw(e)) {
		for (i = 0; i < ca && i < cb; i++) {
			na = dtfl_type_value_len(e, pa, alen);
			nb = dtfl_type_value_len(e, pb, blen);
			if (na == SIZE_MAX || nb == SIZE_MAX)
				return cmp_raw(pa, alen, pb, blen);
			if ((c = dtfl_type_cmp(e, pa, na, pb, nb)) != 0)
				return c;
			pa += na;
			alen -= na;
			pb += nb;
			blen -= nb;
		}
	}
	if (ca != cb)
		return ca < cb ? -1 : 1;
	return cmp_raw(pa, alen, pb, blen);
}

#define VEC_FMT_MAX 4096

static int
vec_fmt(const dtfl_type *t, FILE *f, const void *p, size_t len)
{
	const dtfl_type *e = RT(t)->fields[0];
	const unsigned char *s = p;
	uint64_t count, i;
	size_t off = 8, n;
	int out, w;

	if (vec_len(t, p, len) != len)
		return fmt_hex(f, p, len);
	(void)vec_count(t, s, len, &count);
	if ((out = fprintf(f, "[")) < 0)
		return out;
	for (i = 0; i < count; i++) {
		n = dtfl_type_value_len(e, s + off, len - off);
		if (i > 0) {
			if ((w = fprintf(f, ", ")) < 0)
				return w;
			out += w;
		}
		/*
		 * A vec of zero-width elements can claim 2^64 of them in 8
		 * bytes; print the first VEC_FMT_MAX and elide the rest rather
		 * than loop for ever.
		 */
		if (i == VEC_FMT_MAX) {
			if ((w = fprintf(f, "...")) < 0)
				return w;
			out += w;
			break;
		}
		if ((w = dtfl_type_fmt(e, f, s + off, n)) < 0)
			return w;
		out += w;
		off += n;
	}
	if ((w = fprintf(f, "]")) < 0)
		return w;
	return out + w;
}

/* Allocates an rtype with n field slots; the caller fills it in. */
static rtype *
rtype_new(enum rkind kind, size_t n)
{
	rtype *r = __dtfl_calloc(1, sizeof(rtype) +
	    __dtfl_mul_size(n, sizeof(const dtfl_type *)));

	__dtfl_arc_init(&r->ref);
	r->kind = kind;
	r->n = n;
	r->pub.internal = r;
	r->pub.ctx = (void *)r->fields;
	return r;
}

/* Tuple: fields concatenated; every field but the last self-delimits. */
const dtfl_type *
dtfl_type_tuple(const dtfl_type *const *fields, size_t n)
{
	rtype *r = rtype_new(K_TUPLE, n);
	bool fixed = true;
	bool mcmp = true;
	size_t i, fs = 0;

	r->zw = true;
	for (i = 0; i < n; i++) {
		const dtfl_type *f = fields[i];

		if (i + 1 < n && is_tail(f))
			__dtfl_panic("dtfl_type_tuple: field %zu (%s) does not "
			    "self-delimit and is not last", i,
			    f->name != NULL ? f->name : "?");
		r->fields[i] = dtfl_type_retain(f);
		r->min += type_min(f);
		r->zw = r->zw && type_zw(f);
		fixed = fixed && f->fixed_size != 0;
		fs += f->fixed_size;
		/*
		 * memcmp order survives concatenation only when every field is
		 * memcmp-ordered and every field but the last is fixed width (so
		 * two values that agree on a field agree on its byte span).
		 */
		mcmp = mcmp && type_mcmp(f) &&
		    (i + 1 == n || f->fixed_size != 0);
	}
	r->tail = n > 0 && is_tail(fields[n - 1]);
	r->mcmp = mcmp;
	r->pub.name = "tuple";
	/* fixed_size 0 means variable, so an all-fixed tuple needs fs > 0. */
	r->pub.fixed_size = fixed && fs > 0 ? fs : 0;
	r->pub.len = r->tail ? NULL : tuple_len;
	/* A memcmp-ordered tuple uses the cmp_raw fast path (cmp == NULL). */
	r->pub.cmp = mcmp ? NULL : tuple_cmp;
	r->pub.fmt = tuple_fmt;
	return &r->pub;
}

const dtfl_type *
dtfl_type_pair(const dtfl_type *a, const dtfl_type *b)
{
	const dtfl_type *f[2] = { a, b };

	return dtfl_type_tuple(f, 2);
}

const dtfl_type *
dtfl_type_option(const dtfl_type *inner)
{
	rtype *r = rtype_new(K_OPTION, 1);

	r->fields[0] = dtfl_type_retain(inner);
	r->min = 1;
	r->tail = is_tail(inner);
	r->mcmp = type_mcmp(inner);
	r->pub.name = "option";
	r->pub.len = r->tail ? NULL : option_len;
	/* None (tag 0) sorts before Some (tag 1), then the inner value. */
	r->pub.cmp = r->mcmp ? NULL : option_cmp;
	r->pub.fmt = option_fmt;
	return &r->pub;
}

const dtfl_type *
dtfl_type_vec(const dtfl_type *elem)
{
	rtype *r;

	if (is_tail(elem))
		__dtfl_panic("dtfl_type_vec: element type %s does not "
		    "self-delimit", elem->name != NULL ? elem->name : "?");
	r = rtype_new(K_VEC, 1);
	r->fields[0] = dtfl_type_retain(elem);
	r->min = 8;
	r->pub.name = "vec";
	r->pub.len = vec_len;
	r->pub.cmp = vec_cmp;
	r->pub.fmt = vec_fmt;
	return &r->pub;
}

/*
 * Copies *def into a reference-counted descriptor; def->internal must be
 * 0.  def->name is not copied and must outlive the descriptor.
 */
const dtfl_type *
dtfl_type_new(const dtfl_type *def)
{
	rtype *r;

	if (def->internal != NULL)
		__dtfl_panic("dtfl_type_new: internal must be zero");
	r = rtype_new(K_USER, 0);
	r->pub = *def;
	r->pub.internal = r;
	r->min = def->fixed_size;
	r->tail = is_tail(def);
	return &r->pub;
}

/* Built-ins and unwrapped user descriptors are not counted. */
const dtfl_type *
dtfl_type_retain(const dtfl_type *t)
{
	if (t->internal != NULL)
		__dtfl_arc_retain(&RT(t)->ref);
	return t;
}

/* Drops one reference; the last one releases the fields or the user ctx. */
void
dtfl_type_free(const dtfl_type *t)
{
	rtype *r;
	size_t i;

	if (t == NULL || t->internal == NULL)
		return;
	r = RT(t);
	if (!__dtfl_arc_release(&r->ref))
		return;
	for (i = 0; i < r->n; i++)
		dtfl_type_free(r->fields[i]);
	if (r->kind == K_USER && r->pub.ctx_free != NULL)
		r->pub.ctx_free(r->pub.ctx);
	__dtfl_free(r);
}

static const rtype *
tuple_of(const dtfl_type *t, const char *fn)
{
	if (t->internal == NULL || RT(t)->kind != K_TUPLE)
		__dtfl_panic("%s: %s is not a tuple descriptor", fn,
		    t->name != NULL ? t->name : "?");
	return RT(t);
}

size_t
dtfl_type_tuple_len(const dtfl_type *t)
{
	return tuple_of(t, "dtfl_type_tuple_len")->n;
}

const dtfl_type *
dtfl_type_tuple_field(const dtfl_type *t, size_t i)
{
	const rtype *r = tuple_of(t, "dtfl_type_tuple_field");

	if (i >= r->n)
		__dtfl_panic("dtfl_type_tuple_field: field %zu of %zu", i, r->n);
	return r->fields[i];
}

/* Parses fields 0..i only; later fields are not validated. */
size_t
dtfl_type_tuple_offset(const dtfl_type *t, const void *p, size_t len,
    size_t i, size_t *flen)
{
	const rtype *r = tuple_of(t, "dtfl_type_tuple_offset");
	size_t off = 0, j, n = 0;

	if (i >= r->n)
		__dtfl_panic("dtfl_type_tuple_offset: field %zu of %zu", i, r->n);
	for (j = 0; j <= i; j++) {
		n = dtfl_type_value_len(r->fields[j],
		    (const unsigned char *)p + off, len - off);
		if (n == SIZE_MAX)
			return SIZE_MAX;
		if (j < i)
			off += n;
	}
	*flen = n;
	return off;
}
