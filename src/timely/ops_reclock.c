/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_reclock.c: release data at the times of a clock.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/reclock.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * reclock stashes every data record with its time and, for each record
 * seen on the clock input, requests a notification at that clock time.
 * When both input frontiers have passed a clock time c, every stashed
 * record with time <= c is released at time c, and those records are
 * dropped from the stash (reclock.rs).  Upstream expresses this with
 * binary_notify and a Notificator; this builds the same two-input,
 * one-output operator with the generic builder and tracks the pending
 * clock times itself, since a time is ready exactly when neither input
 * frontier is less-or-equal to it.
 *
 * This operator is public at the pin and was removed upstream later
 * (#818); the port keeps the pinned behavior.
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

#include "dtfl/record.h"
#include "dtfl/timely_ops.h"

/* A stashed data record: its time (depth words) then its bytes. */
typedef struct stash_rec {
	uint64_t time[DTFL_TIME_MAX];
	void *rec;	/* owned */
	size_t len;
} stash_rec;

typedef struct reclock_ctx {
	const dtfl_time_class *tc;
	__dtfl_vec stash;	/* stash_rec */
	__dtfl_vec pending;	/* pending_note: a noted clock time and its capability */
} reclock_ctx;

/* A requested notification: the clock time and a retained output capability. */
typedef struct pending_note {
	uint64_t time[DTFL_TIME_MAX];
	dtfl_cap *cap;	/* owned; at `time`, for output 0 */
} pending_note;

static void
reclock_note(reclock_ctx *c, const uint64_t *t, dtfl_cap *cap)
{
	size_t i;
	pending_note note;

	/* Distinct notifications only (Notificator::notify_at dedups). */
	for (i = 0; i < c->pending.len; i++) {
		pending_note *p = DTFL_VEC_AT(&c->pending, pending_note, i);

		if (dtfl_time_cmp(c->tc, p->time, t) == 0) {
			dtfl_cap_drop(cap);	/* already noted; drop the extra */
			return;
		}
	}
	__dtfl_time_copy(c->tc, note.time, t);
	note.cap = cap;
	__dtfl_vec_push(&c->pending, &note);
}

static bool
reclock_logic(dtfl_op *op, void *vctx)
{
	reclock_ctx *c = vctx;
	dtfl_message *m;
	size_t p;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* input1 (data): stash each record with its time. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			stash_rec sr;

			__dtfl_time_copy(c->tc, sr.time, t);
			sr.len = len;
			sr.rec = __dtfl_memdup(rec, len);
			__dtfl_vec_push(&c->stash, &sr);
		}
	}

	/*
	 * input2 (clock): request a notification at each clock time, keeping
	 * a capability retained from the clock input (Notificator::notify_at
	 * with time.retain(output_index)).
	 */
	while ((m = dtfl_op_input_next(op, 1)) != NULL)
		reclock_note(c, dtfl_message_time(m), dtfl_op_input_cap(op, 1, 0));

	/*
	 * Each noted clock time whose frontier is complete on both inputs
	 * can be flushed: release stashed data with time <= c at time c.
	 */
	for (p = 0; p < c->pending.len;) {
		pending_note *note = DTFL_VEC_AT(&c->pending, pending_note, p);

		if (dtfl_op_frontier_less_equal(op, 0, note->time) ||
		    dtfl_op_frontier_less_equal(op, 1, note->time)) {
			p++;	/* not yet complete; keep it pending */
			continue;
		}
		{
			dtfl_session *s = dtfl_op_session(op, 0, note->cap);
			size_t i;

			for (i = 0; i < c->stash.len;) {
				stash_rec *sr = DTFL_VEC_AT(&c->stash, stash_rec, i);

				if (dtfl_time_less_equal(c->tc, sr->time, note->time)) {
					dtfl_session_give(s, sr->rec, sr->len);
					__dtfl_free(sr->rec);
					__dtfl_vec_remove(&c->stash, i);
				} else {
					i++;
				}
			}
			dtfl_session_close(s);
		}
		dtfl_cap_drop(note->cap);
		__dtfl_vec_remove(&c->pending, p);
	}
	return false;
}

static void
reclock_ctx_free(void *vctx)
{
	reclock_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->stash.len; i++)
		__dtfl_free(DTFL_VEC_AT(&c->stash, stash_rec, i)->rec);
	for (i = 0; i < c->pending.len; i++)
		dtfl_cap_drop(DTFL_VEC_AT(&c->pending, pending_note, i)->cap);
	__dtfl_vec_free(&c->stash);
	__dtfl_vec_free(&c->pending);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_reclock(dtfl_stream *data, dtfl_stream *clock)
{
	dtfl_scope *scope = dtfl_stream_scope(data);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Reclock");
	dtfl_stream *out;
	reclock_ctx *c;

	dtfl_op_builder_input(b, data, dtfl_pact_pipeline());
	dtfl_op_builder_input(b, clock, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, dtfl_stream_type(data));

	c = __dtfl_calloc(1, sizeof *c);
	c->tc = tc;
	__dtfl_vec_init(&c->stash, sizeof(stash_rec));
	__dtfl_vec_init(&c->pending, sizeof(pending_note));
	dtfl_op_builder_build(b, reclock_logic, c, reclock_ctx_free);
	return out;
}
