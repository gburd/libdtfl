/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_diff.c: diff classes (PLAN D4, difference.rs).  Each
 * built-in's algebra, the capabilities it must lack, tuples, user
 * classes, and multiply.  difference_vector__test_semigroup_vec covers
 * the Vec<R> algebra (difference.rs vector module).
 */
#include "dtfl_test.h"
#include "dd/dd_int.h"

#define B(id) __dtfl_diff_builtin(DTFL_DIFF_##id)

static int64_t
get_i(const void *p, size_t w)
{
	int8_t a; int16_t b; int32_t c; int64_t d;

	switch (w) {
	case 1: memcpy(&a, p, 1); return a;
	case 2: memcpy(&b, p, 2); return b;
	case 4: memcpy(&c, p, 4); return c;
	default: memcpy(&d, p, 8); return d;
	}
}

static void
put_i(void *p, size_t w, int64_t v)
{
	int8_t a = (int8_t)v; int16_t b = (int16_t)v; int32_t c = (int32_t)v;

	switch (w) {
	case 1: memcpy(p, &a, 1); break;
	case 2: memcpy(p, &b, 2); break;
	case 4: memcpy(p, &c, 4); break;
	default: memcpy(p, &v, 8); break;
	}
}

/* Signed and wrapping classes: Abelian, wrap at the class width. */
DTFL_TEST(diff_signed_algebra)
{
	static const int ids[] = { DTFL_DIFF_I8, DTFL_DIFF_I16, DTFL_DIFF_I32,
	    DTFL_DIFF_I64, DTFL_DIFF_WRAP_I8, DTFL_DIFF_WRAP_I16,
	    DTFL_DIFF_WRAP_I32, DTFL_DIFF_WRAP_I64 };
	unsigned char a[8], x[8];
	size_t i, w;

	(void)params; (void)data;
	for (i = 0; i < DTFL_ARRAY_LEN(ids); i++) {
		const __dtfl_diff_class *c = __dtfl_diff_builtin(ids[i]);

		w = c->width;
		assert_not_null(c->zero);
		assert_not_null(c->negate);
		assert_true((c->one != NULL) == (i < 4));
		c->zero(c, a);
		assert_true(c->is_zero(c, a));
		put_i(x, w, -3);
		c->plus_equals(c, a, x);
		assert_int64(get_i(a, w), ==, -3);
		assert_false(c->is_zero(c, a));
		c->negate(c, a);
		assert_int64(get_i(a, w), ==, 3);
		c->plus_equals(c, a, x);
		assert_true(c->is_zero(c, a));
		put_i(x, w, -1);
		put_i(a, w, 1);
		assert_int(c->cmp(c, x, a), <, 0);
		assert_int(c->cmp(c, a, x), >, 0);
		assert_int(c->cmp(c, a, a), ==, 0);
		/* max + 1 wraps to min at the class width. */
		put_i(a, w, (int64_t)(UINT64_MAX >> (65 - 8 * w)));
		put_i(x, w, 1);
		c->plus_equals(c, a, x);
		assert_int64(get_i(a, w), ==, -(int64_t)(UINT64_MAX >> (65 - 8 * w)) - 1);
		if (c->one != NULL) {
			c->one(c, a);
			assert_int64(get_i(a, w), ==, 1);
		}
		assert_size(__dtfl_diff_type(c)->fixed_size, ==, w);
	}
	return MUNIT_OK;
}

/* Unsigned classes: Monoid with one, no negate. */
DTFL_TEST(diff_unsigned_algebra)
{
	static const int ids[] = { DTFL_DIFF_U8, DTFL_DIFF_U16, DTFL_DIFF_U32,
	    DTFL_DIFF_U64 };
	unsigned char a[8], x[8];
	size_t i, w;

	(void)params; (void)data;
	for (i = 0; i < DTFL_ARRAY_LEN(ids); i++) {
		const __dtfl_diff_class *c = __dtfl_diff_builtin(ids[i]);

		w = c->width;
		assert_size(w, ==, (size_t)1 << i);
		assert_null(c->negate);
		assert_not_null(c->zero);
		assert_not_null(c->one);
		c->zero(c, a);
		assert_true(c->is_zero(c, a));
		c->one(c, x);
		c->plus_equals(c, a, x);
		c->plus_equals(c, a, x);
		assert_int64(get_i(a, w), ==, 2);
		/* All-ones is the maximum, not -1. */
		memset(x, 0xff, w);
		assert_int(c->cmp(c, x, a), >, 0);
		c->plus_equals(c, a, x);
		assert_int64(get_i(a, w), ==, 1);
		assert_ptr_equal(__dtfl_diff_type(c), i == 0 ? dtfl_type_u8() :
		    i == 1 ? dtfl_type_u16() : i == 2 ? dtfl_type_u32() :
		    dtfl_type_u64());
	}
	return MUNIT_OK;
}

/* Present: zero width, never zero, Semigroup only. */
DTFL_TEST(diff_present)
{
	const __dtfl_diff_class *c = B(PRESENT);

	(void)params; (void)data;
	assert_size(c->width, ==, 0);
	assert_null(c->zero);
	assert_null(c->negate);
	assert_null(c->one);
	c->plus_equals(c, NULL, NULL);
	assert_false(c->is_zero(c, NULL));
	assert_int(c->cmp(c, NULL, NULL), ==, 0);
	assert_ptr_equal(__dtfl_diff_type(c), dtfl_type_unit());
	return MUNIT_OK;
}

/* Tuples: component-wise; the algebra is the weakest component's. */
DTFL_TEST(diff_tuple)
{
	const __dtfl_diff_class *f[3] = { B(I64), B(I32), B(U8) };
	const __dtfl_diff_class *t, *ab, *sg;
	unsigned char a[13], x[13];
	int64_t v64;
	int32_t v32;

	(void)params; (void)data;
	ab = __dtfl_diff_tuple(f, 2);
	assert_size(ab->width, ==, 12);
	assert_not_null(ab->negate);
	assert_null(ab->one);
	ab->zero(ab, a);
	assert_true(ab->is_zero(ab, a));
	v64 = 5; v32 = -7;
	memcpy(x, &v64, 8);
	memcpy(x + 8, &v32, 4);
	ab->plus_equals(ab, a, x);
	ab->plus_equals(ab, a, x);
	memcpy(&v64, a, 8);
	memcpy(&v32, a + 8, 4);
	assert_int64(v64, ==, 10);
	assert_int32(v32, ==, -14);
	assert_false(ab->is_zero(ab, a));
	ab->negate(ab, a);
	memcpy(&v32, a + 8, 4);
	assert_int32(v32, ==, 14);
	/* Ord is lexicographic: (10, ..) > (5, ..). */
	ab->negate(ab, a);
	assert_int(ab->cmp(ab, a, x), >, 0);
	/* Zero only when every component is zero. */
	memset(a, 0, sizeof a);
	a[8] = 1;
	assert_false(ab->is_zero(ab, a));
	assert_size(dtfl_type_tuple_len(__dtfl_diff_type(ab)), ==, 2);

	t = __dtfl_diff_tuple(f, 3);
	assert_size(t->width, ==, 13);
	assert_null(t->negate);
	assert_not_null(t->zero);

	f[1] = B(PRESENT);
	sg = __dtfl_diff_tuple(f, 2);
	assert_size(sg->width, ==, 8);
	assert_null(sg->zero);
	assert_null(sg->negate);
	memset(a, 0, 8);
	assert_false(sg->is_zero(sg, a));

	__dtfl_diff_class_free(ab);
	__dtfl_diff_class_free(t);
	__dtfl_diff_class_free(sg);
	DTFL_EXPECT_PANIC(__dtfl_diff_tuple(f, 5));
	DTFL_EXPECT_PANIC(__dtfl_diff_tuple(f, 0));
	return MUNIT_OK;
}

/* A user max-plus semigroup: plus_equals is max, never zero. */
static void
max_plus(const __dtfl_diff_class *c, void *acc, const void *x)
{
	uint32_t a, b;

	(void)c;
	memcpy(&a, acc, 4);
	memcpy(&b, x, 4);
	if (b > a)
		memcpy(acc, &b, 4);
}

static bool
max_is_zero(const __dtfl_diff_class *c, const void *x)
{
	(void)c; (void)x;
	return false;
}

static int
max_cmp(const __dtfl_diff_class *c, const void *a, const void *b)
{
	uint32_t x, y;

	(void)c;
	memcpy(&x, a, 4);
	memcpy(&y, b, 4);
	return (x > y) - (x < y);
}

static void
max_negate(const __dtfl_diff_class *c, void *x)
{
	(void)c; (void)x;
}

/* A user min-plus Multiply<Self>: acc.value += x.value (saturating). */
static void
min_plus_multiply(const __dtfl_diff_class *c, void *acc, const void *x)
{
	uint32_t a, b;

	(void)c;
	memcpy(&a, acc, 4);
	memcpy(&b, x, 4);
	memcpy(acc, &(uint32_t){ a > UINT32_MAX - b ? UINT32_MAX : a + b }, 4);
}

static void
count_free(void *ctx)
{
	(*(int *)ctx)++;
}

DTFL_TEST(diff_user_class)
{
	__dtfl_diff_class def = { 0 };
	const __dtfl_diff_class *c;
	uint32_t a = 3, x = 9;
	int freed = 0;

	(void)params; (void)data;
	def.name = "max";
	def.width = 4;
	def.plus_equals = max_plus;
	def.is_zero = max_is_zero;
	def.cmp = max_cmp;
	def.type = dtfl_type_u32();
	def.ctx = &freed;
	def.ctx_free = count_free;
	c = __dtfl_diff_class_new(&def);
	assert_ptr_not_equal(c, &def);
	c->plus_equals(c, &a, &x);
	assert_uint32(a, ==, 9);
	x = 1;
	c->plus_equals(c, &a, &x);
	assert_uint32(a, ==, 9);
	assert_ptr_equal(__dtfl_diff_type(c), dtfl_type_u32());
	assert_null(__dtfl_diff_multiply_class(c, c));
	__dtfl_diff_class_free(c);
	assert_int(freed, ==, 1);

	/* A user class with a multiply callback has Multiply<Self>. */
	freed = 0;
	def.multiply = min_plus_multiply;
	c = __dtfl_diff_class_new(&def);
	assert_ptr_equal(__dtfl_diff_multiply_class(c, c), c);
	{
		uint32_t p = 4, q = 5, o;
		const __dtfl_diff_class *oc;

		assert_int(__dtfl_diff_multiply(c, c, &p, &q, &o, &oc), ==,
		    DTFL_OK);
		assert_uint32(o, ==, 9);	/* 4 + 5, min-plus product */
		assert_ptr_equal(oc, c);
	}
	__dtfl_diff_class_free(c);
	assert_int(freed, ==, 1);
	def.multiply = NULL;

	def.negate = max_negate;
	DTFL_EXPECT_PANIC(__dtfl_diff_class_new(&def));
	def.negate = NULL;
	def.cmp = NULL;
	DTFL_EXPECT_PANIC(__dtfl_diff_class_new(&def));
	return MUNIT_OK;
}

DTFL_TEST(diff_multiply)
{
	const __dtfl_diff_class *f[2] = { B(I64), B(I64) };
	const __dtfl_diff_class *t = __dtfl_diff_tuple(f, 2), *oc;
	int64_t x = -3, y = 7, out = 0, tx[2] = { 2, -5 }, to[2];
	int8_t b8 = 100, c8 = 3, o8;
	uint16_t u = 300, uo;

	(void)params; (void)data;
	/* integer * same integer */
	assert_int(__dtfl_diff_multiply(B(I64), B(I64), &x, &y, &out, &oc),
	    ==, DTFL_OK);
	assert_int64(out, ==, -21);
	assert_ptr_equal(oc, B(I64));
	/* Wraps at the class width. */
	assert_int(__dtfl_diff_multiply(B(WRAP_I8), B(WRAP_I8), &b8, &c8, &o8,
	    NULL), ==, DTFL_OK);
	assert_int(o8, ==, (int8_t)44);
	assert_int(__dtfl_diff_multiply(B(U16), B(U16), &u, &u, &uo, &oc),
	    ==, DTFL_OK);
	assert_uint16(uo, ==, (uint16_t)(300 * 300));
	/* present * T -> T, the value of y */
	assert_ptr_equal(__dtfl_diff_multiply_class(B(PRESENT), B(I32)), B(I32));
	assert_ptr_equal(__dtfl_diff_multiply_class(B(PRESENT), t), t);
	out = 0;
	assert_int(__dtfl_diff_multiply(B(PRESENT), B(I64), NULL, &y, &out,
	    &oc), ==, DTFL_OK);
	assert_int64(out, ==, 7);
	assert_ptr_equal(oc, B(I64));
	assert_int(__dtfl_diff_multiply(B(PRESENT), B(PRESENT), NULL, NULL,
	    NULL, &oc), ==, DTFL_OK);
	assert_ptr_equal(oc, B(PRESENT));
	/* tuple * integer, per component */
	assert_int(__dtfl_diff_multiply(t, B(I64), tx, &x, to, &oc), ==,
	    DTFL_OK);
	assert_int64(to[0], ==, -6);
	assert_int64(to[1], ==, 15);
	assert_ptr_equal(oc, t);
	/* Unsupported pairs. */
	assert_null(__dtfl_diff_multiply_class(B(I64), B(I32)));
	assert_null(__dtfl_diff_multiply_class(B(I64), B(U64)));
	assert_null(__dtfl_diff_multiply_class(B(I64), B(WRAP_I64)));
	assert_null(__dtfl_diff_multiply_class(B(I64), B(PRESENT)));
	assert_null(__dtfl_diff_multiply_class(t, B(I32)));
	assert_null(__dtfl_diff_multiply_class(t, t));
	assert_null(__dtfl_diff_multiply_class(B(I64), t));
	assert_int(__dtfl_diff_multiply(B(I64), B(I32), &x, &y, &out, &oc),
	    ==, DTFL_EALGEBRA);
	__dtfl_diff_class_free(t);
	return MUNIT_OK;
}

/*
 * difference_vector__test_semigroup_vec (difference.rs vector tests):
 * [1,2,3].plus_equals([1,1,1,1]) == [2,3,4,1].  Also exercise the
 * Vec<R> is_zero, negate and multiply algebra over i64 elements.
 */
DTFL_TEST(difference_vector__test_semigroup_vec)
{
	const __dtfl_diff_class *e = B(I64);
	int64_t a[4] = { 1, 2, 3, 0 };
	int64_t b[4] = { 1, 1, 1, 1 };
	size_t alen = 3;
	int64_t out[4];
	const __dtfl_diff_class *oc;

	(void)params; (void)data;
	/* Element-wise add over the overlap, then append the leftover. */
	__dtfl_diff_vec_plus_equals(e, a, &alen, b, 4);
	assert_size(alen, ==, 4);
	assert_int64(a[0], ==, 2);
	assert_int64(a[1], ==, 3);
	assert_int64(a[2], ==, 4);
	assert_int64(a[3], ==, 1);

	/* is_zero: an all-zero vector (and the empty vector) is zero. */
	assert_false(__dtfl_diff_vec_is_zero(e, a, 4));
	assert_true(__dtfl_diff_vec_is_zero(e, a, 0));
	{
		int64_t z[3] = { 0, 0, 0 };

		assert_true(__dtfl_diff_vec_is_zero(e, z, 3));
	}

	/* negate: element-wise, and plus_equals of a vector and its negation
	 * is zero. */
	{
		int64_t c[4] = { 2, 3, 4, 1 };
		int64_t nc[4] = { 2, 3, 4, 1 };
		size_t clen = 4;

		__dtfl_diff_vec_negate(e, nc, 4);
		assert_int64(nc[0], ==, -2);
		assert_int64(nc[3], ==, -1);
		__dtfl_diff_vec_plus_equals(e, c, &clen, nc, 4);
		assert_true(__dtfl_diff_vec_is_zero(e, c, 4));
	}

	/* multiply: each element times the scalar, output class is i64. */
	{
		int64_t v[3] = { 2, 3, 4 }, s = -3;

		oc = __dtfl_diff_vec_multiply(e, e, v, 3, &s, out);
		assert_ptr_equal(oc, e);
		assert_int64(out[0], ==, -6);
		assert_int64(out[1], ==, -9);
		assert_int64(out[2], ==, -12);
		/* No Multiply for mismatched element and scalar classes. */
		assert_null(__dtfl_diff_vec_multiply(e, B(I32), v, 3, &s, out));
	}
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(diff_signed_algebra),
	DTFL_T(diff_unsigned_algebra),
	DTFL_T(diff_present),
	DTFL_T(diff_tuple),
	DTFL_T(diff_user_class),
	DTFL_T(diff_multiply),
	DTFL_T(difference_vector__test_semigroup_vec),
	DTFL_T_END
};

DTFL_TEST_MAIN("diff", tests)
