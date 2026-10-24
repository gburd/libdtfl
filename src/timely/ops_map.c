/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_map.c: flat_map and map.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/map.rs
 *   timely/src/dataflow/operators/vec/map.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * flat_map is a pipeline unary operator: for each input message, open an
 * output session at the message's time and run the callback once per
 * record, emitting zero or more output records.  map is flat_map with
 * one output per input (map.rs: map calls flat_map with std::iter::once).
 */
#include "timely/ops_int.h"

/* ------------------------------------------------------------------ */
/* The record sink a producing callback writes into                   */
/* ------------------------------------------------------------------ */

void
dtfl_emit_record(dtfl_emit *e, const void *rec, size_t len)
{
	dtfl_session_give(e->s, rec, len);
}

void *
dtfl_emit_reserve(dtfl_emit *e, size_t len)
{
	return __dtfl_session_reserve(e->s, len);
}

/* ------------------------------------------------------------------ */
/* flat_map                                                            */
/* ------------------------------------------------------------------ */

typedef struct map_ctx {
	dtfl_map_fn fn;
	void *user;
	void (*user_free)(void *);
} map_ctx;

static bool
map_logic(dtfl_op *op, void *vctx)
{
	map_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		dtfl_emit emit = { s };
		const uint64_t *t = dtfl_message_time(m);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			c->fn(c->user, t, rec, len, &emit);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
map_free(void *vctx)
{
	map_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_flat_map(dtfl_stream *st, const dtfl_type *out_type, dtfl_map_fn fn,
    dtfl_ctx ctx)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st), "FlatMap");
	dtfl_stream *out;
	map_ctx *c = __dtfl_alloc(sizeof *c);

	c->fn = fn;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	/*
	 * Operator::unary (generic/operator.rs): frontier
	 * changes alone do not schedule.
	 */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	out = dtfl_op_builder_output(b, out_type);
	dtfl_op_builder_build(b, map_logic, c, map_free);
	return out;
}
