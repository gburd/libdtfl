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
 * storage that replaces the batch's.
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
	r = dtfl_type_cmp(c->rt, a, alen, b, blen);
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
 * consolidate_updates_slice_slow: sort, then accumulate each run of
 * equal (record, time) into its first diff and keep the run only if the
 * sum is non-zero.  b must be uniquely held (it is rewritten).
 */
void
__dtfl_consolidate_batch(__dtfl_batch *b, const dtfl_type *rt,
    const __dtfl_diff_class *dc)
{
	size_t n = __dtfl_batch_len(b), i, k, len;
	size_t *idx, *tmp;
	row_cmp c = { b, rt };
	unsigned char *accum;
	__dtfl_batch *out;
	const void *rec;

	DTFL_ASSERT(__dtfl_arc_count(&b->ref) == 1);
	DTFL_ASSERT(dc->width == b->dw);
	if (n == 0)
		return;
	idx = __dtfl_alloc(__dtfl_mul_size(n, 2 * sizeof *idx));
	tmp = idx + n;
	for (i = 0; i < n; i++)
		idx[i] = i;
	msort(&c, idx, tmp, n);

	accum = __dtfl_alloc(DTFL_MAX(b->dw, 1u));
	out = __dtfl_batch_new(b->tw, b->dw);
	for (i = 0; i < n; i = k) {
		/* accum = sum of the diffs of rows idx[i..k), all equal. */
		if (b->dw != 0)
			memcpy(accum, __dtfl_batch_diff(b, idx[i]), b->dw);
		for (k = i + 1; k < n && cmp_rows(&c, idx[i], idx[k]) == 0; k++)
			dc->plus_equals(dc, accum, __dtfl_batch_diff(b, idx[k]));
		if (dc->is_zero(dc, accum))
			continue;
		rec = __dtfl_batch_rec(b, idx[i], &len);
		__dtfl_batch_push(out, rec, len, __dtfl_batch_time(b, idx[i]),
		    accum);
	}
	replace_storage(b, out);
	__dtfl_free(accum);
	__dtfl_free(idx);
}

/* ------------------------------------------------------------------ */
/* ConsolidatingContainerBuilder                                       */
/* ------------------------------------------------------------------ */

/*
 * timely container::buffer::default_capacity::<(D, T, R)>(): the rows
 * that fit in BUFFER_SIZE_BYTES.  A variable-length record counts as
 * 24 bytes, the size of the String or Vec that holds it in Rust.
 */
static size_t
default_capacity(const dtfl_type *rt, unsigned tw, size_t dw)
{
	size_t size = (rt->fixed_size != 0 ? rt->fixed_size : 24) +
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
	for (i = 0; i < end; i = j) {
		out = __dtfl_batch_new(cur->tw, cur->dw);
		for (j = i; j < end && j - i < c->cap; j++)
			__dtfl_batch_push_from(out, cur, j);
		__dtfl_vec_push(&c->outbound, &out);
	}
	if (end == 0)
		return;
	rest = __dtfl_batch_new(cur->tw, cur->dw);
	for (i = end; i < n; i++)
		__dtfl_batch_push_from(rest, cur, i);
	replace_storage(cur, rest);
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
