/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/progress/subgraph.c: a dataflow subgraph presented as an operator.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/progress/subgraph.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A literal port (PLAN D10).  By convention child 0 stands for the
 * subgraph's own inputs and outputs: its outputs are the scope's inputs
 * and its inputs the scope's outputs.  A subgraph is an Operate over the
 * outer time class; inside, everything is in the inner class, and times
 * cross the boundary through Refines (to_inner / to_outer / summarize).
 *
 * Ownership: the subgraph owns its children's schedules and shared
 * progress, its tracker and progcaster.  The input message counters
 * (input_messages) belong to the enter operators that write them and
 * outlive the subgraph (they are dropped with the dataflow).
 */
#include "base/probes_int.h"
#include "progress/operate_int.h"
#include "timely/worker_int.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Per-child state (PerOperatorState)                                  */
/* ------------------------------------------------------------------ */

typedef struct child_state {
	char *name;
	size_t index;
	size_t id;
	bool local;
	__dtfl_frontier_interest *notify;	/* per input */
	size_t inputs, outputs;
	__dtfl_schedule *op;			/* NULL once shut down or for child 0 */
	__dtfl_vec *edges;			/* per output: __dtfl_location */
	__dtfl_shared_progress *progress;
	__dtfl_connectivity summary;		/* moved into the builder at build */
	bool logging;
} child_state;

typedef struct pending_child {
	__dtfl_operate *op;
	size_t index;
	size_t identifier;
} pending_child;

struct __dtfl_subgraph_builder {
	char *name;
	size_t *path;
	size_t path_len;
	size_t index;
	size_t identifier;
	const dtfl_time_class *outer, *inner;
	__dtfl_vec children;		/* pending_child */
	size_t child_count;
	__dtfl_vec edge_stash;		/* pairs of __dtfl_location */
	__dtfl_vec input_messages;	/* __dtfl_change_batch * (not owned) */
	size_t outputs;
};

typedef struct subgraph {
	__dtfl_operate operate;
	__dtfl_schedule sched;
	__dtfl_worker *worker;
	const dtfl_time_class *outer, *inner;
	size_t inputs, outputs;
	child_state *children;
	size_t nchildren;
	bool *incomplete;
	size_t incomplete_count;
	__dtfl_vec temp_active;		/* min-heap of size_t */
	__dtfl_vec maybe_shutdown;	/* size_t */
	__dtfl_change_batch **input_messages;	/* not owned */
	__dtfl_mutable_antichain *output_capabilities;	/* outer times */
	__dtfl_change_batch local_pointstamp;	/* (Location, inner) */
	__dtfl_change_batch final_pointstamp;	/* (Location, inner) */
	__dtfl_tracker *tracker;
	__dtfl_progcaster *progcaster;
	__dtfl_shared_progress *shared_progress;	/* outer */
	__dtfl_connectivity scope_summary;	/* inner summaries */
	dtfl_progress_mode progress_mode;
	__dtfl_frontier_interest *notify_me;
	bool initialized;
} subgraph;

/* ------------------------------------------------------------------ */
/* Builder                                                             */
/* ------------------------------------------------------------------ */

__dtfl_subgraph_builder *
__dtfl_subgraph_builder_new(const size_t *path, size_t path_len,
    size_t identifier, const char *name, const dtfl_time_class *outer,
    const dtfl_time_class *inner)
{
	__dtfl_subgraph_builder *b = __dtfl_calloc(1, sizeof *b);

	DTFL_ASSERT(path_len > 0);
	if (!__dtfl_refines(inner, outer))
		__dtfl_panic("subgraph %s: inner time class does not refine outer", name);
	b->name = __dtfl_strdup(name);
	b->path = __dtfl_memdup(path, path_len * sizeof(size_t));
	b->path_len = path_len;
	b->index = path[path_len - 1];
	b->identifier = identifier;
	b->outer = dtfl_time_class_retain(outer);
	b->inner = dtfl_time_class_retain(inner);
	__dtfl_vec_init(&b->children, sizeof(pending_child));
	b->child_count = 1;
	__dtfl_vec_init(&b->edge_stash, 2 * sizeof(__dtfl_location));
	__dtfl_vec_init(&b->input_messages, sizeof(__dtfl_change_batch *));
	return b;
}

void
__dtfl_subgraph_builder_free(__dtfl_subgraph_builder *b)
{
	size_t i;

	if (b == NULL)
		return;
	for (i = 0; i < b->children.len; i++) {
		__dtfl_operate *op = DTFL_VEC_AT(&b->children, pending_child, i)->op;
		op->ops->destroy(op);
	}
	__dtfl_vec_free(&b->children);
	__dtfl_vec_free(&b->edge_stash);
	__dtfl_vec_free(&b->input_messages);
	dtfl_time_class_free(b->outer);
	dtfl_time_class_free(b->inner);
	__dtfl_free(b->name);
	__dtfl_free(b->path);
	__dtfl_free(b);
}

size_t
__dtfl_subgraph_builder_new_input(__dtfl_subgraph_builder *b,
    __dtfl_change_batch *shared_counts)
{
	__dtfl_vec_push(&b->input_messages, &shared_counts);
	return b->input_messages.len - 1;
}

size_t
__dtfl_subgraph_builder_new_output(__dtfl_subgraph_builder *b)
{
	return b->outputs++;
}

void
__dtfl_subgraph_builder_connect(__dtfl_subgraph_builder *b,
    __dtfl_location source, __dtfl_location target)
{
	__dtfl_location pair[2] = { source, target };

	__dtfl_vec_push(&b->edge_stash, pair);
}

size_t
__dtfl_subgraph_builder_allocate_child_id(__dtfl_subgraph_builder *b)
{
	return b->child_count++;
}

void
__dtfl_subgraph_builder_add_child(__dtfl_subgraph_builder *b,
    __dtfl_operate *child, size_t index, size_t identifier)
{
	pending_child p = { child, index, identifier };

	__dtfl_vec_push(&b->children, &p);
}

const size_t *
__dtfl_subgraph_builder_path(const __dtfl_subgraph_builder *b, size_t *len)
{
	*len = b->path_len;
	return b->path;
}

size_t
__dtfl_subgraph_builder_index(const __dtfl_subgraph_builder *b)
{
	return b->index;
}

const char *
__dtfl_subgraph_builder_name(const __dtfl_subgraph_builder *b)
{
	return b->name;
}

const dtfl_time_class *
__dtfl_subgraph_builder_inner(const __dtfl_subgraph_builder *b)
{
	return b->inner;
}

const dtfl_time_class *
__dtfl_subgraph_builder_outer(const __dtfl_subgraph_builder *b)
{
	return b->outer;
}

/* ------------------------------------------------------------------ */
/* Children                                                            */
/* ------------------------------------------------------------------ */

static void
child_init_empty(child_state *c, const dtfl_time_class *tc, size_t inputs,
    size_t outputs)
{
	size_t i;

	memset(c, 0, sizeof *c);
	c->name = __dtfl_strdup("External");
	c->id = SIZE_MAX;
	c->inputs = inputs;
	c->outputs = outputs;
	c->notify = __dtfl_calloc(DTFL_MAX(inputs, 1), sizeof *c->notify);
	for (i = 0; i < inputs; i++)
		c->notify[i] = DTFL_INTEREST_IF_CAPABILITY;
	c->edges = __dtfl_calloc(DTFL_MAX(outputs, 1), sizeof(__dtfl_vec));
	for (i = 0; i < outputs; i++)
		__dtfl_vec_init(&c->edges[i], sizeof(__dtfl_location));
	c->progress = __dtfl_shared_progress_new(tc, inputs, outputs);
}

/* PerOperatorState::new: initializes the child operator. */
static void
child_init(child_state *c, __dtfl_operate *op, size_t index, size_t identifier,
    bool logging)
{
	size_t i;
	const __dtfl_frontier_interest *nm;

	memset(c, 0, sizeof *c);
	c->index = index;
	c->id = identifier;
	c->logging = logging;
	c->local = op->ops->local(op);
	c->inputs = op->ops->inputs(op);
	c->outputs = op->ops->outputs(op);
	c->notify = __dtfl_calloc(DTFL_MAX(c->inputs, 1), sizeof *c->notify);
	nm = op->ops->notify_me(op);
	for (i = 0; i < c->inputs; i++)
		c->notify[i] = nm[i];
	c->op = op->ops->initialize(op, &c->summary, &c->progress);
	c->name = __dtfl_strdup(c->op->name);
	if (c->summary.len != c->inputs)
		__dtfl_panic("operator summary has %zu inputs when %zu were expected",
		    c->summary.len, c->inputs);
	for (i = 0; i < c->summary.len; i++) {
		const __dtfl_port_conn *pc = &c->summary.ports[i];
		size_t e;

		for (e = 0; e < pc->entries.len; e++)
			if (DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, e)->port >= c->outputs)
				__dtfl_panic("operator summary references invalid output port");
	}
	c->edges = __dtfl_calloc(DTFL_MAX(c->outputs, 1), sizeof(__dtfl_vec));
	for (i = 0; i < c->outputs; i++)
		__dtfl_vec_init(&c->edges[i], sizeof(__dtfl_location));
}

static void
child_shut_down(subgraph *sg, child_state *c)
{
	if (c->op != NULL) {
		if (c->logging)
			__dtfl_worker_log_shutdown(sg->worker, c->id);
		c->op->ops->destroy(c->op);
		c->op = NULL;
	}
}

static void
child_free(subgraph *sg, child_state *c)
{
	size_t i;

	child_shut_down(sg, c);
	for (i = 0; i < c->outputs; i++)
		__dtfl_vec_free(&c->edges[i]);
	__dtfl_free(c->edges);
	__dtfl_free(c->notify);
	__dtfl_shared_progress_free(c->progress);
	__dtfl_connectivity_free(&c->summary);
	__dtfl_free(c->name);
}

/* PerOperatorState::schedule: runs the child; logs start and stop. */
static bool
child_schedule(subgraph *sg, child_state *c)
{
	size_t i;
	bool incomplete;

	if (c->op == NULL) {
		/* A closed operator must not be receiving progress. */
		for (i = 0; i < c->inputs; i++)
			if (!__dtfl_cb_is_empty(&c->progress->frontiers[i]))
				__dtfl_panic("Operator prematurely shut down: %s", c->name);
		return false;
	}
	if (c->logging) {
		for (i = 0; i < c->inputs; i++)
			if (!__dtfl_cb_is_empty(&c->progress->frontiers[i])) {
				__dtfl_worker_log_push_progress(sg->worker, c->id);
				break;
			}
		__dtfl_worker_log_schedule(sg->worker, c->id, true);
	}
	DTFL_PROBE5(operator_start, (uint64_t)sg->worker->allocator->index,
	    (uint64_t)sg->sched.path[0], (uint64_t)c->id, (uint64_t)c->index,
	    (const char *)c->name);
	incomplete = c->op->ops->schedule(c->op);
	DTFL_PROBE5(operator_end, (uint64_t)sg->worker->allocator->index,
	    (uint64_t)sg->sched.path[0], (uint64_t)c->id, (uint64_t)c->index,
	    (uint64_t)incomplete);
	if (c->logging)
		__dtfl_worker_log_schedule(sg->worker, c->id, false);
	return incomplete;
}

static void
heap_push(__dtfl_vec *h, size_t x)
{
	size_t *v, i;

	__dtfl_vec_push(h, &x);
	v = (size_t *)(void *)h->data;
	for (i = h->len - 1; i > 0;) {
		size_t p = (i - 1) / 2, t;

		if (v[p] <= v[i])
			break;
		t = v[p]; v[p] = v[i]; v[i] = t;
		i = p;
	}
}

static size_t
heap_pop(__dtfl_vec *h)
{
	size_t *v = (size_t *)(void *)h->data, top = v[0], n = h->len - 1, i = 0;

	v[0] = v[n];
	__dtfl_vec_truncate(h, n);
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, m = i, t;

		if (l < n && v[l] < v[m])
			m = l;
		if (r < n && v[r] < v[m])
			m = r;
		if (m == i)
			break;
		t = v[m]; v[m] = v[i]; v[i] = t;
		i = m;
	}
	return top;
}

/* (Location, time) key into a change batch. */
static void
pointstamp_update(__dtfl_change_batch *cb, const dtfl_time_class *tc,
    __dtfl_location l, const uint64_t *time, int64_t delta)
{
	uint64_t key[2 + DTFL_TIME_MAX];

	__dtfl_location_key(l, key);
	__dtfl_time_copy(tc, key + 2, time);
	__dtfl_cb_update(cb, key, delta);
}

/*
 * PerOperatorState::extract_progress: moves consumeds, internals and
 * produceds into pointstamp statements, activating message recipients.
 */
static void
child_extract_progress(subgraph *sg, child_state *c, __dtfl_change_batch *pointstamps)
{
	const dtfl_time_class *tc = sg->inner;
	__dtfl_shared_progress *sp = c->progress;
	size_t i, n, k, e;

	for (i = 0; i < c->inputs; i++) {
		__dtfl_location target = { c->index, i, false };

		n = __dtfl_cb_iter(&sp->consumeds[i]);
		for (k = 0; k < n; k++)
			pointstamp_update(pointstamps, tc, target,
			    __dtfl_cb_key(&sp->consumeds[i], k),
			    -__dtfl_cb_delta(&sp->consumeds[i], k));
		__dtfl_cb_clear(&sp->consumeds[i]);
	}
	for (i = 0; i < c->outputs; i++) {
		__dtfl_location source = { c->index, i, true };

		n = __dtfl_cb_iter(&sp->internals[i]);
		for (k = 0; k < n; k++)
			pointstamp_update(pointstamps, tc, source,
			    __dtfl_cb_key(&sp->internals[i], k),
			    __dtfl_cb_delta(&sp->internals[i], k));
		__dtfl_cb_clear(&sp->internals[i]);
	}
	for (i = 0; i < c->outputs; i++) {
		n = __dtfl_cb_iter(&sp->produceds[i]);
		for (k = 0; k < n; k++) {
			const uint64_t *time = __dtfl_cb_key(&sp->produceds[i], k);
			int64_t delta = __dtfl_cb_delta(&sp->produceds[i], k);

			for (e = 0; e < c->edges[i].len; e++) {
				__dtfl_location t = *DTFL_VEC_AT(&c->edges[i], __dtfl_location, e);

				pointstamp_update(pointstamps, tc, t, time, delta);
				heap_push(&sg->temp_active, t.node);
			}
		}
		__dtfl_cb_clear(&sp->produceds[i]);
	}
}

/*
 * PerOperatorState::validate_progress (debug builds): increments to
 * internal capabilities or produced messages must be supported by a
 * consumed message or an existing capability at or before their time.
 */
#ifdef DTFL_DEBUG
static void
child_validate_progress(subgraph *sg, child_state *c)
{
	const dtfl_time_class *tc = sg->inner;
	__dtfl_shared_progress *sp = c->progress;
	size_t o, k, i, j;

	for (o = 0; o < c->outputs; o++) {
		__dtfl_change_batch *lists[2] = { &sp->internals[o], &sp->produceds[o] };
		const __dtfl_mutable_antichain *imp =
		    __dtfl_tracker_source_implications(sg->tracker, c->index, o);
		size_t l;

		for (l = 0; l < 2; l++) {
			size_t n = __dtfl_cb_iter(lists[l]);

			for (k = 0; k < n; k++) {
				const uint64_t *time = __dtfl_cb_key(lists[l], k);
				bool consumed = false;

				if (__dtfl_cb_delta(lists[l], k) <= 0)
					continue;
				for (i = 0; i < c->inputs && !consumed; i++) {
					size_t m = __dtfl_cb_iter(&sp->consumeds[i]);

					for (j = 0; j < m; j++)
						if (__dtfl_cb_delta(&sp->consumeds[i], j) > 0 &&
						    dtfl_time_less_equal(tc,
						    __dtfl_cb_key(&sp->consumeds[i], j), time))
							consumed = true;
				}
				if (!consumed && !__dtfl_mantichain_less_equal(imp, time))
					__dtfl_panic("Progress error; %s %s",
					    l == 0 ? "internal" : "produced", c->name);
			}
		}
	}
}
#endif

/* ------------------------------------------------------------------ */
/* Subgraph scheduling                                                 */
/* ------------------------------------------------------------------ */

static bool
activate_child(subgraph *sg, size_t index)
{
	child_state *c = &sg->children[index];
	bool incomplete = child_schedule(sg, c);

	if (incomplete != sg->incomplete[index]) {
		if (incomplete)
			sg->incomplete_count++;
		else
			sg->incomplete_count--;
		sg->incomplete[index] = incomplete;
	}
	if (!incomplete) {
		/* Shut down if neither input frontier nor capabilities remain. */
		if (__dtfl_tracker_node_frontiers_empty(sg->tracker, index) &&
		    __dtfl_tracker_node_no_pointstamps(sg->tracker, index))
			child_shut_down(sg, c);
	} else {
#ifdef DTFL_DEBUG
		child_validate_progress(sg, c);
#endif
	}
	child_extract_progress(sg, c,
	    c->local ? &sg->local_pointstamp : &sg->final_pointstamp);
	return incomplete;
}

/* accept_frontier: parent frontier changes become scope-input capabilities. */
static void
accept_frontier(subgraph *sg)
{
	size_t p, k, n;
	uint64_t inner[DTFL_TIME_MAX];

	for (p = 0; p < sg->inputs; p++) {
		__dtfl_change_batch *cb = &sg->shared_progress->frontiers[p];
		__dtfl_location source = { 0, p, true };

		n = __dtfl_cb_iter(cb);
		for (k = 0; k < n; k++) {
			__dtfl_time_to_inner(sg->inner, sg->outer, __dtfl_cb_key(cb, k), inner);
			__dtfl_tracker_update(sg->tracker, source, inner, __dtfl_cb_delta(cb, k));
		}
		__dtfl_cb_clear(cb);
	}
}

/* harvest_inputs: records entering the scope move from scope input to targets. */
static void
harvest_inputs(subgraph *sg)
{
	size_t input, k, n, e;

	for (input = 0; input < sg->inputs; input++) {
		__dtfl_change_batch *cb = sg->input_messages[input];
		__dtfl_location source = { 0, input, true };
		const __dtfl_vec *edges = &sg->children[0].edges[input];

		n = __dtfl_cb_iter(cb);
		for (k = 0; k < n; k++) {
			const uint64_t *time = __dtfl_cb_key(cb, k);
			int64_t delta = __dtfl_cb_delta(cb, k);

			for (e = 0; e < edges->len; e++)
				pointstamp_update(&sg->local_pointstamp, sg->inner,
				    *DTFL_VEC_AT(edges, __dtfl_location, e), time, delta);
			pointstamp_update(&sg->local_pointstamp, sg->inner, source, time,
			    -delta);
		}
		__dtfl_cb_clear(cb);
	}
}

static void
push_maybe_shutdown(subgraph *sg, size_t node)
{
	__dtfl_vec_push(&sg->maybe_shutdown, &node);
}

static int
size_cmp(const void *a, const void *b)
{
	size_t x = *(const size_t *)a, y = *(const size_t *)b;

	return x < y ? -1 : x > y;
}

/* propagate_pointstamps: commit exchanged pointstamps and propagate. */
static void
propagate_pointstamps(subgraph *sg)
{
	const dtfl_time_class *in = sg->inner;
	__dtfl_change_batch *pushed;
	size_t n, k, o;
	uint64_t outer[DTFL_TIME_MAX];

	n = __dtfl_cb_iter(&sg->final_pointstamp);
	for (k = 0; k < n; k++) {
		const uint64_t *key = __dtfl_cb_key(&sg->final_pointstamp, k);
		int64_t delta = __dtfl_cb_delta(&sg->final_pointstamp, k);
		__dtfl_location loc = __dtfl_location_from_key(key);

		if (loc.node == 0) {
			__dtfl_time_to_outer(in, sg->outer, key + 2, outer);
			if (loc.source)
				/* Child 0's capabilities are consumed messages (re-negated). */
				__dtfl_cb_update(&sg->shared_progress->consumeds[loc.port],
				    outer, -delta);
			else
				/* Child 0's input messages are produced messages. */
				__dtfl_cb_update(&sg->shared_progress->produceds[loc.port],
				    outer, delta);
		} else {
			__dtfl_tracker_update(sg->tracker, loc, key + 2, delta);
		}
	}
	__dtfl_cb_clear(&sg->final_pointstamp);

	__dtfl_tracker_propagate_all(sg->tracker);

	pushed = __dtfl_tracker_pushed(sg->tracker);
	n = __dtfl_cb_iter(pushed);
	for (k = 0; k < n; k++) {
		const uint64_t *key = __dtfl_cb_key(pushed, k);
		int64_t diff = __dtfl_cb_delta(pushed, k);
		__dtfl_location loc = __dtfl_location_from_key(key);
		child_state *c = &sg->children[loc.node];
		bool activate = false;

		push_maybe_shutdown(sg, loc.node);
		if (loc.source)
			continue;
		switch (c->notify[loc.port]) {
		case DTFL_INTEREST_ALWAYS:
			activate = true;
			break;
		case DTFL_INTEREST_IF_CAPABILITY:
			activate = __dtfl_tracker_node_cap_counts(sg->tracker, loc.node) > 0;
			break;
		case DTFL_INTEREST_NEVER:
			break;
		}
		if (activate)
			heap_push(&sg->temp_active, loc.node);
		__dtfl_cb_update(&c->progress->frontiers[loc.port], key + 2, diff);
	}
	__dtfl_cb_clear(pushed);

	/* Consider each recipient of progress for shutdown. */
	if (sg->maybe_shutdown.len > 1)
		qsort(sg->maybe_shutdown.data, sg->maybe_shutdown.len, sizeof(size_t),
		    size_cmp);
	for (k = 0; k < sg->maybe_shutdown.len; k++) {
		size_t idx = *DTFL_VEC_AT(&sg->maybe_shutdown, size_t, k);

		if (k > 0 && idx == *DTFL_VEC_AT(&sg->maybe_shutdown, size_t, k - 1))
			continue;
		if (__dtfl_tracker_node_frontiers_empty(sg->tracker, idx) &&
		    __dtfl_tracker_node_cap_counts(sg->tracker, idx) == 0)
			heap_push(&sg->temp_active, idx);
	}
	__dtfl_vec_clear(&sg->maybe_shutdown);

	/* Child zero frontier changes become internal capability changes. */
	for (o = 0; o < sg->outputs; o++) {
		__dtfl_change_batch *out = __dtfl_tracker_pushed_output(sg->tracker, o);
		__dtfl_change_batch tmp;
		size_t m, j;

		__dtfl_cb_init(&tmp, sg->outer->depth);
		m = __dtfl_cb_iter(out);
		for (j = 0; j < m; j++) {
			__dtfl_time_to_outer(in, sg->outer, __dtfl_cb_key(out, j), outer);
			__dtfl_cb_update(&tmp, outer, __dtfl_cb_delta(out, j));
		}
		__dtfl_cb_clear(out);
		/* filter_through(output_capabilities[o]) */
		__dtfl_mantichain_update_batch(&sg->output_capabilities[o], &tmp);
		m = __dtfl_cb_iter(&sg->output_capabilities[o].changes);
		for (j = 0; j < m; j++)
			__dtfl_cb_update(&sg->shared_progress->internals[o],
			    __dtfl_cb_key(&sg->output_capabilities[o].changes, j),
			    __dtfl_cb_delta(&sg->output_capabilities[o].changes, j));
		__dtfl_cb_clear(&sg->output_capabilities[o].changes);
		__dtfl_cb_free(&tmp);
	}
}

/*
 * send_progress: in eager mode, or if any local update is a globally
 * visible retraction or a receipt of an inbound message, broadcast.
 */
static void
send_progress(subgraph *sg)
{
	bool must = sg->progress_mode == DTFL_PROGRESS_EAGER;
	size_t n, k;

	if (!must) {
		n = __dtfl_cb_iter(&sg->local_pointstamp);
		for (k = 0; k < n && !must; k++) {
			const uint64_t *key = __dtfl_cb_key(&sg->local_pointstamp, k);
			__dtfl_location loc = __dtfl_location_from_key(key);
			int64_t diff = __dtfl_cb_delta(&sg->local_pointstamp, k);

			if ((__dtfl_tracker_is_global(sg->tracker, loc, key + 2) && diff < 0) ||
			    loc.node == 0)
				must = true;
		}
	}
	if (must)
		__dtfl_progcaster_send(sg->progcaster, &sg->local_pointstamp);
}

static void
push_active(void *ctx, size_t ext)
{
	heap_push(&((subgraph *)ctx)->temp_active, ext);
}

/* Schedule::schedule for the subgraph. */
static bool
subgraph_schedule(__dtfl_schedule *s)
{
	subgraph *sg = (subgraph *)(void *)((char *)s - offsetof(subgraph, sched));
	size_t previous = 0;

	accept_frontier(sg);
	harvest_inputs(sg);
	__dtfl_progcaster_recv(sg->progcaster, &sg->final_pointstamp);
	propagate_pointstamps(sg);
	__dtfl_activations_for_extensions(&sg->worker->activations, sg->sched.path,
	    sg->sched.path_len, push_active, sg);
	while (sg->temp_active.len > 0) {
		size_t index = heap_pop(&sg->temp_active);

		/* De-duplicate, and don't revisit. */
		if (index > previous) {
			activate_child(sg, index);
			previous = index;
		}
	}
	send_progress(sg);
	if (!__dtfl_cb_is_empty(&sg->final_pointstamp))
		__dtfl_activations_activate(&sg->worker->activations, sg->sched.path,
		    sg->sched.path_len);
	return sg->incomplete_count > 0 ||
	    __dtfl_tracker_tracking_anything(sg->tracker);
}

static void
subgraph_destroy(subgraph *sg)
{
	size_t i;

	for (i = 0; i < sg->nchildren; i++)
		child_free(sg, &sg->children[i]);
	__dtfl_free(sg->children);
	__dtfl_free(sg->incomplete);
	__dtfl_vec_free(&sg->temp_active);
	__dtfl_vec_free(&sg->maybe_shutdown);
	/*
	 * The scope-input counters (enter operators' produced batches) are
	 * owned here: the enter nub lives in the parent scope and may be
	 * freed before this subgraph stops reading them in harvest_inputs.
	 */
	for (i = 0; i < sg->inputs; i++) {
		__dtfl_cb_free(sg->input_messages[i]);
		__dtfl_free(sg->input_messages[i]);
	}
	__dtfl_free(sg->input_messages);
	for (i = 0; i < sg->outputs; i++)
		__dtfl_mantichain_free(&sg->output_capabilities[i]);
	__dtfl_free(sg->output_capabilities);
	__dtfl_cb_free(&sg->local_pointstamp);
	__dtfl_cb_free(&sg->final_pointstamp);
	__dtfl_tracker_free(sg->tracker);
	__dtfl_progcaster_free(sg->progcaster);
	if (!sg->initialized)
		__dtfl_shared_progress_free(sg->shared_progress);
	__dtfl_connectivity_free(&sg->scope_summary);
	__dtfl_free(sg->notify_me);
	__dtfl_free(sg->sched.name);
	__dtfl_free(sg->sched.path);
	dtfl_time_class_free(sg->outer);
	dtfl_time_class_free(sg->inner);
	__dtfl_free(sg);
}

static void
sched_destroy(__dtfl_schedule *s)
{
	subgraph_destroy((subgraph *)(void *)((char *)s - offsetof(subgraph, sched)));
}

static const __dtfl_schedule_ops subgraph_sched_ops = {
	subgraph_schedule, sched_destroy
};

/* ------------------------------------------------------------------ */
/* Operate                                                             */
/* ------------------------------------------------------------------ */

static bool
sg_local(const __dtfl_operate *op)
{
	(void)op;
	return false;
}

static size_t
sg_inputs(const __dtfl_operate *op)
{
	return ((const subgraph *)op)->inputs;
}

static size_t
sg_outputs(const __dtfl_operate *op)
{
	return ((const subgraph *)op)->outputs;
}

static const __dtfl_frontier_interest *
sg_notify_me(const __dtfl_operate *op)
{
	return ((const subgraph *)op)->notify_me;
}

/*
 * Operate::initialize: summarizes inner connectivity as outer, injects
 * the children's initial capabilities, propagates them to the scope's
 * outputs, and returns the subgraph as its own schedule.
 */
static __dtfl_schedule *
sg_initialize(__dtfl_operate *op, __dtfl_connectivity *conn,
    __dtfl_shared_progress **progress)
{
	subgraph *sg = (subgraph *)op;
	size_t i, p, e, k;
	uint64_t outer_s[DTFL_TIME_MAX + 1];

	DTFL_ASSERT(sg->children[0].outputs == sg->inputs);
	DTFL_ASSERT(sg->children[0].inputs == sg->outputs);
	__dtfl_connectivity_init(conn, sg->outer, sg->inputs);
	for (i = 0; i < sg->scope_summary.len && i < sg->inputs; i++) {
		const __dtfl_port_conn *pc = &sg->scope_summary.ports[i];

		for (e = 0; e < pc->entries.len; e++) {
			const __dtfl_port_conn_entry *ent =
			    DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, e);
			size_t sw = DTFL_MAX(sg->inner->sdepth, 1u);
			size_t ns = ent->summaries.len / sw;

			for (k = 0; k < ns; k++) {
				__dtfl_summary_to_outer(sg->inner, sg->outer,
				    (const uint64_t *)(void *)ent->summaries.data + k * sw, outer_s);
				__dtfl_port_conn_insert(&conn->ports[i], ent->port, outer_s);
			}
		}
	}
	for (p = 0; p < sg->nchildren; p++)
		child_extract_progress(sg, &sg->children[p], &sg->final_pointstamp);
	propagate_pointstamps(sg);
	/*
	 * Ownership of the shared progress passes to the parent (it frees it
	 * with the child state); the subgraph keeps using the pointer, which
	 * stays valid because the parent frees it only after destroying this
	 * schedule (child_free).
	 */
	sg->initialized = true;
	*progress = sg->shared_progress;
	return &sg->sched;
}

static void
sg_destroy(__dtfl_operate *op)
{
	subgraph_destroy((subgraph *)op);
}

static const __dtfl_operate_ops subgraph_operate_ops = {
	sg_local, sg_inputs, sg_outputs, sg_notify_me, sg_initialize, sg_destroy
};

static int
pending_cmp(const void *a, const void *b)
{
	size_t x = ((const pending_child *)a)->index, y = ((const pending_child *)b)->index;

	return x < y ? -1 : x > y;
}

/*
 * SubgraphBuilder::build.  Consumes the builder.  The subgraph's shared
 * progress is handed to the parent at initialize(); until then the
 * subgraph owns it.
 */
__dtfl_operate *
__dtfl_subgraph_builder_build(__dtfl_subgraph_builder *b, struct __dtfl_worker *w)
{
	subgraph *sg = __dtfl_calloc(1, sizeof *sg);
	__dtfl_reach_builder *rb;
	__dtfl_logger *timely = __dtfl_worker_logger(w, "timely");
	bool logging = timely != NULL;
	size_t i, k;
	__dtfl_frontier_interest max_interest = DTFL_INTEREST_NEVER;

	__dtfl_logger_release(timely);
	sg->operate.ops = &subgraph_operate_ops;
	sg->sched.ops = &subgraph_sched_ops;
	sg->sched.name = b->name;
	sg->sched.path = b->path;
	sg->sched.path_len = b->path_len;
	b->name = NULL;
	b->path = NULL;
	sg->worker = w;
	sg->outer = b->outer;
	sg->inner = b->inner;
	b->outer = b->inner = NULL;
	sg->inputs = b->input_messages.len;
	sg->outputs = b->outputs;

	/* Sort children by index, with child zero mirroring the scope shape. */
	if (b->children.len > 1)
		qsort(b->children.data, b->children.len, sizeof(pending_child), pending_cmp);
	sg->nchildren = b->children.len + 1;
	sg->children = __dtfl_calloc(sg->nchildren, sizeof(child_state));
	child_init_empty(&sg->children[0], sg->inner, sg->outputs, sg->inputs);
	for (i = 0; i < b->children.len; i++) {
		pending_child *pc = DTFL_VEC_AT(&b->children, pending_child, i);
		child_state *c = &sg->children[i + 1];

		child_init(c, pc->op, pc->index, pc->identifier, logging);
		if (logging) {
			size_t *addr = __dtfl_alloc((sg->sched.path_len + 1) * sizeof(size_t));

			memcpy(addr, sg->sched.path, sg->sched.path_len * sizeof(size_t));
			addr[sg->sched.path_len] = pc->index;
			__dtfl_worker_log_operates(w, pc->identifier, addr,
			    sg->sched.path_len + 1, c->name);
			__dtfl_free(addr);
		}
	}
	__dtfl_vec_clear(&b->children);
	for (i = 0; i < sg->nchildren; i++)
		if (sg->children[i].index != i)
			__dtfl_panic("subgraph %s: child %zu has index %zu", sg->sched.name,
			    i, sg->children[i].index);

	rb = __dtfl_reach_builder_new(sg->inner);
	{
		__dtfl_connectivity c0;

		__dtfl_connectivity_init(&c0, sg->inner, sg->outputs);
		__dtfl_reach_builder_add_node(rb, 0, sg->outputs, sg->inputs, &c0);
	}
	for (i = 1; i < sg->nchildren; i++) {
		child_state *c = &sg->children[i];

		__dtfl_reach_builder_add_node(rb, i, c->inputs, c->outputs, &c->summary);
	}
	for (k = 0; k < b->edge_stash.len; k++) {
		const __dtfl_location *pair = __dtfl_vec_at(&b->edge_stash, k);

		__dtfl_vec_push(&sg->children[pair[0].node].edges[pair[0].port], &pair[1]);
		__dtfl_reach_builder_add_edge(rb, pair[0], pair[1]);
	}
	sg->tracker = __dtfl_reach_builder_build(rb, &sg->scope_summary);
	sg->progcaster = __dtfl_progcaster_new(w, sg->sched.path, sg->sched.path_len,
	    b->identifier, sg->inner);

	sg->incomplete = __dtfl_calloc(sg->nchildren, sizeof(bool));
	for (i = 1; i < sg->nchildren; i++)
		sg->incomplete[i] = true;
	sg->incomplete_count = sg->nchildren - 1;
	__dtfl_activations_activate(&w->activations, sg->sched.path, sg->sched.path_len);

	for (i = 0; i < sg->nchildren; i++)
		for (k = 0; k < sg->children[i].inputs; k++)
			if (sg->children[i].notify[k] > max_interest)
				max_interest = sg->children[i].notify[k];
	sg->notify_me = __dtfl_calloc(DTFL_MAX(sg->inputs, 1), sizeof *sg->notify_me);
	for (i = 0; i < sg->inputs; i++)
		sg->notify_me[i] = max_interest;

	__dtfl_vec_init(&sg->temp_active, sizeof(size_t));
	__dtfl_vec_init(&sg->maybe_shutdown, sizeof(size_t));
	sg->input_messages = __dtfl_calloc(DTFL_MAX(sg->inputs, 1),
	    sizeof(__dtfl_change_batch *));
	for (i = 0; i < sg->inputs; i++)
		sg->input_messages[i] = *DTFL_VEC_AT(&b->input_messages,
		    __dtfl_change_batch *, i);
	sg->output_capabilities = __dtfl_calloc(DTFL_MAX(sg->outputs, 1),
	    sizeof(__dtfl_mutable_antichain));
	for (i = 0; i < sg->outputs; i++)
		__dtfl_mantichain_init(&sg->output_capabilities[i], sg->outer);
	__dtfl_cb_init(&sg->local_pointstamp, 2 + sg->inner->depth);
	__dtfl_cb_init(&sg->final_pointstamp, 2 + sg->inner->depth);
	sg->shared_progress = __dtfl_shared_progress_new(sg->outer, sg->inputs,
	    sg->outputs);
	sg->progress_mode = w->progress_mode;
	__dtfl_subgraph_builder_free(b);
	return &sg->operate;
}
