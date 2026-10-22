/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/progress/progress_int.h: change batches and antichains.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/progress/change_batch.rs
 *   timely/src/progress/frontier.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Keys are fixed-width runs of uint64_t words compared
 * word-lexicographically, which is Rust's Ord for every key type upstream
 * uses: a time T, and (Location, T) where Location is encoded as two
 * words (node, is_source << 63 | port).  That encoding keeps Rust's
 * derived Ord on Location { node, port: Port }: node first, then every
 * Port::Target before every Port::Source (variant order), then the index.  The key width is fixed per
 * batch at init.  A ChangeBatch never interprets its keys beyond
 * comparing them.
 *
 * Antichains store times of one dtfl_time_class as flat runs.  They use
 * the class's partial order (less_equal) for domination and its Ord
 * (words) for sorting, exactly as upstream separates PartialOrder from
 * Ord.
 */
#ifndef DTFL_PROGRESS_INT_H
#define DTFL_PROGRESS_INT_H

#include "progress/time_int.h"

/* ------------------------------------------------------------------ */
/* ChangeBatch                                                         */
/* ------------------------------------------------------------------ */

/*
 * A collection of updates (key, i64).  Entries are `kw` key words
 * followed by one int64 delta, stored contiguously.  The first `clean`
 * entries are sorted, distinct and non-zero.  Compaction happens
 * automatically once more than 32 entries exist and at most half are
 * clean (change_batch.rs maintain_bounds).
 */
typedef struct __dtfl_change_batch {
	__dtfl_vec updates;	/* element size (kw + 1) * 8 */
	size_t clean;
	unsigned kw;		/* key words */
} __dtfl_change_batch;

void __dtfl_cb_init(__dtfl_change_batch *cb, unsigned key_words);
void __dtfl_cb_free(__dtfl_change_batch *cb);
void __dtfl_cb_clear(__dtfl_change_batch *cb);
bool __dtfl_cb_is_dirty(const __dtfl_change_batch *cb);
void __dtfl_cb_compact(__dtfl_change_batch *cb);
/* is_empty and len compact first, as upstream. */
bool __dtfl_cb_is_empty(__dtfl_change_batch *cb);
size_t __dtfl_cb_len(__dtfl_change_batch *cb);
/* Moves all updates into `other` (drain_into); `cb` is left empty. */
void __dtfl_cb_drain_into(__dtfl_change_batch *cb, __dtfl_change_batch *other);
/* Appends every update of `src` to `cb` (extend); src is unchanged. */
void __dtfl_cb_extend(__dtfl_change_batch *cb, const __dtfl_change_batch *src);

/*
 * Iteration after compaction: __dtfl_cb_iter compacts and returns the
 * entry count; entry i is at __dtfl_cb_key(cb, i), __dtfl_cb_delta(cb, i).
 * "drain" is iterate then clear.
 */

/*
 * Adds an update for `key` with `delta` (ChangeBatch::update), then
 * maintain_bounds: compact once there are more than 32 entries and at
 * least half of them are not clean.  Inline: the progress tracker calls
 * it several times per pointstamp.
 */
static inline void
__dtfl_cb_update(__dtfl_change_batch *cb, const uint64_t *key, int64_t delta)
{
	uint64_t *e = (uint64_t *)__dtfl_vec_push_n_uninit(&cb->updates, 1);
	unsigned w;

	for (w = 0; w < cb->kw; w++)
		e[w] = key[w];
	e[cb->kw] = (uint64_t)delta;
	if (cb->updates.len > 32 && cb->updates.len >> 1 >= cb->clean)
		__dtfl_cb_compact(cb);
}

/* Compacts if dirty and returns the entry count. */
static inline size_t
__dtfl_cb_iter(__dtfl_change_batch *cb)
{
	if (cb->clean < cb->updates.len)
		__dtfl_cb_compact(cb);
	return cb->updates.len;
}

static inline const uint64_t *
__dtfl_cb_key(const __dtfl_change_batch *cb, size_t i)
{
	return (const uint64_t *)__dtfl_vec_at(&cb->updates, i);
}

static inline int64_t
__dtfl_cb_delta(const __dtfl_change_batch *cb, size_t i)
{
	return (int64_t)((const uint64_t *)__dtfl_vec_at(&cb->updates, i))[cb->kw];
}

/* ------------------------------------------------------------------ */
/* Antichain                                                           */
/* ------------------------------------------------------------------ */

/*
 * A set of mutually incomparable times (frontier.rs Antichain).  `n`
 * times of `depth` words each, contiguous in `elems`.  The order of
 * elements is insertion order unless __dtfl_antichain_sort is called.
 */
typedef struct __dtfl_antichain {
	const dtfl_time_class *tc;	/* not retained; owner keeps it alive */
	__dtfl_vec elems;		/* element size depth * 8 (min 8) */
} __dtfl_antichain;

void __dtfl_antichain_init(__dtfl_antichain *a, const dtfl_time_class *tc);
void __dtfl_antichain_free(__dtfl_antichain *a);
void __dtfl_antichain_clear(__dtfl_antichain *a);
/* Antichain::from_elem */
void __dtfl_antichain_init_elem(__dtfl_antichain *a, const dtfl_time_class *tc,
    const uint64_t *t);
/*
 * Antichain::insert: adds t unless some element is less_equal to it, and
 * removes elements t is less_equal to.  Returns true if t was added.
 */
bool __dtfl_antichain_insert(__dtfl_antichain *a, const uint64_t *t);
/* True if some element is less_than / less_equal t. */
bool __dtfl_antichain_less_than(const __dtfl_antichain *a, const uint64_t *t);
bool __dtfl_antichain_less_equal(const __dtfl_antichain *a, const uint64_t *t);
/* Antichain::dominates: every element of `other` is >= some element of a. */
bool __dtfl_antichain_dominates(const __dtfl_antichain *a,
    const __dtfl_antichain *other);
/* Sorts the elements by Ord (words). */
void __dtfl_antichain_sort(__dtfl_antichain *a);
/* Set equality, independent of element order. */
bool __dtfl_antichain_eq(const __dtfl_antichain *a, const __dtfl_antichain *b);
void __dtfl_antichain_copy(__dtfl_antichain *dst, const __dtfl_antichain *src);

static inline size_t
__dtfl_antichain_len(const __dtfl_antichain *a)
{
	return a->elems.len;
}

static inline const uint64_t *
__dtfl_antichain_at(const __dtfl_antichain *a, size_t i)
{
	return (const uint64_t *)__dtfl_vec_at(&a->elems, i);
}

/* Raw slice helpers used where upstream takes AntichainRef. */
bool __dtfl_frontier_less_than(const dtfl_time_class *tc, const uint64_t *f,
    size_t n, const uint64_t *t);
bool __dtfl_frontier_less_equal(const dtfl_time_class *tc, const uint64_t *f,
    size_t n, const uint64_t *t);

/* ------------------------------------------------------------------ */
/* MutableAntichain                                                    */
/* ------------------------------------------------------------------ */

/*
 * A multiset of times with signed counts, maintaining the antichain of
 * its minimal elements with positive count (frontier.rs
 * MutableAntichain).
 */
typedef struct __dtfl_mutable_antichain {
	const dtfl_time_class *tc;
	__dtfl_change_batch updates;	/* key: a time */
	__dtfl_vec frontier;		/* times, depth words each */
	__dtfl_change_batch changes;	/* pending frontier changes */
} __dtfl_mutable_antichain;

void __dtfl_mantichain_init(__dtfl_mutable_antichain *m, const dtfl_time_class *tc);
void __dtfl_mantichain_init_elem(__dtfl_mutable_antichain *m,
    const dtfl_time_class *tc, const uint64_t *bottom);
void __dtfl_mantichain_free(__dtfl_mutable_antichain *m);
void __dtfl_mantichain_clear(__dtfl_mutable_antichain *m);
bool __dtfl_mantichain_is_empty(const __dtfl_mutable_antichain *m);
bool __dtfl_mantichain_less_than(const __dtfl_mutable_antichain *m, const uint64_t *t);
bool __dtfl_mantichain_less_equal(const __dtfl_mutable_antichain *m, const uint64_t *t);

static inline size_t
__dtfl_mantichain_frontier_len(const __dtfl_mutable_antichain *m)
{
	return m->frontier.len;
}

static inline const uint64_t *
__dtfl_mantichain_frontier(const __dtfl_mutable_antichain *m)
{
	return (const uint64_t *)(void *)m->frontier.data;
}

/*
 * update_iter: applies (time, delta) updates from a change batch whose
 * keys are times (kw == depth), rebuilding the frontier if needed.  The
 * resulting frontier changes are left in `m->changes`; the caller
 * iterates and clears them (upstream returns a drain of that batch).
 */
void __dtfl_mantichain_update_batch(__dtfl_mutable_antichain *m,
    __dtfl_change_batch *updates);
void __dtfl_mantichain_update(__dtfl_mutable_antichain *m, const uint64_t *t,
    int64_t delta);
/* Sum of counts for times equal to t. */
int64_t __dtfl_mantichain_count_for(const __dtfl_mutable_antichain *m,
    const uint64_t *t);
/*
 * MutableAntichain::updates: rebuilds, then returns the number of
 * compacted (time, count) entries in m->updates for iteration.
 */
size_t __dtfl_mantichain_updates(__dtfl_mutable_antichain *m);

/* lattice.rs antichain_join_into / antichain_meet over flat runs. */
void __dtfl_antichain_join_into(const dtfl_time_class *tc, const uint64_t *one,
    size_t na, const uint64_t *other, size_t nb, __dtfl_antichain *upper);
void __dtfl_antichain_meet_into(const dtfl_time_class *tc, const uint64_t *one,
    size_t na, const uint64_t *other, size_t nb, __dtfl_antichain *out);

#endif /* DTFL_PROGRESS_INT_H */
