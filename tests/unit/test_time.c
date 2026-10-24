/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_time.c: time classes, per shape.  Expected values are
 * computed by hand from the pinned upstream sources named in each test
 * (verification.md oracle 4).
 */
#include "dtfl_test.h"
#include "progress/time_int.h"

#define U(v) ((uint64_t)(v))
#define S(v) dtfl_coord_from_i64(v)

static const dtfl_coord C_U8 = { DTFL_COORD_UNSIGNED, 8 };
static const dtfl_coord C_U32 = { DTFL_COORD_UNSIGNED, 32 };
static const dtfl_coord C_U64 = { DTFL_COORD_UNSIGNED, 64 };
static const dtfl_coord C_I8 = { DTFL_COORD_SIGNED, 8 };
static const dtfl_coord C_I64 = { DTFL_COORD_SIGNED, 64 };

/* Encodes t and checks the bytes, then decodes them back to t. */
static void
check_codec(const dtfl_time_class *tc, const uint64_t *t,
    const unsigned char *want, size_t n)
{
	__dtfl_vec v;
	uint64_t back[DTFL_TIME_MAX];
	size_t used = 99;

	__dtfl_vec_init(&v, 1);
	assert_size(__dtfl_time_encode(tc, t, &v), ==, n);
	assert_size(__dtfl_time_encoded_len(tc, t), ==, n);
	assert_size(v.len, ==, n);
	if (n != 0)
		assert_memory_equal(n, v.data, want);
	assert_int(__dtfl_time_decode(tc, v.data, v.len, back, &used), ==, 0);
	assert_size(used, ==, n);
	assert_true(__dtfl_time_eq(tc, back, t));
	if (n != 0)
		assert_int(__dtfl_time_decode(tc, v.data, n - 1, back, &used), ==,
		    DTFL_EPROTO);
	__dtfl_vec_free(&v);
}

DTFL_TEST(unit_class)
{
	dtfl_time_class *tc = dtfl_time_unit();
	uint64_t x[1] = { 0 };
	char buf[16];

	(void)params; (void)data;
	assert_uint(dtfl_time_depth(tc), ==, 0);
	assert_uint(dtfl_summary_depth(tc), ==, 0);
	assert_true(dtfl_time_is_total(tc));
	assert_true(dtfl_time_less_equal(tc, x, x));
	assert_false(dtfl_time_less_than(tc, x, x));
	assert_int(dtfl_time_cmp(tc, x, x), ==, 0);
	assert_true(dtfl_summary_results_in(tc, x, x, x));
	assert_true(dtfl_summary_followed_by(tc, x, x, x));
	check_codec(tc, x, NULL, 0);
	assert_int(dtfl_time_format(tc, x, buf, sizeof buf), ==, 2);
	assert_string_equal(buf, "()");
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* timestamp.rs implement_timestamp_add: checked_add at the width. */
DTFL_TEST(total_u32_overflow)
{
	dtfl_time_class *tc = dtfl_time_u32();
	uint64_t t = U(4294967290), s = 5, out = 7;

	(void)params; (void)data;
	assert_true(dtfl_summary_results_in(tc, &s, &t, &out));
	assert_uint64(out, ==, UINT32_MAX);
	s = 6;
	out = 7;
	assert_false(dtfl_summary_results_in(tc, &s, &t, &out));
	assert_uint64(out, ==, 7);	/* untouched on None */
	t = 1;
	assert_false(dtfl_summary_followed_by(tc, &s, &(uint64_t){ UINT32_MAX - 5 },
	    &out));
	assert_true(dtfl_summary_followed_by(tc, &s, &(uint64_t){ UINT32_MAX - 6 },
	    &out));
	assert_uint64(out, ==, UINT32_MAX);
	dtfl_summary_default(tc, &out);
	assert_uint64(out, ==, 0);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(total_u64_lattice)
{
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t a = 3, b = 9, out, f[3] = { 5, 2, 7 }, t;
	char buf[32];

	(void)params; (void)data;
	assert_true(dtfl_time_less_equal(tc, &a, &b));
	assert_false(dtfl_time_less_equal(tc, &b, &a));
	assert_true(dtfl_time_less_than(tc, &a, &b));
	dtfl_time_join(tc, &a, &b, &out);
	assert_uint64(out, ==, 9);
	dtfl_time_meet(tc, &a, &b, &out);
	assert_uint64(out, ==, 3);
	dtfl_time_minimum(tc, &out);
	assert_uint64(out, ==, 0);
	__dtfl_time_maximum(tc, &out);
	assert_uint64(out, ==, UINT64_MAX);
	/* advance_by {5,2,7}: min over max(t, f). */
	t = 1;
	__dtfl_time_advance_by(tc, &t, f, 3);
	assert_uint64(t, ==, 2);
	t = 4;
	__dtfl_time_advance_by(tc, &t, f, 3);
	assert_uint64(t, ==, 4);
	__dtfl_time_advance_by(tc, &t, f, 0);
	assert_uint64(t, ==, 4);
	t = UINT64_MAX - 1;
	assert_false(dtfl_summary_results_in(tc, &(uint64_t){ 2 }, &t, &out));
	check_codec(tc, &(uint64_t){ UINT64_C(0x0102030405060708) },
	    (const unsigned char *)"\x08\x07\x06\x05\x04\x03\x02\x01", 8);
	assert_int(dtfl_time_format(tc, &b, buf, sizeof buf), ==, 1);
	assert_string_equal(buf, "9");
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(total_u8_codec)
{
	dtfl_time_class *tc = dtfl_time_total(C_U8);
	uint64_t t = 200, s = 56, out;

	(void)params; (void)data;
	check_codec(tc, &t, (const unsigned char *)"\xc8", 1);
	assert_false(dtfl_summary_results_in(tc, &s, &t, &out));
	s = 55;
	assert_true(dtfl_summary_results_in(tc, &s, &t, &out));
	assert_uint64(out, ==, 255);
	__dtfl_time_maximum(tc, &out);
	assert_uint64(out, ==, 255);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* Signed coordinates: biased storage, checked add at the width. */
DTFL_TEST(signed_bias)
{
	dtfl_time_class *i8 = dtfl_time_total(C_I8);
	dtfl_time_class *i64 = dtfl_time_total(C_I64);
	uint64_t t, s, out;
	char buf[32];

	(void)params; (void)data;
	assert_uint64(S(INT64_MIN), ==, 0);
	assert_uint64(S(-1), ==, UINT64_MAX >> 1);
	assert_uint64(S(0), ==, UINT64_C(1) << 63);
	assert_uint64(S(INT64_MAX), ==, UINT64_MAX);
	assert_int64(dtfl_coord_to_i64(0), ==, INT64_MIN);
	assert_int64(dtfl_coord_to_i64(UINT64_MAX), ==, INT64_MAX);
	assert_int64(dtfl_coord_to_i64(S(-5)), ==, -5);
	assert_int(dtfl_time_cmp(i8, &(uint64_t){ S(-3) }, &(uint64_t){ S(2) }),
	    <, 0);

	dtfl_time_minimum(i8, &out);
	assert_uint64(out, ==, S(-128));
	__dtfl_time_maximum(i8, &out);
	assert_uint64(out, ==, S(127));
	dtfl_time_minimum(i64, &out);
	assert_uint64(out, ==, 0);
	__dtfl_time_maximum(i64, &out);
	assert_uint64(out, ==, UINT64_MAX);
	dtfl_summary_default(i8, &s);
	assert_uint64(s, ==, S(0));

	/* i8: 100 + 27 = 127 fits, 100 + 28 does not; -100 + -29 does not. */
	t = S(100);
	assert_true(dtfl_summary_results_in(i8, &(uint64_t){ S(27) }, &t, &out));
	assert_uint64(out, ==, S(127));
	assert_false(dtfl_summary_results_in(i8, &(uint64_t){ S(28) }, &t, &out));
	t = S(-100);
	assert_true(dtfl_summary_results_in(i8, &(uint64_t){ S(-28) }, &t, &out));
	assert_uint64(out, ==, S(-128));
	assert_false(dtfl_summary_results_in(i8, &(uint64_t){ S(-29) }, &t, &out));
	/* Negative summaries walk times backwards (timestamp.rs allows it). */
	t = S(5);
	assert_true(dtfl_summary_results_in(i8, &(uint64_t){ S(-7) }, &t, &out));
	assert_uint64(out, ==, S(-2));

	/* i64 extremes. */
	t = S(INT64_MAX);
	assert_false(dtfl_summary_results_in(i64, &(uint64_t){ S(1) }, &t, &out));
	t = S(INT64_MIN);
	assert_false(dtfl_summary_results_in(i64, &(uint64_t){ S(-1) }, &t, &out));
	assert_true(dtfl_summary_results_in(i64, &(uint64_t){ S(INT64_MAX) }, &t,
	    &out));
	assert_uint64(out, ==, S(-1));

	/* Codec: two's complement at the width. */
	check_codec(i8, &(uint64_t){ S(-128) }, (const unsigned char *)"\x80", 1);
	check_codec(i8, &(uint64_t){ S(-1) }, (const unsigned char *)"\xff", 1);
	check_codec(i8, &(uint64_t){ S(127) }, (const unsigned char *)"\x7f", 1);
	check_codec(i64, &(uint64_t){ S(INT64_MIN) },
	    (const unsigned char *)"\0\0\0\0\0\0\0\x80", 8);
	check_codec(i64, &(uint64_t){ S(-2) },
	    (const unsigned char *)"\xfe\xff\xff\xff\xff\xff\xff\xff", 8);
	assert_int(dtfl_time_format(i8, &(uint64_t){ S(-7) }, buf, sizeof buf), ==,
	    2);
	assert_string_equal(buf, "-7");
	dtfl_time_class_free(i8);
	dtfl_time_class_free(i64);
	return MUNIT_OK;
}

/* Duration: u64 nanoseconds; bincode (u64 secs, u32 nanos). */
DTFL_TEST(duration)
{
	dtfl_time_class *tc = dtfl_time_duration();
	uint64_t t = UINT64_C(3000000007), out;
	unsigned char carry[12] = { 1, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xca, 0x9a, 0x3b };
	unsigned char huge[12] = { 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0 };
	size_t used;
	char buf[32];

	(void)params; (void)data;
	check_codec(tc, &t, (const unsigned char *)
	    "\x03\0\0\0\0\0\0\0\x07\0\0\0", 12);
	/* nanos >= 10^9 carry into seconds, as Duration::new does. */
	assert_int(__dtfl_time_decode(tc, carry, 12, &out, &used), ==, 0);
	assert_uint64(out, ==, UINT64_C(2000000000));
	/* 2^32 - 1 seconds fits; 2^64 ns is about 584 years. */
	assert_int(__dtfl_time_decode(tc, huge, 12, &out, &used), ==, 0);
	huge[4] = 0xff;
	assert_int(__dtfl_time_decode(tc, huge, 12, &out, &used), ==,
	    DTFL_ERANGE);
	assert_false(dtfl_summary_results_in(tc, &(uint64_t){ 1 },
	    &(uint64_t){ UINT64_MAX }, &out));
	assert_int(dtfl_time_format(tc, &t, buf, sizeof buf), ==, 12);
	assert_string_equal(buf, "3.000000007s");
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* lattice.rs doc examples for Product<u64, u64>. */
DTFL_TEST(product)
{
	dtfl_time_class *o = dtfl_time_u64(), *i = dtfl_time_u32();
	dtfl_time_class *tc = dtfl_time_product(o, i);
	uint64_t a[2] = { 3, 7 }, b[2] = { 4, 6 }, out[2];
	uint64_t f[4] = { 4, 8, 5, 3 }, t[2] = { 3, 7 };
	uint64_t s[2] = { 1, 2 }, s2[2];
	char buf[32];

	(void)params; (void)data;
	dtfl_time_class_free(o);	/* the product keeps its children alive */
	dtfl_time_class_free(i);
	assert_uint(dtfl_time_depth(tc), ==, 2);
	assert_uint(dtfl_summary_depth(tc), ==, 2);
	assert_false(dtfl_time_is_total(tc));
	assert_false(dtfl_time_less_equal(tc, a, b));
	assert_false(dtfl_time_less_equal(tc, b, a));
	assert_int(dtfl_time_cmp(tc, a, b), <, 0);
	dtfl_time_join(tc, a, b, out);
	assert_uint64(out[0], ==, 4);
	assert_uint64(out[1], ==, 7);
	dtfl_time_meet(tc, a, b, out);
	assert_uint64(out[0], ==, 3);
	assert_uint64(out[1], ==, 6);
	/* lattice.rs advance_by doc example: (3,7) by {(4,8),(5,3)} = (4,7). */
	__dtfl_time_advance_by(tc, t, f, 2);
	assert_uint64(t[0], ==, 4);
	assert_uint64(t[1], ==, 7);
	assert_true(dtfl_summary_results_in(tc, s, a, out));
	assert_uint64(out[0], ==, 4);
	assert_uint64(out[1], ==, 9);
	/* Inner overflow drops the whole time. */
	s2[0] = 0;
	s2[1] = UINT32_MAX;
	assert_false(dtfl_summary_results_in(tc, s2, a, out));
	assert_true(dtfl_summary_less_equal(tc, s, (uint64_t[2]){ 1, 3 }));
	assert_false(dtfl_summary_less_equal(tc, s, (uint64_t[2]){ 2, 1 }));
	check_codec(tc, a, (const unsigned char *)
	    "\x03\0\0\0\0\0\0\0\x07\0\0\0", 12);
	assert_int(dtfl_time_format(tc, a, buf, sizeof buf), ==, 6);
	assert_string_equal(buf, "(3, 7)");
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * order.rs tuple PartialOrder and lattice.rs:204-230, over a first
 * component that is itself a product, so that firsts can be
 * incomparable.
 */
DTFL_TEST(lex)
{
	dtfl_time_class *u = dtfl_time_u64(), *u8 = dtfl_time_total(C_U8);
	dtfl_time_class *p = dtfl_time_product(u, u);
	dtfl_time_class *tc = dtfl_time_lex(p, u8);
	uint64_t a[3] = { 1, 2, 9 }, b[3] = { 2, 1, 3 }, c[3] = { 1, 3, 0 };
	uint64_t out[3];
	dtfl_time_class *tt = dtfl_time_lex(u, u);

	(void)params; (void)data;
	assert_uint(dtfl_time_depth(tc), ==, 3);
	assert_false(dtfl_time_is_total(tc));
	assert_true(dtfl_time_is_total(tt));
	/* (1,2) < (1,3) strictly, so a <= c whatever the second. */
	assert_true(dtfl_time_less_equal(tc, a, c));
	assert_false(dtfl_time_less_equal(tc, c, a));
	/* Equal firsts compare seconds. */
	assert_true(dtfl_time_less_equal(tc, (uint64_t[3]){ 1, 2, 3 }, a));
	assert_false(dtfl_time_less_equal(tc, a, (uint64_t[3]){ 1, 2, 3 }));
	/* Incomparable firsts. */
	assert_false(dtfl_time_less_equal(tc, a, b));
	assert_false(dtfl_time_less_equal(tc, b, a));

	dtfl_time_join(tc, a, c, out);	/* a.0 < c.0: c */
	assert_memory_equal(sizeof out, out, c);
	dtfl_time_meet(tc, a, c, out);	/* a */
	assert_memory_equal(sizeof out, out, a);
	dtfl_time_join(tc, a, (uint64_t[3]){ 1, 2, 4 }, out);
	assert_memory_equal(sizeof out, out, ((uint64_t[3]){ 1, 2, 9 }));
	dtfl_time_meet(tc, a, (uint64_t[3]){ 1, 2, 4 }, out);
	assert_memory_equal(sizeof out, out, ((uint64_t[3]){ 1, 2, 4 }));
	/* Incomparable: join (2,2) with minimum, meet (1,1) with maximum. */
	dtfl_time_join(tc, a, b, out);
	assert_memory_equal(sizeof out, out, ((uint64_t[3]){ 2, 2, 0 }));
	dtfl_time_meet(tc, a, b, out);
	assert_memory_equal(sizeof out, out, ((uint64_t[3]){ 1, 1, 255 }));
	/* Aliased output. */
	__dtfl_time_copy(tc, out, a);
	dtfl_time_join(tc, out, b, out);
	assert_memory_equal(sizeof out, out, ((uint64_t[3]){ 2, 2, 0 }));
	/* Tuple summaries apply both halves. */
	assert_true(dtfl_summary_results_in(tc, (uint64_t[3]){ 0, 1, 1 }, a, out));
	assert_memory_equal(sizeof out, out, ((uint64_t[3]){ 1, 3, 10 }));
	assert_false(dtfl_summary_results_in(tc, (uint64_t[3]){ 0, 0, 250 }, a,
	    out));
	/* Summary order is the tuple order too. */
	assert_true(dtfl_summary_less_equal(tc, (uint64_t[3]){ 0, 1, 9 },
	    (uint64_t[3]){ 0, 2, 0 }));
	assert_false(dtfl_summary_less_equal(tc, (uint64_t[3]){ 0, 2, 0 },
	    (uint64_t[3]){ 1, 1, 9 }));
	dtfl_time_class_free(tt);
	dtfl_time_class_free(tc);
	dtfl_time_class_free(p);
	dtfl_time_class_free(u8);
	dtfl_time_class_free(u);
	return MUNIT_OK;
}

/* altneu.rs: (time, neu) as 2t + neu. */
DTFL_TEST(altneu)
{
	dtfl_time_class *u = dtfl_time_u64(), *i8 = dtfl_time_total(C_I8);
	dtfl_time_class *tc = dtfl_time_altneu(u);
	uint64_t alt3 = 6, neu3 = 7, out, t;
	unsigned char bad[9] = { 3, 0, 0, 0, 0, 0, 0, 0, 2 };
	size_t used;
	char buf[48];

	(void)params; (void)data;
	assert_not_null(tc);
	assert_uint(dtfl_summary_depth(tc), ==, 0);
	assert_true(dtfl_time_is_total(tc));
	assert_true(dtfl_time_less_than(tc, &alt3, &neu3));
	assert_true(dtfl_time_less_than(tc, &neu3, &(uint64_t){ 8 }));
	dtfl_time_join(tc, &alt3, &neu3, &out);
	assert_uint64(out, ==, neu3);
	dtfl_time_meet(tc, &alt3, &neu3, &out);
	assert_uint64(out, ==, alt3);
	assert_true(dtfl_summary_results_in(tc, NULL, &neu3, &out));
	assert_uint64(out, ==, neu3);
	check_codec(tc, &neu3, (const unsigned char *)"\x03\0\0\0\0\0\0\0\x01", 9);
	assert_int(__dtfl_time_decode(tc, bad, 9, &out, &used), ==, DTFL_EPROTO);
	bad[8] = 0;
	bad[7] = 0x80;	/* t = 2^63 + 3 */
	assert_int(__dtfl_time_decode(tc, bad, 9, &out, &used), ==, DTFL_ERANGE);
	assert_int(dtfl_time_format(tc, &neu3, buf, sizeof buf), >, 0);
	assert_string_equal(buf, "AltNeu { time: 3, neu: true }");

	/* Refines<T> for AltNeu<T>. */
	assert_true(__dtfl_refines(tc, u));
	t = 5;
	__dtfl_time_to_inner(tc, u, &t, &out);
	assert_uint64(out, ==, 10);
	__dtfl_time_to_outer(tc, u, &(uint64_t){ 11 }, &t);
	assert_uint64(t, ==, 5);
	t = 99;
	__dtfl_summary_to_outer(tc, u, NULL, &t);
	assert_uint64(t, ==, 0);
	DTFL_EXPECT_PANIC(__dtfl_time_to_inner(tc, u, &(uint64_t){ UINT64_C(1) << 63 },
	    &out));

	assert_null(dtfl_time_altneu(i8));
	assert_string_not_equal(dtfl_last_error_detail(), "");
	assert_null(dtfl_time_altneu(NULL));
	dtfl_time_class_free(tc);
	dtfl_time_class_free(i8);
	dtfl_time_class_free(u);
	return MUNIT_OK;
}

/* pointstamp.rs order, lattice and summaries, padded to max_len 4. */
DTFL_TEST(pointstamp)
{
	dtfl_time_class *tc = dtfl_time_pointstamp(C_U64, 4);
	uint64_t a[4] = { 1, 5, 0, 0 }, b[4] = { 2, 0, 0, 0 }, out[4];
	uint64_t s[5], s2[5], sum[5];
	char buf[64];

	(void)params; (void)data;
	assert_uint(dtfl_time_depth(tc), ==, 4);
	assert_uint(dtfl_summary_depth(tc), ==, 5);
	assert_false(dtfl_time_is_total(tc));
	assert_false(dtfl_time_less_equal(tc, a, b));
	assert_false(dtfl_time_less_equal(tc, b, a));
	/* [1] <= [1, 5] (missing coordinates are the minimum). */
	assert_true(dtfl_time_less_equal(tc, (uint64_t[4]){ 1 }, a));
	/* Ord: [1, 5] < [2] as vectors, and as padded words. */
	assert_int(dtfl_time_cmp(tc, a, b), <, 0);
	dtfl_time_join(tc, a, b, out);
	assert_memory_equal(sizeof out, out, ((uint64_t[4]){ 2, 5, 0, 0 }));
	dtfl_time_meet(tc, a, b, out);
	assert_memory_equal(sizeof out, out, ((uint64_t[4]){ 1, 0, 0, 0 }));

	/* Default summary: retain None, no actions. */
	dtfl_summary_default(tc, s);
	assert_uint64(s[0], ==, UINT64_MAX);
	assert_true(dtfl_summary_results_in(tc, s, a, out));
	assert_memory_equal(sizeof out, out, a);
	/* retain 1, actions [0, 3]: [1, 5] -> [1] -> [1, 3]. */
	memcpy(s, (uint64_t[5]){ 1, 0, 3, 0, 0 }, sizeof s);
	assert_true(dtfl_summary_results_in(tc, s, a, out));
	assert_memory_equal(sizeof out, out, ((uint64_t[4]){ 1, 3, 0, 0 }));
	/* retain None, actions [1]: [1, 5] -> [2, 5]. */
	memcpy(s2, (uint64_t[5]){ UINT64_MAX, 1, 0, 0, 0 }, sizeof s2);
	assert_true(dtfl_summary_results_in(tc, s2, a, out));
	assert_memory_equal(sizeof out, out, ((uint64_t[4]){ 2, 5, 0, 0 }));
	/*
	 * followed_by: s then s2: retain min(1, None) = 1; s's actions are
	 * not cut (s2 retains all) and add to s2's: [1, 3].
	 */
	assert_true(dtfl_summary_followed_by(tc, s, s2, sum));
	assert_memory_equal(sizeof sum, sum, ((uint64_t[5]){ 1, 1, 3, 0, 0 }));
	/* s2 then s: retain 1 cuts s2's actions to [1], plus [0, 3]. */
	assert_true(dtfl_summary_followed_by(tc, s2, s, sum));
	assert_memory_equal(sizeof sum, sum, ((uint64_t[5]){ 1, 1, 3, 0, 0 }));
	/*
	 * retain 0, actions [7, 7], then s: s retains 1, so only [7]
	 * survives, composed with [0, 3]: retain 0, actions [7, 3].
	 */
	memcpy(s2, (uint64_t[5]){ 0, 7, 7, 0, 0 }, sizeof s2);
	assert_true(dtfl_summary_followed_by(tc, s2, s, sum));
	assert_memory_equal(sizeof sum, sum, ((uint64_t[5]){ 0, 7, 3, 0, 0 }));
	/* An action overflowing drops the time. */
	memcpy(s2, (uint64_t[5]){ UINT64_MAX, 0, UINT64_MAX - 4, 0, 0 },
	    sizeof s2);
	assert_true(dtfl_summary_results_in(tc, s2,
	    (uint64_t[4]){ 1, 4, 0, 0 }, out));
	assert_uint64(out[1], ==, UINT64_MAX);
	assert_false(dtfl_summary_results_in(tc, s2, a, out));
	/* Summary order: equal retain, then pointwise. */
	assert_true(dtfl_summary_less_equal(tc, (uint64_t[5]){ 1, 0, 2 }, s));
	assert_false(dtfl_summary_less_equal(tc, (uint64_t[5]){ 2, 0, 2 }, s));
	assert_false(dtfl_summary_less_equal(tc, (uint64_t[5]){ 1, 1, 2 }, s));

	assert_int(dtfl_time_format(tc, a, buf, sizeof buf), >, 0);
	assert_string_equal(buf, "PointStamp { vector: [1, 5] }");
	dtfl_time_format(tc, (uint64_t[4]){ 0 }, buf, sizeof buf);
	assert_string_equal(buf, "PointStamp { vector: [] }");
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * PointStamp bincode: u64 length then the trimmed coordinates.  A
 * non-canonical input (trailing minima, which Rust's PointStamp::new
 * never writes) is accepted and canonicalized; so are trailing minima
 * past max_len.  A real coordinate past max_len is DTFL_ERANGE.
 */
DTFL_TEST(pointstamp_codec)
{
	dtfl_time_class *tc = dtfl_time_pointstamp(C_U32, 3);
	uint64_t t[3] = { 4, 0, 0 }, out[3];
	unsigned char trailing[20] = { 3, 0, 0, 0, 0, 0, 0, 0,
	    4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	unsigned char longer[24] = { 4, 0, 0, 0, 0, 0, 0, 0,
	    4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	unsigned char huge_len[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	__dtfl_vec v;
	uint64_t s[4] = { 2, 0, 1, 0 }, sback[4];
	size_t used;

	(void)params; (void)data;
	check_codec(tc, t, (const unsigned char *)"\x01\0\0\0\0\0\0\0\x04\0\0\0",
	    12);
	check_codec(tc, (uint64_t[3]){ 0 }, (const unsigned char *)
	    "\0\0\0\0\0\0\0\0", 8);
	assert_int(__dtfl_time_decode(tc, trailing, 20, out, &used), ==, 0);
	assert_size(used, ==, 20);
	assert_memory_equal(sizeof out, out, t);
	assert_int(__dtfl_time_decode(tc, longer, 24, out, &used), ==, 0);
	assert_memory_equal(sizeof out, out, t);
	longer[20] = 1;
	assert_int(__dtfl_time_decode(tc, longer, 24, out, &used), ==, DTFL_ERANGE);
	assert_int(__dtfl_time_decode(tc, huge_len, 8, out, &used), ==,
	    DTFL_EPROTO);

	/* Summary: Option<usize> retain, then Vec of actions, trimmed. */
	__dtfl_vec_init(&v, 1);
	assert_size(__dtfl_summary_encode(tc, s, &v), ==, 1 + 8 + 8 + 8);
	assert_memory_equal(25, v.data, (const unsigned char *)
	    "\x01\x02\0\0\0\0\0\0\0\x02\0\0\0\0\0\0\0\0\0\0\0\x01\0\0\0");
	assert_int(__dtfl_summary_decode(tc, v.data, v.len, sback, &used), ==, 0);
	assert_size(used, ==, 25);
	assert_memory_equal(sizeof s, sback, s);
	__dtfl_vec_clear(&v);
	s[0] = UINT64_MAX;
	assert_size(__dtfl_summary_encode(tc, s, &v), ==, 1 + 8 + 8);
	assert_uint8(v.data[0], ==, 0);
	assert_int(__dtfl_summary_decode(tc, v.data, v.len, sback, &used), ==, 0);
	assert_memory_equal(sizeof s, sback, s);
	v.data[0] = 2;
	assert_int(__dtfl_summary_decode(tc, v.data, v.len, sback, &used), ==,
	    DTFL_EPROTO);
	__dtfl_vec_free(&v);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(constructors_reject)
{
	dtfl_time_class *u = dtfl_time_u64(), *p = dtfl_time_pointstamp(C_U64, 15);
	dtfl_time_class *chain = dtfl_time_unit(), *next;
	int i;

	(void)params; (void)data;
	assert_null(dtfl_time_total((dtfl_coord){ DTFL_COORD_UNSIGNED, 12 }));
	assert_null(dtfl_time_total((dtfl_coord){ DTFL_COORD_DURATION, 32 }));
	assert_null(dtfl_time_pointstamp(C_U64, 0));
	assert_null(dtfl_time_pointstamp(C_U64, DTFL_TIME_MAX));
	assert_null(dtfl_time_pointstamp(C_I64, 2));
	assert_not_null(p);
	/* PointStamp(15) has a 16-word summary: nothing more fits. */
	assert_null(dtfl_time_product(u, p));
	assert_null(dtfl_time_product(NULL, u));
	for (i = 0; i < DTFL_TIME_MAX; i++) {
		next = dtfl_time_product(chain, u);
		assert_not_null(next);
		dtfl_time_class_free(chain);
		chain = next;
	}
	assert_uint(dtfl_time_depth(chain), ==, DTFL_TIME_MAX);
	assert_null(dtfl_time_lex(chain, u));
	assert_string_not_equal(dtfl_last_error_detail(), "");
	assert_not_null(dtfl_time_class_retain(chain));
	dtfl_time_class_free(chain);
	dtfl_time_class_free(chain);
	dtfl_time_class_free(NULL);
	dtfl_time_class_free(p);
	dtfl_time_class_free(u);
	return MUNIT_OK;
}

/* Refines: identity, Product(outer, X), outer UNIT (timestamp.rs). */
DTFL_TEST(refines)
{
	dtfl_time_class *unit = dtfl_time_unit(), *u = dtfl_time_u64();
	dtfl_time_class *u2 = dtfl_time_u64(), *i8 = dtfl_time_total(C_I8);
	dtfl_time_class *ps = dtfl_time_pointstamp(C_U64, 2);
	dtfl_time_class *p = dtfl_time_product(u, ps);
	uint64_t t3[3], t, s3[4] = { 4, 1, 2, 3 }, s;

	(void)params; (void)data;
	assert_true(__dtfl_refines(u, u2));	/* structural identity */
	assert_true(__dtfl_refines(p, u));
	assert_true(__dtfl_refines(p, unit));
	assert_true(__dtfl_refines(i8, unit));
	assert_false(__dtfl_refines(u, p));
	assert_false(__dtfl_refines(u, i8));
	t = 9;
	__dtfl_time_to_inner(p, u, &t, t3);
	assert_memory_equal(sizeof t3, t3, ((uint64_t[3]){ 9, 0, 0 }));
	t3[1] = 4;
	__dtfl_time_to_outer(p, u, t3, &t);
	assert_uint64(t, ==, 9);
	__dtfl_summary_to_outer(p, u, s3, &s);
	assert_uint64(s, ==, 4);
	__dtfl_time_to_inner(u, u2, &(uint64_t){ 7 }, &t);
	assert_uint64(t, ==, 7);
	/* Refines<()> for i8 is Default::default() = 0, not i8::MIN. */
	__dtfl_time_to_inner(i8, unit, NULL, &t);
	assert_uint64(t, ==, S(0));
	__dtfl_time_to_inner(p, unit, NULL, t3);
	assert_memory_equal(sizeof t3, t3, ((uint64_t[3]){ 0, 0, 0 }));
	DTFL_EXPECT_PANIC(__dtfl_time_to_inner(u, i8, &t, &t));
	dtfl_time_class_free(p);
	dtfl_time_class_free(ps);
	dtfl_time_class_free(i8);
	dtfl_time_class_free(u2);
	dtfl_time_class_free(u);
	dtfl_time_class_free(unit);
	return MUNIT_OK;
}

DTFL_TEST(format_truncates)
{
	dtfl_time_class *u = dtfl_time_u64(), *p = dtfl_time_product(u, u);
	char buf[4];

	(void)params; (void)data;
	assert_int(dtfl_time_format(p, (uint64_t[2]){ 10, 20 }, buf, sizeof buf),
	    ==, 8);
	assert_string_equal(buf, "(10");
	assert_int(dtfl_time_format(p, (uint64_t[2]){ 10, 20 }, NULL, 0), ==, 8);
	dtfl_time_class_free(p);
	dtfl_time_class_free(u);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(unit_class),
	DTFL_T(total_u32_overflow),
	DTFL_T(total_u64_lattice),
	DTFL_T(total_u8_codec),
	DTFL_T(signed_bias),
	DTFL_T(duration),
	DTFL_T(product),
	DTFL_T(lex),
	DTFL_T(altneu),
	DTFL_T(pointstamp),
	DTFL_T(pointstamp_codec),
	DTFL_T(constructors_reject),
	DTFL_T(refines),
	DTFL_T(format_truncates),
	DTFL_T_END
};

DTFL_TEST_MAIN("time", tests)
