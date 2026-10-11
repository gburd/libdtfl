/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_partition.c: partition.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/partition.rs
 *   timely/src/dataflow/operators/vec/partition.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A unary operator with `parts` outputs: route(record) names the output
 * a record goes to, at the record's time.  Each output carries the
 * input's record type.  Upstream groups records per intended output
 * before shipping; here, one session per output per message serves the
 * same effect, since a session already buffers at its time.
 */
#include "timely/ops_int.h"

typedef struct partition_ctx {
	size_t parts;
	size_t (*route)(void *ctx, const void *rec, size_t len);
	void *user;
	void (*user_free)(void *);
} partition_ctx;

static bool
partition_logic(dtfl_op *op, void *vctx)
{
	partition_ctx *c = vctx;
	size_t parts = c->parts;
	dtfl_message *m;
	size_t o;

	for (o = 0; o < parts; o++)
		dtfl_cap_drop(dtfl_op_initial_cap(op, o));

	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap **caps = __dtfl_calloc(DTFL_MAX(parts, 1), sizeof *caps);
		dtfl_session **sess = __dtfl_calloc(DTFL_MAX(parts, 1), sizeof *sess);
		size_t i, n = dtfl_message_len(m);

		for (o = 0; o < parts; o++) {
			caps[o] = dtfl_op_input_cap(op, 0, o);
			sess[o] = dtfl_op_session(op, o, caps[o]);
		}
		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t part = c->route(c->user, rec, len);

			if (part < parts)
				dtfl_session_give(sess[part], rec, len);
		}
		for (o = 0; o < parts; o++) {
			dtfl_session_close(sess[o]);
			dtfl_cap_drop(caps[o]);
		}
		__dtfl_free(sess);
		__dtfl_free(caps);
	}
	return false;
}

static void
partition_free(void *vctx)
{
	partition_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

void
dtfl_stream_partition(dtfl_stream *st, size_t parts,
    size_t (*route)(void *ctx, const void *rec, size_t len), dtfl_ctx ctx,
    dtfl_stream **outs)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st), "Partition");
	const dtfl_type *type = dtfl_stream_type(st);
	partition_ctx *c = __dtfl_alloc(sizeof *c);
	size_t o;

	c->parts = parts;
	c->route = route;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	for (o = 0; o < parts; o++)
		outs[o] = dtfl_op_builder_output(b, type);
	dtfl_op_builder_build(b, partition_logic, c, partition_free);
}
