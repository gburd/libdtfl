/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_consolidate.c: consolidation and the consolidating
 * container builder.  The consolidation__ tests port the #[test]s of
 * differential-dataflow/src/consolidation.rs at
 * differential-dataflow-v0.25.1 (PLAN D6).  Upstream's &str records are
 * string records, integer records u64, and integer diffs the i64 class.
 */
#include "dtfl_test.h"
#include "dd/dd_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

typedef struct supd {
	const char *d;
	uint64_t t;
	int64_t r;
} supd;

/* A batch of (string, [time], i64) updates; tw is 0 or 1. */
static __dtfl_batch *
str_batch(const supd *u, size_t n, unsigned tw)
{
	__dtfl_batch *b = __dtfl_batch_new(tw, 8);
	unsigned char buf[64];
	size_t i, len;

	for (i = 0; i < n; i++) {
		len = dtfl_put_bytes(buf, u[i].d, strlen(u[i].d));
		__dtfl_batch_push(b, buf, len, &u[i].t, &u[i].r);
	}
	return b;
}

/* True if b holds exactly the updates u[0..n), in order. */
static bool
str_batch_is(const __dtfl_batch *b, const supd *u, size_t n)
{
	const void *p, *s;
	size_t i, len, slen;
	int64_t r;

	if (__dtfl_batch_len(b) != n)
		return false;
	for (i = 0; i < n; i++) {
		p = __dtfl_batch_rec(b, i, &len);
		s = dtfl_get_bytes(p, &slen);
		memcpy(&r, __dtfl_batch_diff(b, i), 8);
		if (slen != strlen(u[i].d) || memcmp(s, u[i].d, slen) != 0 ||
		    r != u[i].r || (b->tw != 0 && __dtfl_batch_time(b, i)[0] !=
		    u[i].t))
			return false;
	}
	return true;
}

/* A (u64, u64, i64) update. */
typedef struct uupd {
	uint64_t d, t;
	int64_t r;
} uupd;

static void
push_u(__dtfl_batch *b, uint64_t d, uint64_t t, const void *r)
{
	unsigned char rec[8];

	dtfl_put_u64(rec, d);
	__dtfl_batch_push(b, rec, 8, &t, r);
}

static uupd
get_u(const __dtfl_batch *b, size_t i)
{
	uupd u;
	size_t len;

	u.d = dtfl_get_u64(__dtfl_batch_rec(b, i, &len));
	u.t = __dtfl_batch_time(b, i)[0];
	memcpy(&u.r, __dtfl_batch_diff(b, i), 8);
	return u;
}

DTFL_TEST(consolidation__test_consolidate)
{
	static const supd in[5][3] = {
		{ { "a", 0, -1 }, { "b", 0, -2 }, { "a", 0, 1 } },
		{ { "a", 0, -1 }, { "b", 0, 0 }, { "a", 0, 1 } },
		{ { "a", 0, 0 } },
		{ { "a", 0, 0 }, { "b", 0, 0 } },
		{ { "a", 0, 1 }, { "b", 0, 1 } },
	};
	static const size_t nin[5] = { 3, 3, 1, 2, 2 };
	static const supd out[5][2] = {
		{ { "b", 0, -2 } }, { { "", 0, 0 } }, { { "", 0, 0 } }, { { "", 0, 0 } },
		{ { "a", 0, 1 }, { "b", 0, 1 } },
	};
	static const size_t nout[5] = { 1, 0, 0, 0, 2 };
	__dtfl_batch *b;
	size_t i;

	(void)params; (void)data;
	for (i = 0; i < 5; i++) {
		b = str_batch(in[i], nin[i], 0);
		__dtfl_consolidate_batch(b, dtfl_type_string(), I64);
		assert_true(str_batch_is(b, out[i], nout[i]));
		__dtfl_batch_release(b);
	}
	return MUNIT_OK;
}

DTFL_TEST(consolidation__test_consolidate_updates)
{
	static const supd in[5][3] = {
		{ { "a", 1, -1 }, { "b", 1, -2 }, { "a", 1, 1 } },
		{ { "a", 1, -1 }, { "b", 1, 0 }, { "a", 1, 1 } },
		{ { "a", 1, 0 } },
		{ { "a", 1, 0 }, { "b", 1, 0 } },
		{ { "a", 1, 1 }, { "b", 2, 1 } },
	};
	static const size_t nin[5] = { 3, 3, 1, 2, 2 };
	static const supd out[5][2] = {
		{ { "b", 1, -2 } }, { { "", 0, 0 } }, { { "", 0, 0 } }, { { "", 0, 0 } },
		{ { "a", 1, 1 }, { "b", 2, 1 } },
	};
	static const size_t nout[5] = { 1, 0, 0, 0, 2 };
	__dtfl_batch *b;
	size_t i;

	(void)params; (void)data;
	for (i = 0; i < 5; i++) {
		b = str_batch(in[i], nin[i], 1);
		__dtfl_consolidate_batch(b, dtfl_type_string(), I64);
		assert_true(str_batch_is(b, out[i], nout[i]));
		__dtfl_batch_release(b);
	}
	return MUNIT_OK;
}

static int
cmp_uupd(const void *a, const void *b)
{
	const uupd *x = a, *y = b;

	if (x->d != y->d)
		return x->d < y->d ? -1 : 1;
	if (x->t != y->t)
		return x->t < y->t ? -1 : 1;
	return (x->r > y->r) - (x->r < y->r);
}

/* Upstream's (usize, usize, usize) is (u64 record, u64 time, u64 diff). */
DTFL_TEST(consolidation__test_consolidating_container_builder)
{
	const __dtfl_diff_class *u64c = __dtfl_diff_builtin(DTFL_DIFF_U64);
	static uupd collected[1024];
	__dtfl_consolidator ccb;
	__dtfl_batch *c;
	uint64_t zero = 0, one = 1;
	size_t i, n = 0;

	(void)params; (void)data;
	__dtfl_consolidator_init(&ccb, dtfl_type_u64(), 1, u64c);
	assert_size(ccb.cap, ==, 8192 / 24);
	for (i = 0; i < 1024; i++) {
		unsigned char rec[8];

		dtfl_put_u64(rec, 0);
		__dtfl_consolidator_push(&ccb, rec, 8, &zero, &zero);
	}
	assert_null(__dtfl_consolidator_extract(&ccb));
	assert_null(__dtfl_consolidator_finish(&ccb));

	for (i = 0; i < 1024; i++) {
		unsigned char rec[8];

		dtfl_put_u64(rec, i);
		__dtfl_consolidator_push(&ccb, rec, 8, &zero, &one);
	}
	while ((c = __dtfl_consolidator_finish(&ccb)) != NULL) {
		assert_size(__dtfl_batch_len(c), <=, ccb.cap);
		for (i = 0; i < __dtfl_batch_len(c); i++) {
			assert_size(n, <, 1024);
			collected[n++] = get_u(c, i);
		}
		__dtfl_batch_release(c);
	}
	/* The output happens to be sorted, but it's not guaranteed. */
	assert_size(n, ==, 1024);
	qsort(collected, n, sizeof collected[0], cmp_uupd);
	for (i = 0; i < 1024; i++) {
		assert_uint64(collected[i].d, ==, i);
		assert_uint64(collected[i].t, ==, 0);
		assert_int64(collected[i].r, ==, 1);
	}
	__dtfl_consolidator_free(&ccb);
	return MUNIT_OK;
}

/*
 * Consolidate::consolidate_into is consolidate_updates followed by a
 * swap into target; in place, the batch is the target.
 */
DTFL_TEST(consolidation__test_consolidate_into)
{
	__dtfl_batch *b = __dtfl_batch_new(1, 8);
	uupd u;

	(void)params; (void)data;
	push_u(b, 1, 1, &(int64_t){ 1 });
	push_u(b, 2, 1, &(int64_t){ 1 });
	push_u(b, 1, 1, &(int64_t){ -1 });
	__dtfl_consolidate_batch(b, dtfl_type_u64(), I64);
	assert_size(__dtfl_batch_len(b), ==, 1);
	u = get_u(b, 0);
	assert_uint64(u.d, ==, 2);
	assert_uint64(u.t, ==, 1);
	assert_int64(u.r, ==, 1);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

/* Upstream's debug-build sizes. */
#define LEN (256 << 1)
#define REPS (10 << 1)

/* (i/4, 1, -2 + i%4) for i in 0..LEN, optionally sorted by record. */
static __dtfl_batch *
duration_data(unsigned tw, bool sorted)
{
	__dtfl_batch *b = __dtfl_batch_new(tw, 8);
	size_t i, k;
	int64_t r;

	for (k = 0; k < LEN; k++) {
		/* Unsorted: walk the rows in a fixed scrambled order. */
		i = sorted ? k : (k * 37) % LEN;
		r = -2 + (int64_t)(i % 4);
		push_u(b, i / 4, 1, &r);
	}
	return b;
}

/*
 * Upstream's time is the integer 1 and the "duration" is the elapsed
 * wall-clock time it prints.  The time here is the Duration class's
 * single word (1 ns); consolidation only compares time words.
 */
DTFL_TEST(consolidation__test_consolidator_duration)
{
	dtfl_time_class *tc = dtfl_time_duration();
	unsigned tw = dtfl_time_depth(tc);
	__dtfl_batch *d1, *d2;
	uint64_t start, ns = 0;
	size_t rep, i;

	(void)params; (void)data;
	for (rep = 0; rep < REPS; rep++) {
		d1 = duration_data(tw, true);
		d2 = duration_data(tw, false);
		start = __dtfl_clock_ns();
		__dtfl_consolidate_batch(d1, dtfl_type_u64(), I64);
		ns += __dtfl_clock_ns() - start;
		__dtfl_consolidate_batch(d2, dtfl_type_u64(), I64);
		assert_size(__dtfl_batch_len(d1), ==, __dtfl_batch_len(d2));
		assert_size(__dtfl_batch_len(d1), ==, LEN / 4);
		for (i = 0; i < __dtfl_batch_len(d1); i++) {
			uupd a = get_u(d1, i), b = get_u(d2, i);

			assert_int(cmp_uupd(&a, &b), ==, 0);
			assert_uint64(a.d, ==, i);
			assert_int64(a.r, ==, -2);
		}
		__dtfl_batch_release(d1);
		__dtfl_batch_release(d2);
	}
	munit_logf(MUNIT_LOG_INFO, "elapsed consolidator %" PRIu64 " ns", ns);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * The "_vec" variant times consolidate_updates on the Vec directly; its
 * diffs are isize, not Vec diffs, so it ports as is.  Upstream asserts
 * nothing; this checks the result as the sibling test does.
 */
DTFL_TEST(consolidation__test_consolidator_duration_vec)
{
	dtfl_time_class *tc = dtfl_time_duration();
	unsigned tw = dtfl_time_depth(tc);
	__dtfl_batch *d;
	uint64_t start, ns = 0;
	size_t rep, i;

	(void)params; (void)data;
	for (rep = 0; rep < REPS; rep++) {
		d = duration_data(tw, true);
		start = __dtfl_clock_ns();
		__dtfl_consolidate_batch(d, dtfl_type_u64(), I64);
		ns += __dtfl_clock_ns() - start;
		assert_size(__dtfl_batch_len(d), ==, LEN / 4);
		for (i = 0; i < LEN / 4; i++) {
			uupd a = get_u(d, i);

			assert_uint64(a.d, ==, i);
			assert_uint64(a.t, ==, 1);
			assert_int64(a.r, ==, -2);
		}
		__dtfl_batch_release(d);
	}
	munit_logf(MUNIT_LOG_INFO, "elapsed vec %" PRIu64 " ns", ns);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* A consolidated batch keeps its storage invariants for later pushes. */
DTFL_TEST(consolidate_then_push)
{
	__dtfl_batch *b = __dtfl_batch_new(1, 8);

	(void)params; (void)data;
	__dtfl_consolidate_batch(b, dtfl_type_u64(), I64);
	assert_size(__dtfl_batch_len(b), ==, 0);
	push_u(b, 3, 0, &(int64_t){ 1 });
	push_u(b, 3, 0, &(int64_t){ -1 });
	__dtfl_consolidate_batch(b, dtfl_type_u64(), I64);
	assert_size(__dtfl_batch_len(b), ==, 0);
	push_u(b, 4, 0, &(int64_t){ 2 });
	assert_size(__dtfl_batch_len(b), ==, 1);
	assert_uint64(get_u(b, 0).d, ==, 4);
	/* Shared batches are not consolidated in place. */
	(void)__dtfl_batch_retain(b);
	DTFL_EXPECT_PANIC(__dtfl_consolidate_batch(b, dtfl_type_u64(), I64));
	__dtfl_batch_release(b);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

/* description.rs: new asserts a non-empty lower; eq is per frontier. */
DTFL_TEST(description_basics)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_antichain lo, up, si, e;
	__dtfl_description d, d2;

	(void)params; (void)data;
	__dtfl_antichain_init_elem(&lo, tc, &(uint64_t){ 0 });
	__dtfl_antichain_init_elem(&up, tc, &(uint64_t){ 5 });
	__dtfl_antichain_init_elem(&si, tc, &(uint64_t){ 2 });
	__dtfl_description_init(&d, &lo, &up, &si);
	assert_uint64(__dtfl_antichain_at(__dtfl_description_lower(&d), 0)[0],
	    ==, 0);
	assert_uint64(__dtfl_antichain_at(__dtfl_description_upper(&d), 0)[0],
	    ==, 5);
	assert_uint64(__dtfl_antichain_at(__dtfl_description_since(&d), 0)[0],
	    ==, 2);

	__dtfl_antichain_init_elem(&lo, tc, &(uint64_t){ 0 });
	__dtfl_antichain_init_elem(&up, tc, &(uint64_t){ 5 });
	__dtfl_antichain_init(&si, tc);
	__dtfl_description_init(&d2, &lo, &up, &si);
	assert_true(__dtfl_description_eq(&d, &d));
	assert_false(__dtfl_description_eq(&d, &d2));
	__dtfl_description_free(&d);
	__dtfl_description_free(&d2);

	__dtfl_antichain_init(&e, tc);
	__dtfl_antichain_init(&up, tc);
	__dtfl_antichain_init(&si, tc);
	DTFL_EXPECT_PANIC(__dtfl_description_init(&d, &e, &up, &si));
	__dtfl_antichain_free(&e);
	__dtfl_antichain_free(&up);
	__dtfl_antichain_free(&si);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* memcmp then length, as the NULL cmp does, but through a user cmp. */
static int
user_memcmp(const dtfl_type *t, const void *a, size_t alen, const void *b,
    size_t blen)
{
	size_t m = alen < blen ? alen : blen;
	int c = m != 0 ? memcmp(a, b, m) : 0;

	(void)t;
	if (c != 0)
		return c;
	return (alen > blen) - (alen < blen);
}

/*
 * The packed fast sort (memcmp-ordered records of one width up to 8
 * bytes, at most one time word) must produce the same batch, byte for
 * byte, as the index sort, which a user cmp of the same order forces.
 * Widths 0, 3 and 8 and tw 0, 1 and 2 (2 takes the index sort in both).
 * 40 rows take the packed merge sort and 3000 the packed radix sort; the
 * last rep draws times over all 64 bits, so the radix sort passes over
 * an even number of bytes there and an odd number elsewhere.
 */
DTFL_TEST(consolidate_packed_matches_index_sort)
{
	static const size_t widths[] = { 0, 3, 8 }, sizes[] = { 40, 3000 };
	dtfl_type slow = *dtfl_type_u64();
	uint64_t x = 0x9e3779b97f4a7c15u;
	size_t wi, ni, i, n;
	unsigned tw, rep;

	(void)params; (void)data;
	slow.cmp = user_memcmp;
	for (ni = 0; ni < 2; ni++)
	for (rep = 0; rep < 4; rep++)
	for (wi = 0; wi < 3; wi++)
	for (tw = 0; tw <= 2; tw++) {
		size_t w = widths[wi];
		__dtfl_batch *a = __dtfl_batch_new(tw, 8);
		__dtfl_batch *b;
		unsigned char rec[8];
		uint64_t t[2];
		int64_t r;

		slow.fixed_size = w;
		n = sizes[ni];
		for (i = 0; i < n; i++) {
			size_t j;

			for (j = 0; j < w; j++) {
				x ^= x << 13, x ^= x >> 7, x ^= x << 17;
				/* Few distinct bytes so runs collide. */
				rec[j] = (unsigned char)(x % (rep + 2));
			}
			x ^= x << 13, x ^= x >> 7, x ^= x << 17;
			t[0] = rep == 3 ? x : x % 3;
			t[1] = (x >> 8) % 2;
			r = (int64_t)((x >> 16) % 5) - 2;
			__dtfl_batch_push(a, rec, w, t, &r);
		}
		b = __dtfl_batch_copy(a);
		__dtfl_consolidate_batch(a, dtfl_type_u64(), I64);
		__dtfl_consolidate_batch(b, &slow, I64);
		assert_size(__dtfl_batch_len(a), ==, __dtfl_batch_len(b));
		assert_size(a->arena.len, ==, b->arena.len);
		assert_size(a->times.len, ==, b->times.len);
		assert_size(a->diffs.len, ==, b->diffs.len);
		/* A zero-width column is NULL; memcmp must not see it. */
		if (a->arena.len != 0)
			assert_memory_equal(a->arena.len, a->arena.data,
			    b->arena.data);
		assert_memory_equal(a->offs.len * sizeof(size_t), a->offs.data,
		    b->offs.data);
		if (a->times.len != 0)
			assert_memory_equal(a->times.len * a->times.elem,
			    a->times.data, b->times.data);
		assert_memory_equal(a->diffs.len, a->diffs.data, b->diffs.data);
		__dtfl_batch_release(a);
		__dtfl_batch_release(b);
	}
	return MUNIT_OK;
}

/*
 * A memcmp-ordered type whose values vary in length (cmp NULL, a len
 * callback) must not take the packed sort: packing pads to 8 bytes, so
 * "\x01" and "\x01\x00" would pack to the same word and merge.  Mixed
 * lengths in one batch must sort by memcmp then length and keep the two
 * records distinct, exactly as the index sort does.
 */
static size_t
first_byte_len(const dtfl_type *t, const void *p, size_t avail)
{
	(void)t;
	if (avail == 0)
		return SIZE_MAX;
	return 1 + *(const unsigned char *)p % 3;	/* 1, 2 or 3 bytes */
}

DTFL_TEST(consolidate_packed_rejects_mixed_lengths)
{
	dtfl_type var = { 0 };
	dtfl_type slow;
	static const unsigned char r1[] = { 0x03 };		/* len 1 */
	static const unsigned char r2[] = { 0x03, 0x00 };	/* len 1 too */
	static const unsigned char r3[] = { 0x04, 0x00 };	/* len 2 */
	static const unsigned char r4[] = { 0x04 };		/* prefix of r3 */
	const unsigned char *recs[] = { r3, r1, r4, r3, r4 };
	const size_t lens[] = { 2, 1, 1, 2, 1 };
	__dtfl_batch *a = __dtfl_batch_new(1, 8), *b;
	uint64_t t = 0;
	int64_t one = 1;
	size_t i, len;
	const unsigned char *p;

	(void)params; (void)data; (void)r2;
	var.name = "var";
	var.len = first_byte_len;
	slow = var;
	slow.cmp = user_memcmp;
	for (i = 0; i < 5; i++)
		__dtfl_batch_push(a, recs[i], lens[i], &t, &one);
	b = __dtfl_batch_copy(a);
	__dtfl_consolidate_batch(a, &var, I64);
	__dtfl_consolidate_batch(b, &slow, I64);
	/* {0x03}, {0x04}, {0x04,0x00}: three records, prefix before longer. */
	assert_size(__dtfl_batch_len(a), ==, 3);
	assert_size(__dtfl_batch_len(a), ==, __dtfl_batch_len(b));
	assert_memory_equal(a->offs.len * sizeof(size_t), a->offs.data,
	    b->offs.data);
	assert_memory_equal(a->arena.len, a->arena.data, b->arena.data);
	assert_memory_equal(a->diffs.len, a->diffs.data, b->diffs.data);
	p = __dtfl_batch_rec(a, 1, &len);
	assert_size(len, ==, 1);
	assert_uint8(p[0], ==, 0x04);
	p = __dtfl_batch_rec(a, 2, &len);
	assert_size(len, ==, 2);
	__dtfl_batch_release(a);
	__dtfl_batch_release(b);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(consolidation__test_consolidate),
	DTFL_T(consolidation__test_consolidate_updates),
	DTFL_T(consolidation__test_consolidating_container_builder),
	DTFL_T(consolidation__test_consolidate_into),
	DTFL_T(consolidation__test_consolidator_duration),
	DTFL_T(consolidation__test_consolidator_duration_vec),
	DTFL_T(consolidate_then_push),
	DTFL_T(consolidate_packed_matches_index_sort),
	DTFL_T(consolidate_packed_rejects_mixed_lengths),
	DTFL_T(description_basics),
	DTFL_T_END
};

DTFL_TEST_MAIN("consolidate", tests)
