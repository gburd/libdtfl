/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/consolidation.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/consolidate.c: consolidation of update batches.
 *
 * Upstream consolidates a Vec<(D, R)> or Vec<(D, T, R)> in place by
 * sorting and folding equal runs.  Here the updates are the rows of a
 * __dtfl_batch (record bytes, tw time words, dw diff bytes), so both
 * consolidate and consolidate_updates are __dtfl_consolidate_batch,
 * with tw == 0 for the former.  Records order by their dtfl_type (D1)
 * and times by Ord, which is word order (D3).  The sort is a stable
 * merge sort of row indices, then the runs are rebuilt into fresh
 * storage that replaces the batch's.  Rows with a memcmp-ordered record
 * of one width up to 8 bytes and at most one time word sort as packed
 * (word, time, index) triples instead, in the same order.
 */
#include "dd/dd_int.h"

/* Rows of one batch, compared by (record, time). */
typedef struct row_cmp {
	const __dtfl_batch *b;
	const dtfl_type *rt;
} row_cmp;

static int
cmp_rows(const row_cmp *c, size_t i, size_t j)
{
	const void *a, *b;
	size_t alen, blen;
	int r;

	a = __dtfl_batch_rec(c->b, i, &alen);
	b = __dtfl_batch_rec(c->b, j, &blen);
	r = __dtfl_type_cmp(c->rt, a, alen, b, blen);
	if (r != 0)
		return r;
	return __dtfl_words_cmp(__dtfl_batch_time(c->b, i),
	    __dtfl_batch_time(c->b, j), c->b->tw);
}

/* Stable top-down merge sort of v[0..n) using tmp[0..n) as scratch. */
static void
msort(const row_cmp *c, size_t *v, size_t *tmp, size_t n)
{
	size_t h = n / 2, i = 0, j = h, k = 0;

	if (n < 2)
		return;
	msort(c, v, tmp, h);
	msort(c, v + h, tmp, n - h);
	if (cmp_rows(c, v[h - 1], v[h]) <= 0)
		return;		/* already in order */
	while (i < h && j < n)
		tmp[k++] = cmp_rows(c, v[j], v[i]) < 0 ? v[j++] : v[i++];
	while (i < h)
		tmp[k++] = v[i++];
	memcpy(v, tmp, k * sizeof *v);	/* v[j..n) is already in place */
}

/* Moves out's storage into b (whose old storage out takes) and frees out. */
static void
replace_storage(__dtfl_batch *b, __dtfl_batch *out)
{
	__dtfl_vec_swap(&b->arena, &out->arena);
	__dtfl_vec_swap(&b->offs, &out->offs);
	__dtfl_vec_swap(&b->times, &out->times);
	__dtfl_vec_swap(&b->diffs, &out->diffs);
	__dtfl_batch_release(out);
}

/*
 * A row packed for the fixed-width sort: the record as a big-endian word
 * (so word order is memcmp order), its time word, and its row index.
 */
typedef struct packed_row {
	uint64_t k;
	uint64_t t;
	size_t i;
} packed_row;

static inline int
packed_cmp(const packed_row *a, const packed_row *b)
{
	if (a->k != b->k)
		return a->k < b->k ? -1 : 1;
	return (a->t > b->t) - (a->t < b->t);
}

/* msort over packed rows: the same stable merge, compared inline. */
static void
msort_packed(packed_row *v, packed_row *tmp, size_t n)
{
	size_t h = n / 2, i = 0, j = h, k = 0;

	if (n < 2)
		return;
	msort_packed(v, tmp, h);
	msort_packed(v + h, tmp, n - h);
	if (packed_cmp(&v[h - 1], &v[h]) <= 0)
		return;		/* already in order */
	while (i < h && j < n)
		tmp[k++] = packed_cmp(&v[j], &v[i]) < 0 ? v[j++] : v[i++];
	while (i < h)
		tmp[k++] = v[i++];
	memcpy(v, tmp, k * sizeof *v);	/* v[j..n) is already in place */
}

/*
 * The packed rows of b, sorted, when b admits the packed sort: a
 * memcmp-ordered record type, every record the same length of at most 8
 * bytes, and at most one time word.  A record left-aligned in a
 * big-endian word then orders as memcmp does, the lengths tie, and
 * msort_packed is the same stable merge as msort, so the row order comes
 * out identical.  Returns NULL otherwise.  The caller frees the array,
 * which has room for 2n rows (the second half is scratch).
 */
static packed_row *
sort_packed(const __dtfl_batch *b, const dtfl_type *rt)
{
	size_t n = __dtfl_batch_len(b), i, len, len0;
	packed_row *v;

	if (rt->cmp != NULL || b->tw > 1)
		return NULL;
	(void)__dtfl_batch_rec(b, 0, &len0);
	if (len0 > sizeof(uint64_t))
		return NULL;
	v = __dtfl_alloc(__dtfl_mul_size(n, 2 * sizeof *v));
	for (i = 0; i < n; i++) {
		const unsigned char *rec = __dtfl_batch_rec(b, i, &len);
		uint64_t k = 0;
		size_t j;

		if (len != len0) {
			__dtfl_free(v);
			return NULL;
		}
		for (j = 0; j < len; j++)
			k |= (uint64_t)rec[j] << (56 - 8 * j);
		v[i].k = k;
		v[i].t = b->tw != 0 ? __dtfl_batch_time(b, i)[0] : 0;
		v[i].i = i;
	}
	msort_packed(v, v + n, n);
	return v;
}

/* Pushes row r of b with diff accum, unless accum is zero. */
static inline void
push_folded(__dtfl_batch *out, const __dtfl_batch *b, size_t r,
    const __dtfl_diff_class *dc, const void *accum)
{
	const void *rec;
	size_t len;

	if (dc->is_zero(dc, accum))
		return;
	rec = __dtfl_batch_rec(b, r, &len);
	__dtfl_batch_push(out, rec, len, __dtfl_batch_time(b, r), accum);
}

/*
 * consolidate_updates_slice_slow: sort, then accumulate each run of
 * equal (record, time) into its first diff and keep the run only if the
 * sum is non-zero.  b must be uniquely held (it is rewritten).
 */
void
__dtfl_consolidate_batch(__dtfl_batch *b, const dtfl_type *rt,
    const __dtfl_diff_class *dc)
{
	size_t n = __dtfl_batch_len(b), i, k;
	size_t *idx, *tmp;
	row_cmp c = { b, rt };
	unsigned char *accum;
	__dtfl_batch *out;
	packed_row *v;

	DTFL_ASSERT(__dtfl_arc_count(&b->ref) == 1);
	DTFL_ASSERT(dc->width == b->dw);
	if (n == 0)
		return;
	accum = __dtfl_alloc(DTFL_MAX(b->dw, 1u));
	out = __dtfl_batch_new(b->tw, b->dw);
	__dtfl_batch_reserve(out, n, b->arena.len);
	if ((v = sort_packed(b, rt)) != NULL) {
		for (i = 0; i < n; i = k) {
			/* accum = sum of the diffs of rows v[i..k), all equal. */
			if (b->dw != 0)
				memcpy(accum, __dtfl_batch_diff(b, v[i].i),
				    b->dw);
			for (k = i + 1; k < n && packed_cmp(&v[i], &v[k]) == 0;
			    k++)
				dc->plus_equals(dc, accum,
				    __dtfl_batch_diff(b, v[k].i));
			push_folded(out, b, v[i].i, dc, accum);
		}
		__dtfl_free(v);
	} else {
		idx = __dtfl_alloc(__dtfl_mul_size(n, 2 * sizeof *idx));
		tmp = idx + n;
		for (i = 0; i < n; i++)
			idx[i] = i;
		msort(&c, idx, tmp, n);
		for (i = 0; i < n; i = k) {
			/* accum = sum of the diffs of rows idx[i..k), all equal. */
			if (b->dw != 0)
				memcpy(accum, __dtfl_batch_diff(b, idx[i]),
				    b->dw);
			for (k = i + 1;
			    k < n && cmp_rows(&c, idx[i], idx[k]) == 0; k++)
				dc->plus_equals(dc, accum,
				    __dtfl_batch_diff(b, idx[k]));
			push_folded(out, b, idx[i], dc, accum);
		}
		__dtfl_free(idx);
	}
	replace_storage(b, out);
	__dtfl_free(accum);
}

/* ------------------------------------------------------------------ */
/* ConsolidatingContainerBuilder                                       */
/* ------------------------------------------------------------------ */

/*
 * timely container::buffer::default_capacity::<(D, T, R)>(): the rows
 * that fit in BUFFER_SIZE_BYTES.  A record of constant width counts as
 * that width, as size_of does (so (u64, ()) is 8 bytes, the unit adding
 * nothing); a variable-length record counts as 24 bytes, the size of the
 * String or Vec that holds it in Rust.
 */
static size_t
default_capacity(const dtfl_type *rt, unsigned tw, size_t dw)
{
	size_t w = __dtfl_type_const_width(rt);
	size_t size = (w != SIZE_MAX ? w : 24) +
	    (size_t)tw * sizeof(uint64_t) + dw;

	if (size == 0)
		return DTFL_BUFFER_SIZE_BYTES;
	if (size <= DTFL_BUFFER_SIZE_BYTES)
		return DTFL_BUFFER_SIZE_BYTES / size;
	return 1;
}

/* An empty builder.  rt and dc must outlive it. */
void
__dtfl_consolidator_init(__dtfl_consolidator *c, const dtfl_type *rt,
    unsigned tw, const __dtfl_diff_class *dc)
{
	c->rt = rt;
	c->dc = dc;
	c->current = __dtfl_batch_new(tw, (unsigned)dc->width);
	__dtfl_vec_init(&c->outbound, sizeof(__dtfl_batch *));
	c->cap = default_capacity(rt, tw, dc->width);
}

/*
 * consolidate_and_flush_through: consolidates current, then moves its
 * first (len / multiple) * multiple rows to outbound in batches of at
 * most cap rows.  The remainder stays in current.
 */
static void
flush_through(__dtfl_consolidator *c, size_t multiple)
{
	__dtfl_batch *cur = c->current, *rest, *out;
	size_t n, end, i, j;

	__dtfl_consolidate_batch(cur, c->rt, c->dc);
	n = __dtfl_batch_len(cur);
	end = n / multiple * multiple;
	{
		/* Pre-size each output chunk: cap rows and their arena bytes. */
		size_t avg = n != 0 ? cur->arena.len / n : 0;

		for (i = 0; i < end; i = j) {
			size_t run;

			out = __dtfl_batch_new(cur->tw, cur->dw);
			__dtfl_batch_reserve(out, c->cap, c->cap * avg);
			run = DTFL_MIN(c->cap, end - i);
			__dtfl_batch_push_run(out, cur, i, run);
			j = i + run;
			__dtfl_vec_push(&c->outbound, &out);
		}
		if (end == 0)
			return;
		rest = __dtfl_batch_new(cur->tw, cur->dw);
		__dtfl_batch_reserve(rest, n - end, (n - end) * avg);
		__dtfl_batch_push_run(rest, cur, end, n - end);
		replace_storage(cur, rest);
	}
}

/* push_into: buffers one update; flushes full batches at 2 * cap rows. */
void
__dtfl_consolidator_push(__dtfl_consolidator *c, const void *rec, size_t len,
    const uint64_t *time, const void *diff)
{
	__dtfl_batch_push(c->current, rec, len, time, diff);
	if (__dtfl_batch_len(c->current) >= 2 * c->cap)
		flush_through(c, c->cap);
}

/* extract: the oldest completed batch (caller owns it), or NULL. */
__dtfl_batch *
__dtfl_consolidator_extract(__dtfl_consolidator *c)
{
	__dtfl_batch *b;

	if (c->outbound.len == 0)
		return NULL;
	b = *DTFL_VEC_AT(&c->outbound, __dtfl_batch *, 0);
	__dtfl_vec_remove(&c->outbound, 0);
	return b;
}

/* finish: flushes every buffered update, then extract. */
__dtfl_batch *
__dtfl_consolidator_finish(__dtfl_consolidator *c)
{
	if (__dtfl_batch_len(c->current) != 0)
		flush_through(c, 1);
	return __dtfl_consolidator_extract(c);
}

/* Releases the buffer and any batches not yet extracted. */
void
__dtfl_consolidator_free(__dtfl_consolidator *c)
{
	size_t i;

	for (i = 0; i < c->outbound.len; i++)
		__dtfl_batch_release(*DTFL_VEC_AT(&c->outbound,
		    __dtfl_batch *, i));
	__dtfl_vec_free(&c->outbound);
	__dtfl_batch_release(c->current);
	c->current = NULL;
}
