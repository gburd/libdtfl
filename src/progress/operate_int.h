/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/progress/operate_int.h: operators, progress tracking, subgraphs.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/progress/mod.rs
 *   timely/src/progress/operate.rs
 *   timely/src/progress/reachability.rs
 *   timely/src/progress/subgraph.rs
 *   timely/src/progress/broadcast.rs
 *   timely/src/scheduling/mod.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * The timestamp type T of upstream becomes a dtfl_time_class carried by
 * each scope (PLAN D3).  Operate<T> and Schedule become vtables.  Rust's
 * Rc<RefCell<SharedProgress<T>>> becomes a plain pointer: everything here
 * belongs to one worker thread and is never shared across threads.
 *
 * Locations: node 0 is the enclosing scope's boundary.  A Location key
 * in change batches is two words, node then (is_source << 63 | port),
 * followed by the time's words (progress_int.h).
 */
#ifndef DTFL_OPERATE_INT_H
#define DTFL_OPERATE_INT_H

#include "progress/progress_int.h"

struct __dtfl_worker;

/* ------------------------------------------------------------------ */
/* Locations                                                           */
/* ------------------------------------------------------------------ */

#define DTFL_PORT_SOURCE (UINT64_C(1) << 63)

typedef struct __dtfl_location {
	size_t node;
	size_t port;
	bool source;	/* true: operator output (Source); false: input (Target) */
} __dtfl_location;

static inline void
__dtfl_location_key(__dtfl_location l, uint64_t key[2])
{
	key[0] = (uint64_t)l.node;
	key[1] = (l.source ? DTFL_PORT_SOURCE : 0) | (uint64_t)l.port;
}

static inline __dtfl_location
__dtfl_location_from_key(const uint64_t key[2])
{
	__dtfl_location l;

	l.node = (size_t)key[0];
	l.source = (key[1] & DTFL_PORT_SOURCE) != 0;
	l.port = (size_t)(key[1] & ~DTFL_PORT_SOURCE);
	return l;
}

/* ------------------------------------------------------------------ */
/* Connectivity (operate.rs)                                           */
/* ------------------------------------------------------------------ */

/*
 * PortConnectivity: for one input (or output), the opposing ports it
 * reaches and an antichain of path summaries to each.  Canonical form:
 * ports sorted and distinct, antichains non-empty (freeze()).
 * Summaries are stored as flat runs of tc->sdepth words.
 */
typedef struct __dtfl_port_conn_entry {
	size_t port;
	__dtfl_vec summaries;	/* uint64_t words, sdepth per summary */
} __dtfl_port_conn_entry;

typedef struct __dtfl_port_conn {
	const dtfl_time_class *tc;
	__dtfl_vec entries;	/* __dtfl_port_conn_entry, sorted by port */
} __dtfl_port_conn;

void __dtfl_port_conn_init(__dtfl_port_conn *pc, const dtfl_time_class *tc);
void __dtfl_port_conn_free(__dtfl_port_conn *pc);
void __dtfl_port_conn_copy(__dtfl_port_conn *dst, const __dtfl_port_conn *src);
/*
 * PortConnectivityBuilder::add_port/insert then freeze, combined: adds a
 * summary for `port`, merging by antichain insertion under
 * summary_less_equal.  Keeps canonical form after every call.
 */
void __dtfl_port_conn_insert(__dtfl_port_conn *pc, size_t port,
    const uint64_t *summary);
/* The antichain for `port`, or NULL; stores the summary count in *n. */
const uint64_t *__dtfl_port_conn_get(const __dtfl_port_conn *pc, size_t port,
    size_t *n);

/* Connectivity: one __dtfl_port_conn per input. */
typedef struct __dtfl_connectivity {
	size_t len;
	__dtfl_port_conn *ports;
} __dtfl_connectivity;

void __dtfl_connectivity_init(__dtfl_connectivity *c, const dtfl_time_class *tc,
    size_t len);
void __dtfl_connectivity_free(__dtfl_connectivity *c);
/* Full connectivity with the identity summary, inputs x outputs. */
void __dtfl_connectivity_identity(__dtfl_connectivity *c,
    const dtfl_time_class *tc, size_t inputs, size_t outputs);

/* ------------------------------------------------------------------ */
/* SharedProgress, Operate, Schedule                                   */
/* ------------------------------------------------------------------ */

typedef enum __dtfl_frontier_interest {
	DTFL_INTEREST_NEVER = 0,
	DTFL_INTEREST_IF_CAPABILITY = 1,
	DTFL_INTEREST_ALWAYS = 2
} __dtfl_frontier_interest;

/*
 * Progress shared between a scope and one child (SharedProgress<T>).
 * Each array has one change batch per port, keyed by times of `tc`.
 */
typedef struct __dtfl_shared_progress {
	const dtfl_time_class *tc;
	size_t inputs, outputs;
	__dtfl_change_batch *frontiers;	/* inputs: frontier changes from parent */
	__dtfl_change_batch *consumeds;	/* inputs: messages consumed by child */
	__dtfl_change_batch *internals;	/* outputs: capability changes */
	__dtfl_change_batch *produceds;	/* outputs: messages produced */
} __dtfl_shared_progress;

__dtfl_shared_progress *__dtfl_shared_progress_new(const dtfl_time_class *tc,
    size_t inputs, size_t outputs);
void __dtfl_shared_progress_free(__dtfl_shared_progress *sp);

/*
 * Schedule: the runnable form of an operator.  schedule() returns true
 * if the operator is incomplete (should not be shut down even with
 * empty frontiers and no capabilities).
 */
typedef struct __dtfl_schedule __dtfl_schedule;
typedef struct __dtfl_schedule_ops {
	bool (*schedule)(__dtfl_schedule *s);
	void (*destroy)(__dtfl_schedule *s);
} __dtfl_schedule_ops;

struct __dtfl_schedule {
	const __dtfl_schedule_ops *ops;
	char *name;		/* owned */
	size_t *path;		/* owned; address of the operator */
	size_t path_len;
};

/*
 * Operate<T>: the builder form, consumed by initialize().  initialize
 * fills `*connectivity` (inputs entries over `outputs` ports), returns
 * the shared progress (which must already hold any initial internal
 * capabilities), and returns the schedulable.  The Operate object is
 * consumed by initialize (it may become the schedulable itself).
 * notify_me points to `inputs` interests and remains valid until
 * initialize returns.
 *
 * Shared progress ownership (upstream Rc<RefCell<SharedProgress>>): the
 * parent owns it after initialize and frees it after destroying the
 * schedulable; the schedulable may use it for its whole life.
 */
typedef struct __dtfl_operate __dtfl_operate;
typedef struct __dtfl_operate_ops {
	bool (*local)(const __dtfl_operate *op);
	size_t (*inputs)(const __dtfl_operate *op);
	size_t (*outputs)(const __dtfl_operate *op);
	const __dtfl_frontier_interest *(*notify_me)(const __dtfl_operate *op);
	__dtfl_schedule *(*initialize)(__dtfl_operate *op,
	    __dtfl_connectivity *connectivity, __dtfl_shared_progress **progress);
	/* Destroys an operate that was never initialized. */
	void (*destroy)(__dtfl_operate *op);
} __dtfl_operate_ops;

struct __dtfl_operate {
	const __dtfl_operate_ops *ops;
};

/* ------------------------------------------------------------------ */
/* Reachability (reachability.rs)                                      */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_reach_builder __dtfl_reach_builder;
typedef struct __dtfl_tracker __dtfl_tracker;

__dtfl_reach_builder *__dtfl_reach_builder_new(const dtfl_time_class *tc);
void __dtfl_reach_builder_free(__dtfl_reach_builder *b);
/* Builder::add_node; takes ownership of the connectivity contents. */
void __dtfl_reach_builder_add_node(__dtfl_reach_builder *b, size_t index,
    size_t inputs, size_t outputs, __dtfl_connectivity *summary);
void __dtfl_reach_builder_add_edge(__dtfl_reach_builder *b,
    __dtfl_location source, __dtfl_location target);
bool __dtfl_reach_builder_is_acyclic(const __dtfl_reach_builder *b);
/*
 * Builder::build: consumes the builder.  Stores in *scope_summary the
 * summary from scope inputs (node 0 sources) to scope outputs (node 0
 * targets): one __dtfl_port_conn per node-0 output.
 */
__dtfl_tracker *__dtfl_reach_builder_build(__dtfl_reach_builder *b,
    __dtfl_connectivity *scope_summary);

void __dtfl_tracker_free(__dtfl_tracker *t);
void __dtfl_tracker_update(__dtfl_tracker *t, __dtfl_location loc,
    const uint64_t *time, int64_t delta);
bool __dtfl_tracker_tracking_anything(__dtfl_tracker *t);
void __dtfl_tracker_propagate_all(__dtfl_tracker *t);
/*
 * Tracker::pushed: changes to implications, keyed (Location, T) (2 +
 * depth words).  The caller iterates and clears.
 */
__dtfl_change_batch *__dtfl_tracker_pushed(__dtfl_tracker *t);
/* Tracker::pushed_output: per scope output, changes keyed by time. */
__dtfl_change_batch *__dtfl_tracker_pushed_output(__dtfl_tracker *t,
    size_t output);
bool __dtfl_tracker_is_global(const __dtfl_tracker *t, __dtfl_location loc,
    const uint64_t *time);

/* PerOperator state accessors (node_state). */
bool __dtfl_tracker_node_frontiers_empty(const __dtfl_tracker *t, size_t node);
bool __dtfl_tracker_node_no_pointstamps(const __dtfl_tracker *t, size_t node);
int64_t __dtfl_tracker_node_cap_counts(const __dtfl_tracker *t, size_t node);
const __dtfl_mutable_antichain *__dtfl_tracker_target_implications(
    const __dtfl_tracker *t, size_t node, size_t port);
const __dtfl_mutable_antichain *__dtfl_tracker_source_implications(
    const __dtfl_tracker *t, size_t node, size_t port);

/* ------------------------------------------------------------------ */
/* Progcaster (broadcast.rs)                                           */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_progcaster __dtfl_progcaster;

__dtfl_progcaster *__dtfl_progcaster_new(struct __dtfl_worker *w,
    const size_t *path, size_t path_len, size_t identifier,
    const dtfl_time_class *tc);
void __dtfl_progcaster_free(__dtfl_progcaster *pc);
/* Compacts, broadcasts if non-empty, and leaves `changes` empty. */
void __dtfl_progcaster_send(__dtfl_progcaster *pc, __dtfl_change_batch *changes);
/* Absorbs received updates into `changes` (keyed (Location, T)). */
void __dtfl_progcaster_recv(__dtfl_progcaster *pc, __dtfl_change_batch *changes);

/* ------------------------------------------------------------------ */
/* Subgraph (subgraph.rs)                                              */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_subgraph_builder __dtfl_subgraph_builder;

/*
 * SubgraphBuilder::new_from.  `path` (copied) is the subgraph's address;
 * its last element is the subgraph's index in the parent.  `inner` is
 * the subgraph's time class, `outer` the parent's; __dtfl_refines(inner,
 * outer) must hold.
 */
__dtfl_subgraph_builder *__dtfl_subgraph_builder_new(const size_t *path,
    size_t path_len, size_t identifier, const char *name,
    const dtfl_time_class *outer, const dtfl_time_class *inner);
void __dtfl_subgraph_builder_free(__dtfl_subgraph_builder *b);
/*
 * new_input: registers a counter of messages entering the scope through
 * a new input; returns the input port.  The batch (keyed by inner times)
 * is written by the enter operator and read by the subgraph.  Ownership
 * passes to the subgraph, which frees it when the subgraph is destroyed:
 * the enter operator that writes it lives in the parent scope and may be
 * freed before the subgraph stops reading it in harvest_inputs, so the
 * writer cannot own it.  This mirrors upstream's Rc shared between the
 * ingress counter and the subgraph's input_messages (enterleave.rs).
 */
size_t __dtfl_subgraph_builder_new_input(__dtfl_subgraph_builder *b,
    __dtfl_change_batch *shared_counts);
size_t __dtfl_subgraph_builder_new_output(__dtfl_subgraph_builder *b);
void __dtfl_subgraph_builder_connect(__dtfl_subgraph_builder *b,
    __dtfl_location source, __dtfl_location target);
size_t __dtfl_subgraph_builder_allocate_child_id(__dtfl_subgraph_builder *b);
/* add_child: takes ownership of the operate. */
void __dtfl_subgraph_builder_add_child(__dtfl_subgraph_builder *b,
    __dtfl_operate *child, size_t index, size_t identifier);
const size_t *__dtfl_subgraph_builder_path(const __dtfl_subgraph_builder *b,
    size_t *len);
size_t __dtfl_subgraph_builder_index(const __dtfl_subgraph_builder *b);
const char *__dtfl_subgraph_builder_name(const __dtfl_subgraph_builder *b);
const dtfl_time_class *__dtfl_subgraph_builder_inner(const __dtfl_subgraph_builder *b);
const dtfl_time_class *__dtfl_subgraph_builder_outer(const __dtfl_subgraph_builder *b);
/*
 * SubgraphBuilder::build: consumes the builder and returns the subgraph
 * as an Operate over the outer time class.  Uses the worker for the
 * progcaster channel, activations, logging and progress mode.
 */
__dtfl_operate *__dtfl_subgraph_builder_build(__dtfl_subgraph_builder *b,
    struct __dtfl_worker *w);

#endif /* DTFL_OPERATE_INT_H */
