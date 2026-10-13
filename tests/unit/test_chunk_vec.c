/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/chunk/vec.rs (the #[cfg(test)] module)
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/unit/test_chunk_vec.c: the VecChunk test module, ported one test to
 * one DTFL_TEST (PLAN D6: trace_chunk_vec_test__<name>).  Each drives the
 * whole-chunk operations (merge, extract, advance, settle) and the straddle
 * cursor directly over the one chunk layout, checking grading, consolidation,
 * the resumable batch merger, and direction-independent key seeks.
 *
 * Upstream TARGET is 8192; the two grading-at-scale tests use that constant.
 * seek_microbench is #[ignore] upstream (a benchmark, not a correctness test);
 * it is reported SKIP here, as a benchmark belongs in bench/, not make check.
 */
#include "dtfl_test.h"
#include "dd/trace/chunk_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

typedef struct kv {
	uint64_t k, v, t;
	int64_t r;
} kv;

/* A chunk of (pair(k,v), t, r) rows. */
static __dtfl_chunk *
chunk_of(const kv *u, size_t n)
{
	__dtfl_chunk *b = __dtfl_batch_new(1, 8);
	unsigned char rec[16];
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_put_u64(rec, u[i].k);
		dtfl_put_u64(rec + 8, u[i].v);
		__dtfl_batch_push(b, rec, 16, &u[i].t, &u[i].r);
	}
	return b;
}

/* A chunk of `n` single-time rows ((k, v), t, 1), with a key callback. */
static __dtfl_chunk *
chunk_gen(size_t n, uint64_t (*keyf)(size_t), uint64_t v, uint64_t t)
{
	__dtfl_chunk *b = __dtfl_batch_new(1, 8);
	unsigned char rec[16];
	size_t i;
	int64_t one = 1;

	for (i = 0; i < n; i++) {
		dtfl_put_u64(rec, keyf(i));
		dtfl_put_u64(rec + 8, v);
		__dtfl_batch_push(b, rec, 16, &t, &one);
	}
	return b;
}

static void
ctx_of(__dtfl_chunk_ctx *x, const dtfl_type *rt, dtfl_time_class *tc)
{
	__dtfl_chunk_ctx_init(x, false, rt, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64);
}

/* Total rows across a deque. */
static size_t
deque_rows(const __dtfl_chunk_deque *d)
{
	size_t n = __dtfl_chunk_deque_len(d), i, s = 0;

	for (i = 0; i < n; i++)
		s += __dtfl_batch_len(__dtfl_chunk_deque_at(d, i));
	return s;
}

/* ---- extract_partitions_and_grades (vec.rs:407) ------------------- */

/* Smaller than upstream's 4*TARGET so the test is a sub-second case. */
#define XN 64

DTFL_TEST(trace_chunk_vec_test__extract_partitions_and_grades)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk_deque input, keep, ship, keep_s, ship_s;
	__dtfl_antichain frontier, residual, want;
	uint64_t one = 1;
	size_t i;

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	__dtfl_chunk_deque_init(&input);
	__dtfl_chunk_deque_init(&keep);
	__dtfl_chunk_deque_init(&ship);
	__dtfl_chunk_deque_init(&keep_s);
	__dtfl_chunk_deque_init(&ship_s);
	/* XN single-row chunks: even times ship (<1), odd times keep (>=1). */
	for (i = 0; i < XN; i++) {
		kv u = { i, 0, i % 2, 1 };

		__dtfl_chunk_deque_push_back(&input, chunk_of(&u, 1));
	}
	__dtfl_antichain_init_elem(&frontier, tc, &one);
	__dtfl_antichain_init(&residual, tc);
	while (!__dtfl_chunk_deque_empty(&input))
		__dtfl_chunk_extract(&x, &input, &frontier, &residual, &keep,
		    &ship);
	__dtfl_chunk_settle_all(&x, &keep, &keep_s);
	__dtfl_chunk_settle_all(&x, &ship, &ship_s);

	/* Kept times are exactly {1}; that is the residual frontier. */
	__dtfl_antichain_init_elem(&want, tc, &one);
	assert_true(__dtfl_antichain_eq(&residual, &want));
	assert_true(__dtfl_chunk_is_graded(&keep_s));
	assert_true(__dtfl_chunk_is_graded(&ship_s));
	assert_size(deque_rows(&keep_s), ==, XN / 2);
	assert_size(deque_rows(&ship_s), ==, XN / 2);

	__dtfl_chunk_deque_free(&input);
	__dtfl_chunk_deque_free(&keep);
	__dtfl_chunk_deque_free(&ship);
	__dtfl_chunk_deque_free(&keep_s);
	__dtfl_chunk_deque_free(&ship_s);
	__dtfl_antichain_free(&frontier);
	__dtfl_antichain_free(&residual);
	__dtfl_antichain_free(&want);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- advance helpers --------------------------------------------- */

struct row {
	uint64_t k, v, t;
	int64_t r;
};
struct flat {
	struct row *rows;
	size_t n, cap;
};

static size_t
flatten(const __dtfl_chunk_ctx *x, const __dtfl_chunk_deque *d, struct row *out,
    size_t cap)
{
	size_t nd = __dtfl_chunk_deque_len(d), i, n = 0;

	(void)x;
	for (i = 0; i < nd; i++) {
		__dtfl_chunk *c = __dtfl_chunk_deque_at(d, i);
		size_t m = __dtfl_batch_len(c), j, len;

		for (j = 0; j < m && n < cap; j++) {
			const void *rec = __dtfl_batch_rec(c, j, &len);

			out[n].k = dtfl_get_u64(rec);
			out[n].v = dtfl_get_u64((const unsigned char *)rec + 8);
			out[n].t = __dtfl_batch_time(c, j)[0];
			memcpy(&out[n].r, __dtfl_batch_diff(c, j), 8);
			n++;
		}
	}
	return n;
}

/* ---- advance_emits_complete_groups_eagerly (vec.rs:438) ----------- */
DTFL_TEST(trace_chunk_vec_test__advance_emits_complete_groups_eagerly)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk_deque input, out;
	__dtfl_antichain frontier;
	uint64_t f = 5;
	struct row rows[8];
	size_t n;
	/* Group (0,0) complete; group (1,0) might still grow. */
	static const kv c0[] = { { 0, 0, 0, 1 }, { 0, 0, 1, 1 }, { 1, 0, 0, 1 } };

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	__dtfl_chunk_deque_init(&input);
	__dtfl_chunk_deque_init(&out);
	__dtfl_chunk_deque_push_back(&input, chunk_of(c0, 3));
	__dtfl_antichain_init_elem(&frontier, tc, &f);
	__dtfl_chunk_advance(&x, &input, &frontier, false, &out);

	/* (1,0) withheld as the carry; (0,0) advanced to 5 and summed. */
	assert_size(__dtfl_chunk_deque_len(&input), ==, 1);
	assert_size(__dtfl_batch_len(__dtfl_chunk_deque_at(&input, 0)), ==, 1);
	n = flatten(&x, &out, rows, 8);
	assert_size(n, ==, 1);
	assert_uint64(rows[0].k, ==, 0); assert_uint64(rows[0].v, ==, 0);
	assert_uint64(rows[0].t, ==, 5); assert_int64(rows[0].r, ==, 2);

	__dtfl_chunk_deque_free(&input);
	__dtfl_chunk_deque_free(&out);
	__dtfl_antichain_free(&frontier);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- advance_resumable_matches_oneshot (vec.rs:459) --------------- */
DTFL_TEST(trace_chunk_vec_test__advance_resumable_matches_oneshot)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_antichain frontier;
	uint64_t f = 3;
	struct row one[16], inc[16];
	size_t no, ni, i;
	static const kv a[] = { { 0, 0, 0, 1 }, { 0, 0, 1, 1 }, { 1, 0, 0, 1 } };
	static const kv b[] = { { 1, 0, 5, 1 }, { 1, 1, 0, 1 }, { 2, 0, 0, 1 } };
	static const kv cc[] = { { 2, 0, 2, 1 }, { 2, 0, 9, 1 } };

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	__dtfl_antichain_init_elem(&frontier, tc, &f);

	{
		__dtfl_chunk_deque q, out;

		__dtfl_chunk_deque_init(&q);
		__dtfl_chunk_deque_init(&out);
		__dtfl_chunk_deque_push_back(&q, chunk_of(a, 3));
		__dtfl_chunk_deque_push_back(&q, chunk_of(b, 3));
		__dtfl_chunk_deque_push_back(&q, chunk_of(cc, 2));
		__dtfl_chunk_advance(&x, &q, &frontier, false, &out);
		__dtfl_chunk_advance(&x, &q, &frontier, true, &out);
		no = flatten(&x, &out, one, 16);
		__dtfl_chunk_deque_free(&q);
		__dtfl_chunk_deque_free(&out);
	}
	{
		__dtfl_chunk_deque q, out;
		const kv *chunks[3] = { a, b, cc };
		size_t sz[3] = { 3, 3, 2 };

		__dtfl_chunk_deque_init(&q);
		__dtfl_chunk_deque_init(&out);
		for (i = 0; i < 3; i++) {
			__dtfl_chunk_deque_push_back(&q,
			    chunk_of(chunks[i], sz[i]));
			__dtfl_chunk_advance(&x, &q, &frontier, false, &out);
		}
		__dtfl_chunk_advance(&x, &q, &frontier, true, &out);
		ni = flatten(&x, &out, inc, 16);
		__dtfl_chunk_deque_free(&q);
		__dtfl_chunk_deque_free(&out);
	}
	assert_size(no, ==, ni);
	for (i = 0; i < no; i++) {
		assert_uint64(one[i].k, ==, inc[i].k);
		assert_uint64(one[i].v, ==, inc[i].v);
		assert_uint64(one[i].t, ==, inc[i].t);
		assert_int64(one[i].r, ==, inc[i].r);
		assert_uint64(one[i].t, >=, 3);	/* times advanced */
	}

	__dtfl_antichain_free(&frontier);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- advance_single_key_spanning_pushes (vec.rs:493) -------------- */
DTFL_TEST(trace_chunk_vec_test__advance_single_key_spanning_pushes)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk_deque q, out;
	__dtfl_antichain frontier;
	uint64_t f = 100;
	size_t nrec = 50, t;
	struct row rows[4];
	size_t n;

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	__dtfl_antichain_init_elem(&frontier, tc, &f);
	__dtfl_chunk_deque_init(&q);
	__dtfl_chunk_deque_init(&out);
	/* One (7,0) across every pushed chunk; advance makes no progress until done. */
	for (t = 0; t < nrec; t++) {
		kv u = { 7, 0, t, 1 };

		__dtfl_chunk_deque_push_back(&q, chunk_of(&u, 1));
		__dtfl_chunk_advance(&x, &q, &frontier, false, &out);
	}
	__dtfl_chunk_advance(&x, &q, &frontier, true, &out);
	n = flatten(&x, &out, rows, 4);
	assert_size(n, ==, 1);
	assert_uint64(rows[0].k, ==, 7); assert_uint64(rows[0].v, ==, 0);
	assert_uint64(rows[0].t, ==, 100);
	assert_int64(rows[0].r, ==, (int64_t)nrec);

	__dtfl_chunk_deque_free(&q);
	__dtfl_chunk_deque_free(&out);
	__dtfl_antichain_free(&frontier);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- merge_chains_consolidates (vec.rs:509) ----------------------- */
DTFL_TEST(trace_chunk_vec_test__merge_chains_consolidates)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk_deque a, b, out;
	struct row rows[8];
	size_t n;
	static const kv ca[] = { { 0, 0, 0, 1 }, { 1, 0, 0, 1 } };
	static const kv cb[] = { { 0, 0, 0, 1 }, { 2, 0, 0, 1 } };

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	__dtfl_chunk_deque_init(&a);
	__dtfl_chunk_deque_init(&b);
	__dtfl_chunk_deque_init(&out);
	__dtfl_chunk_deque_push_back(&a, chunk_of(ca, 2));
	__dtfl_chunk_deque_push_back(&b, chunk_of(cb, 2));
	__dtfl_chunk_merge_chains(&x, &a, &b, &out);
	n = flatten(&x, &out, rows, 8);
	assert_size(n, ==, 3);
	assert_uint64(rows[0].k, ==, 0); assert_int64(rows[0].r, ==, 2);
	assert_uint64(rows[1].k, ==, 1); assert_int64(rows[1].r, ==, 1);
	assert_uint64(rows[2].k, ==, 2); assert_int64(rows[2].r, ==, 1);

	__dtfl_chunk_deque_free(&a);
	__dtfl_chunk_deque_free(&b);
	__dtfl_chunk_deque_free(&out);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- merge_emits_graded_chunks (vec.rs:521) ----------------------- */
static uint64_t even_i(size_t i) { return (uint64_t)(2 * i); }
static uint64_t odd_i(size_t i) { return (uint64_t)(2 * i + 1); }

DTFL_TEST(trace_chunk_vec_test__merge_emits_graded_chunks)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk_deque a, b, out, settled;
	size_t half = 2 * DTFL_CHUNK_TARGET, i;	/* 4*TARGET total keys */
	size_t n;
	struct row *rows;

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	__dtfl_chunk_deque_init(&a);
	__dtfl_chunk_deque_init(&b);
	__dtfl_chunk_deque_init(&out);
	__dtfl_chunk_deque_init(&settled);
	__dtfl_chunk_deque_push_back(&a, chunk_gen(half, even_i, 0, 0));
	__dtfl_chunk_deque_push_back(&b, chunk_gen(half, odd_i, 0, 0));
	__dtfl_chunk_merge_chains(&x, &a, &b, &out);
	__dtfl_chunk_settle_all(&x, &out, &settled);

	assert_true(__dtfl_chunk_is_graded(&settled));
	rows = __dtfl_alloc(2 * half * sizeof *rows);
	n = flatten(&x, &settled, rows, 2 * half);
	assert_size(n, ==, 2 * half);
	for (i = 0; i < n; i++)
		assert_uint64(rows[i].k, ==, (uint64_t)i);	/* 0..4*TARGET */
	__dtfl_free(rows);

	__dtfl_chunk_deque_free(&a);
	__dtfl_chunk_deque_free(&b);
	__dtfl_chunk_deque_free(&out);
	__dtfl_chunk_deque_free(&settled);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- merge_matches_reference (vec.rs:548) ------------------------- */
/* consolidate_updates oracle over a small (k,v,t) space; xorshift PRNG. */
static int
row_cmp_qsort(const void *pa, const void *pb)
{
	const struct row *a = pa, *b = pb;

	if (a->k != b->k) return a->k < b->k ? -1 : 1;
	if (a->v != b->v) return a->v < b->v ? -1 : 1;
	if (a->t != b->t) return a->t < b->t ? -1 : 1;
	return 0;
}
static size_t
consolidate_ref(struct row *v, size_t n)
{
	size_t i, k, out = 0;

	if (n > 1)
		qsort(v, n, sizeof *v, row_cmp_qsort);
	for (i = 0; i < n; i = k) {
		int64_t s = v[i].r;

		for (k = i + 1; k < n && row_cmp_qsort(&v[i], &v[k]) == 0; k++)
			s += v[k].r;
		if (s != 0) {
			v[out] = v[i];
			v[out].r = s;
			out++;
		}
	}
	return out;
}
static size_t
gen_updates(uint64_t *seed, struct row *v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		*seed ^= *seed << 13; *seed ^= *seed >> 7; *seed ^= *seed << 17;
		v[i].k = *seed % 20;
		*seed ^= *seed << 13; *seed ^= *seed >> 7; *seed ^= *seed << 17;
		v[i].v = *seed % 3;
		*seed ^= *seed << 13; *seed ^= *seed >> 7; *seed ^= *seed << 17;
		v[i].t = *seed % 8;
		*seed ^= *seed << 13; *seed ^= *seed >> 7; *seed ^= *seed << 17;
		v[i].r = (*seed % 4 == 0) ? -1 : 1;
	}
	return consolidate_ref(v, n);
}
/* Cut a consolidated set into a chain of `sz`-row chunks. */
static void
chain_of(__dtfl_chunk_deque *d, const struct row *v, size_t n, size_t sz)
{
	size_t i;

	for (i = 0; i < n; i += sz) {
		__dtfl_chunk *c = __dtfl_batch_new(1, 8);
		size_t end = i + sz < n ? i + sz : n, j;
		unsigned char rec[16];

		for (j = i; j < end; j++) {
			dtfl_put_u64(rec, v[j].k);
			dtfl_put_u64(rec + 8, v[j].v);
			__dtfl_batch_push(c, rec, 16, &v[j].t, &v[j].r);
		}
		__dtfl_chunk_deque_push_back(d, c);
	}
}

DTFL_TEST(trace_chunk_vec_test__merge_matches_reference)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	uint64_t seed = 0x2545F4914F6CDD1Dull;
	struct row u1[80], u2[80], merged[160], ref[160];
	size_t iter;

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	for (iter = 0; iter < 60; iter++) {
		size_t n1 = (size_t)(seed % 60) + 1, n2, nm, nr, i, sz;
		__dtfl_chunk_deque a, b, out;

		n1 = n1 > 80 ? 80 : n1;
		n1 = gen_updates(&seed, u1, n1);
		n2 = (size_t)(seed % 60) + 1;
		n2 = n2 > 80 ? 80 : n2;
		n2 = gen_updates(&seed, u2, n2);
		if (n1 == 0 || n2 == 0)
			continue;
		sz = (size_t)(seed % 5) + 1;	/* tiny chunks -> straddle */

		__dtfl_chunk_deque_init(&a);
		__dtfl_chunk_deque_init(&b);
		__dtfl_chunk_deque_init(&out);
		chain_of(&a, u1, n1, sz);
		chain_of(&b, u2, n2, sz);
		__dtfl_chunk_merge_chains(&x, &a, &b, &out);
		nm = flatten(&x, &out, merged, 160);

		for (i = 0; i < n1; i++) ref[i] = u1[i];
		for (i = 0; i < n2; i++) ref[n1 + i] = u2[i];
		nr = consolidate_ref(ref, n1 + n2);
		assert_size(nm, ==, nr);
		for (i = 0; i < nm; i++) {
			assert_uint64(merged[i].k, ==, ref[i].k);
			assert_uint64(merged[i].v, ==, ref[i].v);
			assert_uint64(merged[i].t, ==, ref[i].t);
			assert_int64(merged[i].r, ==, ref[i].r);
		}
		__dtfl_chunk_deque_free(&a);
		__dtfl_chunk_deque_free(&b);
		__dtfl_chunk_deque_free(&out);
	}

	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- batch_merger_resumable_matches_reference (vec.rs:597) -------- */
static __dtfl_chunk_batch *
batch_of(const __dtfl_chunk_ctx *x, dtfl_time_class *tc, const struct row *v,
    size_t n, size_t sz)
{
	__dtfl_chunk_deque d;
	__dtfl_chunk **arr;
	__dtfl_antichain lo, hi, si;
	__dtfl_description desc;
	__dtfl_chunk_batch *b;
	uint64_t l = 0, h = 10, s = 0;
	size_t m, i;

	__dtfl_chunk_deque_init(&d);
	chain_of(&d, v, n, sz);
	m = __dtfl_chunk_deque_len(&d);
	arr = m != 0 ? __dtfl_alloc(m * sizeof *arr) : NULL;
	for (i = 0; i < m; i++)
		arr[i] = __dtfl_chunk_deque_at(&d, i);
	__dtfl_antichain_init_elem(&lo, tc, &l);
	__dtfl_antichain_init_elem(&hi, tc, &h);
	__dtfl_antichain_init_elem(&si, tc, &s);
	__dtfl_description_init(&desc, &lo, &hi, &si);
	b = __dtfl_chunk_batch_new(x, arr, m, &desc);
	__dtfl_free(arr);
	__dtfl_vec_free(&d.v);		/* chunks moved into the batch */
	return b;
}

struct racc { struct row *rows; size_t n, cap; uint64_t k, v; };
static void
racc_emit(const uint64_t *time, const void *diff, void *vp)
{
	struct racc *a = vp;

	if (a->n < a->cap) {
		a->rows[a->n].k = a->k;
		a->rows[a->n].v = a->v;
		a->rows[a->n].t = time[0];
		memcpy(&a->rows[a->n].r, diff, 8);
		a->n++;
	}
}

DTFL_TEST(trace_chunk_vec_test__batch_merger_resumable_matches_reference)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	uint64_t seed = 0x9E3779B97F4A7C15ull;
	struct row u1[48], u2[48], got[96], ref[96];
	size_t iter;

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	for (iter = 0; iter < 40; iter++) {
		size_t n1 = (size_t)(seed % 40) + 1, n2, sz, i, ng, nr;
		uint64_t f;
		__dtfl_chunk_batch *s1, *s2, *merged;
		__dtfl_antichain compaction;
		__dtfl_chunk_merger m;
		__dtfl_chunk_cursor cur;
		struct racc acc = { got, 0, 96, 0, 0 };
		size_t len;

		n1 = n1 > 48 ? 48 : n1;
		n1 = gen_updates(&seed, u1, n1);
		n2 = (size_t)(seed % 40) + 1;
		n2 = n2 > 48 ? 48 : n2;
		n2 = gen_updates(&seed, u2, n2);
		if (n1 == 0 || n2 == 0)
			continue;
		sz = (size_t)(seed % 4) + 1;
		f = seed % 6;

		s1 = batch_of(&x, tc, u1, n1, sz);
		s2 = batch_of(&x, tc, u2, n2, sz);
		__dtfl_antichain_init_elem(&compaction, tc, &f);
		__dtfl_chunk_merger_new(&m, s1, s2, &compaction);
		for (;;) {
			int64_t fuel = 1;

			__dtfl_chunk_merger_work(&m, &fuel);
			if (fuel > 0)
				break;
		}
		merged = __dtfl_chunk_merger_done(&m);
		assert_true(__dtfl_chunk_is_graded_batch(merged));

		/* Read through the straddle cursor, then consolidate. */
		__dtfl_chunk_cursor_init(&cur, merged);
		while (__dtfl_chunk_cursor_key_valid(&cur, merged)) {
			acc.k = dtfl_get_u64(__dtfl_chunk_cursor_key(&cur,
			    merged, &len));
			while (__dtfl_chunk_cursor_val_valid(&cur, merged)) {
				acc.v = dtfl_get_u64(
				    __dtfl_chunk_cursor_val(&cur, merged, &len));
				__dtfl_chunk_cursor_map_times(&cur, merged,
				    racc_emit, &acc);
				__dtfl_chunk_cursor_step_val(&cur, merged);
			}
			__dtfl_chunk_cursor_step_key(&cur, merged);
		}
		ng = consolidate_ref(got, acc.n);

		/* Reference: union advanced to f, consolidated. */
		for (i = 0; i < n1; i++) {
			ref[i] = u1[i];
			if (ref[i].t < f) ref[i].t = f;
		}
		for (i = 0; i < n2; i++) {
			ref[n1 + i] = u2[i];
			if (ref[n1 + i].t < f) ref[n1 + i].t = f;
		}
		nr = consolidate_ref(ref, n1 + n2);
		assert_size(ng, ==, nr);
		for (i = 0; i < ng; i++) {
			assert_uint64(got[i].k, ==, ref[i].k);
			assert_uint64(got[i].v, ==, ref[i].v);
			assert_uint64(got[i].t, ==, ref[i].t);
			assert_int64(got[i].r, ==, ref[i].r);
		}
		__dtfl_chunk_batch_release(merged);
		__dtfl_antichain_free(&compaction);
		__dtfl_chunk_batch_release(s1);
		__dtfl_chunk_batch_release(s2);
	}

	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- settle_maximal_packing (vec.rs:676) -------------------------- */
DTFL_TEST(trace_chunk_vec_test__settle_maximal_packing)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk_deque input, output;
	size_t t = DTFL_CHUNK_TARGET;
	size_t sizes[] = { t / 3, t / 3, t / 3, t, t / 2, t / 2, t, 1, t - 1 };
	size_t ns = sizeof sizes / sizeof sizes[0], total = 0, i, got = 0;
	uint64_t key = 0, prev = 0;
	bool first = true;

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	__dtfl_chunk_deque_init(&input);
	__dtfl_chunk_deque_init(&output);
	for (i = 0; i < ns; i++) {
		__dtfl_chunk *c = __dtfl_batch_new(1, 8);
		unsigned char rec[16];
		size_t j;
		uint64_t z = 0;
		int64_t one = 1;

		for (j = 0; j < sizes[i]; j++) {
			dtfl_put_u64(rec, key);
			dtfl_put_u64(rec + 8, 0);
			__dtfl_batch_push(c, rec, 16, &z, &one);
			key++;
		}
		total += sizes[i];
		__dtfl_chunk_deque_push_back(&input, c);
		__dtfl_chunk_settle(&x, &input, false, &output);
	}
	__dtfl_chunk_settle(&x, &input, true, &output);

	assert_true(__dtfl_chunk_is_graded(&output));
	for (i = 0; i < __dtfl_chunk_deque_len(&output); i++) {
		__dtfl_chunk *c = __dtfl_chunk_deque_at(&output, i);
		size_t m = __dtfl_batch_len(c), j, len;

		for (j = 0; j < m; j++) {
			uint64_t k = dtfl_get_u64(__dtfl_batch_rec(c, j, &len));

			if (!first)
				assert_uint64(prev, <, k);
			prev = k;
			first = false;
			got++;
		}
	}
	assert_size(got, ==, total);

	__dtfl_chunk_deque_free(&input);
	__dtfl_chunk_deque_free(&output);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- cursor_handles_straddle (vec.rs:708) ------------------------- */
DTFL_TEST(trace_chunk_vec_test__cursor_handles_straddle)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk *chunks[3];
	__dtfl_chunk_batch *b;
	__dtfl_antichain lo, hi, si;
	__dtfl_description desc;
	__dtfl_chunk_cursor cur;
	struct racc acc = { NULL, 0, 16, 0, 0 };
	struct row rows[16];
	uint64_t l = 0, h = 2, s = 0;
	size_t len;
	static const kv c0[] = { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 1, 1, 0, 1 } };
	static const kv c1[] = { { 1, 1, 1, 1 }, { 1, 2, 0, 1 } };
	static const kv c2[] = { { 2, 0, 0, 1 } };

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	chunks[0] = chunk_of(c0, 3);
	chunks[1] = chunk_of(c1, 2);
	chunks[2] = chunk_of(c2, 1);
	__dtfl_antichain_init_elem(&lo, tc, &l);
	__dtfl_antichain_init_elem(&hi, tc, &h);
	__dtfl_antichain_init_elem(&si, tc, &s);
	__dtfl_description_init(&desc, &lo, &hi, &si);
	b = __dtfl_chunk_batch_new(&x, chunks, 3, &desc);

	acc.rows = rows;
	__dtfl_chunk_cursor_init(&cur, b);
	while (__dtfl_chunk_cursor_key_valid(&cur, b)) {
		acc.k = dtfl_get_u64(__dtfl_chunk_cursor_key(&cur, b, &len));
		while (__dtfl_chunk_cursor_val_valid(&cur, b)) {
			acc.v = dtfl_get_u64(__dtfl_chunk_cursor_val(&cur, b,
			    &len));
			__dtfl_chunk_cursor_map_times(&cur, b, racc_emit, &acc);
			__dtfl_chunk_cursor_step_val(&cur, b);
		}
		__dtfl_chunk_cursor_step_key(&cur, b);
	}
	/* (0,0):0 (1,0):0 (1,1):{0,1} (1,2):0 (2,0):0 */
	assert_size(acc.n, ==, 6);
	assert_uint64(rows[0].k, ==, 0); assert_uint64(rows[0].v, ==, 0);
	assert_uint64(rows[1].k, ==, 1); assert_uint64(rows[1].v, ==, 0);
	assert_uint64(rows[2].k, ==, 1); assert_uint64(rows[2].v, ==, 1);
	assert_uint64(rows[2].t, ==, 0);
	assert_uint64(rows[3].k, ==, 1); assert_uint64(rows[3].v, ==, 1);
	assert_uint64(rows[3].t, ==, 1);
	assert_uint64(rows[4].k, ==, 1); assert_uint64(rows[4].v, ==, 2);
	assert_uint64(rows[5].k, ==, 2); assert_uint64(rows[5].v, ==, 0);

	__dtfl_chunk_batch_release(b);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* ---- seek_microbench (vec.rs:741) --------------------------------- */
DTFL_TEST(trace_chunk_vec_test__seek_microbench)
{
	(void)params; (void)data;
	/* #[ignore] upstream: a gallop-vs-linear benchmark, not a test. */
	return MUNIT_SKIP;
}

/* ---- seek_key_hint_is_direction_independent (vec.rs:775) ---------- */
DTFL_TEST(trace_chunk_vec_test__seek_key_hint_is_direction_independent)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk *chunks[20];
	__dtfl_chunk_batch *b;
	__dtfl_antichain lo, hi, si;
	__dtfl_description desc;
	uint64_t l = 0, h = 1, s = 0, start, tgt;
	size_t i, len;

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	/* One even key per chunk: 0, 2, .., 38. */
	for (i = 0; i < 20; i++) {
		kv u = { 2 * (uint64_t)i, 0, 0, 1 };

		chunks[i] = chunk_of(&u, 1);
	}
	__dtfl_antichain_init_elem(&lo, tc, &l);
	__dtfl_antichain_init_elem(&hi, tc, &h);
	__dtfl_antichain_init_elem(&si, tc, &s);
	__dtfl_description_init(&desc, &lo, &hi, &si);
	b = __dtfl_chunk_batch_new(&x, chunks, 20, &desc);

	for (start = 0; start <= 40; start++) {
		for (tgt = 0; tgt <= 40; tgt++) {
			__dtfl_chunk_cursor cur;
			unsigned char sb[8], tb[8];
			uint64_t e = tgt + (tgt & 1);	/* next even >= tgt */
			bool present = e <= 38;

			dtfl_put_u64(sb, start);
			dtfl_put_u64(tb, tgt);
			__dtfl_chunk_cursor_init(&cur, b);
			__dtfl_chunk_cursor_seek_key(&cur, b, sb, 8);
			__dtfl_chunk_cursor_seek_key(&cur, b, tb, 8);
			if (present) {
				assert_true(__dtfl_chunk_cursor_key_valid(&cur,
				    b));
				assert_uint64(dtfl_get_u64(
				    __dtfl_chunk_cursor_key(&cur, b, &len)),
				    ==, e);
			} else {
				assert_false(__dtfl_chunk_cursor_key_valid(&cur,
				    b));
			}
		}
	}

	__dtfl_chunk_batch_release(b);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(trace_chunk_vec_test__extract_partitions_and_grades),
	DTFL_T(trace_chunk_vec_test__advance_emits_complete_groups_eagerly),
	DTFL_T(trace_chunk_vec_test__advance_resumable_matches_oneshot),
	DTFL_T(trace_chunk_vec_test__advance_single_key_spanning_pushes),
	DTFL_T(trace_chunk_vec_test__merge_chains_consolidates),
	DTFL_T(trace_chunk_vec_test__merge_emits_graded_chunks),
	DTFL_T(trace_chunk_vec_test__merge_matches_reference),
	DTFL_T(trace_chunk_vec_test__batch_merger_resumable_matches_reference),
	DTFL_T(trace_chunk_vec_test__settle_maximal_packing),
	DTFL_T(trace_chunk_vec_test__cursor_handles_straddle),
	DTFL_T(trace_chunk_vec_test__seek_microbench),
	DTFL_T(trace_chunk_vec_test__seek_key_hint_is_direction_independent),
	DTFL_T_END
};

DTFL_TEST_MAIN("trace_chunk_vec", tests)
