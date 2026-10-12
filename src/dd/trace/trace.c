/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/mod.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/trace.c: the trace facade over the fuelled spine.  Upstream's
 * TraceReader / Trace traits are realized here as a concrete handle: a spine
 * of ord_neu batches, with insert / close / compaction forwarding, and
 * cursor_through assembling a CursorList over the batches a cut selects (the
 * TraceReader::cursor / cursor_through defaults).  The Batcher, Builder and
 * Merger live in merge_batcher.c and ord_neu.c; the Rc wrappers collapse into
 * the batches' atomic refcounts (PLAN D2), so there is no separate RcBuilder.
 *
 * The P7 arrange operator consumes this; there is no public API in this phase.
 */
#include "dd/trace/trace_int.h"

/*
 * A trace: a spine plus the key, value, time and diff descriptors it was
 * built with.  Owns the spine.
 */
typedef struct __dtfl_trace {
	__dtfl_spine spine;
	bool keyed;
	const dtfl_type *kt;
	const dtfl_type *vt;
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
} __dtfl_trace;

__dtfl_trace *
__dtfl_trace_new(bool keyed, const dtfl_type *kt, const dtfl_type *vt,
    const dtfl_time_class *tc, const __dtfl_diff_class *dc)
{
	__dtfl_trace *t = __dtfl_alloc(sizeof *t);

	__dtfl_spine_init(&t->spine, keyed, kt, vt, tc, dc);
	t->keyed = keyed;
	t->kt = kt;
	t->vt = vt;
	t->tc = tc;
	t->dc = dc;
	return t;
}

void
__dtfl_trace_free(__dtfl_trace *t)
{
	if (t == NULL)
		return;
	__dtfl_spine_free(&t->spine);
	__dtfl_free(t);
}

/* Trace::insert / close. */
void
__dtfl_trace_insert(__dtfl_trace *t, __dtfl_batch_ord *batch)
{
	__dtfl_spine_insert(&t->spine, batch);
}

void
__dtfl_trace_close(__dtfl_trace *t)
{
	__dtfl_spine_close(&t->spine);
}

/* TraceReader compaction forwarding. */
void
__dtfl_trace_set_logical_compaction(__dtfl_trace *t,
    const __dtfl_antichain *frontier)
{
	__dtfl_spine_set_logical_compaction(&t->spine, frontier);
}

void
__dtfl_trace_set_physical_compaction(__dtfl_trace *t,
    const __dtfl_antichain *frontier)
{
	__dtfl_spine_set_physical_compaction(&t->spine, frontier);
}

/*
 * TraceReader::cursor_through: assemble a CursorList over the batches a cut
 * at `upper` selects.  Returns false if there is no clean cut; otherwise it
 * fills *out and transfers the batch references to it.  A NULL/empty `upper`
 * (cursor()) returns every batch.
 */
bool
__dtfl_trace_cursor_through(__dtfl_trace *t, const __dtfl_antichain *upper,
    __dtfl_cursor_list *out)
{
	size_t n;
	__dtfl_batch_ord **batches = __dtfl_spine_batches_through(&t->spine,
	    upper, &n);

	if (n == SIZE_MAX)
		return false;
	__dtfl_cursor_list_init(out, batches, n, t->kt, t->vt);
	__dtfl_free(batches);
	return true;
}

/* cursor(): a cursor over the whole trace (panics if there is no cut). */
void
__dtfl_trace_cursor(__dtfl_trace *t, __dtfl_cursor_list *out)
{
	__dtfl_antichain empty;

	__dtfl_antichain_init(&empty, t->tc);
	if (!__dtfl_trace_cursor_through(t, &empty, out))
		__dtfl_panic("trace: unable to acquire a complete cursor");
	__dtfl_antichain_free(&empty);
}
