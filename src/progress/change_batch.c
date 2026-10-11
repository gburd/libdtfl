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

/* Adds an update for `key` with `delta`. */
void
__dtfl_cb_update(__dtfl_change_batch *cb, const uint64_t *key, int64_t delta)
{
	uint64_t *e = __dtfl_vec_push(&cb->updates, NULL);

	if (cb->kw != 0)
		memcpy(e, key, cb->kw * sizeof(uint64_t));
	e[cb->kw] = (uint64_t)delta;
	maintain_bounds(cb);
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
 * Sorts entries by key only, as upstream's sort_unstable_by on .0.
 * ponytail: heapsort, in place and allocation-free because qsort has no
 * context argument for the key width; switch to a merge sort if
 * profiling shows compaction is hot.
 */
static void
sort_by_key(__dtfl_change_batch *cb)
{
	size_t n = cb->updates.len, i;

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
void
__dtfl_cb_compact(__dtfl_change_batch *cb)
{
	size_t i, n = cb->updates.len, kept = 0;
	unsigned kw = cb->kw;

	if (cb->clean < n && n > 1) {
		sort_by_key(cb);
		for (i = 0; i + 1 < n; i++) {
			uint64_t *x = entry(cb, i), *y = entry(cb, i + 1);

			if (__dtfl_words_cmp(x, y, kw) == 0) {
				y[kw] += x[kw];
				x[kw] = 0;
			}
		}
		/* retain(|x| x.1 != 0), order preserving */
		for (i = 0; i < n; i++) {
			uint64_t *x = entry(cb, i);

			if (x[kw] == 0)
				continue;
			if (kept != i)
				memcpy(entry(cb, kept), x, cb->updates.elem);
			kept++;
		}
		__dtfl_vec_truncate(&cb->updates, kept);
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
	__dtfl_cb_compact(cb);
	return cb->updates.len == 0;
}

/* Number of compacted updates. */
size_t
__dtfl_cb_len(__dtfl_change_batch *cb)
{
	__dtfl_cb_compact(cb);
	return cb->updates.len;
}

/* Compacts and returns the entry count, for __dtfl_cb_key/_delta. */
size_t
__dtfl_cb_iter(__dtfl_change_batch *cb)
{
	return __dtfl_cb_len(cb);
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
