/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/operator.c: the generic unary_notify and binary_notify
 * operator entry points.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/generic/operator.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * unary_notify and binary_notify build a one- or two-input, one-output
 * operator whose logic receives the operator handle and a Notificator.
 * The notificator persists across schedules, holding the capabilities
 * requested with notify_at until the input frontiers pass their times
 * (operator.rs builds these over unary_frontier / binary_frontier with a
 * FrontierNotificator seeded from `init`).  The initial capability is
 * delayed to each `init` time and requested before the first call.
 *
 * These are public at the pin and were removed upstream later (#819);
 * the port keeps the pinned behavior.  The other generic combinators
 * (unary, unary_frontier, binary, source, sink) are the record
 * operators in ops_*.c, built directly on dtfl_op_builder (timely.h).
 */
#include "timely/ops_notify_int.h"
#include "progress/time_int.h"

#include <string.h>

typedef struct notify_ctx {
	void *user;			/* user logic context */
	void (*user_free)(void *);
	bool unary;			/* one input vs. two */
	bool seeded;			/* initial notify_at done */
	uint64_t *init;			/* ninit times, depth words each; owned */
	size_t ninit;
	union {
		dtfl_unary_notify_fn un;
		dtfl_binary_notify_fn bin;
	} logic;
	__dtfl_fnotificator note;	/* persists across schedules */
	dtfl_notificator pub;		/* public handle; bound each schedule */
} notify_ctx;

/*
 * Seeds the notificator from `init`: delay the operator's initial
 * capability (output 0) to each init time and request a notification,
 * then drop the initial capability.  Done once, on the first schedule.
 */
static void
notify_seed(notify_ctx *c, dtfl_op *op)
{
	dtfl_cap *cap0 = dtfl_op_initial_cap(op, 0);
	size_t i;
	unsigned depth = DTFL_MAX(c->note.tc->depth, 1u);

	for (i = 0; i < c->ninit; i++)
		__dtfl_fnote_notify_at(&c->note,
		    dtfl_cap_delayed(cap0, c->init + i * depth));
	if (cap0 != NULL)
		dtfl_cap_drop(cap0);
	c->seeded = true;
}

static bool
notify_logic(dtfl_op *op, void *vctx)
{
	notify_ctx *c = vctx;

	if (!c->seeded)
		notify_seed(c, op);

	__dtfl_fnote_bind(&c->note, op->frontier);
	c->pub.inner = &c->note;
	if (c->unary)
		c->logic.un(c->user, op, &c->pub);
	else
		c->logic.bin(c->user, op, &c->pub);
	return false;
}

static void
notify_free(void *vctx)
{
	notify_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_fnote_free(&c->note);
	__dtfl_free(c->init);
	__dtfl_free(c);
}

static notify_ctx *
notify_ctx_new(const dtfl_time_class *tc, size_t inputs, const uint64_t *init,
    size_t ninit, void *user, void (*user_free)(void *))
{
	notify_ctx *c = __dtfl_calloc(1, sizeof *c);
	unsigned depth = DTFL_MAX(tc->depth, 1u);

	c->user = user;
	c->user_free = user_free;
	c->unary = inputs == 1;
	c->ninit = ninit;
	if (ninit > 0) {
		c->init = __dtfl_alloc(ninit * depth * sizeof(uint64_t));
		memcpy(c->init, init, ninit * depth * sizeof(uint64_t));
	}
	__dtfl_fnote_init(&c->note, tc, inputs);
	return c;
}

dtfl_stream *
dtfl_stream_unary_notify(dtfl_stream *st, dtfl_pact pact, const char *name,
    const uint64_t *init_times, size_t ninit, const dtfl_type *out_type,
    dtfl_unary_notify_fn logic, dtfl_ctx ctx)
{
	dtfl_scope *scope = dtfl_stream_scope(st);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, name);
	dtfl_stream *out;
	notify_ctx *c = notify_ctx_new(tc, 1, init_times, ninit, ctx.ptr, ctx.free);

	c->logic.un = logic;
	dtfl_op_builder_input(b, st, pact);
	out = dtfl_op_builder_output(b, out_type);
	dtfl_op_builder_build(b, notify_logic, c, notify_free);
	return out;
}

dtfl_stream *
dtfl_stream_binary_notify(dtfl_stream *a, dtfl_stream *b_stream, dtfl_pact pa,
    dtfl_pact pb, const char *name, const uint64_t *init_times, size_t ninit,
    const dtfl_type *out_type, dtfl_binary_notify_fn logic, dtfl_ctx ctx)
{
	dtfl_scope *scope = dtfl_stream_scope(a);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, name);
	dtfl_stream *out;
	notify_ctx *c = notify_ctx_new(tc, 2, init_times, ninit, ctx.ptr, ctx.free);

	c->logic.bin = logic;
	dtfl_op_builder_input(b, a, pa);
	dtfl_op_builder_input(b, b_stream, pb);
	out = dtfl_op_builder_output(b, out_type);
	dtfl_op_builder_build(b, notify_logic, c, notify_free);
	return out;
}
