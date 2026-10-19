/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_ord_neu.c: the ord_neu Builder and Merger over the layered
 * trie.  Builds two adjacent batches and merges them, then reads the merged
 * batch through a cursor to assert that equal (key, val) updates are summed,
 * cancelling updates vanish, keys with no surviving updates are dropped, and
 * copied times are advanced to the merged since.  Also exercises the key
 * batch (OrdKeyBatch), whose single value per key is the unit value.
 */
#include "dtfl_test.h"
#include "dd/trace/trace_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

typedef struct kv {
	uint64_t k, v, t;
	int64_t r;
} kv;

static __dtfl_batch *
chunk_of(const kv *u, size_t n, bool keyed)
{
	__dtfl_batch *b = __dtfl_batch_new(1, 8);
	unsigned char rec[16];
	size_t i, len;

	for (i = 0; i < n; i++) {
		dtfl_put_u64(rec, u[i].k);
		if (keyed) {
			len = 8;
		} else {
			dtfl_put_u64(rec + 8, u[i].v);
			len = 16;
		}
		__dtfl_batch_push(b, rec, len, &u[i].t, &u[i].r);
	}
	return b;
}

static __dtfl_batch_ord *
build(const kv *u, size_t n, bool keyed, uint64_t lo, uint64_t hi,
    uint64_t since_t, dtfl_time_class *tc)
{
	__dtfl_batch *chunk = chunk_of(u, n, keyed);
	__dtfl_antichain lower, upper, since;
	__dtfl_description desc;
	__dtfl_batch_ord *b;

	__dtfl_antichain_init_elem(&lower, tc, &lo);
	__dtfl_antichain_init_elem(&upper, tc, &hi);
	__dtfl_antichain_init_elem(&since, tc, &since_t);
	__dtfl_description_init(&desc, &lower, &upper, &since);
	b = __dtfl_builder_ord_seal(keyed, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64, &chunk, 1, &desc);
	__dtfl_batch_release(chunk);
	return b;
}

struct acc {
	size_t ntimes;
	uint64_t times[16];
	int64_t diffs[16];
};

static void
collect(const uint64_t *time, const void *diff, void *ctx)
{
	struct acc *a = ctx;
	int64_t r;

	memcpy(&r, diff, 8);
	if (a->ntimes < 16) {
		a->times[a->ntimes] = time[0];
		a->diffs[a->ntimes] = r;
		a->ntimes++;
	}
}

DTFL_TEST(ord_neu__merge_consolidates_and_advances)
{
	/* Batch 1: [0, 5); key 1 val 2 at 0:+1, key 3 val 3 at 1:+1. */
	static const kv b1[] = { { 1, 2, 0, 1 }, { 3, 3, 1, 1 } };
	/* Batch 2: [5, 10); key 1 val 2 at 6:+1, key 3 val 3 at 7:-1. */
	static const kv b2[] = { { 1, 2, 6, 1 }, { 3, 3, 7, -1 } };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_batch_ord *x = build(b1, 2, false, 0, 5, 0, tc);
	__dtfl_batch_ord *y = build(b2, 2, false, 5, 10, 0, tc);
	__dtfl_antichain compaction;
	__dtfl_merger_ord m;
	int64_t fuel = 1000000;
	__dtfl_batch_ord *merged;
	__dtfl_cursor c;
	size_t len;
	struct acc a;
	uint64_t since8 = 8;

	(void)params; (void)data;
	/* Compact to 8: times <= 8 round up to 8. */
	__dtfl_antichain_init_elem(&compaction, tc, &since8);
	__dtfl_merger_ord_new(&m, x, y, &compaction);
	__dtfl_merger_ord_work(&m, &fuel);
	assert_true(fuel > 0);
	merged = __dtfl_merger_ord_done(&m);

	__dtfl_cursor_init(&c, merged);
	/* key 1 val 2: 0 and 6 both advance to 8 and sum to +2. */
	assert_uint64(dtfl_get_u64(__dtfl_cursor_key(&c, merged, &len)), ==, 1);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_val(&c, merged, &len)), ==, 2);
	memset(&a, 0, sizeof a);
	__dtfl_cursor_map_times(&c, merged, collect, &a);
	assert_size(a.ntimes, ==, 1);
	assert_uint64(a.times[0], ==, 8);
	assert_int64(a.diffs[0], ==, 2);
	__dtfl_cursor_step_key(&c, merged);
	/* key 3 val 3: 1:+1 and 7:-1 both advance to 8 and cancel; key gone. */
	assert_false(__dtfl_cursor_key_valid(&c, merged));

	__dtfl_batch_ord_release(merged);
	__dtfl_antichain_free(&compaction);
	__dtfl_batch_ord_release(x);
	__dtfl_batch_ord_release(y);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(ord_neu__key_batch_single_value)
{
	/* Key batch: keys 1, 2 (value-less); key 1 at 0:+1, key 2 at 0:+1. */
	static const kv u[] = { { 1, 0, 0, 1 }, { 2, 0, 0, 1 } };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_batch_ord *b = build(u, 2, true, 0, 5, 0, tc);
	__dtfl_cursor c;
	size_t len;
	struct acc a;

	(void)params; (void)data;
	__dtfl_cursor_init(&c, b);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_key(&c, b, &len)), ==, 1);
	/* The value is the unit value (zero length). */
	assert_true(__dtfl_cursor_val_valid(&c, b));
	(void)__dtfl_cursor_val(&c, b, &len);
	assert_size(len, ==, 0);
	memset(&a, 0, sizeof a);
	__dtfl_cursor_map_times(&c, b, collect, &a);
	assert_size(a.ntimes, ==, 1);
	/* Only one value per key. */
	__dtfl_cursor_step_val(&c, b);
	assert_false(__dtfl_cursor_val_valid(&c, b));
	__dtfl_cursor_step_key(&c, b);
	assert_uint64(dtfl_get_u64(__dtfl_cursor_key(&c, b, &len)), ==, 2);
	__dtfl_cursor_step_key(&c, b);
	assert_false(__dtfl_cursor_key_valid(&c, b));

	__dtfl_batch_ord_release(b);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * UpdsBuilder::seal must sort the stash before compacting it.  A stash
 * staged out of time order (a merge of partially ordered batches, or an
 * advance_by that reorders times) once compacted in place through a
 * sort permutation, so a later survivor read a slot an earlier one had
 * overwritten: 5, 3, 4 sealed as 3, 4, 3.
 */
DTFL_TEST(ord_neu__upds_seal_unsorted_stash)
{
	static const uint64_t t[] = { 5, 3, 4, 3 };
	static const int64_t r[] = { 1, 2, 3, 4 };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_upds_builder b;
	__dtfl_upds u;
	size_t i;
	int64_t d;

	(void)params; (void)data;
	__dtfl_upds_init(&u, 1, 8);
	__dtfl_upds_builder_init(&b, tc, I64);
	for (i = 0; i < 4; i++)
		__dtfl_upds_builder_push(&b, &t[i], &r[i]);
	assert_true(__dtfl_upds_builder_seal(&b, &u));
	/* 3:+6, 4:+3, 5:+1, in time order. */
	assert_size(u.nupds, ==, 3);
	assert_uint64(__dtfl_upds_time(&u, 0)[0], ==, 3);
	assert_uint64(__dtfl_upds_time(&u, 1)[0], ==, 4);
	assert_uint64(__dtfl_upds_time(&u, 2)[0], ==, 5);
	memcpy(&d, __dtfl_upds_diff(&u, 0), 8);
	assert_int64(d, ==, 6);
	memcpy(&d, __dtfl_upds_diff(&u, 1), 8);
	assert_int64(d, ==, 3);
	memcpy(&d, __dtfl_upds_diff(&u, 2), 8);
	assert_int64(d, ==, 1);

	__dtfl_upds_builder_free(&b);
	__dtfl_upds_free(&u);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * seal skips its sort when the stash is already in time order.  The
 * check must look at every adjacent pair, including the first: a stash
 * out of order only at its start, or one that returns to an earlier time,
 * must still come out strictly increasing with equal times summed.  Also
 * checked over a two-word Product time.
 */
static void
seal_and_check(const dtfl_time_class *tc, unsigned tw, const uint64_t *t,
    const int64_t *r, size_t n, const uint64_t *want_t, const int64_t *want_r,
    size_t want_n)
{
	__dtfl_upds_builder b;
	__dtfl_upds u;
	size_t i;
	int64_t d;

	__dtfl_upds_init(&u, tw, 8);
	__dtfl_upds_builder_init(&b, tc, I64);
	for (i = 0; i < n; i++)
		__dtfl_upds_builder_push(&b, t + (size_t)tw * i, &r[i]);
	assert_true(__dtfl_upds_builder_seal(&b, &u));
	assert_size(u.nupds, ==, want_n);
	for (i = 0; i < want_n; i++) {
		assert_memory_equal((size_t)tw * sizeof(uint64_t),
		    __dtfl_upds_time(&u, i), want_t + (size_t)tw * i);
		memcpy(&d, __dtfl_upds_diff(&u, i), 8);
		assert_int64(d, ==, want_r[i]);
	}
	__dtfl_upds_builder_free(&b);
	__dtfl_upds_free(&u);
}

DTFL_TEST(ord_neu__upds_seal_checks_every_pair)
{
	dtfl_time_class *u64 = dtfl_time_u64();
	dtfl_time_class *pr = dtfl_time_product(u64, u64);

	(void)params; (void)data;
	{	/* out of order only at the first pair */
		const uint64_t t[] = { 5, 3 };
		const int64_t r[] = { 1, 2 };
		const uint64_t wt[] = { 3, 5 };
		const int64_t wr[] = { 2, 1 };

		seal_and_check(u64, 1, t, r, 2, wt, wr, 2);
	}
	{	/* returns to an earlier time: 5 appears twice, apart */
		const uint64_t t[] = { 5, 3, 5 };
		const int64_t r[] = { 1, 2, 4 };
		const uint64_t wt[] = { 3, 5 };
		const int64_t wr[] = { 2, 5 };

		seal_and_check(u64, 1, t, r, 3, wt, wr, 2);
	}
	{	/* Product time, out of order at the first pair, a repeat */
		const uint64_t t[] = { 1, 1,  0, 2,  1, 1 };
		const int64_t r[] = { 1, 7, 2 };
		const uint64_t wt[] = { 0, 2,  1, 1 };
		const int64_t wr[] = { 7, 3 };

		seal_and_check(pr, 2, t, r, 3, wt, wr, 2);
	}
	dtfl_time_class_free(pr);
	dtfl_time_class_free(u64);
	return MUNIT_OK;
}

/*
 * seal takes an inline path for one-word times and a 64-bit built-in
 * integer diff.  A user class with the same i64 algebra takes the general
 * path, so sealing the same stashes through both must give the same
 * updates and the same singleton encoding, run after run.
 */
static void
u_plus(const __dtfl_diff_class *c, void *acc, const void *x)
{
	int64_t a, b;

	(void)c;
	memcpy(&a, acc, 8);
	memcpy(&b, x, 8);
	a = (int64_t)((uint64_t)a + (uint64_t)b);
	memcpy(acc, &a, 8);
}

static bool
u_is_zero(const __dtfl_diff_class *c, const void *x)
{
	int64_t a;

	(void)c;
	memcpy(&a, x, 8);
	return a == 0;
}

static int
u_cmp(const __dtfl_diff_class *c, const void *a, const void *b)
{
	int64_t x, y;

	(void)c;
	memcpy(&x, a, 8);
	memcpy(&y, b, 8);
	return (x > y) - (x < y);
}

DTFL_TEST(ord_neu__upds_seal_fast_path_matches_general)
{
	__dtfl_diff_class def;
	const __dtfl_diff_class *user;
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_upds_builder fb, gb;
	__dtfl_upds fu, gu;
	uint64_t rng = 0x9e3779b97f4a7c15ULL;
	int run;

	(void)params; (void)data;
	memset(&def, 0, sizeof def);
	def.name = "user-i64";
	def.width = 8;
	def.plus_equals = u_plus;
	def.is_zero = u_is_zero;
	def.cmp = u_cmp;
	user = __dtfl_diff_class_new(&def);
	__dtfl_upds_init(&fu, 1, 8);
	__dtfl_upds_init(&gu, 1, 8);
	__dtfl_upds_builder_init(&fb, tc, I64);
	__dtfl_upds_builder_init(&gb, tc, user);
	for (run = 0; run < 2000; run++) {
		size_t n, i;
		bool fr, gr;

		rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
		n = 1 + rng % 6;
		for (i = 0; i < n; i++) {
			uint64_t t;
			int64_t r;

			rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
			/* Few distinct times and diffs: repeats, zeros, sorts. */
			t = rng % 4;
			r = (int64_t)((rng >> 8) % 5) - 2;
			__dtfl_upds_builder_push(&fb, &t, &r);
			__dtfl_upds_builder_push(&gb, &t, &r);
		}
		fr = __dtfl_upds_builder_seal(&fb, &fu);
		gr = __dtfl_upds_builder_seal(&gb, &gu);
		assert_int(fr, ==, gr);
		assert_size(fb.total, ==, gb.total);
	}
	assert_size(fu.nupds, ==, gu.nupds);
	assert_size(fu.offs.v.len, ==, gu.offs.v.len);
	assert_memory_equal(fu.offs.v.len * sizeof(size_t), fu.offs.v.data,
	    gu.offs.v.data);
	assert_memory_equal(fu.nupds * 8, fu.times.data, gu.times.data);
	assert_memory_equal(fu.nupds * 8, fu.diffs.data, gu.diffs.data);

	__dtfl_upds_builder_free(&fb);
	__dtfl_upds_builder_free(&gb);
	__dtfl_upds_free(&fu);
	__dtfl_upds_free(&gu);
	__dtfl_diff_class_free(user);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * The merger's one-word-time path (stash_updates for a TOTAL time,
 * then seal's inline path for an 8-byte integer diff) against the
 * general path.  Product<(), u64> is also one totally ordered word but
 * is not a TOTAL shape, and a user i64 class is not a built-in, so the
 * same batches built and merged under those classes go the general way.
 * The merged storage must match byte for byte, for key and value
 * batches, random compaction frontiers and fuel split across calls.
 */
static uint64_t
xs(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static int
kv_cmp(const void *a, const void *b)
{
	const kv *x = a, *y = b;

	if (x->k != y->k)
		return x->k < y->k ? -1 : 1;
	if (x->v != y->v)
		return x->v < y->v ? -1 : 1;
	return (x->t > y->t) - (x->t < y->t);
}

/* Sort and consolidate, as the merge batcher hands the builder a chain. */
static size_t
kv_consolidate(kv *u, size_t n)
{
	size_t i, out = 0;

	qsort(u, n, sizeof *u, kv_cmp);
	for (i = 0; i < n; i++) {
		if (out != 0 && kv_cmp(&u[out - 1], &u[i]) == 0)
			u[out - 1].r += u[i].r;
		else
			u[out++] = u[i];
		if (u[out - 1].r == 0)
			out--;
	}
	return out;
}

static __dtfl_batch_ord *
build_cls(const kv *u, size_t n, bool keyed, uint64_t lo, uint64_t hi,
    const dtfl_time_class *tc, const __dtfl_diff_class *dc)
{
	__dtfl_batch *chunk = chunk_of(u, n, keyed);
	__dtfl_antichain lower, upper, since;
	__dtfl_description desc;
	uint64_t zero = 0;
	__dtfl_batch_ord *b;

	__dtfl_antichain_init_elem(&lower, tc, &lo);
	__dtfl_antichain_init_elem(&upper, tc, &hi);
	__dtfl_antichain_init_elem(&since, tc, &zero);
	__dtfl_description_init(&desc, &lower, &upper, &since);
	b = __dtfl_builder_ord_seal(keyed, dtfl_type_u64(), dtfl_type_u64(),
	    tc, dc, &chunk, 1, &desc);
	__dtfl_batch_release(chunk);
	return b;
}

static void
assert_vec_eq(const __dtfl_vec *a, const __dtfl_vec *b)
{
	assert_size(a->len * a->elem, ==, b->len * b->elem);
	if (a->len != 0)
		assert_memory_equal(a->len * a->elem, a->data, b->data);
}

static void
assert_storage_eq(const __dtfl_batch_ord *x, const __dtfl_batch_ord *y)
{
	assert_size(x->updates, ==, y->updates);
	assert_vec_eq(&x->storage.keys.arena, &y->storage.keys.arena);
	assert_vec_eq(&x->storage.keys.offs, &y->storage.keys.offs);
	assert_vec_eq(&x->storage.vals.offs.v, &y->storage.vals.offs.v);
	assert_vec_eq(&x->storage.vals.vals.arena, &y->storage.vals.vals.arena);
	assert_vec_eq(&x->storage.vals.vals.offs, &y->storage.vals.vals.offs);
	assert_vec_eq(&x->storage.upds.offs.v, &y->storage.upds.offs.v);
	assert_vec_eq(&x->storage.upds.times, &y->storage.upds.times);
	assert_vec_eq(&x->storage.upds.diffs, &y->storage.upds.diffs);
	assert_size(x->storage.upds.nupds, ==, y->storage.upds.nupds);
}

DTFL_TEST(ord_neu__merge_fast_path_matches_general)
{
	__dtfl_diff_class def;
	const __dtfl_diff_class *user;
	dtfl_time_class *ftc = dtfl_time_u64();
	dtfl_time_class *unit = dtfl_time_unit();
	dtfl_time_class *gtc = dtfl_time_product(unit, ftc);
	uint64_t rng = 0x2545f4914f6cdd1dULL;
	int run;

	(void)params; (void)data;
	assert_uint(dtfl_time_depth(gtc), ==, 1);
	memset(&def, 0, sizeof def);
	def.name = "user-i64";
	def.width = 8;
	def.plus_equals = u_plus;
	def.is_zero = u_is_zero;
	def.cmp = u_cmp;
	user = __dtfl_diff_class_new(&def);
	for (run = 0; run < 400; run++) {
		kv u1[24], u2[24];
		size_t n1 = xs(&rng) % 24, n2 = xs(&rng) % 24, i;
		bool keyed = run % 2 == 0;
		uint64_t c = xs(&rng) % 12;
		__dtfl_batch_ord *f1, *f2, *g1, *g2, *fm, *gm;
		__dtfl_antichain fc, gc;
		__dtfl_merger_ord fmg, gmg;
		int64_t step = 1 + (int64_t)(xs(&rng) % 8), ff = 0, gf = 0;

		for (i = 0; i < n1; i++) {
			u1[i].k = xs(&rng) % 8;
			u1[i].v = keyed ? 0 : xs(&rng) % 3;
			u1[i].t = xs(&rng) % 5;
			u1[i].r = (int64_t)(xs(&rng) % 5) - 2;
		}
		for (i = 0; i < n2; i++) {
			u2[i].k = xs(&rng) % 8;
			u2[i].v = keyed ? 0 : xs(&rng) % 3;
			u2[i].t = 5 + xs(&rng) % 5;
			u2[i].r = (int64_t)(xs(&rng) % 5) - 2;
		}
		n1 = kv_consolidate(u1, n1);
		n2 = kv_consolidate(u2, n2);
		f1 = build_cls(u1, n1, keyed, 0, 5, ftc, I64);
		f2 = build_cls(u2, n2, keyed, 5, 10, ftc, I64);
		g1 = build_cls(u1, n1, keyed, 0, 5, gtc, user);
		g2 = build_cls(u2, n2, keyed, 5, 10, gtc, user);
		assert_storage_eq(f1, g1);
		assert_storage_eq(f2, g2);

		/* The compaction frontier: empty, or {c}. */
		__dtfl_antichain_init(&fc, ftc);
		__dtfl_antichain_init(&gc, gtc);
		if (run % 5 != 0) {
			(void)__dtfl_antichain_insert(&fc, &c);
			(void)__dtfl_antichain_insert(&gc, &c);
		}
		__dtfl_merger_ord_new(&fmg, f1, f2, &fc);
		__dtfl_merger_ord_new(&gmg, g1, g2, &gc);
		/* Small fuel per call, so the merge pauses between keys. */
		while (ff <= 0) {
			ff = step;
			gf = step;
			__dtfl_merger_ord_work(&fmg, &ff);
			__dtfl_merger_ord_work(&gmg, &gf);
			assert_int64(ff, ==, gf);
		}
		fm = __dtfl_merger_ord_done(&fmg);
		gm = __dtfl_merger_ord_done(&gmg);
		assert_storage_eq(fm, gm);

		__dtfl_batch_ord_release(fm);
		__dtfl_batch_ord_release(gm);
		__dtfl_antichain_free(&fc);
		__dtfl_antichain_free(&gc);
		__dtfl_batch_ord_release(f1);
		__dtfl_batch_ord_release(f2);
		__dtfl_batch_ord_release(g1);
		__dtfl_batch_ord_release(g2);
	}
	__dtfl_diff_class_free(user);
	dtfl_time_class_free(gtc);
	dtfl_time_class_free(unit);
	dtfl_time_class_free(ftc);
	return MUNIT_OK;
}

/*
 * BatchContainer::advance over slices: the count of values below a key.
 * The all-8-byte layout (all8) takes its own probe path, so compare it,
 * position by position, with a linear count; then push one 3-byte value
 * so the same slice takes the offsets path, and clear it to get all8
 * back.  Keys are big-endian, so byte order is numeric order.
 */
static size_t
linear_below(const __dtfl_slices *s, size_t start, size_t end, uint64_t k)
{
	unsigned char kb[8];
	size_t i, n = 0;

	dtfl_put_u64(kb, k);
	for (i = start; i < end; i++) {
		size_t len;
		const void *v = __dtfl_slices_get(s, i, &len);

		if (__dtfl_cmp_bytes(v, len, kb, 8) < 0)
			n++;
	}
	return n;
}

static void
check_advance(const __dtfl_slices *s, size_t n)
{
	size_t start, end;
	uint64_t k;

	for (start = 0; start <= n; start += 3)
		for (end = start; end <= n; end += 5)
			for (k = 0; k < 2 * n + 4; k++) {
				unsigned char kb[8];

				dtfl_put_u64(kb, k);
				assert_size(__dtfl_slices_advance(s, dtfl_type_u64(),
				    start, end, kb, 8), ==,
				    linear_below(s, start, end, k));
			}
}

DTFL_TEST(ord_neu__slices_advance_all8_matches_linear)
{
	__dtfl_slices s;
	unsigned char v[8];
	size_t i, n = 70;

	(void)params; (void)data;
	__dtfl_slices_init(&s);
	assert_true(s.all8);
	for (i = 0; i < n; i++) {
		dtfl_put_u64(v, 2 * i + 1);	/* odd keys: probes hit gaps */
		__dtfl_slices_push(&s, v, 8);
	}
	assert_true(s.all8);
	check_advance(&s, n);
	__dtfl_slices_push(&s, "\xff\xff\xff", 3);	/* sorts after them all */
	assert_false(s.all8);
	check_advance(&s, n + 1);
	__dtfl_slices_clear(&s);
	assert_true(s.all8);
	__dtfl_slices_free(&s);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(ord_neu__merge_consolidates_and_advances),
	DTFL_T(ord_neu__merge_fast_path_matches_general),
	DTFL_T(ord_neu__upds_seal_unsorted_stash),
	DTFL_T(ord_neu__upds_seal_checks_every_pair),
	DTFL_T(ord_neu__upds_seal_fast_path_matches_general),
	DTFL_T(ord_neu__slices_advance_all8_matches_linear),
	DTFL_T(ord_neu__key_batch_single_value),
	DTFL_T_END
};

DTFL_TEST_MAIN("ord_neu", tests)
