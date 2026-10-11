/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/difference.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/diff.c: diff classes (PLAN D4), the C form of the Semigroup,
 * Monoid, Abelian and Multiply traits.
 *
 * Integer values are native-endian and unaligned.  Arithmetic runs on
 * uint64_t and truncates to the class width, which is two's complement
 * wrap-around for both signed and unsigned classes.  Upstream's plain
 * integers panic on overflow in debug builds and wrap in release; this
 * port always wraps, and the wrapping classes differ from the plain
 * ones only in name and in which pairs multiply.  Vec<R> diffs are not
 * ported yet (fixed width only; see dd_int.h).
 */
#include "dd/dd_int.h"

/* Reads a w-byte (1, 2, 4 or 8) native unsigned value. */
static uint64_t
ld(const void *p, size_t w)
{
	uint8_t v8;
	uint16_t v16;
	uint32_t v32;
	uint64_t v64;

	switch (w) {
	case 1: memcpy(&v8, p, 1); return v8;
	case 2: memcpy(&v16, p, 2); return v16;
	case 4: memcpy(&v32, p, 4); return v32;
	default: memcpy(&v64, p, 8); return v64;
	}
}

/* Writes the low w bytes of v as a native value. */
static void
st(void *p, size_t w, uint64_t v)
{
	uint8_t v8 = (uint8_t)v;
	uint16_t v16 = (uint16_t)v;
	uint32_t v32 = (uint32_t)v;

	switch (w) {
	case 1: memcpy(p, &v8, 1); break;
	case 2: memcpy(p, &v16, 2); break;
	case 4: memcpy(p, &v32, 4); break;
	default: memcpy(p, &v, 8); break;
	}
}

/* Sign-extends a w-byte value to int64_t. */
static int64_t
sext(uint64_t v, size_t w)
{
	unsigned shift = (unsigned)(64 - 8 * w);

	return (int64_t)(v << shift) >> shift;
}

/* *acc += *x, at the class width. */
static void
int_plus_equals(const __dtfl_diff_class *c, void *acc, const void *x)
{
	st(acc, c->width, ld(acc, c->width) + ld(x, c->width));
}

static bool
int_is_zero(const __dtfl_diff_class *c, const void *x)
{
	return ld(x, c->width) == 0;
}

static void
int_zero(const __dtfl_diff_class *c, void *out)
{
	st(out, c->width, 0);
}

static void
int_one(const __dtfl_diff_class *c, void *out)
{
	st(out, c->width, 1);
}

static void
int_negate(const __dtfl_diff_class *c, void *x)
{
	st(x, c->width, 0 - ld(x, c->width));
}

static int
signed_cmp(const __dtfl_diff_class *c, const void *a, const void *b)
{
	int64_t x = sext(ld(a, c->width), c->width);
	int64_t y = sext(ld(b, c->width), c->width);

	return (x > y) - (x < y);
}

static int
unsigned_cmp(const __dtfl_diff_class *c, const void *a, const void *b)
{
	uint64_t x = ld(a, c->width), y = ld(b, c->width);

	return (x > y) - (x < y);
}

/* Present: addition keeps presence, zero does not inhabit the type. */
static void
present_plus_equals(const __dtfl_diff_class *c, void *acc, const void *x)
{
	(void)c; (void)acc; (void)x;
}

static bool
present_is_zero(const __dtfl_diff_class *c, const void *x)
{
	(void)c; (void)x;
	return false;
}

static int
present_cmp(const __dtfl_diff_class *c, const void *a, const void *b)
{
	(void)c; (void)a; (void)b;
	return 0;
}

#define SIGNED(n, w) { n, w, int_plus_equals, int_is_zero, int_zero,	\
	int_negate, int_one, signed_cmp, NULL, NULL, NULL,		\
	DTFL_DIFF_KIND_SIGNED, 0, { NULL }, { 0 } }
#define UNSIGNED(n, w) { n, w, int_plus_equals, int_is_zero, int_zero,	\
	NULL, int_one, unsigned_cmp, NULL, NULL, NULL,			\
	DTFL_DIFF_KIND_UNSIGNED, 0, { NULL }, { 0 } }
#define WRAPPING(n, w) { n, w, int_plus_equals, int_is_zero, int_zero,	\
	int_negate, NULL, signed_cmp, NULL, NULL, NULL,			\
	DTFL_DIFF_KIND_WRAPPING, 0, { NULL }, { 0 } }

/* Indexed by __dtfl_diff_id. */
static const __dtfl_diff_class builtins[DTFL_DIFF_NBUILTIN] = {
	SIGNED("i8", 1), SIGNED("i16", 2), SIGNED("i32", 4), SIGNED("i64", 8),
	UNSIGNED("u8", 1), UNSIGNED("u16", 2), UNSIGNED("u32", 4),
	UNSIGNED("u64", 8),
	WRAPPING("Wrapping<i8>", 1), WRAPPING("Wrapping<i16>", 2),
	WRAPPING("Wrapping<i32>", 4), WRAPPING("Wrapping<i64>", 8),
	{ "Present", 0, present_plus_equals, present_is_zero, NULL, NULL,
	    NULL, present_cmp, NULL, NULL, NULL, DTFL_DIFF_KIND_PRESENT, 0,
	    { NULL }, { 0 } },
};

/* Returns the static built-in class `id`. */
const __dtfl_diff_class *
__dtfl_diff_builtin(__dtfl_diff_id id)
{
	DTFL_ASSERT((unsigned)id < DTFL_DIFF_NBUILTIN);
	return &builtins[id];
}

/* Built-ins are the static table; users cannot set kind (class_new). */
static bool
is_builtin(const __dtfl_diff_class *c)
{
	return c->kind != DTFL_DIFF_KIND_USER && c->kind != DTFL_DIFF_KIND_TUPLE;
}

static bool
is_int(const __dtfl_diff_class *c)
{
	return c->kind == DTFL_DIFF_KIND_SIGNED ||
	    c->kind == DTFL_DIFF_KIND_UNSIGNED ||
	    c->kind == DTFL_DIFF_KIND_WRAPPING;
}

/* ------------------------------------------------------------------ */
/* Tuples                                                              */
/* ------------------------------------------------------------------ */

#define FIELD(c, i, p) ((unsigned char *)(p) + (c)->off[i])
#define CFIELD(c, i, p) ((const unsigned char *)(p) + (c)->off[i])

static void
tuple_plus_equals(const __dtfl_diff_class *c, void *acc, const void *x)
{
	size_t i;

	for (i = 0; i < c->nfields; i++)
		c->field[i]->plus_equals(c->field[i], FIELD(c, i, acc),
		    CFIELD(c, i, x));
}

/* Zero when every component is zero (Present never is). */
static bool
tuple_is_zero(const __dtfl_diff_class *c, const void *x)
{
	size_t i;

	for (i = 0; i < c->nfields; i++) {
		if (!c->field[i]->is_zero(c->field[i], CFIELD(c, i, x)))
			return false;
	}
	return true;
}

static void
tuple_zero(const __dtfl_diff_class *c, void *out)
{
	size_t i;

	for (i = 0; i < c->nfields; i++)
		c->field[i]->zero(c->field[i], FIELD(c, i, out));
}

static void
tuple_negate(const __dtfl_diff_class *c, void *x)
{
	size_t i;

	for (i = 0; i < c->nfields; i++)
		c->field[i]->negate(c->field[i], FIELD(c, i, x));
}

/* Lexicographic over the components, as Rust's derived tuple Ord. */
static int
tuple_cmp(const __dtfl_diff_class *c, const void *a, const void *b)
{
	size_t i;
	int r;

	for (i = 0; i < c->nfields; i++) {
		r = c->field[i]->cmp(c->field[i], CFIELD(c, i, a),
		    CFIELD(c, i, b));
		if (r != 0)
			return r;
	}
	return 0;
}

/*
 * A tuple of n built-in classes.  It has zero (negate) only when every
 * component has zero (negate), and never has `one`: upstream tuples do
 * not implement From<i8>.  The class owns its record descriptor.
 */
const __dtfl_diff_class *
__dtfl_diff_tuple(const __dtfl_diff_class *const *f, size_t n)
{
	__dtfl_diff_class *c;
	const dtfl_type *types[DTFL_DIFF_TUPLE_MAX];
	bool monoid = true, abelian = true;
	size_t i;

	if (n == 0 || n > DTFL_DIFF_TUPLE_MAX)
		__dtfl_panic("__dtfl_diff_tuple: %zu components (1 to %d)", n,
		    DTFL_DIFF_TUPLE_MAX);
	c = __dtfl_calloc(1, sizeof *c);
	c->name = "tuple";
	c->kind = DTFL_DIFF_KIND_TUPLE;
	c->nfields = n;
	for (i = 0; i < n; i++) {
		if (!is_builtin(f[i]))
			__dtfl_panic("__dtfl_diff_tuple: component %zu (%s) is "
			    "not a built-in class", i, f[i]->name);
		c->field[i] = f[i];
		c->off[i] = c->width;
		c->width += f[i]->width;
		monoid = monoid && f[i]->zero != NULL;
		abelian = abelian && f[i]->negate != NULL;
		types[i] = __dtfl_diff_type(f[i]);
	}
	c->plus_equals = tuple_plus_equals;
	c->is_zero = tuple_is_zero;
	c->zero = monoid ? tuple_zero : NULL;
	c->negate = abelian ? tuple_negate : NULL;
	c->cmp = tuple_cmp;
	c->type = dtfl_type_tuple(types, n);
	return c;
}

/* ------------------------------------------------------------------ */
/* User classes                                                        */
/* ------------------------------------------------------------------ */

/*
 * Copies a caller-defined class.  def must supply plus_equals, is_zero
 * and cmp; negate requires zero (Abelian: Monoid).  The library fields
 * (kind, nfields, field, off) must be zero.
 */
const __dtfl_diff_class *
__dtfl_diff_class_new(const __dtfl_diff_class *def)
{
	__dtfl_diff_class *c;

	if (def->plus_equals == NULL || def->is_zero == NULL ||
	    def->cmp == NULL)
		__dtfl_panic("__dtfl_diff_class_new: %s lacks plus_equals, "
		    "is_zero or cmp", def->name != NULL ? def->name : "?");
	if (def->negate != NULL && def->zero == NULL)
		__dtfl_panic("__dtfl_diff_class_new: %s has negate but no zero",
		    def->name != NULL ? def->name : "?");
	if (def->kind != DTFL_DIFF_KIND_USER || def->nfields != 0)
		__dtfl_panic("__dtfl_diff_class_new: library fields set");
	c = __dtfl_memdup(def, sizeof *def);
	if (c->type != NULL)
		c->type = dtfl_type_retain(c->type);
	return c;
}

/* Frees a tuple or user class; built-ins are static and ignored. */
void
__dtfl_diff_class_free(const __dtfl_diff_class *c)
{
	__dtfl_diff_class *m = (__dtfl_diff_class *)c;

	if (c == NULL || is_builtin(c))
		return;
	if (m->ctx_free != NULL)
		m->ctx_free(m->ctx);
	dtfl_type_free(m->type);
	__dtfl_free(m);
}

/*
 * The record descriptor of a class's values.  Built-ins map to the
 * matching integer descriptor (Wrapping<T> to T) and Present to unit.
 * A user class without one gets unit, which can carry no value.
 */
const dtfl_type *
__dtfl_diff_type(const __dtfl_diff_class *c)
{
	static const dtfl_type *(*const ints[2][4])(void) = {
		{ dtfl_type_i8, dtfl_type_i16, dtfl_type_i32, dtfl_type_i64 },
		{ dtfl_type_u8, dtfl_type_u16, dtfl_type_u32, dtfl_type_u64 },
	};
	unsigned lg = c->width == 1 ? 0 : c->width == 2 ? 1 :
	    c->width == 4 ? 2 : 3;

	if (c->type != NULL)
		return c->type;
	if (is_int(c))
		return ints[c->kind == DTFL_DIFF_KIND_UNSIGNED][lg]();
	return dtfl_type_unit();
}

/* ------------------------------------------------------------------ */
/* Multiply                                                            */
/* ------------------------------------------------------------------ */

static bool
same_int(const __dtfl_diff_class *a, const __dtfl_diff_class *b)
{
	return is_int(a) && a->kind == b->kind && a->width == b->width;
}

/* True if every component of tuple a is the integer class b. */
static bool
tuple_of(const __dtfl_diff_class *a, const __dtfl_diff_class *b)
{
	size_t i;

	if (a->kind != DTFL_DIFF_KIND_TUPLE || !is_int(b))
		return false;
	for (i = 0; i < a->nfields; i++) {
		if (!same_int(a->field[i], b))
			return false;
	}
	return true;
}

/*
 * The class of a * b, or NULL if upstream has no Multiply impl for the
 * pair (difference.rs: integers Multiply<Self>; Present Multiply<T>;
 * tuples Multiply<T> when each component does).
 */
const __dtfl_diff_class *
__dtfl_diff_multiply_class(const __dtfl_diff_class *a,
    const __dtfl_diff_class *b)
{
	if (a->kind == DTFL_DIFF_KIND_PRESENT)
		return b;
	if (same_int(a, b) || tuple_of(a, b))
		return a;
	return NULL;
}

/* out = x * y; see __dtfl_diff_multiply_class for the supported pairs. */
int
__dtfl_diff_multiply(const __dtfl_diff_class *a, const __dtfl_diff_class *b,
    const void *x, const void *y, void *out, const __dtfl_diff_class **out_cls)
{
	const __dtfl_diff_class *oc = __dtfl_diff_multiply_class(a, b);
	uint64_t yv;
	size_t i;

	if (oc == NULL)
		return __dtfl_err(DTFL_EALGEBRA, "no multiply for %s * %s",
		    a->name, b->name);
	if (a->kind == DTFL_DIFF_KIND_PRESENT) {
		if (b->width != 0)
			memmove(out, y, b->width);
	} else if (a->kind == DTFL_DIFF_KIND_TUPLE) {
		yv = ld(y, b->width);
		for (i = 0; i < a->nfields; i++)
			st(FIELD(a, i, out), b->width,
			    ld(CFIELD(a, i, x), b->width) * yv);
	} else {
		st(out, a->width, ld(x, a->width) * ld(y, a->width));
	}
	if (out_cls != NULL)
		*out_cls = oc;
	return DTFL_OK;
}
