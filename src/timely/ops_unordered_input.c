/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_unordered_input.c: capability-driven inputs.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/unordered_input.rs
 *   timely/src/dataflow/operators/vec/unordered_input.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * An unordered input is a raw operator with no inputs and one output
 * whose data is sent under capabilities the caller manages (several
 * epochs may be open at once).  The handle shares the output's internal
 * capability batch with its first capability: Capability::new(minimum,
 * internal) places a +1 at the minimum.  initialize drains that batch
 * into the shared progress scaled by the number of peers, so the output
 * holds `peers` copies of the capability; the user downgrades or drops
 * the capability to let the frontier advance.  Each schedule drains any
 * further internal changes (×1) and the records produced.
 *
 * Unlike the ordered input (ops_input.c) this is NOT restricted to a
 * totally ordered class, matching upstream.
 */
#include "timely/ops_src_int.h"

#include "dtfl/record.h"
#include "dtfl/timely_ops.h"

/* ------------------------------------------------------------------ */
/* The unordered raw operator                                          */
/* ------------------------------------------------------------------ */

typedef struct uinput_op {
	__dtfl_operate operate;
	__dtfl_schedule sched;
	const dtfl_time_class *tc;	/* retained */
	size_t peers;
	__dtfl_src_shared *shared;	/* retained; internal + produced */
	__dtfl_shared_progress *sp;	/* owned until initialize */
	dtfl_stream *stream;		/* the operator's output; owned */
} uinput_op;

static bool
uop_local(const __dtfl_operate *op)
{
	(void)op;
	return true;
}

static size_t
uop_inputs(const __dtfl_operate *op)
{
	(void)op;
	return 0;
}

static size_t
uop_outputs(const __dtfl_operate *op)
{
	(void)op;
	return 1;
}

static const __dtfl_frontier_interest *
uop_notify_me(const __dtfl_operate *op)
{
	(void)op;
	return NULL;
}

static bool
uop_schedule(__dtfl_schedule *s)
{
	uinput_op *o = (uinput_op *)(void *)((char *)s - offsetof(uinput_op, sched));

	__dtfl_cb_drain_into(&o->shared->progress, &o->sp->internals[0]);
	__dtfl_cb_drain_into(&o->shared->produced, &o->sp->produceds[0]);
	return false;
}

static void
uop_free(uinput_op *o)
{
	__dtfl_src_shared_release(o->shared);
	__dtfl_stream_free(o->stream);
	dtfl_time_class_free(o->tc);
	__dtfl_free(o->sched.name);
	__dtfl_free(o->sched.path);
	__dtfl_free(o);
}

static void
uop_sched_destroy(__dtfl_schedule *s)
{
	uinput_op *o = (uinput_op *)(void *)((char *)s - offsetof(uinput_op, sched));

	uop_free(o);
}

static const __dtfl_schedule_ops uop_sched_ops = { uop_schedule, uop_sched_destroy };

static __dtfl_schedule *
uop_initialize(__dtfl_operate *op, __dtfl_connectivity *conn,
    __dtfl_shared_progress **progress)
{
	uinput_op *o = (uinput_op *)op;
	size_t n, i;

	/* Drain the initial capability holds scaled by peers (unordered_input). */
	n = __dtfl_cb_iter(&o->shared->progress);
	for (i = 0; i < n; i++) {
		const uint64_t *time = __dtfl_cb_key(&o->shared->progress, i);
		int64_t count = __dtfl_cb_delta(&o->shared->progress, i);

		__dtfl_cb_update(&o->sp->internals[0], time, count * (int64_t)o->peers);
	}
	__dtfl_cb_clear(&o->shared->progress);

	__dtfl_connectivity_init(conn, o->tc, 0);
	*progress = o->sp;
	return &o->sched;
}

static void
uop_operate_destroy(__dtfl_operate *op)
{
	uinput_op *o = (uinput_op *)op;

	__dtfl_shared_progress_free(o->sp);
	uop_free(o);
}

static const __dtfl_operate_ops uop_ops = {
	uop_local, uop_inputs, uop_outputs, uop_notify_me, uop_initialize,
	uop_operate_destroy
};

/* ------------------------------------------------------------------ */
/* The handle                                                          */
/* ------------------------------------------------------------------ */

struct dtfl_unordered_input {
	dtfl_time_class *tc;		/* retained */
	__dtfl_src_shared *shared;	/* retained */
	__dtfl_push *pusher;		/* Counter over Tee; owned */
	dtfl_activator *activator;	/* owned */
};

dtfl_stream *
dtfl_unordered_input_new(dtfl_scope *scope, const dtfl_type *type,
    dtfl_unordered_input **handle, dtfl_cap **cap)
{
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	__dtfl_slot slot = __dtfl_scope_reserve(scope);
	size_t *addr, addr_len;
	__dtfl_tee *tee = __dtfl_tee_new();
	__dtfl_location source = { slot.index, 0, true };
	dtfl_stream *stream = __dtfl_stream_new(scope, source, tee, type);
	__dtfl_src_shared *shared = __dtfl_src_shared_new(tc);
	dtfl_unordered_input *h;
	uinput_op *o;
	uint64_t minimum[DTFL_TIME_MAX];

	addr = __dtfl_scope_slot_addr(scope, &slot, &addr_len);

	o = __dtfl_calloc(1, sizeof *o);
	o->operate.ops = &uop_ops;
	o->sched.ops = &uop_sched_ops;
	o->sched.name = __dtfl_strdup("UnorderedInput");
	o->sched.path = __dtfl_memdup(addr, addr_len * sizeof(size_t));
	o->sched.path_len = addr_len;
	o->tc = dtfl_time_class_retain(tc);
	o->peers = dtfl_scope_peers(scope);
	o->shared = __dtfl_src_shared_retain(shared);
	o->sp = __dtfl_shared_progress_new(tc, 0, 1);
	o->stream = stream;	/* the operator owns the stream (builder.c) */
	__dtfl_scope_install(scope, &slot, &o->operate);

	h = __dtfl_alloc(sizeof *h);
	h->tc = dtfl_time_class_retain(tc);
	h->shared = shared;	/* takes the construction reference */
	h->pusher = __dtfl_push_counter_new(__dtfl_tee_pusher(tee), &shared->produced);
	h->activator = __dtfl_activator_new(scope->worker, addr, addr_len);

	/* Capability::new(minimum, internal): a +1 at the minimum time. */
	dtfl_time_minimum(tc, minimum);
	*cap = __dtfl_cap_new(tc, minimum, &shared->progress, 0);

	__dtfl_free(addr);
	__dtfl_tee_release(tee);	/* the stream holds its own reference */
	*handle = h;
	return stream;
}

void
dtfl_unordered_input_send(dtfl_unordered_input *h, const dtfl_cap *cap,
    const void *rec, size_t len)
{
	__dtfl_batch *data = __dtfl_batch_new(0, 0);
	void *bundle;

	(void)cap;	/* the cap's time; validity is the caller's contract */
	__dtfl_batch_push(data, rec, len, NULL, NULL);
	bundle = __dtfl_message_new(h->tc, dtfl_cap_time(cap), data);
	__dtfl_push_push(h->pusher, &bundle);
	if (bundle != NULL) {
		dtfl_message *m = bundle;

		__dtfl_batch_release(m->data);
		__dtfl_free(m);
	}
	dtfl_activator_activate(h->activator);
}

void
dtfl_unordered_input_flush(dtfl_unordered_input *h)
{
	__dtfl_push_done(h->pusher);
	dtfl_activator_activate(h->activator);
}

void
dtfl_unordered_input_free(dtfl_unordered_input *h)
{
	if (h == NULL)
		return;
	/*
	 * Activate once more so a later step drains the capability changes
	 * the caller made since the last send (dropping the final capability
	 * retires its hold); upstream's ActivateCapability does this on drop.
	 */
	dtfl_activator_activate(h->activator);
	__dtfl_push_destroy(h->pusher);
	__dtfl_src_shared_release(h->shared);
	dtfl_activator_free(h->activator);
	dtfl_time_class_free(h->tc);
	__dtfl_free(h);
}
