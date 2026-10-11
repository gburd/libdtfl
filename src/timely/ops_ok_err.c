/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_ok_err.c: ok_err.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/ok_err.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A unary operator with two outputs: for each input record the predicate
 * sends it to the first output (ok) when true and the second (err) when
 * false, each at the record's time.  Both outputs carry the input's
 * record type.  The input's frontier is not needed (notify Never).
 */
#include "timely/ops_int.h"

typedef struct ok_err_ctx {
	dtfl_pred_fn pred;
	void *user;
	void (*user_free)(void *);
} ok_err_ctx;

static bool
ok_err_logic(dtfl_op *op, void *vctx)
{
	ok_err_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	dtfl_cap_drop(dtfl_op_initial_cap(op, 1));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap0 = dtfl_op_input_cap(op, 0, 0);
		dtfl_cap *cap1 = dtfl_op_input_cap(op, 0, 1);
		dtfl_session *ok = dtfl_op_session(op, 0, cap0);
		dtfl_session *err = dtfl_op_session(op, 1, cap1);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			if (c->pred(c->user, rec, len))
				dtfl_session_give(ok, rec, len);
			else
				dtfl_session_give(err, rec, len);
		}
		dtfl_session_close(ok);
		dtfl_session_close(err);
		dtfl_cap_drop(cap0);
		dtfl_cap_drop(cap1);
	}
	return false;
}

static void
ok_err_free(void *vctx)
{
	ok_err_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

void
dtfl_stream_ok_err(dtfl_stream *st, dtfl_pred_fn pred, dtfl_ctx ctx,
    dtfl_stream **ok, dtfl_stream **err)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st), "OkErr");
	const dtfl_type *type = dtfl_stream_type(st);
	ok_err_ctx *c = __dtfl_alloc(sizeof *c);

	c->pred = pred;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	*ok = dtfl_op_builder_output(b, type);
	*err = dtfl_op_builder_output(b, type);
	dtfl_op_builder_build(b, ok_err_logic, c, ok_err_free);
}
