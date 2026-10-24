/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_concat.c: concat and concatenate.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/concat.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * concatenate builds an operator with one pipeline input per source and
 * a single output: on each schedule it plays out every input message at
 * its own time, merging the streams.  concat is concatenate over the two
 * given streams.  The output type is the sources' type (every source
 * must carry the same record type).
 */
#include "timely/ops_int.h"

static bool
concat_logic(dtfl_op *op, void *vctx)
{
	size_t inputs = dtfl_op_inputs(op);
	size_t i;

	(void)vctx;
	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	for (i = 0; i < inputs; i++) {
		dtfl_message *m;

		while ((m = dtfl_op_input_next(op, i)) != NULL) {
			dtfl_cap *cap = dtfl_op_input_cap(op, i, 0);
			dtfl_session *s = dtfl_op_session(op, 0, cap);

			dtfl_session_give_message(s, m);
			dtfl_session_close(s);
			dtfl_cap_drop(cap);
		}
	}
	return false;
}

dtfl_stream *
dtfl_stream_concatenate(dtfl_scope *scope, dtfl_stream *const *streams, size_t n)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Concatenate");
	dtfl_stream *out;
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_op_builder_input(b, streams[i], dtfl_pact_pipeline());
		dtfl_op_builder_set_notify(b, i, DTFL_NOTIFY_NEVER);
	}
	out = dtfl_op_builder_output(b, n > 0 ? dtfl_stream_type(streams[0]) : NULL);
	dtfl_op_builder_build(b, concat_logic, NULL, NULL);
	return out;
}

dtfl_stream *
dtfl_stream_concat(dtfl_stream *a, dtfl_stream *b)
{
	dtfl_stream *streams[2] = { a, b };

	return dtfl_stream_concatenate(dtfl_stream_scope(a), streams, 2);
}
