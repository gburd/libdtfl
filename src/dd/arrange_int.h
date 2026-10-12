/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/dd/arrange_int.h: library-internal arrange / shared-trace plumbing.
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1:
 *   differential-dataflow/src/operators/arrange/mod.rs
 *   differential-dataflow/src/operators/arrange/writer.rs
 *   differential-dataflow/src/operators/arrange/agent.rs
 *   differential-dataflow/src/trace/wrappers/frontier.rs
 *   differential-dataflow/src/trace/wrappers/enter.rs
 *   differential-dataflow/src/trace/wrappers/enter_at.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * An arrangement shares one spine-backed trace between a single writer (the
 * arrange operator) and many readers (join, reduce, import) on one worker.
 * The TraceBox wraps the trace with two MutableAntichains that accumulate the
 * logical and physical compaction holds of every live reader; the trace is
 * only compacted to the meet of those holds, so compaction never runs past a
 * reader's frontier and a reader releasing its hold lets compaction advance.
 * The box is reference counted; the writer keeps a weak-style view that stops
 * writing once every reader is gone (PLAN section 5 rule 4: a trace is Rc, so
 * sharing is worker-local).
 *
 * The arrange operator's output stream carries one record per sealed batch:
 * the record bytes are a retained __dtfl_batch_ord pointer (DTFL_BATCH_PTR_T,
 * a Pipeline stream, so the pointer never leaves the worker).  as_collection
 * and the P8 join/reduce operators read those batch pointers, or the shared
 * trace through the agent cursor.  Nothing here is installed or exported.
 */
#ifndef DTFL_ARRANGE_INT_H
#define DTFL_ARRANGE_INT_H

#include "dd/collection_int.h"
#include "dd/trace/trace_int.h"

/* ------------------------------------------------------------------ */
/* Batch pointer records on the arrange output stream.                 */
/* ------------------------------------------------------------------ */

/*
 * The arrange output stream carries a __dtfl_batch_ord * per record (one
 * sealed batch), on a Pipeline pact so the pointer stays on this worker.
 * The record is the pointer bytes; the retained reference belongs to the
 * record, released when the record is consumed.
 */
#define DTFL_BATCH_PTR_SIZE (sizeof(__dtfl_batch_ord *))

/* The dtfl_type for a batch-pointer stream (fixed width, memcmp order). */
const dtfl_type *__dtfl_batch_ptr_type(void);

/* ------------------------------------------------------------------ */
/* TraceBox (agent.rs trace_box::TraceBox).                            */
/* ------------------------------------------------------------------ */

/*
 * A reference-counted wrapper over the trace that tracks the compaction
 * holds of all referees.  adjust_* replaces the hold `lower` with `upper`
 * in the relevant MutableAntichain, then sets the trace's compaction to the
 * resulting frontier.  Owns the trace; freed when the last reference drops.
 */
typedef struct __dtfl_trace_box {
	__dtfl_arc ref;
	__dtfl_mutable_antichain logical_compaction;
	__dtfl_mutable_antichain physical_compaction;
	__dtfl_trace *trace;		/* owned */
	const dtfl_time_class *tc;	/* not retained */
	/* The descriptors the trace was built with, for empty-batch seals. */
	bool keyed;
	const dtfl_type *kt;
	const dtfl_type *vt;
	const __dtfl_diff_class *dc;
} __dtfl_trace_box;

/* TraceBox::new: moves `trace` in and seeds holds from its frontiers. */
__dtfl_trace_box *__dtfl_trace_box_new(__dtfl_trace *trace, bool keyed,
    const dtfl_type *kt, const dtfl_type *vt, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc);
__dtfl_trace_box *__dtfl_trace_box_retain(__dtfl_trace_box *b);
void __dtfl_trace_box_release(__dtfl_trace_box *b);
/* adjust_logical_compaction / adjust_physical_compaction (frontier runs). */
void __dtfl_trace_box_adjust_logical(__dtfl_trace_box *b, const uint64_t *lower,
    size_t nlo, const uint64_t *upper, size_t nhi);
void __dtfl_trace_box_adjust_physical(__dtfl_trace_box *b,
    const uint64_t *lower, size_t nlo, const uint64_t *upper, size_t nhi);

/* ------------------------------------------------------------------ */
/* Replay queues (arrange mod.rs TraceReplayInstruction + queue types).*/
/* ------------------------------------------------------------------ */

typedef enum __dtfl_replay_kind {
	DTFL_REPLAY_BATCH = 0,
	DTFL_REPLAY_FRONTIER
} __dtfl_replay_kind;

/*
 * One replay instruction: a batch with an optional capability hint, or a
 * frontier advance.  BATCH owns a batch reference; FRONTIER owns its
 * antichain.  has_hint distinguishes None (empty-batch seal) from Some.
 */
typedef struct __dtfl_replay_instr {
	__dtfl_replay_kind kind;
	__dtfl_batch_ord *batch;	/* BATCH: retained */
	bool has_hint;			/* BATCH: hint is Some(hint_time) */
	uint64_t hint_time[DTFL_TIME_MAX];
	__dtfl_antichain frontier;	/* FRONTIER: owned */
} __dtfl_replay_instr;

/*
 * A listener queue (agent.rs TraceAgentQueueReader): a FIFO of replay
 * instructions plus the activator of the importing source operator.  It is
 * reference counted: the agent's queue list holds a weak-style view and the
 * import source operator holds the strong reference, so a dropped source
 * stops receiving instructions.  dropped is set when the source releases its
 * strong reference, letting the writer prune it.
 */
typedef struct __dtfl_replay_queue {
	__dtfl_arc ref;
	dtfl_activator *activator;	/* owned; may be NULL */
	__dtfl_vec instrs;		/* __dtfl_replay_instr, FIFO */
	bool dropped;			/* source released its reference */
	const dtfl_time_class *tc;
} __dtfl_replay_queue;

__dtfl_replay_queue *__dtfl_replay_queue_new(dtfl_activator *activator,
    const dtfl_time_class *tc);
__dtfl_replay_queue *__dtfl_replay_queue_retain(__dtfl_replay_queue *q);
void __dtfl_replay_queue_release(__dtfl_replay_queue *q);
/* Push a batch instruction (retains `batch`; hint NULL for None). */
void __dtfl_replay_queue_push_batch(__dtfl_replay_queue *q,
    __dtfl_batch_ord *batch, const uint64_t *hint);
/* Push a frontier instruction (copies the frontier run). */
void __dtfl_replay_queue_push_frontier(__dtfl_replay_queue *q,
    const uint64_t *frontier, size_t n);

/*
 * The shared list of listener queues (arrange mod.rs
 * Rc<RefCell<Vec<TraceAgentQueueWriter>>>).  Reference counted so the agent
 * (strong) and the writer (weak-style view through the same handle) share
 * it; when the agent is gone the writer sees it closed and stops.
 */
typedef struct __dtfl_queue_list {
	__dtfl_arc ref;
	__dtfl_vec queues;		/* __dtfl_replay_queue *, weak-style */
} __dtfl_queue_list;

__dtfl_queue_list *__dtfl_queue_list_new(void);
__dtfl_queue_list *__dtfl_queue_list_retain(__dtfl_queue_list *l);
void __dtfl_queue_list_release(__dtfl_queue_list *l);

/* ------------------------------------------------------------------ */
/* TraceWriter (writer.rs).                                            */
/* ------------------------------------------------------------------ */

/*
 * The write endpoint: pushes finished batches into the shared trace and the
 * listener queues.  `upper` tracks the current upper limit (next batch must
 * have lower == upper).  Holds a strong box reference and the shared queue
 * list; when the agent is gone the box reference count still keeps the trace
 * alive for the writer, but importers can no longer attach.
 */
typedef struct __dtfl_trace_writer {
	__dtfl_antichain upper;
	__dtfl_trace_box *box;		/* retained */
	__dtfl_queue_list *queues;	/* retained */
	const dtfl_time_class *tc;
} __dtfl_trace_writer;

void __dtfl_trace_writer_init(__dtfl_trace_writer *w, __dtfl_trace_box *box,
    __dtfl_queue_list *queues, const dtfl_time_class *tc);
void __dtfl_trace_writer_free(__dtfl_trace_writer *w);
/*
 * insert: advance the trace by `batch` (takes ownership of the reference).
 * `hint` is NULL (empty batch) or a time <= every update in the batch.
 * Asserts upper == batch.lower and batch.lower != batch.upper.
 */
void __dtfl_trace_writer_insert(__dtfl_trace_writer *w, __dtfl_batch_ord *batch,
    const uint64_t *hint);
/* seal: insert an empty batch up to `upper` if it differs from the current. */
void __dtfl_trace_writer_seal(__dtfl_trace_writer *w,
    const __dtfl_antichain *upper);

/* ------------------------------------------------------------------ */
/* TraceAgent (agent.rs).                                              */
/* ------------------------------------------------------------------ */

/*
 * The shared, reference-counted trace reader.  It holds a strong box
 * reference and its own logical / physical compaction antichains, which are
 * holds on the box (released on free, so compaction can then advance).  The
 * agent clones with __dtfl_trace_agent_clone, each clone registering its own
 * holds; this is what import does so two downstream operators share one
 * trace.  The queue list is held strongly here (the writer's view is weak).
 */
typedef struct __dtfl_trace_agent {
	__dtfl_trace_box *box;		/* retained */
	__dtfl_queue_list *queues;	/* retained; NULL once source-only */
	__dtfl_antichain logical_compaction;
	__dtfl_antichain physical_compaction;
	const dtfl_time_class *tc;
	bool keyed;
	const dtfl_type *kt;
	const dtfl_type *vt;
	const __dtfl_diff_class *dc;
} __dtfl_trace_agent;

/*
 * TraceAgent::new: box an empty trace, returning the reader agent and the
 * writer.  Takes ownership of `trace`.  `queues` is created here and shared.
 */
void __dtfl_trace_agent_new(__dtfl_trace *trace, bool keyed,
    const dtfl_type *kt, const dtfl_type *vt, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc, __dtfl_trace_agent *reader,
    __dtfl_trace_writer *writer);
/* Clone: a second reader sharing the box, re-registering this agent's holds. */
void __dtfl_trace_agent_clone(const __dtfl_trace_agent *src,
    __dtfl_trace_agent *dst);
/* Drop: release holds (letting compaction advance) and the box / queues. */
void __dtfl_trace_agent_free(__dtfl_trace_agent *a);

/* TraceReader::set_logical_compaction / set_physical_compaction. */
void __dtfl_trace_agent_set_logical(__dtfl_trace_agent *a,
    const uint64_t *frontier, size_t n);
void __dtfl_trace_agent_set_physical(__dtfl_trace_agent *a,
    const uint64_t *frontier, size_t n);
/* get_logical_compaction / get_physical_compaction (borrowed). */
const __dtfl_antichain *__dtfl_trace_agent_logical(const __dtfl_trace_agent *a);
const __dtfl_antichain *__dtfl_trace_agent_physical(
    const __dtfl_trace_agent *a);
/* A cursor over the whole shared trace (reader_local.cursor()). */
void __dtfl_trace_agent_cursor(__dtfl_trace_agent *a, __dtfl_cursor_list *out);
/* batches_through: the batches with a clean cut at `upper`, or false. */
bool __dtfl_trace_agent_cursor_through(__dtfl_trace_agent *a,
    const __dtfl_antichain *upper, __dtfl_cursor_list *out);

/*
 * new_listener: attach a new replay queue to the trace, seeded with the
 * existing batches (hint = minimum) and their upper frontier.  The returned
 * queue is reference counted; the caller (import source operator) owns the
 * strong reference and the agent's list keeps a weak-style view.
 */
__dtfl_replay_queue *__dtfl_trace_agent_new_listener(__dtfl_trace_agent *a,
    dtfl_activator *activator);

/* ------------------------------------------------------------------ */
/* P8 entry points on an arrangement (join_core, reduce_*).            */
/* ------------------------------------------------------------------ */

/*
 * These are the Arranged::join_core, reduce_core and reduce_abelian entry
 * points (arrangement.rs).  The arrangement<->join/reduce cycle in the plan
 * is resolved by declaring them here, in the internal header, so P7 does not
 * link them: P8 defines join_core in src/dd/join.c and reduce_core /
 * reduce_abelian in src/dd/reduce.c.  Signatures are intentionally opaque
 * (void) until P8 fixes them against the pinned join.rs / reduce.rs; P7
 * neither declares them in the public header nor references them, so the
 * symbols gate and docs gate do not require a definition here.
 *
 * dtfl_arranged is forward declared via dtfl/arrange.h, included indirectly
 * through collection_int.h is not; P8 includes dtfl/arrange.h for the type.
 */
#if 0 /* P8: defined in src/dd/join.c and src/dd/reduce.c. */
struct dtfl_arranged;
struct dtfl_collection *__dtfl_arranged_join_core(struct dtfl_arranged *a,
    struct dtfl_arranged *other, void *result);
struct dtfl_arranged *__dtfl_arranged_reduce_core(struct dtfl_arranged *a,
    const char *name, void *logic, void *push);
struct dtfl_arranged *__dtfl_arranged_reduce_abelian(struct dtfl_arranged *a,
    const char *name, void *logic, void *push);
#endif

#endif /* DTFL_ARRANGE_INT_H */
