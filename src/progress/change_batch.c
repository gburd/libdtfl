/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/progress/change_batch.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/progress/change_batch.c: a collection of updates (key, i64).
 *
 * A ChangeBatch accumulates updates, consolidating the representation
 * and removing keys whose value accumulates to zero.  It is as lazy as
 * possible: updates are appended until they are required, so several
 * simple-looking operations may compact.  Every operation is amortized
 * constant or logarithmic time.
 *
 * An entry is kw key words then one int64 delta, stored as a uint64_t
 * (two's complement).  Keys compare word-lexicographically, which is
 * Rust's Ord for every key upstream uses (progress_int.h).  The delta
 * sum wraps on overflow, where Rust's release build also wraps.
 */
#include "progress/progress_int.h"

static size_t
entry_size(unsigned kw)
{
	return ((size_t)kw + 1) * sizeof(uint64_t);
}

static uint64_t *
entry(__dtfl_change_batch *cb, size_t i)
{
	return (uint64_t *)__dtfl_vec_at(&cb->updates, i);
}

void
__dtfl_cb_init(__dtfl_change_batch *cb, unsigned key_words)
{
	__dtfl_vec_init(&cb->updates, entry_size(key_words));
	cb->clean = 0;
	cb->kw = key_words;
}

void
__dtfl_cb_free(__dtfl_change_batch *cb)
{
	__dtfl_vec_free(&cb->updates);
	cb->clean = 0;
}

/* Clears the batch, keeping its allocation. */
void
__dtfl_cb_clear(__dtfl_change_batch *cb)
{
	__dtfl_vec_clear(&cb->updates);
	cb->clean = 0;
}

/* True if the batch is not guaranteed compact. */
bool
__dtfl_cb_is_dirty(const __dtfl_change_batch *cb)
{
	return cb->updates.len > cb->clean;
}

/*
 * Maintain the bounds of pending (non-compacted) updates versus clean
 * (compacted) data, compacting only once enough work has accumulated:
 * more than 32 elements and at least half of them not clean.
 */
static void
maintain_bounds(__dtfl_change_batch *cb)
{
	if (cb->updates.len > 32 && cb->updates.len >> 1 >= cb->clean)
		__dtfl_cb_compact(cb);
}

/* Appends every update of `src` (extend); `src` is unchanged. */
void
__dtfl_cb_extend(__dtfl_change_batch *cb, const __dtfl_change_batch *src)
{
	/* Rust's &mut rules forbid aliasing; extending from itself would read freed memory. */
	DTFL_ASSERT(cb != src);
	DTFL_ASSERT(cb->kw == src->kw);
	__dtfl_vec_push_n(&cb->updates, src->updates.data, src->updates.len);
	maintain_bounds(cb);
}

/* Swaps entries i and j word by word. */
static void
entry_swap(__dtfl_change_batch *cb, size_t i, size_t j)
{
	uint64_t *x = entry(cb, i), *y = entry(cb, j), t;
	unsigned w;

	for (w = 0; w <= cb->kw; w++) {
		t = x[w];
		x[w] = y[w];
		y[w] = t;
	}
}

static void
sift_down(__dtfl_change_batch *cb, size_t root, size_t n)
{
	size_t child;

	while ((child = 2 * root + 1) < n) {
		if (child + 1 < n && __dtfl_words_cmp(entry(cb, child),
		    entry(cb, child + 1), cb->kw) < 0)
			child++;
		if (__dtfl_words_cmp(entry(cb, root), entry(cb, child),
		    cb->kw) >= 0)
			return;
		entry_swap(cb, root, child);
		root = child;
	}
}

/*
 * Insertion sort by key, for short runs.  Most compactions in the
 * progress tracker see two to eight entries; Rust's sort_unstable_by
 * also switches to insertion sort below 20 elements (core::slice::sort
 * smallsort).
 */
#define CB_SMALL_SORT 20

static void
insertion_sort(__dtfl_change_batch *cb, size_t n)
{
	unsigned kw = cb->kw, w;
	size_t i, j, words = (size_t)kw + 1;
	uint64_t *base = (uint64_t *)(void *)cb->updates.data, tmp[DTFL_TIME_MAX + 3];

	DTFL_ASSERT(words <= DTFL_TIME_MAX + 3);
	for (i = 1; i < n; i++) {
		uint64_t *x = base + i * words;

		if (__dtfl_words_cmp(x - words, x, kw) <= 0)
			continue;
		for (w = 0; w < words; w++)
			tmp[w] = x[w];
		j = i;
		do {
			uint64_t *d = base + j * words;
			const uint64_t *src = d - words;

			for (w = 0; w < words; w++)
				d[w] = src[w];
		} while (--j > 0 && __dtfl_words_cmp(base + (j - 1) * words,
		    tmp, kw) > 0);
		for (w = 0; w < words; w++)
			base[j * words + w] = tmp[w];
	}
}

/*
 * Sorts entries by key only, as upstream's sort_unstable_by on .0.
 * The order among equal keys is unspecified, as upstream's: compaction
 * sums them, so it does not show.  Short runs use insertion sort; longer
 * ones heapsort, in place and allocation-free because qsort has no
 * context argument for the key width.
 */
static void
sort_by_key(__dtfl_change_batch *cb)
{
	size_t n = cb->updates.len, i;

	if (n <= CB_SMALL_SORT) {
		insertion_sort(cb, n);
		return;
	}
	for (i = n / 2; i-- > 0;)
		sift_down(cb, i, n);
	for (i = n; i-- > 1;) {
		entry_swap(cb, 0, i);
		sift_down(cb, 0, i);
	}
}

/*
 * Compact the internal representation: sort by key, consolidate equal
 * keys and discard any whose accumulation is zero.  Only does work if
 * some element is dirty.
 */
/*
 * compact for short runs of fixed-width keys: the progress tracker's
 * common cases are a u64 time (one key word) and a pointstamp, a
 * location and a u64 time (three).  The same sort, merge and retain as
 * the general path, with the entry width a constant.
 */
#define COMPACT_FIXED(name, KW)						\
static void								\
name(__dtfl_change_batch *cb, size_t n)					\
{									\
	uint64_t (*e)[KW + 1] =						\
	    (uint64_t (*)[KW + 1])(void *)cb->updates.data;		\
	uint64_t tmp[KW + 1];						\
	size_t i, j, kept = 0;						\
	unsigned w;							\
									\
	for (i = 1; i < n; i++) {					\
		if (__dtfl_words_cmp(e[i - 1], e[i], KW) <= 0)		\
			continue;					\
		for (w = 0; w <= KW; w++)				\
			tmp[w] = e[i][w];				\
		for (j = i; j > 0 &&					\
		    __dtfl_words_cmp(e[j - 1], tmp, KW) > 0; j--) {	\
			for (w = 0; w <= KW; w++)			\
				e[j][w] = e[j - 1][w];			\
		}							\
		for (w = 0; w <= KW; w++)				\
			e[j][w] = tmp[w];				\
	}								\
	for (i = 0; i + 1 < n; i++) {					\
		if (__dtfl_words_cmp(e[i], e[i + 1], KW) == 0) {	\
			e[i + 1][KW] += e[i][KW];			\
			e[i][KW] = 0;					\
		}							\
	}								\
	for (i = 0; i < n; i++) {					\
		if (e[i][KW] == 0)					\
			continue;					\
		if (kept != i)						\
			for (w = 0; w <= KW; w++)			\
				e[kept][w] = e[i][w];			\
		kept++;							\
	}								\
	cb->updates.len = kept;						\
	cb->clean = kept;						\
}

COMPACT_FIXED(compact_kw1, 1)
COMPACT_FIXED(compact_kw3, 3)

void
__dtfl_cb_compact(__dtfl_change_batch *cb)
{
	size_t i, n = cb->updates.len, kept = 0, words = (size_t)cb->kw + 1;
	unsigned kw = cb->kw, w;
	uint64_t *base;

	if (n <= CB_SMALL_SORT && cb->clean < n && n > 1) {
		if (kw == 1) {
			compact_kw1(cb, n);
			return;
		}
		if (kw == 3) {
			compact_kw3(cb, n);
			return;
		}
	}
	if (cb->clean < n && n > 1) {
		sort_by_key(cb);
		base = (uint64_t *)(void *)cb->updates.data;
		for (i = 0; i + 1 < n; i++) {
			uint64_t *x = base + i * words, *y = x + words;

			if (__dtfl_words_cmp(x, y, kw) == 0) {
				y[kw] += x[kw];
				x[kw] = 0;
			}
		}
		/* retain(|x| x.1 != 0), order preserving */
		for (i = 0; i < n; i++) {
			const uint64_t *x = base + i * words;

			if (x[kw] == 0)
				continue;
			if (kept != i) {
				uint64_t *d = base + kept * words;

				for (w = 0; w < words; w++)
					d[w] = x[w];
			}
			kept++;
		}
		/* Shrinking: no element destructor, no reallocation. */
		cb->updates.len = kept;
	}
	cb->clean = cb->updates.len;
}

/*
 * True iff all keys have value zero.  Skips compaction when more than
 * half the entries are clean, as a clean entry is never zero.
 */
bool
__dtfl_cb_is_empty(__dtfl_change_batch *cb)
{
	if (cb->clean > cb->updates.len / 2)
		return false;
	if (cb->clean < cb->updates.len)
		__dtfl_cb_compact(cb);
	return cb->updates.len == 0;
}

/* Number of compacted updates. */
size_t
__dtfl_cb_len(__dtfl_change_batch *cb)
{
	return __dtfl_cb_iter(cb);
}

/*
 * Drains `cb` into `other`.  The same as extending `other` with all of
 * `cb`'s updates, but a swap when `other` holds no updates, which is the
 * common case when propagating.  `cb` is left empty either way.
 */
void
__dtfl_cb_drain_into(__dtfl_change_batch *cb, __dtfl_change_batch *other)
{
	size_t clean;

	DTFL_ASSERT(cb != other);
	DTFL_ASSERT(cb->kw == other->kw);
	if (other->updates.len == 0) {
		__dtfl_vec_swap(&cb->updates, &other->updates);
		clean = cb->clean;
		cb->clean = other->clean;
		other->clean = clean;
	} else {
		__dtfl_cb_extend(other, cb);
		__dtfl_vec_clear(&cb->updates);
		cb->clean = 0;
	}
}
