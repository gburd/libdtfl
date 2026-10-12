/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_enterleave.c: moving a stream across a scope boundary.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/enterleave.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Enter projects a stream of an outer scope into one of its direct child
 * scopes: an IngressNub re-times each message with Refines::to_inner and
 * pushes it into a Counter over the child's input Tee, whose produced
 * counts the subgraph registers as a scope input (new_input).  Leave
 * projects a stream of a child scope back to its parent: an EgressNub
 * re-times each message with to_outer and pushes it into the parent Tee
 * of a new scope output (new_output).  Neither nub is a scheduled
 * operator; both are pushers wired directly onto the boundary, so a
 * pass-through region adds no node to the graph (enterleave.rs).  The
 * boundary edge is recorded so progress crosses the scope correctly.
 *
 * The boundary pushers do not log MessagesEvent the way the pact's
 * LogPusher does for ordinary channels, so a diagnostics stream does not
 * see records crossing an enter or leave edge.  Add a logging wrapper
 * here if the diagnostics layer needs boundary message events.
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

#include "dtfl/timely_ops.h"

/* Asserts `inner` is a direct child of `outer` (same addr plus one). */
static void
assert_child(const dtfl_scope *inner, const dtfl_scope *outer, const char *who)
{
	size_t ilen, olen;
	const size_t *iaddr = dtfl_scope_addr(inner, &ilen);
	const size_t *oaddr = dtfl_scope_addr(outer, &olen);

	if (ilen != olen + 1 || memcmp(iaddr, oaddr, olen * sizeof(size_t)) != 0)
		__dtfl_panic("%s: scope is not a direct child of the other", who);
}

/* ------------------------------------------------------------------ */
/* IngressNub (enter)                                                  */
/* ------------------------------------------------------------------ */

typedef struct ingress_nub {
	__dtfl_push base;
	__dtfl_push *targets;		/* Counter over Tee; owned */
	const dtfl_time_class *outer;	/* retained */
	const dtfl_time_class *inner;	/* retained */
	__dtfl_worker *worker;
	size_t *addr;			/* child address; owned, for activate */
	size_t addr_len;
	dtfl_stream *stream;		/* the inner stream; owned, freed on teardown */
	bool active;
} ingress_nub;

static void
ingress_push(__dtfl_push *p, void **msg)
{
	ingress_nub *n = (ingress_nub *)p;

	if (*msg != NULL) {
		dtfl_message *outer_m = *msg;
		uint64_t inner_time[DTFL_TIME_MAX];
		dtfl_message *inner_m;

		__dtfl_time_to_inner(n->inner, n->outer, outer_m->time, inner_time);
		/* Share the batch (records are bytes, D2); re-time only. */
		inner_m = __dtfl_message_new(n->inner, inner_time,
		    __dtfl_batch_retain(outer_m->data));
		__dtfl_push_send(n->targets, __dtfl_message_class(), inner_m);
		n->active = true;
		/* The outer message is dropped; the caller frees it. */
	} else {
		if (n->active) {
			__dtfl_activations_activate(&n->worker->activations,
			    n->addr, n->addr_len);
			n->active = false;
		}
		__dtfl_push_done(n->targets);
	}
}

static void
ingress_destroy(__dtfl_push *p)
{
	ingress_nub *n = (ingress_nub *)p;

	__dtfl_push_destroy(n->targets);
	dtfl_time_class_free(n->outer);
	dtfl_time_class_free(n->inner);
	__dtfl_stream_free(n->stream);
	__dtfl_free(n->addr);
	__dtfl_free(n);
}

static const __dtfl_push_ops ingress_ops = { ingress_push, ingress_destroy };

dtfl_stream *
dtfl_stream_enter(dtfl_stream *st, dtfl_scope *child)
{
	dtfl_scope *outer_scope = dtfl_stream_scope(st);
	const dtfl_time_class *outer = dtfl_scope_time_class(outer_scope);
	const dtfl_time_class *inner = dtfl_scope_time_class(child);
	__dtfl_tee *tee = __dtfl_tee_new();
	__dtfl_change_batch *produced = __dtfl_alloc(sizeof *produced);
	ingress_nub *nub = __dtfl_alloc(sizeof *nub);
	size_t input_port, channel, caddr_len;
	const size_t *caddr = dtfl_scope_addr(child, &caddr_len);
	__dtfl_location target;
	dtfl_stream *inner_stream;
	__dtfl_location source;

	assert_child(child, outer_scope, "Enter::enter");

	/* The ingress counter's produced batch is the subgraph's new input. */
	__dtfl_cb_init(produced, inner->depth);
	input_port = __dtfl_subgraph_builder_new_input(child->builder, produced);

	nub->base.ops = &ingress_ops;
	nub->targets = __dtfl_push_counter_new(__dtfl_tee_pusher(tee), produced);
	nub->outer = dtfl_time_class_retain(outer);
	nub->inner = dtfl_time_class_retain(inner);
	nub->worker = child->worker;
	nub->addr = __dtfl_memdup(caddr, caddr_len * sizeof(size_t));
	nub->addr_len = caddr_len;
	nub->active = false;

	/*
	 * Connect the outer stream to the child's input (a Target at the
	 * child's own index in the parent) through the ingress nub.  The
	 * produced batch is now owned by the subgraph (new_input), which
	 * reads it in harvest_inputs and frees it on teardown; the nub only
	 * writes it through the counter during stepping.
	 */
	channel = __dtfl_worker_new_identifier(outer_scope->worker);
	target.node = __dtfl_subgraph_builder_index(child->builder);
	target.port = input_port;
	target.source = false;
	__dtfl_stream_connect_to(st, target, &nub->base, channel);

	/* The inner stream's source is the scope boundary (node 0). */
	source.node = 0;
	source.port = input_port;
	source.source = true;
	inner_stream = __dtfl_stream_new(child, source, tee, dtfl_stream_type(st));
	nub->stream = inner_stream;
	__dtfl_tee_release(tee);	/* stream and nub counter hold references */
	return inner_stream;
}

/* ------------------------------------------------------------------ */
/* EgressNub (leave)                                                   */
/* ------------------------------------------------------------------ */

typedef struct egress_nub {
	__dtfl_push base;
	__dtfl_push *targets;		/* Tee pusher into the parent; owned */
	const dtfl_time_class *inner;	/* retained */
	const dtfl_time_class *outer;	/* retained */
	dtfl_stream *stream;		/* the outer stream; owned, freed on teardown */
} egress_nub;

static void
egress_push(__dtfl_push *p, void **msg)
{
	egress_nub *n = (egress_nub *)p;

	if (*msg != NULL) {
		dtfl_message *inner_m = *msg;
		uint64_t outer_time[DTFL_TIME_MAX];
		dtfl_message *outer_m;

		__dtfl_time_to_outer(n->inner, n->outer, inner_m->time, outer_time);
		outer_m = __dtfl_message_new(n->outer, outer_time,
		    __dtfl_batch_retain(inner_m->data));
		__dtfl_push_send(n->targets, __dtfl_message_class(), outer_m);
	} else {
		__dtfl_push_done(n->targets);
	}
}

static void
egress_destroy(__dtfl_push *p)
{
	egress_nub *n = (egress_nub *)p;

	__dtfl_push_destroy(n->targets);
	dtfl_time_class_free(n->inner);
	dtfl_time_class_free(n->outer);
	__dtfl_stream_free(n->stream);
	__dtfl_free(n);
}

static const __dtfl_push_ops egress_ops = { egress_push, egress_destroy };

dtfl_stream *
dtfl_stream_leave(dtfl_stream *st)
{
	dtfl_scope *inner_scope = dtfl_stream_scope(st);
	dtfl_scope *outer_scope = dtfl_scope_parent(inner_scope);
	const dtfl_time_class *inner = dtfl_scope_time_class(inner_scope);
	const dtfl_time_class *outer;
	__dtfl_tee *tee;
	egress_nub *nub;
	size_t output_port, channel;
	__dtfl_location target, source;
	dtfl_stream *outer_stream;

	if (outer_scope == NULL)
		__dtfl_panic("Leave::leave: the stream's scope has no parent");
	outer = dtfl_scope_time_class(outer_scope);
	assert_child(inner_scope, outer_scope, "Leave::leave");

	/* new_output on the child; the source is at the child's index above. */
	output_port = __dtfl_subgraph_builder_new_output(inner_scope->builder);

	tee = __dtfl_tee_new();
	nub = __dtfl_alloc(sizeof *nub);
	nub->base.ops = &egress_ops;
	nub->targets = __dtfl_tee_pusher(tee);
	nub->inner = dtfl_time_class_retain(inner);
	nub->outer = dtfl_time_class_retain(outer);

	/*
	 * Connect the inner stream to the scope-output boundary target
	 * (node 0, output_port) inside the child, through the egress nub.
	 */
	channel = __dtfl_worker_new_identifier(inner_scope->worker);
	target.node = 0;
	target.port = output_port;
	target.source = false;
	__dtfl_stream_connect_to(st, target, &nub->base, channel);

	/* The outer stream's source is the child node's output in the parent. */
	source.node = __dtfl_subgraph_builder_index(inner_scope->builder);
	source.port = output_port;
	source.source = true;
	outer_stream = __dtfl_stream_new(outer_scope, source, tee,
	    dtfl_stream_type(st));
	nub->stream = outer_stream;
	__dtfl_tee_release(tee);	/* the stream and the nub pusher hold references */
	return outer_stream;
}
