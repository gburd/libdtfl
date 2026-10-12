/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/notificator.c: the FrontierNotificator and Notificator.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/generic/notificator.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A FrontierNotificator buffers notification requests, each a retained
 * capability and a count, and delivers the capabilities whose times the
 * input frontiers have passed.  A request at time t is available when no
 * input frontier is less-or-equal to t and no smaller request is still
 * pending; make_available moves such requests from `pending` to
 * `available` in non-decreasing time order, and next_count delivers the
 * least available time, folding equal-time requests into one count.
 *
 * Upstream keeps `available` in a binary heap keyed by reversed time.
 * This keeps it in a vector and selects the least time with a linear
 * scan: operators hold few open times at once, and the delivery order
 * (least available time first) is the same.  The public Notificator is
 * this notificator with its operator handle bound for one schedule
 * (Notificator::new calls make_available; next calls next_count).
 *
 * The notificator owns every capability it holds; a delivered capability
 * is transferred to the caller.  Public at the pin, removed upstream
 * later (#819); the port keeps the pinned behavior.
 */
#include "timely/ops_notify_int.h"
#include "progress/time_int.h"

/* ------------------------------------------------------------------ */
/* FrontierNotificator (internal)                                      */
/* ------------------------------------------------------------------ */

void
__dtfl_fnote_init(__dtfl_fnotificator *n, const dtfl_time_class *tc,
    size_t inputs)
{
	n->tc = tc;
	n->frontiers = NULL;
	n->inputs = inputs;
	__dtfl_vec_init(&n->pending, sizeof(__dtfl_fnote_entry));
	__dtfl_vec_init(&n->available, sizeof(__dtfl_fnote_entry));
}

void
__dtfl_fnote_free(__dtfl_fnotificator *n)
{
	size_t i;

	for (i = 0; i < n->pending.len; i++)
		dtfl_cap_drop(DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, i)->cap);
	for (i = 0; i < n->available.len; i++)
		dtfl_cap_drop(DTFL_VEC_AT(&n->available, __dtfl_fnote_entry, i)->cap);
	__dtfl_vec_free(&n->pending);
	__dtfl_vec_free(&n->available);
}

const uint64_t *
__dtfl_fnote_frontier(const __dtfl_fnotificator *n, size_t input, size_t *k)
{
	*k = __dtfl_mantichain_frontier_len(&n->frontiers[input]);
	return __dtfl_mantichain_frontier(&n->frontiers[input]);
}

/* True if no input frontier is at or before `time` (the time is ready). */
static bool
fnote_ready(const __dtfl_fnotificator *n, const uint64_t *time)
{
	size_t i;

	for (i = 0; i < n->inputs; i++)
		if (__dtfl_mantichain_less_equal(&n->frontiers[i], time))
			return false;
	return true;
}

void
__dtfl_fnote_notify_at(__dtfl_fnotificator *n, dtfl_cap *cap)
{
	__dtfl_fnote_entry e;

	e.cap = cap;
	e.count = 1;
	/* notify_at_frontiered: available now, or pending for a later pass. */
	if (fnote_ready(n, dtfl_cap_time(cap)))
		__dtfl_vec_push(&n->available, &e);
	else
		__dtfl_vec_push(&n->pending, &e);
}

/*
 * Insertion sort of `pending` by time (Ord), least first.  Operators
 * hold few open times at once, so an insertion sort over the vector is
 * cheap and, unlike qsort, needs no comparator context (the library has
 * no writable globals, PLAN section 5).
 */
static void
fnote_sort_pending(__dtfl_fnotificator *n)
{
	size_t i, j;
	__dtfl_fnote_entry tmp;

	for (i = 1; i < n->pending.len; i++) {
		tmp = *DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, i);
		j = i;
		while (j > 0 && dtfl_time_cmp(n->tc,
		    dtfl_cap_time(DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, j - 1)->cap),
		    dtfl_cap_time(tmp.cap)) > 0) {
			*DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, j) =
			    *DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, j - 1);
			j--;
		}
		*DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, j) = tmp;
	}
}

/*
 * Enables pending requests the frontier has passed, folding equal-time
 * requests so each distinct time appears once (make_available).
 */
static void
fnote_make_available(__dtfl_fnotificator *n)
{
	size_t i;

	if (n->pending.len == 0)
		return;

	/* Sort then merge equal times, as upstream does before draining. */
	if (n->pending.len > 1) {
		fnote_sort_pending(n);
		for (i = 0; i + 1 < n->pending.len; i++) {
			__dtfl_fnote_entry *cur =
			    DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, i);
			__dtfl_fnote_entry *nxt =
			    DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, i + 1);

			if (dtfl_time_cmp(n->tc, dtfl_cap_time(cur->cap),
			    dtfl_cap_time(nxt->cap)) == 0) {
				nxt->count += cur->count;
				cur->count = 0;	/* folded into the next */
			}
		}
		/* Drop the folded (count 0) entries, releasing their caps. */
		for (i = 0; i < n->pending.len;) {
			__dtfl_fnote_entry *e =
			    DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, i);

			if (e->count == 0) {
				dtfl_cap_drop(e->cap);
				__dtfl_vec_remove(&n->pending, i);
			} else {
				i++;
			}
		}
	}

	/* Move every request the frontier has passed into `available`. */
	for (i = 0; i < n->pending.len;) {
		__dtfl_fnote_entry *e =
		    DTFL_VEC_AT(&n->pending, __dtfl_fnote_entry, i);

		if (fnote_ready(n, dtfl_cap_time(e->cap))) {
			__dtfl_vec_push(&n->available, e);
			__dtfl_vec_remove(&n->pending, i);
		} else {
			i++;
		}
	}
}

bool
__dtfl_fnote_next(__dtfl_fnotificator *n, dtfl_cap **cap, uint64_t *count)
{
	size_t i, least;
	__dtfl_fnote_entry chosen;

	if (n->available.len == 0)
		fnote_make_available(n);
	if (n->available.len == 0)
		return false;

	/* Pop the least available time (linear scan over the open times). */
	least = 0;
	for (i = 1; i < n->available.len; i++) {
		const uint64_t *ti =
		    dtfl_cap_time(DTFL_VEC_AT(&n->available, __dtfl_fnote_entry, i)->cap);
		const uint64_t *tl =
		    dtfl_cap_time(DTFL_VEC_AT(&n->available, __dtfl_fnote_entry, least)->cap);

		if (dtfl_time_cmp(n->tc, ti, tl) < 0)
			least = i;
	}
	chosen = *DTFL_VEC_AT(&n->available, __dtfl_fnote_entry, least);
	__dtfl_vec_remove(&n->available, least);

	/* Fold any other available request at the same time into the count. */
	for (i = 0; i < n->available.len;) {
		__dtfl_fnote_entry *e =
		    DTFL_VEC_AT(&n->available, __dtfl_fnote_entry, i);

		if (dtfl_time_cmp(n->tc, dtfl_cap_time(e->cap),
		    dtfl_cap_time(chosen.cap)) == 0) {
			chosen.count += e->count;
			dtfl_cap_drop(e->cap);
			__dtfl_vec_remove(&n->available, i);
		} else {
			i++;
		}
	}

	*cap = chosen.cap;
	*count = chosen.count;
	return true;
}

/* ------------------------------------------------------------------ */
/* Public Notificator (notificator.rs Notificator)                     */
/* ------------------------------------------------------------------ */

/*
 * The public Notificator wraps the live notificator and operator for one
 * schedule.  Notificator::new runs make_available first; the operator
 * logic requests with notify_at and drains with next.  The struct is
 * defined in ops_notify_int.h so the notify operators can embed one.
 */

void
dtfl_notificator_notify_at(dtfl_notificator *note, dtfl_cap *cap)
{
	__dtfl_fnote_notify_at(note->inner, cap);
}

bool
dtfl_notificator_next(dtfl_notificator *note, dtfl_cap **cap, uint64_t *count)
{
	return __dtfl_fnote_next(note->inner, cap, count);
}

const uint64_t *
dtfl_notificator_frontier(const dtfl_notificator *note, size_t input, size_t *k)
{
	return __dtfl_fnote_frontier(note->inner, input, k);
}
