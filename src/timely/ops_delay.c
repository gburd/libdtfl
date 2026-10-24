/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_delay.c: delay, delay_total and delay_batch.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/vec/delay.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * delay re-times each record to fn(rec, time) >= time; delay_batch
 * re-times a whole message to fn(time) >= time.  Each stashes records
 * keyed by the new time and, when a new time first appears, requests a
 * notification at it (through the input capability delayed to the new
 * time).  When the input frontier passes a stashed time, every record
 * held at that time is emitted there.  delay_total is delay specialised
 * to a totally ordered time; the behaviour is identical (delay.rs).
 *
 * Built on dtfl_stream_unary_notify (operator.c), as upstream builds on
 * unary_notify.  The operator asserts time <= new_time, matching
 * upstream's assert.  Public at the pin, removed upstream later (#818).
 */
#include "timely/ops_int.h"
#include "timely/ops_notify_int.h"
#include "progress/time_int.h"

#include <string.h>

/* A set of records held at one re-timed output time. */
typedef struct delay_slot {
	uint64_t time[DTFL_TIME_MAX];
	__dtfl_vec recs;	/* each: a size_t len then len bytes, packed */
} delay_slot;

typedef struct delay_ctx {
	const dtfl_time_class *tc;
	bool batch;			/* delay_batch vs. per-record delay */
	dtfl_delay_fn fn;		/* per-record retime */
	dtfl_delay_batch_fn bfn;	/* per-message retime */
	void *user;
	void (*user_free)(void *);
	__dtfl_vec slots;		/* delay_slot, keyed by output time */
} delay_ctx;

/* A packed record: a size_t length header then the bytes. */
static void
slot_push_rec(delay_slot *s, const void *rec, size_t len)
{
	__dtfl_vec_push_n(&s->recs, &len, sizeof len);
	if (len > 0)
		__dtfl_vec_push_n(&s->recs, rec, len);
}

/* Finds the slot for `time`, or creates one (requesting a notification). */
static delay_slot *
delay_slot_for(delay_ctx *c, dtfl_op *op, dtfl_notificator *note,
    const uint64_t *time)
{
	size_t i;
	delay_slot s;
	delay_slot *slot;

	for (i = 0; i < c->slots.len; i++) {
		slot = DTFL_VEC_AT(&c->slots, delay_slot, i);
		if (dtfl_time_cmp(c->tc, slot->time, time) == 0)
			return slot;
	}
	/* New time: request a notification at it and open a slot. */
	__dtfl_time_copy(c->tc, s.time, time);
	__dtfl_vec_init(&s.recs, 1);
	slot = __dtfl_vec_push(&c->slots, &s);
	dtfl_notificator_notify_at(note,
	    dtfl_op_input_cap_delayed(op, 0, 0, time));
	return slot;
}

static void
delay_run(void *vctx, dtfl_op *op, dtfl_notificator *note)
{
	delay_ctx *c = vctx;
	dtfl_message *m;
	dtfl_cap *cap;
	uint64_t count;

	/* Stash each record (or message) under its re-timed output time. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		size_t i, n = dtfl_message_len(m);
		uint64_t nt[DTFL_TIME_MAX];

		if (c->batch) {
			c->bfn(c->user, t, nt);
			if (!dtfl_time_less_equal(c->tc, t, nt))
				__dtfl_panic("delay_batch: new time not >= old");
			{
				delay_slot *slot = delay_slot_for(c, op, note, nt);

				for (i = 0; i < n; i++) {
					size_t len;
					const void *rec = dtfl_message_record(m, i, &len);

					slot_push_rec(slot, rec, len);
				}
			}
		} else {
			for (i = 0; i < n; i++) {
				size_t len;
				const void *rec = dtfl_message_record(m, i, &len);
				delay_slot *slot;

				c->fn(c->user, rec, len, t, nt);
				if (!dtfl_time_less_equal(c->tc, t, nt))
					__dtfl_panic("delay: new time not >= old");
				slot = delay_slot_for(c, op, note, nt);
				slot_push_rec(slot, rec, len);
			}
		}
	}

	/* Emit the records held at each time the frontier has passed. */
	while (dtfl_notificator_next(note, &cap, &count)) {
		const uint64_t *t = dtfl_cap_time(cap);
		size_t i;

		for (i = 0; i < c->slots.len; i++) {
			delay_slot *slot = DTFL_VEC_AT(&c->slots, delay_slot, i);

			if (dtfl_time_cmp(c->tc, slot->time, t) == 0) {
				dtfl_session *s = dtfl_op_session(op, 0, cap);
				size_t off = 0;

				while (off < slot->recs.len) {
					size_t len;

					memcpy(&len, slot->recs.data + off, sizeof len);
					off += sizeof len;
					dtfl_session_give(s, slot->recs.data + off, len);
					off += len;
				}
				dtfl_session_close(s);
				__dtfl_vec_free(&slot->recs);
				__dtfl_vec_remove(&c->slots, i);
				break;
			}
		}
		dtfl_cap_drop(cap);
	}
}

static void
delay_free(void *vctx)
{
	delay_ctx *c = vctx;
	size_t i;

	if (c->user_free != NULL)
		c->user_free(c->user);
	for (i = 0; i < c->slots.len; i++)
		__dtfl_vec_free(&DTFL_VEC_AT(&c->slots, delay_slot, i)->recs);
	__dtfl_vec_free(&c->slots);
	__dtfl_free(c);
}

static dtfl_stream *
delay_build(dtfl_stream *st, delay_ctx *c)
{
	return dtfl_stream_unary_notify(st, dtfl_pact_pipeline(), "Delay", NULL,
	    0, dtfl_stream_type(st), delay_run, DTFL_CTX(c, delay_free));
}

static delay_ctx *
delay_ctx_new(dtfl_stream *st, dtfl_ctx ctx)
{
	delay_ctx *c = __dtfl_calloc(1, sizeof *c);

	c->tc = dtfl_scope_time_class(dtfl_stream_scope(st));
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	__dtfl_vec_init(&c->slots, sizeof(delay_slot));
	return c;
}

dtfl_stream *
dtfl_stream_delay(dtfl_stream *st, dtfl_delay_fn fn, dtfl_ctx ctx)
{
	delay_ctx *c = delay_ctx_new(st, ctx);

	c->batch = false;
	c->fn = fn;
	return delay_build(st, c);
}

dtfl_stream *
dtfl_stream_delay_total(dtfl_stream *st, dtfl_delay_fn fn, dtfl_ctx ctx)
{
	/* delay_total is delay for a totally ordered time (delay.rs). */
	return dtfl_stream_delay(st, fn, ctx);
}

dtfl_stream *
dtfl_stream_delay_batch(dtfl_stream *st, dtfl_delay_batch_fn fn, dtfl_ctx ctx)
{
	delay_ctx *c = delay_ctx_new(st, ctx);

	c->batch = true;
	c->bfn = fn;
	return delay_build(st, c);
}
