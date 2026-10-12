/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/capture.c: capturing a stream for later replay.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/capture/capture.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Capture is a raw operator (builder_raw) with one input and no output.
 * It turns each received message into an Event::Messages and each
 * frontier change into an Event::Progress, pushing them to a user
 * EventPusher.  On its first schedule it discards the initial capability
 * it is reserved by subtracting one at the minimum time from the input
 * frontier batch, exactly as upstream, so progress does not stall.  Its
 * consumption of every message is reported back to the parent.
 */
#include "timely/capture_int.h"

#include "timely/scope_int.h"
#include "progress/operate_int.h"

typedef struct capture_op {
	dtfl_event_pusher pusher;
	void (*ctx_free)(void *);
	void *ctx_ptr;			/* dtfl_ctx.ptr; freed with ctx_free */
	const dtfl_time_class *tc;	/* not retained; the core retains it */
	__dtfl_pull_counter *pull;	/* owned */
	__dtfl_change_batch consumed;	/* fed by the pull counter */
	bool started;
} capture_op;

static bool
capture_logic(void *vctx, __dtfl_shared_progress *sp)
{
	capture_op *c = vctx;
	dtfl_message *m;
	unsigned d = c->tc->depth;

	if (!c->started) {
		uint64_t minimum[DTFL_TIME_MAX];

		dtfl_time_minimum(c->tc, minimum);
		__dtfl_cb_update(&sp->frontiers[0], minimum, -1);
		c->started = true;
	}

	/* Transmit any frontier progress as an Event::Progress. */
	if (!__dtfl_cb_is_empty(&sp->frontiers[0])) {
		size_t i, n = __dtfl_cb_iter(&sp->frontiers[0]);
		uint64_t *times = NULL;
		int64_t *diffs = NULL;

		if (n > 0) {
			times = __dtfl_alloc(n * (size_t)d * sizeof(uint64_t));
			diffs = __dtfl_alloc(n * sizeof(int64_t));
			for (i = 0; i < n; i++) {
				__dtfl_time_copy(c->tc, times + (size_t)d * i,
				    __dtfl_cb_key(&sp->frontiers[0], i));
				diffs[i] = __dtfl_cb_delta(&sp->frontiers[0], i);
			}
		}
		c->pusher.progress(c->pusher.ctx, times, diffs, n);
		__dtfl_free(times);
		__dtfl_free(diffs);
		__dtfl_cb_clear(&sp->frontiers[0]);
	}

	/* Turn each received message into an Event::Messages. */
	while ((m = __dtfl_pull_counter_next(c->pull)) != NULL)
		c->pusher.messages(c->pusher.ctx, m->time, m);
	__dtfl_pull_counter_finish(c->pull);

	/* Report consumption to the parent. */
	__dtfl_cb_drain_into(&c->consumed, &sp->consumeds[0]);
	return false;
}

static void
capture_free(void *vctx)
{
	capture_op *c = vctx;

	__dtfl_pull_counter_free(c->pull);
	__dtfl_cb_free(&c->consumed);
	if (c->ctx_free != NULL)
		c->ctx_free(c->ctx_ptr);
	__dtfl_free(c);
}

void
dtfl_stream_capture_into(dtfl_stream *st, dtfl_event_pusher p, dtfl_ctx ctx)
{
	dtfl_scope *scope = dtfl_stream_scope(st);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	capture_op *c = __dtfl_calloc(1, sizeof *c);
	__dtfl_slot slot;
	size_t *addr, alen, channel;
	__dtfl_push *pusher;
	__dtfl_pull *puller;
	__dtfl_connectivity summary;
	__dtfl_frontier_interest notify = DTFL_INTEREST_ALWAYS;
	__dtfl_operate *op;
	dtfl_pact pipe = dtfl_pact_pipeline();
	__dtfl_location target;

	c->pusher = p;
	c->ctx_free = ctx.free;
	c->ctx_ptr = ctx.ptr;
	c->tc = tc;

	/* Reserve the operator slot and wire its single input channel. */
	slot = __dtfl_scope_reserve(scope);
	addr = __dtfl_scope_slot_addr(scope, &slot, &alen);
	channel = __dtfl_worker_new_identifier(scope->worker);
	__dtfl_pact_connect(scope, &pipe, channel, addr, alen, tc,
	    dtfl_stream_type(st), &pusher, &puller);
	target.node = slot.index;
	target.port = 0;
	target.source = false;
	__dtfl_stream_connect_to(st, target, pusher, channel);

	__dtfl_cb_init(&c->consumed, tc->depth);
	c->pull = __dtfl_pull_counter_new(puller, &c->consumed);

	/* One input, no outputs; the identity summary is empty. */
	__dtfl_connectivity_init(&summary, tc, 1);

	op = __dtfl_op_core_new(scope->worker, "Capture", addr, alen, tc,
	    dtfl_scope_peers(scope), 1, 0, &notify, &summary, capture_logic, c,
	    capture_free);
	__dtfl_free(addr);
	__dtfl_scope_install(scope, &slot, op);
}
