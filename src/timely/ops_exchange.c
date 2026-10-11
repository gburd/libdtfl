/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_exchange.c: exchange.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/exchange.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A unary operator whose input uses an Exchange pact: each record is
 * routed to worker route(record) % peers (the record type's hash when
 * route is NULL), then forwarded unchanged at its time.  The output type
 * is the input's type.
 */
#include "timely/ops_int.h"

static bool
exchange_logic(dtfl_op *op, void *vctx)
{
	dtfl_message *m;

	(void)vctx;
	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);

		dtfl_session_give_message(s, m);
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

dtfl_stream *
dtfl_stream_exchange(dtfl_stream *st,
    uint64_t (*route)(const void *rec, size_t len, void *ctx), void *ctx)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st), "Exchange");
	dtfl_stream *out;

	dtfl_op_builder_input(b, st, dtfl_pact_exchange(route, ctx));
	out = dtfl_op_builder_output(b, dtfl_stream_type(st));
	dtfl_op_builder_build(b, exchange_logic, NULL, NULL);
	return out;
}
