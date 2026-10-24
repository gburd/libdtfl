/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_chunk.c: the chunk-sequence batch backing.  Builds a
 * ChunkBatch from updates through the builder, reads it back through the
 * straddle cursor, and asserts key/val/time/diff order and consolidation
 * (mirrors test_ord_neu.c over the chunk layout).  Also exercises pack()
 * (combine/split to a maximal packing) and a resumable ChunkBatchMerger of two
 * adjacent batches (the spine-of-chunks merge step) under tiny fuel.
 */
#include "dtfl_test.h"
#include "dd/trace/chunk_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

typedef struct kv {
	uint64_t k, v, t;
	int64_t r;
} kv;

/* A chunk: a __dtfl_batch of (pair(k,v), t, r) rows (pre-consolidated here). */
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

static void
ctx_of(__dtfl_chunk_ctx *x, const dtfl_type *rt, dtfl_time_class *tc)
{
	__dtfl_chunk_ctx_init(x, false, rt, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64);
}

/* Flatten a ChunkBatch through its cursor into (k, v, t, r) rows. */
struct row {
	uint64_t k, v, t;
	int64_t r;
};
struct collector {
	struct row *rows;
	size_t n, cap;
	uint64_t k, v;
};
static void
emit(const uint64_t *time, const void *diff, void *ctx)
{
	struct collector *c = ctx;
	int64_t r;

	memcpy(&r, diff, 8);
	c->rows[c->n].k = c->k;
	c->rows[c->n].v = c->v;
	c->rows[c->n].t = time[0];
	c->rows[c->n].r = r;
	c->n++;
}

static size_t
read_batch(const __dtfl_chunk_batch *b, struct row *out, size_t cap)
{
	struct collector c = { out, 0, cap, 0, 0 };
	__dtfl_chunk_cursor cur;
	size_t len;

	__dtfl_chunk_cursor_init(&cur, b);
	while (__dtfl_chunk_cursor_key_valid(&cur, b)) {
		c.k = dtfl_get_u64(__dtfl_chunk_cursor_key(&cur, b, &len));
		while (__dtfl_chunk_cursor_val_valid(&cur, b)) {
			c.v = dtfl_get_u64(__dtfl_chunk_cursor_val(&cur, b,
			    &len));
			__dtfl_chunk_cursor_map_times(&cur, b, emit, &c);
			__dtfl_chunk_cursor_step_val(&cur, b);
		}
		__dtfl_chunk_cursor_step_key(&cur, b);
	}
	return c.n;
}

static __dtfl_description
desc_of(dtfl_time_class *tc, uint64_t lo, uint64_t hi, uint64_t since_t)
{
	__dtfl_antichain lower, upper, since;
	__dtfl_description d;

	__dtfl_antichain_init_elem(&lower, tc, &lo);
	__dtfl_antichain_init_elem(&upper, tc, &hi);
	__dtfl_antichain_init_elem(&since, tc, &since_t);
	__dtfl_description_init(&d, &lower, &upper, &since);
	return d;
}

/*
 * Build a ChunkBatch from several small chunks (so a key straddles chunk
 * boundaries), read it back and assert the grouped, consolidated contents.
 */
DTFL_TEST(chunk__build_and_read_straddle)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk *chunks[3];
	__dtfl_chunk_batch *b;
	__dtfl_description d;
	struct row rows[16];
	size_t n;

	/* Key 1 and (1,1) straddle chunk 0 and chunk 1. */
	static const kv c0[] = { { 0, 0, 0, 1 }, { 1, 0, 0, 1 }, { 1, 1, 0, 1 } };
	static const kv c1[] = { { 1, 1, 1, 1 }, { 1, 2, 0, 1 } };
	static const kv c2[] = { { 2, 0, 0, 1 } };

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	chunks[0] = chunk_of(c0, 3);
	chunks[1] = chunk_of(c1, 2);
	chunks[2] = chunk_of(c2, 1);
	d = desc_of(tc, 0, 2, 0);
	b = __dtfl_chunk_builder_seal(&x, chunks, 3, &d);
	__dtfl_batch_release(chunks[0]);
	__dtfl_batch_release(chunks[1]);
	__dtfl_batch_release(chunks[2]);

	n = read_batch(b, rows, 16);
	assert_size(n, ==, 6);
	/* (0,0):0:+1 (1,0):0:+1 (1,1):{0:+1,1:+1} (1,2):0:+1 (2,0):0:+1 */
	assert_uint64(rows[0].k, ==, 0); assert_uint64(rows[0].v, ==, 0);
	assert_uint64(rows[1].k, ==, 1); assert_uint64(rows[1].v, ==, 0);
	assert_uint64(rows[2].k, ==, 1); assert_uint64(rows[2].v, ==, 1);
	assert_uint64(rows[2].t, ==, 0); assert_int64(rows[2].r, ==, 1);
	assert_uint64(rows[3].k, ==, 1); assert_uint64(rows[3].v, ==, 1);
	assert_uint64(rows[3].t, ==, 1); assert_int64(rows[3].r, ==, 1);
	assert_uint64(rows[4].k, ==, 1); assert_uint64(rows[4].v, ==, 2);
	assert_uint64(rows[5].k, ==, 2); assert_uint64(rows[5].v, ==, 0);

	__dtfl_chunk_batch_release(b);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * pack (settle): a mix of small and full chunks with distinct, increasing
 * keys packs to a maximal grading, preserving contents and sort order.
 */
DTFL_TEST(chunk__pack_maximal_packing)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk_deque input, output;
	size_t sizes[] = { DTFL_CHUNK_TARGET / 3, DTFL_CHUNK_TARGET / 3,
	    DTFL_CHUNK_TARGET / 3, DTFL_CHUNK_TARGET, DTFL_CHUNK_TARGET / 2,
	    DTFL_CHUNK_TARGET / 2, DTFL_CHUNK_TARGET, 1,
	    DTFL_CHUNK_TARGET - 1 };
	size_t ns = sizeof sizes / sizeof sizes[0], total = 0, i, got = 0;
	uint64_t key = 0, prev;
	bool first = true;

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	__dtfl_chunk_deque_init(&input);
	__dtfl_chunk_deque_init(&output);
	for (i = 0; i < ns; i++) {
		__dtfl_chunk *c = __dtfl_batch_new(1, 8);
		unsigned char rec[16];
		size_t j;
		uint64_t t = 0;
		int64_t r = 1;

		for (j = 0; j < sizes[i]; j++) {
			dtfl_put_u64(rec, key);
			dtfl_put_u64(rec + 8, 0);
			__dtfl_batch_push(c, rec, 16, &t, &r);
			key++;
		}
		total += sizes[i];
		__dtfl_chunk_deque_push_back(&input, c);
		__dtfl_chunk_settle(&x, &input, false, &output);
	}
	__dtfl_chunk_settle(&x, &input, true, &output);

	assert_true(__dtfl_chunk_is_graded(&output));
	/* Nothing lost; keys stay strictly sorted across the new breaks. */
	prev = 0;
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

/*
 * A resumable ChunkBatchMerger of two adjacent batches under tiny fuel (so it
 * suspends and settles on nearly every tick) yields the advanced, consolidated
 * union, and the result is graded (the spine-of-chunks merge step).
 */
DTFL_TEST(chunk__merger_resumable_consolidates)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_chunk *c1, *c2;
	__dtfl_chunk_batch *s1, *s2, *merged;
	__dtfl_description d1, d2;
	__dtfl_antichain compaction;
	__dtfl_chunk_merger m;
	struct row rows[16];
	size_t n;
	uint64_t since8 = 8;

	/* Batch 1: [0,5); (1,2):0:+1 (3,3):1:+1.  Batch 2: [5,10); (1,2):6:+1 (3,3):7:-1. */
	static const kv b1[] = { { 1, 2, 0, 1 }, { 3, 3, 1, 1 } };
	static const kv b2[] = { { 1, 2, 6, 1 }, { 3, 3, 7, -1 } };

	(void)params; (void)data;
	ctx_of(&x, rt, tc);
	c1 = chunk_of(b1, 2);
	c2 = chunk_of(b2, 2);
	d1 = desc_of(tc, 0, 5, 0);
	d2 = desc_of(tc, 5, 10, 0);
	s1 = __dtfl_chunk_builder_seal(&x, &c1, 1, &d1);
	s2 = __dtfl_chunk_builder_seal(&x, &c2, 1, &d2);
	__dtfl_batch_release(c1);
	__dtfl_batch_release(c2);

	__dtfl_antichain_init_elem(&compaction, tc, &since8);
	__dtfl_chunk_merger_new(&m, s1, s2, &compaction);
	for (;;) {
		int64_t fuel = 1;

		__dtfl_chunk_merger_work(&m, &fuel);
		if (fuel > 0)
			break;
	}
	merged = __dtfl_chunk_merger_done(&m);

	n = read_batch(merged, rows, 16);
	/* (1,2): 0 and 6 both advance to 8 and sum to +2; (3,3) cancels. */
	assert_size(n, ==, 1);
	assert_uint64(rows[0].k, ==, 1); assert_uint64(rows[0].v, ==, 2);
	assert_uint64(rows[0].t, ==, 8); assert_int64(rows[0].r, ==, 2);

	__dtfl_chunk_batch_release(merged);
	__dtfl_antichain_free(&compaction);
	__dtfl_chunk_batch_release(s1);
	__dtfl_chunk_batch_release(s2);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(chunk__build_and_read_straddle),
	DTFL_T(chunk__pack_maximal_packing),
	DTFL_T(chunk__merger_resumable_consolidates),
	DTFL_T_END
};

DTFL_TEST_MAIN("chunk", tests)
