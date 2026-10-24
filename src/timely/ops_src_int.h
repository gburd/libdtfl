/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_src_int.h: shared helpers for the source and probe
 * operators (ops_input.c, ops_unordered_input.c, ops_probe.c).
 *
 * The input and unordered-input operators are raw operators with no
 * inputs and one output, driven from a user-held handle rather than from
 * upstream data.  Both build the same output chain the generic builder
 * does (a Tee the stream reads from, a push Counter over its pusher, and
 * a produced change batch), plus a `progress` change batch that tracks
 * the capabilities the handle holds (input.rs / unordered_input.rs).
 *
 * Upstream keeps these batches in Rc<RefCell<ChangeBatch>> so they
 * outlive both the handle and the installed operator regardless of drop
 * order.  __dtfl_src_shared is that shared cell: it is reference counted
 * (atomic because dtfl_batch counts are, though these never cross a
 * thread), retained by the handle and by the raw operator's logic.
 */
#ifndef DTFL_OPS_SRC_INT_H
#define DTFL_OPS_SRC_INT_H

#include "timely/scope_int.h"
#include "timely/timely_int.h"

/*
 * The source operator's shared capability and produced batches.  The
 * handle updates `progress` (a +1 at each held epoch) and `produced`
 * flows from the output Counter; the raw operator's schedule drains both
 * into its SharedProgress on each step (input.rs Operator::schedule).
 */
typedef struct __dtfl_src_shared {
	__dtfl_arc ref;
	__dtfl_change_batch progress;	/* capability holds, keyed by time */
	__dtfl_change_batch produced;	/* messages sent, keyed by time */
} __dtfl_src_shared;

__dtfl_src_shared *__dtfl_src_shared_new(const dtfl_time_class *tc);
__dtfl_src_shared *__dtfl_src_shared_retain(__dtfl_src_shared *s);
void __dtfl_src_shared_release(__dtfl_src_shared *s);

/*
 * Builds the output side of a source operator in `scope`: reserves an
 * operator slot, allocates the Tee and the push Counter over it (whose
 * produced batch is the shared cell's), installs a raw operator that on
 * each schedule drains the shared progress and produced batches, and
 * returns the stream.  `copies` capabilities at the minimum time are
 * reserved on the output (one per peer, as the input operator does with
 * SharedProgress::new(0, 1) and internals[0] += copies).  The returned
 * push counter is owned by the caller (the handle) and must be destroyed
 * with __dtfl_push_destroy before the shared cell is released.
 */
dtfl_stream *__dtfl_src_build(dtfl_scope *scope, const char *name,
    const dtfl_type *type, __dtfl_src_shared *shared, size_t copies,
    __dtfl_push **out_pusher, size_t **out_addr, size_t *out_addr_len);

#endif /* DTFL_OPS_SRC_INT_H */
