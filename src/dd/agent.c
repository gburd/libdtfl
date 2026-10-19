/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/arrange/agent.rs
 *   differential-dataflow/src/operators/arrange/mod.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/agent.c: the shared, reference-counted trace reader/writer plumbing.
 *
 * TraceBox wraps a trace with two MutableAntichains that accumulate the
 * logical and physical compaction holds of all referees (agent.rs
 * trace_box::TraceBox).  Each TraceAgent is a referee: it keeps its own
 * logical / physical antichains, registered as holds on the box, and clones
 * by re-registering those holds (agent.rs Clone).  Dropping an agent removes
 * its holds, which lets the trace compact further; the box only compacts to
 * the frontier of its MutableAntichain, so compaction never advances past a
 * live reader (agent.rs Drop + adjust_*).  The replay queues (mod.rs) carry
 * batches and frontier advances from the single writer to each importing
 * source operator; a dropped source releases its strong queue reference and
 * the writer prunes it (agent.rs new_listener, writer.rs insert).
 */
#include "dd/arrange_int.h"
#include "timely/timely_int.h"

/* ------------------------------------------------------------------ */
/* Batch pointer records on the arrange output stream.                 */
/* ------------------------------------------------------------------ */

/*
 * A fixed-width descriptor whose value is one __dtfl_batch_ord pointer.  The
 * arrange output stream is Pipeline, so these records never serialize or
 * route by hash; the default memcmp cmp and FNV hash are never used in a way
 * that matters.  Static and never freed.
 */
const dtfl_type *
__dtfl_batch_ptr_type(void)
{
	static const dtfl_type t = {
		.name = "batch_ptr",
		.fixed_size = DTFL_BATCH_PTR_SIZE,
	};

	return &t;
}

/* Arena record hooks: a batch-pointer record is a retained batch pointer. */
static void
batch_ptr_drop(void *rec)
{
	__dtfl_batch_ord *b;

	memcpy(&b, rec, sizeof b);
	__dtfl_batch_ord_release(b);
}

static void
batch_ptr_retain(void *rec)
{
	__dtfl_batch_ord *b;

	memcpy(&b, rec, sizeof b);
	__dtfl_batch_ord_retain(b);
}

void
__dtfl_session_set_batch_ptr(dtfl_session *s)
{
	__dtfl_session_set_batch_ptr_hooks(s, batch_ptr_drop, batch_ptr_retain);
}

/* ------------------------------------------------------------------ */
/* TraceBox (agent.rs trace_box::TraceBox).                            */
/* ------------------------------------------------------------------ */

__dtfl_trace_box *
__dtfl_trace_box_new(__dtfl_trace *trace, bool keyed, const dtfl_type *kt,
    const dtfl_type *vt, const dtfl_time_class *tc, const __dtfl_diff_class *dc)
{
	__dtfl_trace_box *b = __dtfl_alloc(sizeof *b);

	__dtfl_arc_init(&b->ref);
	b->weak = 0;
	__dtfl_mantichain_init(&b->logical_compaction, tc);
	__dtfl_mantichain_init(&b->physical_compaction, tc);
	b->trace = trace;
	b->tc = tc;
	b->keyed = keyed;
	/*
	 * The box outlives its trace while the writer's weak reference holds
	 * it, and the writer's empty-batch seal reads these, so the box keeps
	 * its own references (the trace's die with the trace).
	 */
	b->kt = kt != NULL ? dtfl_type_retain(kt) : NULL;
	b->vt = vt != NULL ? dtfl_type_retain(vt) : NULL;
	b->dc = __dtfl_diff_class_retain(dc);
	/*
	 * Seed each hold from the trace's current compaction frontier
	 * (TraceBox::new: update_iter over get_logical/physical_compaction).
	 * A fresh spine reports [minimum], so the box starts holding the
	 * minimum, which is what the writer and the first reader expect.
	 */
	{
		const __dtfl_antichain *lo =
		    __dtfl_trace_get_logical_compaction(trace);
		const __dtfl_antichain *ph =
		    __dtfl_trace_get_physical_compaction(trace);
		size_t i;

		for (i = 0; i < __dtfl_antichain_len(lo); i++)
			__dtfl_mantichain_update(&b->logical_compaction,
			    __dtfl_antichain_at(lo, i), 1);
		for (i = 0; i < __dtfl_antichain_len(ph); i++)
			__dtfl_mantichain_update(&b->physical_compaction,
			    __dtfl_antichain_at(ph, i), 1);
	}
	return b;
}

__dtfl_trace_box *
__dtfl_trace_box_retain(__dtfl_trace_box *b)
{
	__dtfl_arc_retain(&b->ref);
	return b;
}

/* Frees the box struct once no strong or weak reference remains. */
static void
box_free(__dtfl_trace_box *b)
{
	if (b->kt != NULL)
		dtfl_type_free(b->kt);
	if (b->vt != NULL)
		dtfl_type_free(b->vt);
	__dtfl_diff_class_free(b->dc);
	__dtfl_free(b);
}

void
__dtfl_trace_box_release(__dtfl_trace_box *b)
{
	if (b == NULL || !__dtfl_arc_release(&b->ref))
		return;
	/* The last strong owner: drop the TraceBox (and its trace). */
	__dtfl_mantichain_free(&b->logical_compaction);
	__dtfl_mantichain_free(&b->physical_compaction);
	__dtfl_trace_free(b->trace);
	b->trace = NULL;
	if (b->weak == 0)
		box_free(b);
}

__dtfl_trace_box *
__dtfl_trace_box_downgrade(__dtfl_trace_box *b)
{
	b->weak++;
	return b;
}

void
__dtfl_trace_box_release_weak(__dtfl_trace_box *b)
{
	if (b == NULL)
		return;
	DTFL_ASSERT(b->weak > 0);
	if (--b->weak == 0 && b->trace == NULL)
		box_free(b);
}

/*
 * adjust_*: replace the hold `lower` with `upper` in the MutableAntichain
 * (add upper with +1, remove lower with -1), then set the trace's compaction
 * to the resulting frontier.  This is upstream's adjust_logical_compaction /
 * adjust_physical_compaction, which keeps the trace compacted exactly to the
 * meet of every live referee's hold.
 */
static void
box_adjust(__dtfl_mutable_antichain *m, const uint64_t *lower, size_t nlo,
    const uint64_t *upper, size_t nhi)
{
	size_t i;
	unsigned depth = m->tc->depth;

	for (i = 0; i < nhi; i++)
		__dtfl_mantichain_update(m, upper + (size_t)depth * i, 1);
	for (i = 0; i < nlo; i++)
		__dtfl_mantichain_update(m, lower + (size_t)depth * i, -1);
}

void
__dtfl_trace_box_adjust_logical(__dtfl_trace_box *b, const uint64_t *lower,
    size_t nlo, const uint64_t *upper, size_t nhi)
{
	__dtfl_antichain f;

	box_adjust(&b->logical_compaction, lower, nlo, upper, nhi);
	__dtfl_antichain_init(&f, b->tc);
	for (size_t i = 0; i < __dtfl_mantichain_frontier_len(&b->logical_compaction); i++)
		__dtfl_antichain_insert(&f, __dtfl_mantichain_frontier(&b->logical_compaction) +
		    (size_t)b->tc->depth * i);
	__dtfl_trace_set_logical_compaction(b->trace, &f);
	__dtfl_antichain_free(&f);
}

void
__dtfl_trace_box_adjust_physical(__dtfl_trace_box *b, const uint64_t *lower,
    size_t nlo, const uint64_t *upper, size_t nhi)
{
	__dtfl_antichain f;

	box_adjust(&b->physical_compaction, lower, nlo, upper, nhi);
	__dtfl_antichain_init(&f, b->tc);
	for (size_t i = 0; i < __dtfl_mantichain_frontier_len(&b->physical_compaction); i++)
		__dtfl_antichain_insert(&f, __dtfl_mantichain_frontier(&b->physical_compaction) +
		    (size_t)b->tc->depth * i);
	__dtfl_trace_set_physical_compaction(b->trace, &f);
	__dtfl_antichain_free(&f);
}

/* ------------------------------------------------------------------ */
/* Replay queues (mod.rs).                                             */
/* ------------------------------------------------------------------ */

__dtfl_replay_queue *
__dtfl_replay_queue_new(dtfl_activator *activator, const dtfl_time_class *tc)
{
	__dtfl_replay_queue *q = __dtfl_alloc(sizeof *q);

	__dtfl_arc_init(&q->ref);
	q->activator = activator;
	__dtfl_vec_init(&q->instrs, sizeof(__dtfl_replay_instr));
	q->dropped = false;
	q->tc = tc;
	return q;
}

__dtfl_replay_queue *
__dtfl_replay_queue_retain(__dtfl_replay_queue *q)
{
	__dtfl_arc_retain(&q->ref);
	return q;
}

static void
instr_free(__dtfl_replay_instr *in)
{
	if (in->kind == DTFL_REPLAY_BATCH)
		__dtfl_batch_ord_release(in->batch);
	else
		__dtfl_antichain_free(&in->frontier);
}

void
__dtfl_replay_queue_release(__dtfl_replay_queue *q)
{
	size_t i;

	if (q == NULL || !__dtfl_arc_release(&q->ref))
		return;
	for (i = 0; i < q->instrs.len; i++)
		instr_free(DTFL_VEC_AT(&q->instrs, __dtfl_replay_instr, i));
	__dtfl_vec_free(&q->instrs);
	if (q->activator != NULL)
		dtfl_activator_free(q->activator);
	__dtfl_free(q);
}

void
__dtfl_replay_queue_push_batch(__dtfl_replay_queue *q, __dtfl_batch_ord *batch,
    const uint64_t *hint)
{
	__dtfl_replay_instr in;

	in.kind = DTFL_REPLAY_BATCH;
	in.batch = __dtfl_batch_ord_retain(batch);
	in.has_hint = hint != NULL;
	if (hint != NULL)
		memcpy(in.hint_time, hint,
		    (size_t)q->tc->depth * sizeof(uint64_t));
	__dtfl_vec_push(&q->instrs, &in);
}

void
__dtfl_replay_queue_push_frontier(__dtfl_replay_queue *q,
    const uint64_t *frontier, size_t n)
{
	__dtfl_replay_instr in;
	size_t i;

	in.kind = DTFL_REPLAY_FRONTIER;
	in.batch = NULL;
	in.has_hint = false;
	__dtfl_antichain_init(&in.frontier, q->tc);
	for (i = 0; i < n; i++)
		__dtfl_antichain_insert(&in.frontier,
		    frontier + (size_t)q->tc->depth * i);
	__dtfl_vec_push(&q->instrs, &in);
}

/* ------------------------------------------------------------------ */
/* Shared queue list (mod.rs Rc<RefCell<Vec<...>>>).                   */
/* ------------------------------------------------------------------ */

__dtfl_queue_list *
__dtfl_queue_list_new(void)
{
	__dtfl_queue_list *l = __dtfl_alloc(sizeof *l);

	__dtfl_arc_init(&l->ref);
	__dtfl_vec_init(&l->queues, sizeof(__dtfl_replay_queue *));
	return l;
}

__dtfl_queue_list *
__dtfl_queue_list_retain(__dtfl_queue_list *l)
{
	__dtfl_arc_retain(&l->ref);
	return l;
}

void
__dtfl_queue_list_release(__dtfl_queue_list *l)
{
	size_t i;

	if (l == NULL || !__dtfl_arc_release(&l->ref))
		return;
	/* The list holds weak-style views; release each remaining one. */
	for (i = 0; i < l->queues.len; i++)
		__dtfl_replay_queue_release(*DTFL_VEC_AT(&l->queues,
		    __dtfl_replay_queue *, i));
	__dtfl_vec_free(&l->queues);
	__dtfl_free(l);
}

/* ------------------------------------------------------------------ */
/* TraceAgent (agent.rs).                                              */
/* ------------------------------------------------------------------ */

/* Logs TraceShare { operator, diff } if the shared trace has a logger. */
static void
agent_log_share(const __dtfl_trace_agent *a, int64_t diff)
{
	size_t op;
	__dtfl_logger *l = __dtfl_trace_logger(a->box->trace, &op);

	if (l != NULL)
		__dtfl_dd_log_trace_share(l, op, diff);
}

void
__dtfl_trace_agent_new(__dtfl_trace *trace, bool keyed, const dtfl_type *kt,
    const dtfl_type *vt, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc, __dtfl_trace_agent *reader,
    __dtfl_trace_writer *writer)
{
	__dtfl_trace_box *box = __dtfl_trace_box_new(trace, keyed, kt, vt, tc,
	    dc);
	__dtfl_queue_list *queues = __dtfl_queue_list_new();

	reader->box = box;	/* takes the box's initial reference */
	reader->queues = __dtfl_queue_list_retain(queues);
	__dtfl_antichain_init(&reader->logical_compaction, tc);
	__dtfl_antichain_init(&reader->physical_compaction, tc);
	/*
	 * The first reader adopts the box's seeded frontier as its hold,
	 * without re-registering: TraceBox::new already counted it, and
	 * TraceAgent::new copies trace.logical_compaction.frontier() into the
	 * agent.  So the agent's free later decrements exactly that seed.
	 */
	{
		size_t i;
		const __dtfl_mutable_antichain *lo = &box->logical_compaction;
		const __dtfl_mutable_antichain *ph = &box->physical_compaction;

		for (i = 0; i < __dtfl_mantichain_frontier_len(lo); i++)
			__dtfl_antichain_insert(&reader->logical_compaction,
			    __dtfl_mantichain_frontier(lo) + (size_t)tc->depth * i);
		for (i = 0; i < __dtfl_mantichain_frontier_len(ph); i++)
			__dtfl_antichain_insert(&reader->physical_compaction,
			    __dtfl_mantichain_frontier(ph) + (size_t)tc->depth * i);
	}
	reader->tc = tc;
	reader->keyed = keyed;
	reader->kt = kt;
	reader->vt = vt;
	reader->dc = dc;
	agent_log_share(reader, 1);

	/*
	 * The writer holds the box weakly (writer.rs new: Rc::downgrade) and
	 * the queue list strongly.
	 */
	__dtfl_trace_writer_init(writer, __dtfl_trace_box_downgrade(box),
	    queues, tc);
}

void
__dtfl_trace_agent_clone(const __dtfl_trace_agent *src, __dtfl_trace_agent *dst)
{
	__dtfl_antichain empty;

	dst->box = __dtfl_trace_box_retain(src->box);
	agent_log_share(src, 1);
	dst->queues = src->queues != NULL ?
	    __dtfl_queue_list_retain(src->queues) : NULL;
	dst->tc = src->tc;
	dst->keyed = src->keyed;
	dst->kt = src->kt;
	dst->vt = src->vt;
	dst->dc = src->dc;
	__dtfl_antichain_init(&dst->logical_compaction, src->tc);
	__dtfl_antichain_init(&dst->physical_compaction, src->tc);
	__dtfl_antichain_copy(&dst->logical_compaction, &src->logical_compaction);
	__dtfl_antichain_copy(&dst->physical_compaction, &src->physical_compaction);

	/* Register this clone's holds on the box (agent.rs Clone). */
	__dtfl_antichain_init(&empty, src->tc);
	__dtfl_trace_box_adjust_logical(dst->box,
	    __dtfl_ac_base(&empty), 0,
	    (const uint64_t *)(void *)dst->logical_compaction.elems.data,
	    __dtfl_antichain_len(&dst->logical_compaction));
	__dtfl_trace_box_adjust_physical(dst->box,
	    __dtfl_ac_base(&empty), 0,
	    (const uint64_t *)(void *)dst->physical_compaction.elems.data,
	    __dtfl_antichain_len(&dst->physical_compaction));
	__dtfl_antichain_free(&empty);
}

void
__dtfl_trace_agent_free(__dtfl_trace_agent *a)
{
	__dtfl_antichain empty;

	if (a->box == NULL)
		return;
	agent_log_share(a, -1);
	/* Decrement holds to zero (agent.rs Drop), letting compaction run. */
	__dtfl_antichain_init(&empty, a->tc);
	__dtfl_trace_box_adjust_logical(a->box,
	    (const uint64_t *)(void *)a->logical_compaction.elems.data,
	    __dtfl_antichain_len(&a->logical_compaction),
	    __dtfl_ac_base(&empty), 0);
	__dtfl_trace_box_adjust_physical(a->box,
	    (const uint64_t *)(void *)a->physical_compaction.elems.data,
	    __dtfl_antichain_len(&a->physical_compaction),
	    __dtfl_ac_base(&empty), 0);
	__dtfl_antichain_free(&empty);

	__dtfl_antichain_free(&a->logical_compaction);
	__dtfl_antichain_free(&a->physical_compaction);
	__dtfl_queue_list_release(a->queues);
	__dtfl_trace_box_release(a->box);
	a->box = NULL;
}

/*
 * set_logical_compaction: join the current hold with `frontier`, adjust the
 * box from the old hold to the join, then adopt the join as this agent's
 * hold.  Upstream does not require frontier >= the current hold; it moves to
 * the joint consequence of both (agent.rs set_logical_compaction).
 */
static void
agent_set(__dtfl_trace_agent *a, __dtfl_antichain *hold, const uint64_t *frontier,
    size_t n, bool physical)
{
	__dtfl_antichain joined;

	__dtfl_antichain_init(&joined, a->tc);
	__dtfl_antichain_join_into(a->tc,
	    (const uint64_t *)(void *)hold->elems.data, __dtfl_antichain_len(hold),
	    frontier, n, &joined);
	if (physical)
		__dtfl_trace_box_adjust_physical(a->box,
		    (const uint64_t *)(void *)hold->elems.data,
		    __dtfl_antichain_len(hold),
		    (const uint64_t *)(void *)joined.elems.data,
		    __dtfl_antichain_len(&joined));
	else
		__dtfl_trace_box_adjust_logical(a->box,
		    (const uint64_t *)(void *)hold->elems.data,
		    __dtfl_antichain_len(hold),
		    (const uint64_t *)(void *)joined.elems.data,
		    __dtfl_antichain_len(&joined));
	__dtfl_antichain_free(hold);
	*hold = joined;
}

void
__dtfl_trace_agent_set_logical(__dtfl_trace_agent *a, const uint64_t *frontier,
    size_t n)
{
	agent_set(a, &a->logical_compaction, frontier, n, false);
}

void
__dtfl_trace_agent_set_physical(__dtfl_trace_agent *a, const uint64_t *frontier,
    size_t n)
{
	agent_set(a, &a->physical_compaction, frontier, n, true);
}

const __dtfl_antichain *
__dtfl_trace_agent_logical(const __dtfl_trace_agent *a)
{
	return &a->logical_compaction;
}

const __dtfl_antichain *
__dtfl_trace_agent_physical(const __dtfl_trace_agent *a)
{
	return &a->physical_compaction;
}

void
__dtfl_trace_agent_cursor(__dtfl_trace_agent *a, __dtfl_cursor_list *out)
{
	__dtfl_trace_cursor(a->box->trace, out);
}

bool
__dtfl_trace_agent_cursor_through(__dtfl_trace_agent *a,
    const __dtfl_antichain *upper, __dtfl_cursor_list *out)
{
	return __dtfl_trace_cursor_through(a->box->trace, upper, out);
}

/*
 * new_listener: seed a new queue with the trace's existing batches (hint =
 * minimum) and their final upper frontier, then register it in the shared
 * list as a weak-style view and activate the source.  The strong reference
 * is returned to the caller (the import source operator).
 */
__dtfl_replay_queue *
__dtfl_trace_agent_new_listener(__dtfl_trace_agent *a, dtfl_activator *activator)
{
	__dtfl_replay_queue *q = __dtfl_replay_queue_new(activator, a->tc);
	__dtfl_batch_ord **batches;
	size_t n, i;
	uint64_t minimum[DTFL_TIME_MAX] = { 0 };
	const __dtfl_antichain *final_upper;

	/* map_batches: all batches currently in the trace. */
	batches = __dtfl_trace_map_batches(a->box->trace, &n);
	for (i = 0; i < n; i++)
		__dtfl_replay_queue_push_batch(q, batches[i], minimum);
	/*
	 * Seed the frontier from the trace's current upper (spine_fueled.rs
	 * Spine.upper, maintained by insert as the last batch's upper).
	 * Upstream agent.rs new_listener takes this frontier from the upper()
	 * of the last batch map_batches yields, and upstream map_batches
	 * (spine_fueled.rs) reports EVERY batch, including the logically-empty
	 * seal batches that writer.rs seal inserts to advance the frontier
	 * without data; so for upstream that last upper equals Spine.upper.
	 * The C port's map_batches is built on batches_through, which drops
	 * empty batches (correct for a cursor), so the last non-empty batch's
	 * upper would lag.  Reading Spine.upper directly reproduces upstream's
	 * seed frontier exactly.  Not pushing the empty seal batch into the
	 * queue is observationally identical: an empty batch yields no cursor
	 * updates, and import_logic skips empty batches on emission, so only
	 * the frontier it announces matters, and that now matches.  Without
	 * this an importer created after a pure frontier advance starts behind
	 * the real frontier (import.rs test_import_stalled_dataflow fails).
	 */
	final_upper = __dtfl_trace_upper(a->box->trace);
	__dtfl_replay_queue_push_frontier(q,
	    (const uint64_t *)(void *)final_upper->elems.data,
	    __dtfl_antichain_len(final_upper));
	for (i = 0; i < n; i++)
		__dtfl_batch_ord_release(batches[i]);
	if (batches != NULL)
		__dtfl_free(batches);

	/* Register a weak-style view and activate (agent.rs new_listener). */
	if (a->queues != NULL) {
		__dtfl_replay_queue *view = __dtfl_replay_queue_retain(q);

		__dtfl_vec_push(&a->queues->queues, &view);
	}
	if (q->activator != NULL)
		dtfl_activator_activate(q->activator);
	return q;
}
