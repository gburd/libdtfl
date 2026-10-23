/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/push_progress.c: capability-validating output pusher.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/channels/pushers/progress.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * The output chain is a Progress pusher over a push Counter over a Tee
 * (pushers/mod.rs Output).  A session validates its capability against
 * the output's internal capability batch (an output is valid for a
 * capability if the capability's internal batch is this output's and the
 * summary to the port is the identity, which holds for every capability
 * minted for this output), opens a buffer at the capability's time, and
 * ships non-empty batches through the counter and tee.  A flush is sent
 * when the session closes.
 */
#include "timely/timely_int.h"

/*
 * progress.rs valid: the capability must be for this output's internal
 * batch and port.  Every dtfl_cap minted for an output carries that
 * output's internal batch and port, and InputCapability's summary to the
 * port is the identity, so an equality of (internal, port) is sufficient.
 */
bool
__dtfl_output_valid(const dtfl_output *o, const dtfl_cap *cap)
{
	return cap->internal == o->internal && cap->port == o->port;
}

/* give: ships `data` at `time` through the chain, if non-empty. */
void
__dtfl_output_give(dtfl_output *o, const uint64_t *time, __dtfl_batch *data)
{
	void *bundle;

	if (__dtfl_batch_len(data) == 0) {
		__dtfl_batch_release(data);
		return;
	}
	bundle = o->msg_pool != NULL ?
	    __dtfl_message_new_pooled(o->msg_pool, o->tc, time, data) :
	    __dtfl_message_new(o->tc, time, data);
	__dtfl_push_push(o->pushee, &bundle);
	/* The counter+tee take ownership; a recycled element is unexpected. */
	if (bundle != NULL) {
		dtfl_message *m = bundle;

		__dtfl_message_free(m);
	}
}

/* done: flushes the chain (ProgressSession drop). */
void
__dtfl_output_done(dtfl_output *o)
{
	__dtfl_push_done(o->pushee);
}
