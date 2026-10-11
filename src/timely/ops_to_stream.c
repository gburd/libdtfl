/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_to_stream.c: to_stream.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/to_stream.rs
 *   timely/src/dataflow/operators/vec/to_stream.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A source operator (no inputs, one output): on each schedule it pulls a
 * bounded chunk of records from the iterator at the minimum time and, if
 * the iterator is not yet exhausted, reactivates itself; once `next`
 * returns false it drops the capability so the output frontier can
 * advance.  The capability and activator are held across schedules in
 * the operator context (ToStreamBuilder's `source`).
 */
#include "timely/ops_int.h"

/* Records emitted per schedule, bounding the work of one activation. */
#define TO_STREAM_CHUNK 1024

typedef struct to_stream_ctx {
	dtfl_iter_fn next;
	void *user;
	void (*user_free)(void *);
	dtfl_cap *cap;			/* held until the iterator drains */
	dtfl_activator *act;		/* reschedules the operator */
	bool started;
	bool done;
} to_stream_ctx;

static bool
to_stream_logic(dtfl_op *op, void *vctx)
{
	to_stream_ctx *c = vctx;
	dtfl_session *s;
	dtfl_emit emit;
	size_t i;

	if (!c->started) {
		c->cap = dtfl_op_initial_cap(op, 0);
		c->act = dtfl_op_activator(op);
		c->started = true;
	}
	if (c->done)
		return false;

	s = dtfl_op_session(op, 0, c->cap);
	emit.s = s;
	for (i = 0; i < TO_STREAM_CHUNK; i++) {
		if (!c->next(c->user, &emit)) {
			c->done = true;
			break;
		}
	}
	dtfl_session_close(s);

	if (c->done) {
		dtfl_cap_drop(c->cap);
		c->cap = NULL;
	} else {
		dtfl_activator_activate(c->act);
	}
	return false;
}

static void
to_stream_free(void *vctx)
{
	to_stream_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	if (c->cap != NULL)
		dtfl_cap_drop(c->cap);
	if (c->act != NULL)
		dtfl_activator_free(c->act);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_to_stream(dtfl_scope *scope, const dtfl_type *type, dtfl_iter_fn next,
    dtfl_ctx ctx)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "ToStream");
	dtfl_stream *out;
	to_stream_ctx *c = __dtfl_calloc(1, sizeof *c);

	c->next = next;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	out = dtfl_op_builder_output(b, type);
	dtfl_op_builder_build(b, to_stream_logic, c, to_stream_free);
	return out;
}
