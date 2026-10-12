/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_branch.c: branch and branch_when.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/vec/branch.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * branch splits a stream into two by a per-record predicate over the
 * record and its time: records for which the predicate holds go to the
 * first output (`yes`), the rest to the second (`no`).  branch_when
 * splits whole messages by a predicate over the time alone.  Both are
 * pipeline operators with one input and two outputs that need no
 * frontier information (notify Never), each output carrying the input's
 * record type.
 *
 * Upstream sends the predicate-true records to the *second* returned
 * stream; here the two outputs are named `yes` (predicate true) and `no`
 * (predicate false) through pointer-out parameters, so `yes` is
 * upstream's second stream.  Public at the pin, removed upstream later
 * (#818).
 */
#include "timely/ops_int.h"

typedef struct branch_ctx {
	bool when;		/* branch_when (by time) vs. branch (by record) */
	dtfl_tpred_fn pred;	/* branch: predicate over (time, record) */
	bool (*wpred)(void *ctx, const uint64_t *time);	/* branch_when */
	void *user;
	void (*user_free)(void *);
} branch_ctx;

static bool
branch_logic(dtfl_op *op, void *vctx)
{
	branch_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));	/* output 0: yes */
	dtfl_cap_drop(dtfl_op_initial_cap(op, 1));	/* output 1: no */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cy = dtfl_op_input_cap(op, 0, 0);
		dtfl_cap *cn = dtfl_op_input_cap(op, 0, 1);
		const uint64_t *t = dtfl_message_time(m);

		if (c->when) {
			/* Whole message by the time predicate. */
			bool yes = c->wpred(c->user, t);
			dtfl_session *s = dtfl_op_session(op, yes ? 0 : 1,
			    yes ? cy : cn);

			dtfl_session_give_message(s, m);
			dtfl_session_close(s);
		} else {
			dtfl_session *sy = dtfl_op_session(op, 0, cy);
			dtfl_session *sn = dtfl_op_session(op, 1, cn);
			size_t i, n = dtfl_message_len(m);

			for (i = 0; i < n; i++) {
				size_t len;
				const void *rec = dtfl_message_record(m, i, &len);

				if (c->pred(c->user, t, rec, len))
					dtfl_session_give(sy, rec, len);
				else
					dtfl_session_give(sn, rec, len);
			}
			dtfl_session_close(sy);
			dtfl_session_close(sn);
		}
		dtfl_cap_drop(cy);
		dtfl_cap_drop(cn);
	}
	return false;
}

static void
branch_free(void *vctx)
{
	branch_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

static branch_ctx *
branch_build(dtfl_stream *st, branch_ctx *c, dtfl_stream **yes,
    dtfl_stream **no)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st), "Branch");
	const dtfl_type *type = dtfl_stream_type(st);

	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	*yes = dtfl_op_builder_output(b, type);
	*no = dtfl_op_builder_output(b, type);
	dtfl_op_builder_build(b, branch_logic, c, branch_free);
	return c;
}

void
dtfl_stream_branch(dtfl_stream *st, dtfl_tpred_fn pred, dtfl_ctx ctx,
    dtfl_stream **yes, dtfl_stream **no)
{
	branch_ctx *c = __dtfl_calloc(1, sizeof *c);

	c->when = false;
	c->pred = pred;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	branch_build(st, c, yes, no);
}

void
dtfl_stream_branch_when(dtfl_stream *st,
    bool (*pred)(void *ctx, const uint64_t *time), dtfl_ctx ctx,
    dtfl_stream **yes, dtfl_stream **no)
{
	branch_ctx *c = __dtfl_calloc(1, sizeof *c);

	c->when = true;
	c->wpred = pred;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	branch_build(st, c, yes, no);
}
