/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_filter.c: filter.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/filter.rs
 *   timely/src/dataflow/operators/vec/filter.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A pipeline unary operator: for each input message, open an output
 * session at the message's time and forward each record the predicate
 * accepts, dropping the rest.  The output type is the input's type.
 */
#include "timely/ops_int.h"

typedef struct filter_ctx {
	dtfl_pred_fn pred;
	void *user;
	void (*user_free)(void *);
} filter_ctx;

static bool
filter_logic(dtfl_op *op, void *vctx)
{
	filter_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			if (c->pred(c->user, rec, len))
				dtfl_session_give(s, rec, len);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
filter_free(void *vctx)
{
	filter_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_filter(dtfl_stream *st, dtfl_pred_fn fn, dtfl_ctx ctx)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st), "Filter");
	dtfl_stream *out;
	filter_ctx *c = __dtfl_alloc(sizeof *c);

	c->pred = fn;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	/* Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	out = dtfl_op_builder_output(b, dtfl_stream_type(st));
	dtfl_op_builder_build(b, filter_logic, c, filter_free);
	return out;
}
