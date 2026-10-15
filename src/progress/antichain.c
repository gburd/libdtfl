/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/progress/frontier.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/progress/antichain.c: minimal sets of mutually incomparable
 * elements of a partial order.
 *
 * Antichain keeps the minimal elements of everything inserted.
 * MutableAntichain keeps a multiset of times with counts, and the
 * antichain of minimal elements with positive count.  Both store times
 * as flat runs of tc->depth words (one word per element for depth 0, so
 * that vector elements are never zero-sized).  Domination uses the
 * class's partial order; sorting uses Ord, which is word order.  The
 * caller keeps the class alive.
 */
#include "progress/progress_int.h"

/* Bytes per stored element: depth words, at least one. */
static size_t
elem_size(const dtfl_time_class *tc)
{
	return (size_t)DTFL_MAX(tc->depth, 1u) * sizeof(uint64_t);
}

/* ------------------------------------------------------------------ */
/* AntichainRef                                                        */
/* ------------------------------------------------------------------ */

/*
 * True if any of the n elements of `f` is strictly less than `t`.
 * `f` is a flat run of tc->depth words per element.
 */
bool
__dtfl_frontier_less_than(const dtfl_time_class *tc, const uint64_t *f,
    size_t n, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (dtfl_time_less_than(tc, f + i * tc->depth, t))
			return true;
	}
	return false;
}

/* True if any of the n elements of `f` is less than or equal to `t`. */
bool
__dtfl_frontier_less_equal(const dtfl_time_class *tc, const uint64_t *f,
    size_t n, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (dtfl_time_less_equal(tc, f + i * tc->depth, t))
			return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */
/* Antichain                                                           */
/* ------------------------------------------------------------------ */

void
__dtfl_antichain_init(__dtfl_antichain *a, const dtfl_time_class *tc)
{
	a->tc = tc;
	__dtfl_vec_init(&a->elems, elem_size(tc));
}

void
__dtfl_antichain_free(__dtfl_antichain *a)
{
	__dtfl_vec_free(&a->elems);
}

void
__dtfl_antichain_clear(__dtfl_antichain *a)
{
	__dtfl_vec_clear(&a->elems);
}

/* Antichain::from_elem: a new singleton antichain. */
void
__dtfl_antichain_init_elem(__dtfl_antichain *a, const dtfl_time_class *tc,
    const uint64_t *t)
{
	__dtfl_antichain_init(a, tc);
	__dtfl_vec_push(&a->elems, NULL);
	__dtfl_time_copy(tc, (uint64_t *)__dtfl_vec_at(&a->elems, 0), t);
}

/* Element i, writable. */
static uint64_t *
elem_at(__dtfl_antichain *a, size_t i)
{
	return (uint64_t *)__dtfl_vec_at(&a->elems, i);
}

/*
 * Updates the antichain if `t` is not greater than or equal to some
 * present element: removes the elements `t` is less than or equal to,
 * then appends `t`.  Returns true if `t` was added.
 */
bool
__dtfl_antichain_insert(__dtfl_antichain *a, const uint64_t *t)
{
	const dtfl_time_class *tc = a->tc;
	size_t i, kept = 0;

	if (__dtfl_antichain_less_equal(a, t))
		return false;
	/* retain(|x| !element.less_equal(x)), order preserving */
	for (i = 0; i < a->elems.len; i++) {
		if (dtfl_time_less_equal(tc, t, elem_at(a, i)))
			continue;
		if (kept != i)
			memcpy(elem_at(a, kept), elem_at(a, i), a->elems.elem);
		kept++;
	}
	__dtfl_vec_truncate(&a->elems, kept);
	__dtfl_vec_push(&a->elems, NULL);
	__dtfl_time_copy(tc, elem_at(a, kept), t);
	return true;
}

/* True if any element is strictly less than `t`. */
bool
__dtfl_antichain_less_than(const __dtfl_antichain *a, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < a->elems.len; i++) {
		if (dtfl_time_less_than(a->tc, __dtfl_antichain_at(a, i), t))
			return true;
	}
	return false;
}

/* True if any element is less than or equal to `t`. */
bool
__dtfl_antichain_less_equal(const __dtfl_antichain *a, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < a->elems.len; i++) {
		if (dtfl_time_less_equal(a->tc, __dtfl_antichain_at(a, i), t))
			return true;
	}
	return false;
}

/*
 * True if every element of `other` is greater or equal to some element
 * of `a` (PartialOrder for Antichain, which dominates forwards to).
 */
bool
__dtfl_antichain_dominates(const __dtfl_antichain *a,
    const __dtfl_antichain *other)
{
	size_t i;

	for (i = 0; i < other->elems.len; i++) {
		if (!__dtfl_antichain_less_equal(a, __dtfl_antichain_at(other, i)))
			return false;
	}
	return true;
}

/*
 * Sorts the elements by Ord so antichains can be compared.  Insertion
 * sort: antichains are short, and this needs no allocation.
 */
void
__dtfl_antichain_sort(__dtfl_antichain *a)
{
	unsigned d = a->tc->depth;
	uint64_t tmp[DTFL_TIME_MAX];
	size_t i, j;

	for (i = 1; i < a->elems.len; i++) {
		__dtfl_time_copy(a->tc, tmp, elem_at(a, i));
		for (j = i; j > 0 && __dtfl_words_cmp(elem_at(a, j - 1), tmp, d) > 0;
		    j--)
			memcpy(elem_at(a, j), elem_at(a, j - 1), a->elems.elem);
		__dtfl_time_copy(a->tc, elem_at(a, j), tmp);
	}
}

/*
 * Set equality: same length, and either identical sequences (the fast
 * path) or every element of `a` present in `b`.
 */
bool
__dtfl_antichain_eq(const __dtfl_antichain *a, const __dtfl_antichain *b)
{
	const dtfl_time_class *tc = a->tc;
	size_t i, j, n = a->elems.len;
	bool same = true, found;

	if (n != b->elems.len)
		return false;
	for (i = 0; i < n && same; i++)
		same = __dtfl_time_eq(tc, __dtfl_antichain_at(a, i),
		    __dtfl_antichain_at(b, i));
	if (same)
		return true;
	for (i = 0; i < n; i++) {
		found = false;
		for (j = 0; j < n && !found; j++)
			found = __dtfl_time_eq(tc, __dtfl_antichain_at(a, i),
			    __dtfl_antichain_at(b, j));
		if (!found)
			return false;
	}
	return true;
}

/* clone_from: `dst` is initialized and takes `src`'s class and elements. */
void
__dtfl_antichain_copy(__dtfl_antichain *dst, const __dtfl_antichain *src)
{
	if (dst->elems.elem != src->elems.elem) {
		__dtfl_vec_free(&dst->elems);
		__dtfl_vec_init(&dst->elems, src->elems.elem);
	}
	dst->tc = src->tc;
	__dtfl_vec_copy(&dst->elems, &src->elems);
}

/* ------------------------------------------------------------------ */
/* MutableAntichain                                                    */
/* ------------------------------------------------------------------ */

/* Creates a new empty MutableAntichain. */
void
__dtfl_mantichain_init(__dtfl_mutable_antichain *m, const dtfl_time_class *tc)
{
	m->tc = tc;
	__dtfl_cb_init(&m->updates, tc->depth);
	__dtfl_vec_init(&m->frontier, elem_size(tc));
	__dtfl_cb_init(&m->changes, tc->depth);
}

/* MutableAntichain::from_elem: `bottom` with count one. */
void
__dtfl_mantichain_init_elem(__dtfl_mutable_antichain *m,
    const dtfl_time_class *tc, const uint64_t *bottom)
{
	__dtfl_mantichain_init(m, tc);
	__dtfl_cb_update(&m->updates, bottom, 1);
	__dtfl_vec_push(&m->frontier, NULL);
	__dtfl_time_copy(tc, (uint64_t *)(void *)m->frontier.data, bottom);
}

void
__dtfl_mantichain_free(__dtfl_mutable_antichain *m)
{
	__dtfl_cb_free(&m->updates);
	__dtfl_vec_free(&m->frontier);
	__dtfl_cb_free(&m->changes);
}

/* Removes all elements. */
void
__dtfl_mantichain_clear(__dtfl_mutable_antichain *m)
{
	__dtfl_cb_clear(&m->updates);
	__dtfl_vec_clear(&m->frontier);
	__dtfl_cb_clear(&m->changes);
}

/* True if there are no elements with positive count. */
bool
__dtfl_mantichain_is_empty(const __dtfl_mutable_antichain *m)
{
	return m->frontier.len == 0;
}

bool
__dtfl_mantichain_less_than(const __dtfl_mutable_antichain *m, const uint64_t *t)
{
	return __dtfl_frontier_less_than(m->tc, __dtfl_mantichain_frontier(m),
	    m->frontier.len, t);
}

bool
__dtfl_mantichain_less_equal(const __dtfl_mutable_antichain *m,
    const uint64_t *t)
{
	return __dtfl_frontier_less_equal(m->tc, __dtfl_mantichain_frontier(m),
	    m->frontier.len, t);
}

/* Frontier element i, read through the element stride. */
static const uint64_t *
frontier_at(const __dtfl_mutable_antichain *m, size_t i)
{
	return (const uint64_t *)__dtfl_vec_at(&m->frontier, i);
}

/*
 * Tests whether applying (time, delta) will require a frontier rebuild:
 * not when some frontier element is strictly less than `time`, nor when
 * a negative delta lands where no frontier element is less or equal.
 * One pass over the frontier, as upstream.
 */
static bool
requires_rebuild(const __dtfl_mutable_antichain *m, const uint64_t *time,
    int64_t delta)
{
	bool beyond_frontier = false, before_frontier = true;
	size_t i;

	for (i = 0; i < m->frontier.len; i++) {
		const uint64_t *f = frontier_at(m, i);

		if (!beyond_frontier && dtfl_time_less_than(m->tc, f, time))
			beyond_frontier = true;
		if (before_frontier && dtfl_time_less_equal(m->tc, f, time))
			before_frontier = false;
		if (beyond_frontier && !before_frontier)
			break;
	}
	return !(beyond_frontier || (delta < 0 && before_frontier));
}

/*
 * Rebuilds the frontier from the updates.  Meant for bulk updates: it
 * does more work than a single update needs, but handles many at once.
 * The old frontier goes to `changes` with -1, the new with +1.
 */
static void
rebuild(__dtfl_mutable_antichain *m)
{
	const dtfl_time_class *tc = m->tc;
	size_t i, n;

	for (i = 0; i < m->frontier.len; i++)
		__dtfl_cb_update(&m->changes, frontier_at(m, i), -1);
	__dtfl_vec_clear(&m->frontier);

	/*
	 * Build the new frontier from strictly positive times.  The times
	 * are sorted, so a new time never displaces a frontier element.
	 */
	n = __dtfl_cb_iter(&m->updates);
	for (i = 0; i < n; i++) {
		const uint64_t *t = __dtfl_cb_key(&m->updates, i);

		if (__dtfl_cb_delta(&m->updates, i) <= 0)
			continue;
		if (!__dtfl_mantichain_less_equal(m, t)) {
			__dtfl_vec_push(&m->frontier, NULL);
			__dtfl_time_copy(tc, (uint64_t *)__dtfl_vec_last(&m->frontier),
			    t);
		}
	}

	for (i = 0; i < m->frontier.len; i++)
		__dtfl_cb_update(&m->changes, frontier_at(m, i), 1);
}

/* One step of update_iter; returns the updated rebuild flag. */
static bool
update_one(__dtfl_mutable_antichain *m, const uint64_t *time, int64_t delta,
    bool rebuild_required)
{
	if (!rebuild_required)
		rebuild_required = requires_rebuild(m, time, delta);
	__dtfl_cb_update(&m->updates, time, delta);
	return rebuild_required;
}

/*
 * update_iter over the compacted contents of `updates`, which is then
 * cleared (upstream callers pass `batch.drain()`).  Frontier changes are
 * added to m->changes for the caller to iterate and clear.
 */
void
__dtfl_mantichain_update_batch(__dtfl_mutable_antichain *m,
    __dtfl_change_batch *updates)
{
	bool rebuild_required = false;
	size_t i, n;

	DTFL_ASSERT(updates->kw == m->tc->depth);
	n = __dtfl_cb_iter(updates);
	for (i = 0; i < n; i++)
		rebuild_required = update_one(m, __dtfl_cb_key(updates, i),
		    __dtfl_cb_delta(updates, i), rebuild_required);
	__dtfl_cb_clear(updates);
	if (rebuild_required)
		rebuild(m);
}

/* update_iter with a single (time, delta). */
void
__dtfl_mantichain_update(__dtfl_mutable_antichain *m, const uint64_t *t,
    int64_t delta)
{
	if (update_one(m, t, delta, false))
		rebuild(m);
}

/*
 * The count for `t`: the sum over the uncompacted updates equal to it,
 * as upstream reads unstable_internal_updates without compacting.
 */
int64_t
__dtfl_mantichain_count_for(const __dtfl_mutable_antichain *m,
    const uint64_t *t)
{
	const __dtfl_change_batch *u = &m->updates;
	uint64_t sum = 0;
	size_t i;

	for (i = 0; i < u->updates.len; i++) {
		if (__dtfl_time_eq(m->tc, __dtfl_cb_key(u, i), t))
			sum += (uint64_t)__dtfl_cb_delta(u, i);
	}
	return (int64_t)sum;
}

/*
 * MutableAntichain::updates: rebuilds the frontier, then exposes the
 * compacted (time, count) updates for iteration.  Returns the count;
 * entry i is __dtfl_cb_key(&m->updates, i) / __dtfl_cb_delta(...).
 * The rebuild's frontier changes are left in m->changes, as upstream
 * leaves them in its changes batch.
 */
size_t
__dtfl_mantichain_updates(__dtfl_mutable_antichain *m)
{
	rebuild(m);
	return __dtfl_cb_iter(&m->updates);
}

/*
 * lattice.rs antichain_join_into: `upper` (cleared first) becomes the
 * antichain of pairwise joins of `one` and `other` (flat runs of na and
 * nb times).
 */
void
__dtfl_antichain_join_into(const dtfl_time_class *tc, const uint64_t *one,
    size_t na, const uint64_t *other, size_t nb, __dtfl_antichain *upper)
{
	uint64_t j[DTFL_TIME_MAX];
	size_t i, k;

	__dtfl_antichain_clear(upper);
	for (i = 0; i < na; i++)
		for (k = 0; k < nb; k++) {
			dtfl_time_join(tc, one + i * tc->depth, other + k * tc->depth, j);
			(void)__dtfl_antichain_insert(upper, j);
		}
}

/*
 * lattice.rs antichain_meet (and Lattice::meet for Antichain): the
 * antichain of the union of both inputs, into `out` (cleared first).
 */
void
__dtfl_antichain_meet_into(const dtfl_time_class *tc, const uint64_t *one,
    size_t na, const uint64_t *other, size_t nb, __dtfl_antichain *out)
{
	size_t i;

	(void)tc;
	__dtfl_antichain_clear(out);
	for (i = 0; i < na; i++)
		(void)__dtfl_antichain_insert(out, one + i * tc->depth);
	for (i = 0; i < nb; i++)
		(void)__dtfl_antichain_insert(out, other + i * tc->depth);
}
