/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_record.c: record encodings and descriptors (PLAN D1).
 * Expected values are hand-computed from Rust's derived Ord and Debug
 * for the modelled types.
 */
#define _POSIX_C_SOURCE 200809L	/* fmemopen */
#include "dtfl_test.h"
#include "base/base_int.h"
#include "dtfl/record.h"

/* Encodes a bytes/string value into buf; returns its length. */
static size_t
mkbytes(unsigned char *buf, const char *s)
{
	return dtfl_put_bytes(buf, s, strlen(s));
}

static int
cmp_str(const dtfl_type *t, const char *a, const char *b)
{
	unsigned char ea[64], eb[64];
	size_t na = mkbytes(ea, a), nb = mkbytes(eb, b);

	return dtfl_type_cmp(t, ea, na, eb, nb);
}

/* fmt into a string through a memory stream. */
static const char *
fmt(const dtfl_type *t, const void *p, size_t len)
{
	static char out[256];
	FILE *f = fmemopen(out, sizeof out, "w");

	assert_not_null(f);
	assert_int(dtfl_type_fmt(t, f, p, len), >=, 0);
	fclose(f);
	return out;
}

DTFL_TEST(record_encodings_exact_bytes)
{
	unsigned char b[32];
	static const unsigned char m1[8] = { 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff,
	    0xff, 0xff };
	static const unsigned char u32[4] = { 0x01, 0x02, 0x03, 0x04 };
	static const unsigned char i32[4] = { 0x80, 0x00, 0x00, 0x05 };
	static const unsigned char by[11] = { 0, 0, 0, 0, 0, 0, 0, 3, 'a', 'b', 'c' };

	(void)params; (void)data;
	assert_size(dtfl_put_i64(b, -1), ==, 8);
	assert_memory_equal(8, b, m1);
	assert_int64(dtfl_get_i64(b), ==, -1);
	assert_size(dtfl_put_u32(b, 0x01020304), ==, 4);
	assert_memory_equal(4, b, u32);
	assert_uint32(dtfl_get_u32(b), ==, 0x01020304);
	assert_size(dtfl_put_i32(b, 5), ==, 4);
	assert_memory_equal(4, b, i32);
	assert_size(dtfl_put_i8(b, INT8_MIN), ==, 1);
	assert_uint8(b[0], ==, 0);
	assert_int8(dtfl_get_i8(b), ==, INT8_MIN);
	assert_size(dtfl_put_i16(b, -2), ==, 2);
	assert_int16(dtfl_get_i16(b), ==, -2);
	assert_size(dtfl_put_u16(b, 0xbeef), ==, 2);
	assert_uint16(dtfl_get_u16(b), ==, 0xbeef);
	assert_size(dtfl_put_u8(b, 200), ==, 1);
	assert_uint8(dtfl_get_u8(b), ==, 200);
	assert_size(dtfl_put_u64(b, UINT64_MAX - 1), ==, 8);
	assert_uint64(dtfl_get_u64(b), ==, UINT64_MAX - 1);
	assert_size(dtfl_put_bytes(b, "abc", 3), ==, 11);
	assert_memory_equal(11, b, by);
	{
		size_t n;
		const unsigned char *p = dtfl_get_bytes(b, &n);

		assert_size(n, ==, 3);
		assert_ptr_equal(p, b + 8);
	}
	return MUNIT_OK;
}

DTFL_TEST(record_cmp_builtins)
{
	unsigned char a[8], b[8];

	(void)params; (void)data;
	dtfl_put_i32(a, -5);
	dtfl_put_i32(b, 3);
	assert_int(dtfl_type_cmp(dtfl_type_i32(), a, 4, b, 4), ==, -1);
	assert_int(dtfl_type_cmp(dtfl_type_i32(), b, 4, a, 4), ==, 1);
	assert_int(dtfl_type_cmp(dtfl_type_i32(), a, 4, a, 4), ==, 0);
	dtfl_put_i64(a, INT64_MIN);
	dtfl_put_i64(b, INT64_MAX);
	assert_int(dtfl_type_cmp(dtfl_type_i64(), a, 8, b, 8), ==, -1);
	dtfl_put_u64(a, 255);
	dtfl_put_u64(b, 256);
	assert_int(dtfl_type_cmp(dtfl_type_u64(), a, 8, b, 8), ==, -1);
	dtfl_put_i8(a, -1);
	dtfl_put_i8(b, 0);
	assert_int(dtfl_type_cmp(dtfl_type_i8(), a, 1, b, 1), ==, -1);
	assert_int(dtfl_type_cmp(dtfl_type_unit(), a, 0, b, 0), ==, 0);
	/* Vec<u8>/String Ord: "ab" < "abc" < "b", and "" is least. */
	assert_int(cmp_str(dtfl_type_string(), "ab", "abc"), ==, -1);
	assert_int(cmp_str(dtfl_type_string(), "abc", "b"), ==, -1);
	assert_int(cmp_str(dtfl_type_bytes(), "ab", "b"), ==, -1);
	assert_int(cmp_str(dtfl_type_bytes(), "", "a"), ==, -1);
	assert_int(cmp_str(dtfl_type_bytes(), "b", "abc"), ==, 1);
	assert_int(cmp_str(dtfl_type_bytes(), "xy", "xy"), ==, 0);
	return MUNIT_OK;
}

DTFL_TEST(record_tuple_cmp_and_offset)
{
	const dtfl_type *t = dtfl_type_pair(dtfl_type_string(), dtfl_type_i32());
	unsigned char a[32], b[32];
	size_t na, nb, off, flen;

	(void)params; (void)data;
	assert_size(dtfl_type_tuple_len(t), ==, 2);
	assert_ptr_equal(dtfl_type_tuple_field(t, 0), dtfl_type_string());
	assert_ptr_equal(dtfl_type_tuple_field(t, 1), dtfl_type_i32());
	/* ("b", -1) vs ("ab", 7): the first field decides, not length. */
	na = mkbytes(a, "b");
	na += dtfl_put_i32(a + na, -1);
	nb = mkbytes(b, "ab");
	nb += dtfl_put_i32(b + nb, 7);
	assert_int(dtfl_type_cmp(t, a, na, b, nb), ==, 1);
	/* ("ab", -1) < ("ab", 7). */
	na = mkbytes(a, "ab");
	na += dtfl_put_i32(a + na, -1);
	assert_int(dtfl_type_cmp(t, a, na, b, nb), ==, -1);
	assert_size(dtfl_type_value_len(t, a, na), ==, na);
	assert_size(dtfl_type_value_len(t, a, na - 1), ==, SIZE_MAX);
	off = dtfl_type_tuple_offset(t, a, na, 1, &flen);
	assert_size(off, ==, 10);
	assert_size(flen, ==, 4);
	assert_int32(dtfl_get_i32(a + off), ==, -1);
	off = dtfl_type_tuple_offset(t, a, na, 0, &flen);
	assert_size(off, ==, 0);
	assert_size(flen, ==, 10);
	assert_size(dtfl_type_tuple_offset(t, a, 9, 1, &flen), ==, SIZE_MAX);
	assert_string_equal(fmt(t, a, na), "(\"ab\", -1)");
	DTFL_EXPECT_PANIC((void)dtfl_type_tuple_field(t, 2));
	DTFL_EXPECT_PANIC((void)dtfl_type_tuple_len(dtfl_type_u8()));
	dtfl_type_free(t);

	/* All-fixed tuples are fixed size. */
	t = dtfl_type_pair(dtfl_type_u32(), dtfl_type_i64());
	assert_size(t->fixed_size, ==, 12);
	dtfl_type_free(t);
	return MUNIT_OK;
}

DTFL_TEST(record_option_cmp)
{
	const dtfl_type *t = dtfl_type_option(dtfl_type_i64());
	unsigned char none[1] = { 0 }, s1[9], s2[9], bad[2] = { 2, 0 };

	(void)params; (void)data;
	s1[0] = 1;
	dtfl_put_i64(s1 + 1, -100);
	s2[0] = 1;
	dtfl_put_i64(s2 + 1, 4);
	/* None < Some(i64::MIN-ish) < Some(4). */
	assert_int(dtfl_type_cmp(t, none, 1, s1, 9), ==, -1);
	assert_int(dtfl_type_cmp(t, s1, 9, s2, 9), ==, -1);
	assert_int(dtfl_type_cmp(t, s2, 9, none, 1), ==, 1);
	assert_size(dtfl_type_value_len(t, none, 1), ==, 1);
	assert_size(dtfl_type_value_len(t, s1, 9), ==, 9);
	assert_size(dtfl_type_value_len(t, s1, 8), ==, SIZE_MAX);
	assert_size(dtfl_type_value_len(t, bad, 2), ==, SIZE_MAX);
	assert_size(dtfl_type_value_len(t, bad, 0), ==, SIZE_MAX);
	assert_string_equal(fmt(t, none, 1), "None");
	assert_string_equal(fmt(t, s1, 9), "Some(-100)");
	dtfl_type_free(t);
	return MUNIT_OK;
}

DTFL_TEST(record_vec_cmp)
{
	const dtfl_type *t = dtfl_type_vec(dtfl_type_u8());
	unsigned char a[16], b[16];
	size_t na, nb;

	(void)params; (void)data;
	/* [1, 2] vs [3]: element order wins over count. */
	na = dtfl_put_u64(a, 2);
	na += dtfl_put_u8(a + na, 1);
	na += dtfl_put_u8(a + na, 2);
	nb = dtfl_put_u64(b, 1);
	nb += dtfl_put_u8(b + nb, 3);
	assert_int(dtfl_type_cmp(t, a, na, b, nb), ==, -1);
	/* [1] < [1, 2]: a prefix is less. */
	nb = dtfl_put_u64(b, 1);
	nb += dtfl_put_u8(b + nb, 1);
	assert_int(dtfl_type_cmp(t, b, nb, a, na), ==, -1);
	assert_size(dtfl_type_value_len(t, a, na), ==, na);
	assert_size(dtfl_type_value_len(t, a, na - 1), ==, SIZE_MAX);
	assert_string_equal(fmt(t, a, na), "[1, 2]");
	/* A count far beyond the bytes present is malformed, not a long loop. */
	dtfl_put_u64(a, UINT64_MAX);
	assert_size(dtfl_type_value_len(t, a, na), ==, SIZE_MAX);
	dtfl_type_free(t);

	/* Vec<()> holds any count in 8 bytes. */
	t = dtfl_type_vec(dtfl_type_unit());
	dtfl_put_u64(a, 5);
	dtfl_put_u64(b, 3);
	assert_size(dtfl_type_value_len(t, a, 8), ==, 8);
	assert_int(dtfl_type_cmp(t, b, 8, a, 8), ==, -1);
	assert_string_equal(fmt(t, a, 8), "[(), (), (), (), ()]");
	/* A huge count of zero-width elements must not print forever. */
	dtfl_put_u64(a, UINT64_MAX);
	assert_size(dtfl_type_value_len(t, a, 8), ==, 8);
	{
		FILE *null = fopen("/dev/null", "w");

		assert_not_null(null);
		assert_int(dtfl_type_fmt(t, null, a, 8), >, 4096 * 4);
		fclose(null);
	}
	dtfl_type_free(t);
	return MUNIT_OK;
}

DTFL_TEST(record_fmt)
{
	unsigned char b[32];
	size_t n;

	(void)params; (void)data;
	dtfl_put_i32(b, -42);
	assert_string_equal(fmt(dtfl_type_i32(), b, 4), "-42");
	dtfl_put_u64(b, UINT64_MAX);
	assert_string_equal(fmt(dtfl_type_u64(), b, 8), "18446744073709551615");
	assert_string_equal(fmt(dtfl_type_unit(), b, 0), "()");
	n = dtfl_put_bytes(b, "a\"\\\n\x01", 5);
	assert_string_equal(fmt(dtfl_type_string(), b, n), "\"a\\\"\\\\\\n\\x01\"");
	n = dtfl_put_bytes(b, "\x01\xff", 2);
	assert_string_equal(fmt(dtfl_type_bytes(), b, n), "0x01ff");
	/* A malformed value prints as hex of the raw bytes. */
	assert_string_equal(fmt(dtfl_type_string(), b, 3), "0x000000");
	return MUNIT_OK;
}

static int ctx_freed;

static void
count_free(void *ctx)
{
	(void)ctx;
	ctx_freed++;
}

static int
rev_cmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	return -memcmp(a, b, 1) * 7;
}

DTFL_TEST(record_refcounting)
{
	dtfl_type def = { 0 };
	const dtfl_type *u, *p, *o;
	unsigned char a = 1, b = 2;

	(void)params; (void)data;
	def.name = "rev";
	def.fixed_size = 1;
	def.cmp = rev_cmp;
	def.ctx_free = count_free;
	u = dtfl_type_new(&def);
	assert_ptr_not_equal(u, &def);
	assert_not_null(u->internal);
	/* dtfl_type_cmp normalizes to -1/0/1. */
	assert_int(dtfl_type_cmp(u, &a, 1, &b, 1), ==, 1);
	p = dtfl_type_pair(u, dtfl_type_u8());
	o = dtfl_type_option(p);
	dtfl_type_free(u);	/* p still holds u */
	dtfl_type_free(p);	/* o still holds p */
	assert_int(ctx_freed, ==, 0);
	assert_ptr_equal(dtfl_type_retain(o), o);
	dtfl_type_free(o);
	assert_int(ctx_freed, ==, 0);
	dtfl_type_free(o);
	assert_int(ctx_freed, ==, 1);
	/* Built-ins: retain and free are no-ops. */
	assert_ptr_equal(dtfl_type_retain(dtfl_type_u8()), dtfl_type_u8());
	dtfl_type_free(dtfl_type_u8());
	dtfl_type_free(NULL);
	def.internal = &def;
	DTFL_EXPECT_PANIC((void)dtfl_type_new(&def));
	return MUNIT_OK;
}

DTFL_TEST(record_value_len_malformed)
{
	unsigned char b[16] = { 0 };
	const dtfl_type *t;

	(void)params; (void)data;
	assert_size(dtfl_type_value_len(dtfl_type_u64(), b, 7), ==, SIZE_MAX);
	assert_size(dtfl_type_value_len(dtfl_type_u64(), b, 16), ==, 8);
	assert_size(dtfl_type_value_len(dtfl_type_unit(), b, 0), ==, 0);
	/* bytes: short prefix, and a length beyond the span. */
	assert_size(dtfl_type_value_len(dtfl_type_bytes(), b, 7), ==, SIZE_MAX);
	dtfl_put_u64(b, 9);
	assert_size(dtfl_type_value_len(dtfl_type_bytes(), b, 16), ==, SIZE_MAX);
	dtfl_put_u64(b, 8);
	assert_size(dtfl_type_value_len(dtfl_type_bytes(), b, 16), ==, 16);
	dtfl_put_u64(b, UINT64_MAX);
	assert_size(dtfl_type_value_len(dtfl_type_bytes(), b, 16), ==, SIZE_MAX);
	/* An empty tuple is valid and zero bytes long. */
	t = dtfl_type_tuple(NULL, 0);
	assert_size(dtfl_type_value_len(t, b, 0), ==, 0);
	assert_int(dtfl_type_cmp(t, b, 0, b, 0), ==, 0);
	assert_string_equal(fmt(t, b, 0), "()");
	dtfl_type_free(t);
	return MUNIT_OK;
}

DTFL_TEST(record_hash_default_is_fnv)
{
	unsigned char b[8];

	(void)params; (void)data;
	dtfl_put_u64(b, 12345);
	assert_uint64(dtfl_type_hash(dtfl_type_u64(), b, 8), ==,
	    __dtfl_fnv1a(b, 8));
	assert_uint64(dtfl_type_hash(dtfl_type_unit(), b, 0), ==,
	    UINT64_C(0xcbf29ce484222325));
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(record_encodings_exact_bytes),
	DTFL_T(record_cmp_builtins),
	DTFL_T(record_tuple_cmp_and_offset),
	DTFL_T(record_option_cmp),
	DTFL_T(record_vec_cmp),
	DTFL_T(record_fmt),
	DTFL_T(record_refcounting),
	DTFL_T(record_value_len_malformed),
	DTFL_T(record_hash_default_is_fnv),
	DTFL_T_END
};

DTFL_TEST_MAIN("record", tests)
