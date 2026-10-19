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
	/* The trace owns its key and value type references for its whole
	 * lifetime: a trace handle (dtfl_arranged_trace) and anything cloning
	 * the box outlive the arrange operator that supplied these types, so
	 * the trace must keep them alive (dtfl_trace_to_vec and cursor_through
	 * read them after the arrangement is freed). */
	t->kt = kt != NULL ? dtfl_type_retain(kt) : NULL;
	t->vt = vt != NULL ? dtfl_type_retain(vt) : NULL;
	t->tc = tc;
	/* The trace outlives the arrange operator that supplied dc, and a
	 * user diff class is reference counted, so the trace keeps the class
	 * alive for its whole lifetime (as it does kt/vt above). */
	t->dc = __dtfl_diff_class_retain(dc);
	return t;
}

void
__dtfl_trace_free(__dtfl_trace *t)
{
	if (t == NULL)
		return;
	__dtfl_spine_free(&t->spine);
	dtfl_type_free(t->kt);
	dtfl_type_free(t->vt);
	__dtfl_diff_class_free(t->dc);
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

const __dtfl_antichain *
__dtfl_trace_get_logical_compaction(const __dtfl_trace *t)
{
	return &t->spine.logical_frontier;
}

const __dtfl_antichain *
__dtfl_trace_get_physical_compaction(const __dtfl_trace *t)
{
	return &t->spine.physical_frontier;
}

/*
 * The trace's current upper frontier (spine_fueled.rs Spine.upper): the
 * upper of the last batch inserted, which the logically-empty seal batches
 * writer.rs seal inserts advance even when they carry no data.  Upstream's
 * map_batches reports every batch (including empties), so new_listener's seed
 * frontier equals this; the C map_batches is built on batches_through, which
 * drops empties, so new_listener reads this directly.
 */
const __dtfl_antichain *
__dtfl_trace_upper(const __dtfl_trace *t)
{
	return &t->spine.upper;
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

/*
 * map_batches: the batches currently in the spine, as an owned array.  A
 * NULL/empty upper selects every batch; there is always a clean cut at the
 * empty frontier, so this never fails.
 */
__dtfl_batch_ord **
__dtfl_trace_map_batches(__dtfl_trace *t, size_t *nout)
{
	__dtfl_antichain empty;
	__dtfl_batch_ord **batches;

	__dtfl_antichain_init(&empty, t->tc);
	batches = __dtfl_spine_batches_through(&t->spine, &empty, nout);
	__dtfl_antichain_free(&empty);
	return batches;
}

size_t
__dtfl_trace_batch_count(const __dtfl_trace *t)
{
	const __dtfl_spine *s = &t->spine;
	size_t i, n = s->pending.len;

	for (i = 0; i < s->merging.len; i++) {
		const __dtfl_merge_state *m = DTFL_VEC_AT(&s->merging,
		    __dtfl_merge_state, i);

		if (m->kind == DTFL_MERGE_INPROGRESS)
			n += 2;
		else if (m->kind != DTFL_MERGE_VACANT && m->present)
			n += 1;
	}
	return n;
}
