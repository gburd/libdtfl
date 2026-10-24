/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_notify_int.h: the FrontierNotificator shared by the
 * notification-based operators (delay, aggregate, state_machine, the
 * generic unary_notify and binary_notify entry points).
 *
 * A notificator buffers (capability, count) requests and yields the
 * capabilities whose times no input frontier is still at or before, in
 * non-decreasing order within one make_available pass.  It owns every
 * capability it holds; a yielded capability is handed to the caller,
 * which owns it from then on (notificator.rs FrontierNotificator).
 *
 * Availability is decided against the operator's input frontiers, which
 * the generic builder exposes through dtfl_op_frontier_less_equal.  A
 * capability at time t is available when no input frontier is
 * less-or-equal to t: f.less_equal(t) is false for every input, and no
 * smaller pending request remains (each is less-or-equal to itself, so
 * only its own make_available pass can retire it).  Everything here
 * belongs to one worker thread (PLAN section 5).
 */
#ifndef DTFL_OPS_NOTIFY_INT_H
#define DTFL_OPS_NOTIFY_INT_H

#include "timely/timely_int.h"
#include "progress/progress_int.h"
#include "dtfl/timely_ops.h"

/* A pending request: a retained capability and its request count. */
typedef struct __dtfl_fnote_entry {
	dtfl_cap *cap;	/* owned */
	uint64_t count;
} __dtfl_fnote_entry;

/*
 * The FrontierNotificator.  `pending` holds requests not yet known to be
 * available; `available` holds requests the frontier has passed, ordered
 * so pop() returns the least time.  `frontiers` are the `inputs` input
 * frontiers to consult (an operator's per-input antichains, or a test's);
 * `tc` is the scope's class.
 */
typedef struct __dtfl_fnotificator {
	const dtfl_time_class *tc;	/* not retained */
	const __dtfl_mutable_antichain *frontiers;	/* `inputs` antichains */
	size_t inputs;
	__dtfl_vec pending;		/* __dtfl_fnote_entry */
	__dtfl_vec available;		/* __dtfl_fnote_entry, least time first */
} __dtfl_fnotificator;

/*
 * The public Notificator handle: a thin wrapper over a bound
 * notificator, embedded in an operator context and bound for one
 * schedule.  Defined here so the notify operators can embed one.
 */
struct dtfl_notificator {
	__dtfl_fnotificator *inner;	/* not owned */
};

void __dtfl_fnote_init(__dtfl_fnotificator *n, const dtfl_time_class *tc,
    size_t inputs);
/* Drops every held capability and the backing storage. */
void __dtfl_fnote_free(__dtfl_fnotificator *n);

/*
 * Rebinds the live input frontiers for the current schedule.  `frontiers`
 * is a flat array of `inputs` mutable antichains (op->frontier), consulted
 * by index; it must outlive the schedule.
 */
static inline void
__dtfl_fnote_bind(__dtfl_fnotificator *n, const __dtfl_mutable_antichain *frontiers)
{
	n->frontiers = frontiers;
}

/*
 * Requests a notification at `cap`'s time, taking ownership of `cap`.
 * notify_at_frontiered: if no input frontier is at or before the time it
 * is immediately available, else it is pending (notificator.rs).
 */
void __dtfl_fnote_notify_at(__dtfl_fnotificator *n, dtfl_cap *cap);

/*
 * The next available capability and its count, or false when none is
 * available.  Transfers ownership of *cap to the caller.  Mirrors
 * FrontierNotificator::next_count: refills `available` from `pending`
 * when empty, then merges equal-time entries.
 */
bool __dtfl_fnote_next(__dtfl_fnotificator *n, dtfl_cap **cap, uint64_t *count);

/* An input's frontier (Notificator::frontier): *k times of tc->depth words. */
const uint64_t *__dtfl_fnote_frontier(const __dtfl_fnotificator *n,
    size_t input, size_t *k);

#endif /* DTFL_OPS_NOTIFY_INT_H */
