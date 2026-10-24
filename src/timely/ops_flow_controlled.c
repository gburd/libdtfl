/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_flow_controlled.c: iterator_source, a flow-controlled
 * source.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/vec/flow_controlled.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * iterator_source repeatedly calls a user function to ingest input,
 * holding back while a probe is behind the frontier the last ingest
 * targeted.  The function chooses the time of each emitted batch (times
 * must be non-decreasing) and returns false to end the input.  On each
 * schedule the source ingests while the probe is not behind the current
 * target, downgrading its capability to each batch's time; when the
 * probe is behind the target it reactivates and waits.  When the
 * function ends, the capability is dropped so the output frontier can
 * advance (flow_controlled.rs).
 *
 * This is a `source` operator (no inputs, one output), built on the
 * generic builder.  It requires a totally ordered time.  Public at the
 * pin, removed upstream later (#818).
 */
#include "timely/ops_int.h"
#include "progress/time_int.h"

/* Batches ingested per schedule, bounding the work of one activation. */
#define FLOW_CHUNK 1024

typedef struct flow_ctx {
	const dtfl_time_class *tc;
	dtfl_timed_iter_fn next;
	void *user;
	void (*user_free)(void *);
	dtfl_probe *probe;		/* not owned; the caller frees it */
	dtfl_cap *cap;			/* held until the source ends */
	dtfl_activator *act;
	uint64_t target[DTFL_TIME_MAX];	/* probe must reach this to continue */
	bool started;
	bool done;
} flow_ctx;

static bool
flow_logic(dtfl_op *op, void *vctx)
{
	flow_ctx *c = vctx;
	size_t i;

	if (!c->started) {
		c->cap = dtfl_op_initial_cap(op, 0);
		c->act = dtfl_op_activator(op);
		dtfl_time_minimum(c->tc, c->target);
		c->started = true;
	}
	if (c->done)
		return false;

	/* Ingest while the probe is not behind the last target. */
	for (i = 0; i < FLOW_CHUNK; i++) {
		uint64_t t[DTFL_TIME_MAX];
		dtfl_session *s;
		dtfl_emit emit;

		if (dtfl_probe_less_than(c->probe, c->target))
			break;	/* probe is behind: hold back and wait */

		__dtfl_time_copy(c->tc, t, dtfl_cap_time(c->cap));
		s = dtfl_op_session(op, 0, c->cap);
		emit.s = s;
		if (!c->next(c->user, t, &emit)) {
			dtfl_session_close(s);
			c->done = true;
			break;
		}
		dtfl_session_close(s);

		/* Advance the capability to the chosen time (non-decreasing). */
		if (dtfl_time_cmp(c->tc, t, dtfl_cap_time(c->cap)) > 0)
			dtfl_cap_downgrade(c->cap, t);
		__dtfl_time_copy(c->tc, c->target, t);
	}

	if (c->done) {
		dtfl_cap_drop(c->cap);
		c->cap = NULL;
	} else {
		dtfl_activator_activate(c->act);
	}
	return false;
}

static void
flow_free(void *vctx)
{
	flow_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	if (c->cap != NULL)
		dtfl_cap_drop(c->cap);
	if (c->act != NULL)
		dtfl_activator_free(c->act);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_iterator_source(dtfl_scope *scope, const dtfl_type *type,
    dtfl_timed_iter_fn next, dtfl_probe *probe, dtfl_ctx ctx)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Source");
	dtfl_stream *out;
	flow_ctx *c = __dtfl_calloc(1, sizeof *c);

	c->tc = dtfl_scope_time_class(scope);
	c->next = next;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	c->probe = probe;
	out = dtfl_op_builder_output(b, type);
	dtfl_op_builder_build(b, flow_logic, c, flow_free);
	return out;
}
