/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_feedback.c: cycles in a dataflow graph.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/feedback.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * dtfl_feedback_new reserves a Feedback operator with one output and
 * returns its stream before its source exists; dtfl_feedback_connect
 * later binds the source as the operator's input, with a path summary
 * from that input to the output that advances every time by `summary`
 * (feedback.rs connect_loop).  The summary is what makes a loop sound:
 * reachability sees that data re-entering the cycle lands at a strictly
 * later time, so the frontier still advances and the loop terminates.
 * The input is marked FrontierInterest::Never, since the operator acts
 * only on data, not on frontier notifications.  The build is deferred
 * until connect so the input (which does not yet exist at feedback_new)
 * can be added with its summary.
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

#include "dtfl/timely_ops.h"

struct dtfl_feedback {
	dtfl_op_builder *builder;	/* owned until connect */
	dtfl_scope *scope;
	const dtfl_time_class *tc;
	dtfl_stream *stream;		/* the operator's output */
	uint64_t summary[DTFL_TIME_MAX];
};

dtfl_stream *
dtfl_feedback_new(dtfl_scope *scope, const dtfl_type *type,
    const uint64_t *summary, dtfl_feedback **handle)
{
	dtfl_feedback *h = __dtfl_alloc(sizeof *h);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	unsigned sw = DTFL_MAX(dtfl_summary_depth(tc), 1u);

	h->builder = dtfl_op_builder_new(scope, "Feedback");
	h->scope = scope;
	h->tc = tc;
	h->stream = dtfl_op_builder_output(h->builder, type);
	memcpy(h->summary, summary, (size_t)sw * sizeof(uint64_t));

	*handle = h;
	return h->stream;
}

/* The connected loop's logic: re-time each message by the summary. */
typedef struct feedback_ctx {
	const dtfl_time_class *tc;
	uint64_t summary[DTFL_TIME_MAX];
} feedback_ctx;

static bool
feedback_logic(dtfl_op *op, void *vctx)
{
	feedback_ctx *c = vctx;
	dtfl_message *m;

	/* Drop the operator's own initial capability on the first call. */
	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		uint64_t new_time[DTFL_TIME_MAX];

		/*
		 * summary.results_in(cap.time()): None (overflow at the loop
		 * coordinate's width) drops the message, which is how a bounded
		 * loop stops producing (feedback.rs connect_loop).
		 */
		if (dtfl_summary_results_in(c->tc, c->summary,
		    dtfl_message_time(m), new_time)) {
			dtfl_cap *cap = dtfl_op_input_cap_delayed(op, 0, 0, new_time);
			dtfl_session *s = dtfl_op_session(op, 0, cap);

			dtfl_session_give_message(s, m);
			dtfl_session_close(s);
			dtfl_cap_drop(cap);
		}
	}
	return false;
}

static void
feedback_ctx_free(void *vctx)
{
	__dtfl_free(vctx);
}

void
dtfl_feedback_connect(dtfl_feedback *handle, dtfl_stream *st)
{
	unsigned sw = DTFL_MAX(dtfl_summary_depth(handle->tc), 1u);
	const uint64_t *summaries[1];
	feedback_ctx *c;

	/*
	 * new_input_connection with the single summary (0 -> summary) from
	 * this input to output 0, then set_notify Never (connect_loop).
	 */
	summaries[0] = handle->summary;
	dtfl_op_builder_input_connection(handle->builder, st,
	    dtfl_pact_pipeline(), summaries);
	dtfl_op_builder_set_notify(handle->builder, 0, DTFL_NOTIFY_NEVER);

	c = __dtfl_alloc(sizeof *c);
	c->tc = handle->tc;
	memcpy(c->summary, handle->summary, (size_t)sw * sizeof(uint64_t));
	dtfl_op_builder_build(handle->builder, feedback_logic, c, feedback_ctx_free);

	__dtfl_free(handle);	/* the handle is consumed by connect_loop */
}
