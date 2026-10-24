/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_merge_batcher.c: the MergeBatcher frontier tests.  Ports
 * trace_implementations_merge_batcher_test__frontier_is_post_consolidation
 * and __frontier_survivor_minimum from
 * differential-dataflow/src/trace/implementations/merge_batcher.rs
 * (differential-dataflow-v0.25.1).  Upstream uses (u64, u64, i64) updates;
 * here records are order-preserving u64 bytes, times a single u64 word, and
 * diffs the i64 class.  The sealed frontier must reflect the set of distinct
 * kept times AFTER consolidation, so a kept time whose updates cancel to zero
 * holds no capability.
 */
#include "dtfl_test.h"
#include "dd/trace/trace_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

/* A one-chunk chain carrying the given (record u64, time u64, i64) updates. */
typedef struct u3 {
	uint64_t d, t;
	int64_t r;
} u3;

static __dtfl_batch *
chunk_of(const u3 *u, size_t n)
{
	__dtfl_batch *b = __dtfl_batch_new(1, 8);
	unsigned char rec[8];
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_put_u64(rec, u[i].d);
		__dtfl_batch_push(b, rec, 8, &u[i].t, &u[i].r);
	}
	return b;
}

/* Pushes one chain of a single chunk into the batcher. */
static void
push_chain(__dtfl_merge_batcher *mb, const u3 *u, size_t n)
{
	__dtfl_batch *chunk = chunk_of(u, n);

	__dtfl_merge_batcher_chain_push(mb, &chunk, 1);
}

/* The sorted set of frontier times after a seal. */
static void
frontier_times(const __dtfl_antichain *f, uint64_t *out, size_t *n)
{
	size_t i;

	*n = __dtfl_antichain_len(f);
	for (i = 0; i < *n; i++)
		out[i] = __dtfl_antichain_at(f, i)[0];
	/* Sort ascending (small). */
	for (i = 1; i < *n; i++) {
		uint64_t key = out[i];
		size_t j = i;

		while (j > 0 && out[j - 1] > key) {
			out[j] = out[j - 1];
			j--;
		}
		out[j] = key;
	}
}

static void
release_chunks(__dtfl_batch **out, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		__dtfl_batch_release(out[i]);
	__dtfl_free(out);
}

DTFL_TEST(trace_implementations_merge_batcher_test__frontier_is_post_consolidation)
{
	static const u3 chain1[] = { { 100, 5, 1 }, { 200, 7, 1 } };
	static const u3 chain2[] = { { 100, 5, -1 } };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_merge_batcher mb;
	__dtfl_antichain upper;
	__dtfl_description desc;
	__dtfl_batch **out;
	size_t nout, nf;
	uint64_t ft[8], three = 3;

	(void)params; (void)data;
	__dtfl_merge_batcher_init(&mb, dtfl_type_u64(), tc, I64);
	push_chain(&mb, chain1, 2);
	push_chain(&mb, chain2, 1);
	__dtfl_antichain_init_elem(&upper, tc, &three);
	__dtfl_merge_batcher_seal(&mb, &upper, &out, &nout, &desc);

	/* (100, 5) nets to zero; only (200, 7) survives, so frontier = {7}. */
	frontier_times(__dtfl_merge_batcher_frontier(&mb), ft, &nf);
	assert_size(nf, ==, 1);
	assert_uint64(ft[0], ==, 7);

	release_chunks(out, nout);
	__dtfl_description_free(&desc);
	__dtfl_antichain_free(&upper);
	__dtfl_merge_batcher_free(&mb);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(trace_implementations_merge_batcher_test__frontier_survivor_minimum)
{
	static const u3 chain1[] = { { 100, 5, 1 } };
	static const u3 chain2[] = { { 200, 7, 1 } };
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_merge_batcher mb;
	__dtfl_antichain upper;
	__dtfl_description desc;
	__dtfl_batch **out;
	size_t nout, nf;
	uint64_t ft[8], three = 3;

	(void)params; (void)data;
	__dtfl_merge_batcher_init(&mb, dtfl_type_u64(), tc, I64);
	push_chain(&mb, chain1, 1);
	push_chain(&mb, chain2, 1);
	__dtfl_antichain_init_elem(&upper, tc, &three);
	__dtfl_merge_batcher_seal(&mb, &upper, &out, &nout, &desc);

	/* No cancellation; the frontier is the minimal kept time, {5}. */
	frontier_times(__dtfl_merge_batcher_frontier(&mb), ft, &nf);
	assert_size(nf, ==, 1);
	assert_uint64(ft[0], ==, 5);

	release_chunks(out, nout);
	__dtfl_description_free(&desc);
	__dtfl_antichain_free(&upper);
	__dtfl_merge_batcher_free(&mb);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * merge_two takes an inline path for 8-byte memcmp-ordered records, one
 * time word and a 64-bit integer diff.  A user class with the same i64
 * algebra takes the general path, so the same chunks pushed through both
 * batchers must seal to the same chunks, byte for byte, with the same
 * kept frontier.  Chains of several chunks, ties that cancel, and chunks
 * that straddle the output capacity are all drawn.
 */
static void
u_plus(const __dtfl_diff_class *c, void *acc, const void *x)
{
	uint64_t a, b;

	(void)c;
	memcpy(&a, acc, 8);
	memcpy(&b, x, 8);
	a += b;
	memcpy(acc, &a, 8);
}

static bool
u_is_zero(const __dtfl_diff_class *c, const void *x)
{
	uint64_t a;

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

/* A sorted, consolidated chunk of n random updates (keys < 64, t < 8). */
static __dtfl_batch *
random_chunk(uint64_t *rng, size_t n, const __dtfl_diff_class *dc)
{
	__dtfl_batch *b = __dtfl_batch_new(1, 8);
	unsigned char rec[8];
	size_t i;

	for (i = 0; i < n; i++) {
		uint64_t d, t;
		int64_t r;

		*rng ^= *rng << 13; *rng ^= *rng >> 7; *rng ^= *rng << 17;
		d = *rng % 64;
		t = (*rng >> 8) % 8;
		r = (int64_t)((*rng >> 16) % 5) - 2;
		dtfl_put_u64(rec, d);
		__dtfl_batch_push(b, rec, 8, &t, &r);
	}
	__dtfl_consolidate_batch(b, dtfl_type_u64(), dc);
	return b;
}

DTFL_TEST(merge_batcher__fast_merge_matches_general)
{
	__dtfl_diff_class def;
	const __dtfl_diff_class *user;
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t rng = 0x2545f4914f6cdd1dULL, four = 4;
	int round;

	(void)params; (void)data;
	memset(&def, 0, sizeof def);
	def.name = "user-i64";
	def.width = 8;
	def.plus_equals = u_plus;
	def.is_zero = u_is_zero;
	def.cmp = u_cmp;
	user = __dtfl_diff_class_new(&def);
	for (round = 0; round < 40; round++) {
		__dtfl_merge_batcher fm, gm;
		__dtfl_antichain upper;
		__dtfl_description fd, gd;
		__dtfl_batch **fo, **go;
		size_t fn, gn, i, pushes = 2 + (size_t)round % 7;

		__dtfl_merge_batcher_init(&fm, dtfl_type_u64(), tc, I64);
		__dtfl_merge_batcher_init(&gm, dtfl_type_u64(), tc, user);
		for (i = 0; i < pushes; i++) {
			/* Up to 2.5 output chunks, so merges cross chunk ends. */
			size_t n = 1 + (size_t)(rng % (5 * fm.cap / 2));
			__dtfl_batch *c = random_chunk(&rng, n, I64);

			__dtfl_merge_batcher_push(&fm, __dtfl_batch_copy(c));
			__dtfl_merge_batcher_push(&gm, c);
		}
		__dtfl_antichain_init_elem(&upper, tc, &four);
		__dtfl_merge_batcher_seal(&fm, &upper, &fo, &fn, &fd);
		__dtfl_merge_batcher_seal(&gm, &upper, &go, &gn, &gd);
		assert_size(fn, ==, gn);
		for (i = 0; i < fn; i++) {
			assert_size(__dtfl_batch_len(fo[i]), ==,
			    __dtfl_batch_len(go[i]));
			assert_memory_equal(fo[i]->arena.len, fo[i]->arena.data,
			    go[i]->arena.data);
			assert_memory_equal(fo[i]->offs.len * sizeof(size_t),
			    fo[i]->offs.data, go[i]->offs.data);
			assert_memory_equal(fo[i]->times.len * 8,
			    fo[i]->times.data, go[i]->times.data);
			assert_memory_equal(fo[i]->diffs.len, fo[i]->diffs.data,
			    go[i]->diffs.data);
		}
		assert_true(__dtfl_antichain_eq(__dtfl_merge_batcher_frontier(&fm),
		    __dtfl_merge_batcher_frontier(&gm)));
		release_chunks(fo, fn);
		release_chunks(go, gn);
		__dtfl_description_free(&fd);
		__dtfl_description_free(&gd);
		__dtfl_antichain_free(&upper);
		__dtfl_merge_batcher_free(&fm);
		__dtfl_merge_batcher_free(&gm);
	}
	__dtfl_diff_class_free(user);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * The inline merge requires every record to be 8 bytes.  An Option<u64>
 * is memcmp-ordered but encodes None in 1 byte and Some in 9, so a chunk
 * of one None and seven Somes is 64 bytes over 8 rows, which an
 * arena-size test mistakes for eight 8-byte records.  Such chunks must
 * merge as the general path merges them.
 */
DTFL_TEST(merge_batcher__fast_merge_rejects_variable_records)
{
	__dtfl_diff_class def;
	const __dtfl_diff_class *user;
	const dtfl_type *opt = dtfl_type_option(dtfl_type_u64());
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_merge_batcher fm, gm;
	__dtfl_antichain upper;
	__dtfl_description fd, gd;
	__dtfl_batch **fo, **go;
	size_t fn, gn, i, c;
	uint64_t ten = 10;

	(void)params; (void)data;
	memset(&def, 0, sizeof def);
	def.name = "user-i64";
	def.width = 8;
	def.plus_equals = u_plus;
	def.is_zero = u_is_zero;
	def.cmp = u_cmp;
	user = __dtfl_diff_class_new(&def);
	__dtfl_merge_batcher_init(&fm, opt, tc, I64);
	__dtfl_merge_batcher_init(&gm, opt, tc, user);
	for (c = 0; c < 2; c++) {
		__dtfl_batch *b = __dtfl_batch_new(1, 8);
		unsigned char rec[9];
		uint64_t t = 1 + c;
		int64_t r = 1;

		rec[0] = 0;			/* None */
		__dtfl_batch_push(b, rec, 1, &t, &r);
		for (i = 1; i <= 7; i++) {	/* Some(i) */
			rec[0] = 1;
			dtfl_put_u64(rec + 1, i);
			__dtfl_batch_push(b, rec, 9, &t, &r);
		}
		assert_size(b->arena.len, ==, 8 * __dtfl_batch_len(b));
		__dtfl_merge_batcher_push(&fm, __dtfl_batch_copy(b));
		__dtfl_merge_batcher_push(&gm, b);
	}
	__dtfl_antichain_init_elem(&upper, tc, &ten);
	__dtfl_merge_batcher_seal(&fm, &upper, &fo, &fn, &fd);
	__dtfl_merge_batcher_seal(&gm, &upper, &go, &gn, &gd);
	assert_size(fn, ==, gn);
	for (i = 0; i < fn; i++) {
		assert_size(__dtfl_batch_len(fo[i]), ==, __dtfl_batch_len(go[i]));
		assert_memory_equal(fo[i]->offs.len * sizeof(size_t),
		    fo[i]->offs.data, go[i]->offs.data);
		assert_memory_equal(fo[i]->arena.len, fo[i]->arena.data,
		    go[i]->arena.data);
	}
	release_chunks(fo, fn);
	release_chunks(go, gn);
	__dtfl_description_free(&fd);
	__dtfl_description_free(&gd);
	__dtfl_antichain_free(&upper);
	__dtfl_merge_batcher_free(&fm);
	__dtfl_merge_batcher_free(&gm);
	__dtfl_diff_class_free(user);
	dtfl_type_free(opt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * The batcher recycles drained chunks through its stash (VecMerger's
 * empty, refill and extract).  A recycled chunk must come back empty:
 * push many small random chunks, so merges drain and refill the stash
 * over and over, seal at two frontiers, and compare everything sealed
 * with the consolidation of everything pushed.  Updates pushed after the
 * first seal are moved to times at or after its upper (8 + t), as the
 * batcher requires.
 */
static int
row_order(const void *a, const void *b)
{
	const u3 *x = a, *y = b;

	if (x->d != y->d)
		return x->d < y->d ? -1 : 1;
	return (x->t > y->t) - (x->t < y->t);
}

DTFL_TEST(merge_batcher__recycled_chunks_come_back_empty)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_merge_batcher mb;
	__dtfl_batch *all = __dtfl_batch_new(1, 8);
	uint64_t rng = 0x853c49e6748fea9bULL;
	u3 got[4096];
	size_t ngot = 0, push, i, s;

	(void)params; (void)data;
	__dtfl_merge_batcher_init(&mb, dtfl_type_u64(), tc, I64);
	for (s = 0; s < 2; s++) {
		uint64_t up = s == 0 ? 8 : 100;
		__dtfl_antichain upper;
		__dtfl_description desc;
		__dtfl_batch **out;
		size_t nout, c;

		for (push = 0; push < 60; push++) {
			size_t n = 1 + (size_t)(rng % (3 * mb.cap / 2));
			__dtfl_batch *ch = random_chunk(&rng, n, I64);
			size_t r;

			if (s == 1) {
				for (r = 0; r < __dtfl_batch_len(ch); r++)
					__dtfl_batch_time_mut(ch, r)[0] += 8;
			}
			for (r = 0; r < __dtfl_batch_len(ch); r++)
				__dtfl_batch_push_from(all, ch, r);
			__dtfl_merge_batcher_push(&mb, ch);
		}
		__dtfl_antichain_init_elem(&upper, tc, &up);
		__dtfl_merge_batcher_seal(&mb, &upper, &out, &nout, &desc);
		for (c = 0; c < nout; c++) {
			for (i = 0; i < __dtfl_batch_len(out[c]); i++) {
				size_t len;
				int64_t r;

				assert_size(ngot, <, sizeof got / sizeof got[0]);
				got[ngot].d = dtfl_get_u64(__dtfl_batch_rec(out[c], i,
				    &len));
				got[ngot].t = __dtfl_batch_time(out[c], i)[0];
				memcpy(&r, __dtfl_batch_diff(out[c], i), 8);
				got[ngot].r = r;
				ngot++;
			}
		}
		release_chunks(out, nout);
		__dtfl_description_free(&desc);
		__dtfl_antichain_free(&upper);
	}
	/* Both seals together hold every update (times are below 100). */
	__dtfl_consolidate_batch(all, dtfl_type_u64(), I64);
	qsort(got, ngot, sizeof got[0], row_order);
	assert_size(ngot, ==, __dtfl_batch_len(all));
	for (i = 0; i < ngot; i++) {
		size_t len;
		int64_t r;

		memcpy(&r, __dtfl_batch_diff(all, i), 8);
		assert_uint64(got[i].d, ==, dtfl_get_u64(__dtfl_batch_rec(all, i,
		    &len)));
		assert_uint64(got[i].t, ==, __dtfl_batch_time(all, i)[0]);
		assert_int64(got[i].r, ==, r);
	}
	__dtfl_batch_release(all);
	__dtfl_merge_batcher_free(&mb);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * Only a uniquely held chunk may be recycled: a chunk pushed into the
 * batcher while someone else still holds a reference must not be
 * cleared under that holder when the merge drains it.
 */
DTFL_TEST(merge_batcher__shared_chunk_is_not_recycled)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_merge_batcher mb;
	__dtfl_antichain upper;
	__dtfl_description desc;
	__dtfl_batch **out, *held[8];
	uint64_t rng = 0x1d872b41ULL, up = 100;
	size_t nout, i, len0[8];

	(void)params; (void)data;
	__dtfl_merge_batcher_init(&mb, dtfl_type_u64(), tc, I64);
	for (i = 0; i < 8; i++) {
		held[i] = random_chunk(&rng, mb.cap, I64);
		len0[i] = __dtfl_batch_len(held[i]);
		/* The batcher gets one reference; the test keeps another. */
		__dtfl_merge_batcher_push(&mb, __dtfl_batch_retain(held[i]));
	}
	__dtfl_antichain_init_elem(&upper, tc, &up);
	__dtfl_merge_batcher_seal(&mb, &upper, &out, &nout, &desc);
	for (i = 0; i < 8; i++) {
		assert_size(__dtfl_batch_len(held[i]), ==, len0[i]);
		__dtfl_batch_release(held[i]);
	}
	release_chunks(out, nout);
	__dtfl_description_free(&desc);
	__dtfl_antichain_free(&upper);
	__dtfl_merge_batcher_free(&mb);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(trace_implementations_merge_batcher_test__frontier_is_post_consolidation),
	DTFL_T(trace_implementations_merge_batcher_test__frontier_survivor_minimum),
	DTFL_T(merge_batcher__fast_merge_matches_general),
	DTFL_T(merge_batcher__fast_merge_rejects_variable_records),
	DTFL_T(merge_batcher__recycled_chunks_come_back_empty),
	DTFL_T(merge_batcher__shared_chunk_is_not_recycled),
	DTFL_T_END
};

DTFL_TEST_MAIN("merge_batcher", tests)
