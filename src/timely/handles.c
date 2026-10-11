/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/handles.c: operator logic handles and output sessions.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/generic/handles.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * The dtfl_op handle gives user logic the same reach the Rust borrowed
 * handles do: read the next input message (InputHandle::next), mint a
 * capability for the current message's time (InputCapability::retain /
 * delayed), inspect an input frontier, open an output session at a
 * capability's time (OutputHandle::session), and reach the operator's
 * activators.  The handle is valid only during the logic call.  A
 * session buffers records at the capability's time and flushes full
 * batches through the output's progress / counter / tee chain, flushing
 * and sending done when it closes (ProgressSession drop).
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

/* ------------------------------------------------------------------ */
/* Operator information                                                */
/* ------------------------------------------------------------------ */

dtfl_scope *
dtfl_op_scope(const dtfl_op *op)
{
	return op->scope;
}

size_t
dtfl_op_inputs(const dtfl_op *op)
{
	return op->inputs;
}

size_t
dtfl_op_outputs(const dtfl_op *op)
{
	return op->outputs;
}

dtfl_cap *
dtfl_op_initial_cap(dtfl_op *op, size_t output)
{
	dtfl_cap *c;

	if (!op->first_call || output >= op->outputs)
		return NULL;
	c = op->initial_caps[output];
	op->initial_caps[output] = NULL;	/* taken: ownership to the caller */
	return c;
}

/* ------------------------------------------------------------------ */
/* Inputs                                                              */
/* ------------------------------------------------------------------ */

dtfl_message *
dtfl_op_input_next(dtfl_op *op, size_t input)
{
	dtfl_message *m;

	if (input >= op->inputs)
		return NULL;
	m = __dtfl_pull_counter_next(op->inputs_pull[input]);
	if (m != NULL) {
		__dtfl_time_copy(op->tc, op->cur_time, m->time);
		op->cur_input = input;
		op->have_cur = true;
	}
	return m;
}

/*
 * delayed for an output port (InputCapability::delayed/retain): mints a
 * capability on the output's internal batch if some summary from this
 * input to the output, applied to the current message's time, results in
 * a time less or equal to `t`.  retain uses the message's own time.
 */
static dtfl_cap *
input_cap_delayed(dtfl_op *op, size_t input, size_t output, const uint64_t *t)
{
	const uint64_t *summaries;
	size_t ns, k;
	unsigned sw;
	uint64_t result[DTFL_TIME_MAX];

	if (input >= op->inputs || output >= op->outputs)
		return NULL;
	if (!op->have_cur)
		__dtfl_panic("input capability requested with no current message");
	summaries = __dtfl_port_conn_get(&op->summaries->ports[input], output, &ns);
	if (summaries == NULL)
		__dtfl_panic("Attempted to delay a capability for a disconnected output");
	sw = DTFL_MAX(op->tc->sdepth, 1u);
	for (k = 0; k < ns; k++) {
		if (dtfl_summary_results_in(op->tc, summaries + (size_t)k * sw,
		    op->cur_time, result) &&
		    dtfl_time_less_equal(op->tc, result, t))
			return __dtfl_cap_new(op->tc, t, op->internals[output], output);
	}
	__dtfl_panic("Attempted to delay to a time not greater or equal to the "
	    "operator's input-output summary applied to the capability's time");
}

dtfl_cap *
dtfl_op_input_cap(dtfl_op *op, size_t input, size_t output)
{
	return input_cap_delayed(op, input, output, op->cur_time);
}

dtfl_cap *
dtfl_op_input_cap_delayed(dtfl_op *op, size_t input, size_t output,
    const uint64_t *t)
{
	return input_cap_delayed(op, input, output, t);
}

/* ------------------------------------------------------------------ */
/* Frontiers                                                           */
/* ------------------------------------------------------------------ */

const uint64_t *
dtfl_op_frontier(const dtfl_op *op, size_t input, size_t *n)
{
	if (input >= op->inputs) {
		*n = 0;
		return NULL;
	}
	*n = __dtfl_mantichain_frontier_len(&op->frontier[input]);
	return __dtfl_mantichain_frontier(&op->frontier[input]);
}

bool
dtfl_op_frontier_less_equal(const dtfl_op *op, size_t input, const uint64_t *t)
{
	if (input >= op->inputs)
		return false;
	return __dtfl_mantichain_less_equal(&op->frontier[input], t);
}

bool
dtfl_op_frontier_less_than(const dtfl_op *op, size_t input, const uint64_t *t)
{
	if (input >= op->inputs)
		return false;
	return __dtfl_mantichain_less_than(&op->frontier[input], t);
}

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

struct dtfl_session {
	dtfl_output *out;		/* not owned */
	uint64_t time[DTFL_TIME_MAX];
	__dtfl_batch *buffer;		/* records at `time`; owned */
	size_t cap_bytes;		/* flush threshold */
};

dtfl_session *
dtfl_op_session(dtfl_op *op, size_t output, const dtfl_cap *cap)
{
	dtfl_session *s;
	dtfl_output *o;

	if (output >= op->outputs)
		__dtfl_panic("session for invalid output port %zu", output);
	o = &op->outputs_arr[output];
	if (!__dtfl_output_valid(o, cap))
		__dtfl_panic("Attempted to open output session with invalid capability");
	s = __dtfl_alloc(sizeof *s);
	s->out = o;
	__dtfl_time_copy(o->tc, s->time, dtfl_cap_time(cap));
	s->buffer = __dtfl_batch_new(0, 0);
	s->cap_bytes = DTFL_BUFFER_SIZE_BYTES;
	return s;
}

static void
session_flush(dtfl_session *s)
{
	if (__dtfl_batch_len(s->buffer) == 0)
		return;
	__dtfl_output_give(s->out, s->time, s->buffer);	/* consumes buffer */
	s->buffer = __dtfl_batch_new(0, 0);
}

void
dtfl_session_give(dtfl_session *s, const void *rec, size_t len)
{
	__dtfl_batch_push(s->buffer, rec, len, NULL, NULL);
	if (__dtfl_batch_bytes(s->buffer) >= s->cap_bytes)
		session_flush(s);
}

void *
__dtfl_session_reserve(dtfl_session *s, size_t len)
{
	/* Flush first so the returned record stays in a stable buffer. */
	if (__dtfl_batch_bytes(s->buffer) >= s->cap_bytes)
		session_flush(s);
	return __dtfl_batch_push_uninit(s->buffer, len, NULL, NULL);
}

void
dtfl_session_give_message(dtfl_session *s, const dtfl_message *m)
{
	size_t i, n = __dtfl_batch_len(m->data);

	for (i = 0; i < n; i++) {
		size_t len;
		const void *rec = __dtfl_batch_rec(m->data, i, &len);

		dtfl_session_give(s, rec, len);
	}
}

void
dtfl_session_close(dtfl_session *s)
{
	session_flush(s);
	__dtfl_batch_release(s->buffer);
	__dtfl_output_done(s->out);	/* ProgressSession drop: flush the chain */
	__dtfl_free(s);
}

/* ------------------------------------------------------------------ */
/* Activators                                                          */
/* ------------------------------------------------------------------ */

dtfl_activator *
dtfl_op_activator(dtfl_op *op)
{
	return __dtfl_activator_new(op->worker, op->address, op->address_len);
}

dtfl_sync_activator *
dtfl_op_sync_activator(dtfl_op *op)
{
	return __dtfl_sync_activator_new(op->worker, op->address, op->address_len);
}
