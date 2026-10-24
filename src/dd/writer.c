/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/arrange/writer.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/writer.c: the write endpoint of a shared trace.
 *
 * A TraceWriter accepts a sequence of batches whose [lower, upper) intervals
 * tile the timeline, and distributes each both to the shared trace (through
 * the TraceBox) and to every live listener queue (for importers).  insert
 * asserts the batch is the next in sequence (upper == batch.lower) and
 * non-empty in time (batch.lower != batch.upper), pushes the batch and a
 * frontier advance to each listener, prunes listeners whose source has
 * dropped, and appends the batch to the trace.  seal inserts an empty batch
 * to announce progress without data.  The free path seals to the empty
 * frontier (writer.rs Drop), closing the trace.  The writer holds the box
 * weakly: once every reader is gone the trace is freed and batches reach
 * only the listener queues.  exert forwards to the trace's exert.
 */
#include "dd/arrange_int.h"

void
__dtfl_trace_writer_init(__dtfl_trace_writer *w, __dtfl_trace_box *box,
    __dtfl_queue_list *queues, const dtfl_time_class *tc)
{
	uint64_t minimum[DTFL_TIME_MAX] = { 0 };

	/* upper starts at the minimal frontier (writer.rs new: vec![minimum]). */
	__dtfl_antichain_init_elem(&w->upper, tc, minimum);
	w->box = box;
	w->queues = queues;
	w->tc = tc;
}

void
__dtfl_trace_writer_free(__dtfl_trace_writer *w)
{
	__dtfl_antichain empty;

	if (w->box == NULL)
		return;
	/* Drop: seal to the empty frontier, closing the trace. */
	__dtfl_antichain_init(&empty, w->tc);
	__dtfl_trace_writer_seal(w, &empty);
	__dtfl_antichain_free(&empty);
	/*
	 * The writer drops with its operator.  A trace that outlives it (held
	 * by readers) must not activate the dropped operator's address, nor a
	 * worker that may be freed before the last reader; upstream's
	 * Activator keeps the Activations alive through an Rc instead.
	 */
	if (w->box->trace != NULL)
		__dtfl_trace_set_activator(w->box->trace, NULL);

	__dtfl_antichain_free(&w->upper);
	__dtfl_queue_list_release(w->queues);
	__dtfl_trace_box_release_weak(w->box);
	w->box = NULL;
}

/* Exerts merge effort, even without additional updates. */
void
__dtfl_trace_writer_exert(__dtfl_trace_writer *w)
{
	if (w->box->trace != NULL)
		__dtfl_trace_exert(w->box->trace);
}

void
__dtfl_trace_writer_insert(__dtfl_trace_writer *w, __dtfl_batch_ord *batch,
    const uint64_t *hint)
{
	const __dtfl_antichain *lower = __dtfl_batch_ord_lower(batch);
	const __dtfl_antichain *upper = __dtfl_batch_ord_upper(batch);
	size_t i, keep;

	/* Something is wrong if not a sequence (writer.rs insert). */
	DTFL_ASSERT(__dtfl_antichain_eq(&w->upper, lower));
	DTFL_ASSERT(!__dtfl_antichain_eq(lower, upper));

	__dtfl_antichain_copy(&w->upper, upper);

	/* Push to each listener that still exists; prune the rest. */
	keep = 0;
	for (i = 0; i < w->queues->queues.len; i++) {
		__dtfl_replay_queue *q = *DTFL_VEC_AT(&w->queues->queues,
		    __dtfl_replay_queue *, i);

		if (q->dropped) {
			__dtfl_replay_queue_release(q);
			continue;
		}
		__dtfl_replay_queue_push_batch(q, batch, hint);
		__dtfl_replay_queue_push_frontier(q,
		    (const uint64_t *)(void *)upper->elems.data,
		    __dtfl_antichain_len(upper));
		if (q->activator != NULL)
			dtfl_activator_activate(q->activator);
		*DTFL_VEC_AT(&w->queues->queues, __dtfl_replay_queue *, keep++) = q;
	}
	w->queues->queues.len = keep;

	/*
	 * Append to the trace (takes ownership of the reference), if a reader
	 * still holds it (writer.rs `if let Some(trace) = self.trace.upgrade()`);
	 * otherwise no one can read the trace and the batch is dropped.
	 */
	if (w->box->trace != NULL)
		__dtfl_trace_insert(w->box->trace, batch);
	else
		__dtfl_batch_ord_release(batch);
}

void
__dtfl_trace_writer_seal(__dtfl_trace_writer *w, const __dtfl_antichain *upper)
{
	__dtfl_antichain lower, up;
	__dtfl_batch_ord *empty;

	if (__dtfl_antichain_eq(&w->upper, upper))
		return;

	/* Batch::empty(self.upper, upper) (writer.rs seal). */
	__dtfl_antichain_init(&lower, w->tc);
	__dtfl_antichain_copy(&lower, &w->upper);
	__dtfl_antichain_init(&up, w->tc);
	__dtfl_antichain_copy(&up, upper);
	empty = __dtfl_batch_ord_empty(w->box->keyed, w->box->kt, w->box->vt,
	    w->tc, w->box->dc, &lower, &up);
	__dtfl_trace_writer_insert(w, empty, NULL);
}
