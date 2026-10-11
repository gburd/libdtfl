/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/replay.c: replaying a captured event stream into a scope.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/capture/replay.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Replay is a raw operator (builder_raw) with no input and one output.
 * On its first schedule it adjusts its single output capability by
 * (streams - 1), so replaying N streams holds N initial capabilities at
 * the minimum time.  Each schedule reads every pending event: a Progress
 * event extends the output capability batch (internals[0]); a Messages
 * event is pushed through the output as a message at the recorded time.
 * It re-activates itself after a zero delay so a non-blocking source
 * keeps draining, then publishes its produced counts.  The replay
 * protocol (replay.rs) means any interleaving of the input streams still
 * maintains the frontier invariants.
 */
#include "timely/capture_int.h"

#include "timely/scope_int.h"
#include "progress/operate_int.h"

typedef struct replay_op {
	dtfl_event_link *link;		/* not owned; a shared capture log */
	const dtfl_time_class *tc;	/* not retained; the core retains it */
	__dtfl_push *output;		/* Counter over Tee; owned */
	__dtfl_change_batch produced;	/* fed by the push counter */
	dtfl_stream *stream;		/* the operator owns the stream (builder.c) */
	__dtfl_worker *worker;
	size_t *address;		/* owned; for activate_after */
	size_t address_len;
} replay_op;

static bool
replay_logic(void *vctx, __dtfl_shared_progress *sp)
{
	replay_op *r = vctx;
	const dtfl_event *e;

	/*
	 * One link replays one stream, so the single reserved capability
	 * already matches (replay_core adjusts by streams - 1, which is 0).
	 */
	while ((e = __dtfl_event_link_next(r->link)) != NULL) {
		if (e->tag == DTFL_EVENT_PROGRESS) {
			__dtfl_cb_extend(&sp->internals[0], &e->u.progress);
		} else {
			dtfl_message *m = __dtfl_message_new(r->tc, e->u.messages.time,
			    __dtfl_batch_retain(e->u.messages.data));

			__dtfl_push_send(r->output, __dtfl_message_class(), m);
		}
	}

	/* Re-activate after a zero delay (replay_into's period of 0). */
	__dtfl_activations_activate_after(&r->worker->activations, r->address,
	    r->address_len, 0);

	__dtfl_push_done(r->output);
	__dtfl_cb_drain_into(&r->produced, &sp->produceds[0]);
	return false;
}

static void
replay_free(void *vctx)
{
	replay_op *r = vctx;

	__dtfl_push_destroy(r->output);
	__dtfl_cb_free(&r->produced);
	__dtfl_stream_free(r->stream);
	__dtfl_free(r->address);
	__dtfl_free(r);
}

dtfl_stream *
dtfl_event_link_replay(dtfl_event_link *l, dtfl_scope *scope)
{
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	replay_op *r = __dtfl_calloc(1, sizeof *r);
	__dtfl_slot slot;
	size_t *addr, alen;
	__dtfl_tee *tee;
	dtfl_stream *stream;
	__dtfl_location source;
	__dtfl_connectivity summary;
	__dtfl_operate *op;

	r->link = l;
	r->tc = tc;
	r->worker = scope->worker;

	slot = __dtfl_scope_reserve(scope);
	addr = __dtfl_scope_slot_addr(scope, &slot, &alen);
	r->address = __dtfl_memdup(addr, alen * sizeof(size_t));
	r->address_len = alen;

	/* Output 0: a Tee behind the stream, a push counter in front. */
	tee = __dtfl_tee_new();
	source.node = slot.index;
	source.port = 0;
	source.source = true;
	stream = __dtfl_stream_new(scope, source, tee, l->type);
	r->stream = stream;
	__dtfl_cb_init(&r->produced, tc->depth);
	r->output = __dtfl_push_counter_new(__dtfl_tee_pusher(tee), &r->produced);
	__dtfl_tee_release(tee);	/* the stream and the pusher hold references */

	/* No inputs; connectivity has zero input ports. */
	__dtfl_connectivity_init(&summary, tc, 0);

	op = __dtfl_op_core_new(scope->worker, "Replay", addr, alen, tc,
	    dtfl_scope_peers(scope), 0, 1, NULL, &summary, replay_logic, r,
	    replay_free);
	__dtfl_free(addr);
	__dtfl_scope_install(scope, &slot, op);
	return stream;
}
