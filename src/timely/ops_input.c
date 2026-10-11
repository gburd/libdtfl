/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_input.c: interactive inputs and the InputHandle.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/input.rs
 *   timely/src/dataflow/operators/vec/input.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A dtfl_input is upstream's InputHandle: it buffers records at the
 * current epoch and feeds one or more installed Input operators.  Each
 * dtfl_input_to_stream registers a new operator (Handle::register),
 * which is a raw operator with no inputs and one output.  The operator's
 * only job on each schedule is to publish the handle's capability holds
 * (progress) and the records it produced (messages) into its shared
 * progress (input.rs Operator::schedule); the records themselves are
 * pushed from the handle directly into the operator's output chain.
 *
 * The handle holds a capability at its current epoch (now_at) for every
 * registration: a +1 at now_at in that registration's progress batch.
 * advance_to closes the old epoch (a -1), moves now_at forward, and
 * opens the new one (a +1); close (dtfl_input_close) retires the final
 * +1.  Each change activates the operators so the worker drains them.
 * The source scope's class must be totally ordered (input.rs is only
 * implemented for T: TotalOrder).
 */
#include "timely/ops_src_int.h"

#include "dtfl/record.h"
#include "dtfl/timely_ops.h"

/* ------------------------------------------------------------------ */
/* Shared capability / produced cell (ops_src_int.h)                   */
/* ------------------------------------------------------------------ */

__dtfl_src_shared *
__dtfl_src_shared_new(const dtfl_time_class *tc)
{
	__dtfl_src_shared *s = __dtfl_alloc(sizeof *s);

	__dtfl_arc_init(&s->ref);
	__dtfl_cb_init(&s->progress, tc->depth);
	__dtfl_cb_init(&s->produced, tc->depth);
	return s;
}

__dtfl_src_shared *
__dtfl_src_shared_retain(__dtfl_src_shared *s)
{
	__dtfl_arc_retain(&s->ref);
	return s;
}

void
__dtfl_src_shared_release(__dtfl_src_shared *s)
{
	if (s == NULL || !__dtfl_arc_release(&s->ref))
		return;
	__dtfl_cb_free(&s->progress);
	__dtfl_cb_free(&s->produced);
	__dtfl_free(s);
}

/* ------------------------------------------------------------------ */
/* The raw source operator (output side)                               */
/* ------------------------------------------------------------------ */

/*
 * The logic of a source operator: drain the handle's capability holds
 * into internals[0] and its produced counts into produceds[0], then
 * report complete (input.rs schedule returns false; the operator stays
 * live only while it holds a capability or has unsent produced counts,
 * which the shared progress reflects).
 */
typedef struct src_op_ctx {
	__dtfl_src_shared *shared;	/* retained */
} src_op_ctx;

static bool
src_op_logic(void *vctx, __dtfl_shared_progress *sp)
{
	src_op_ctx *c = vctx;

	__dtfl_cb_drain_into(&c->shared->progress, &sp->internals[0]);
	__dtfl_cb_drain_into(&c->shared->produced, &sp->produceds[0]);
	return false;
}

static void
src_op_free(void *vctx)
{
	src_op_ctx *c = vctx;

	__dtfl_src_shared_release(c->shared);
	__dtfl_free(c);
}

dtfl_stream *
__dtfl_src_build(dtfl_scope *scope, const char *name, const dtfl_type *type,
    __dtfl_src_shared *shared, size_t copies, __dtfl_push **out_pusher,
    size_t **out_addr, size_t *out_addr_len)
{
	__dtfl_slot slot = __dtfl_scope_reserve(scope);
	size_t *addr, addr_len;
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	__dtfl_tee *tee = __dtfl_tee_new();
	__dtfl_location source = { slot.index, 0, true };
	dtfl_stream *stream = __dtfl_stream_new(scope, source, tee, type);
	__dtfl_push *counter;
	src_op_ctx *ctx;
	__dtfl_connectivity summary;
	__dtfl_operate *op;

	(void)copies;	/* the raw core reserves `peers` at minimum already */
	addr = __dtfl_scope_slot_addr(scope, &slot, &addr_len);

	/* Counter over the Tee pusher, producing into the shared batch. */
	counter = __dtfl_push_counter_new(__dtfl_tee_pusher(tee), &shared->produced);

	ctx = __dtfl_alloc(sizeof *ctx);
	ctx->shared = __dtfl_src_shared_retain(shared);

	/* No inputs, one output; the raw core adds `peers` caps at minimum. */
	__dtfl_connectivity_init(&summary, tc, 0);
	op = __dtfl_op_core_new(scope->worker, name, addr, addr_len, tc,
	    dtfl_scope_peers(scope), 0, 1, NULL, &summary,
	    src_op_logic, ctx, src_op_free);
	__dtfl_scope_install(scope, &slot, op);

	*out_pusher = counter;
	*out_addr = addr;	/* caller owns the address copy */
	*out_addr_len = addr_len;
	__dtfl_tee_release(tee);	/* the stream holds its own reference */
	return stream;
}

/* ------------------------------------------------------------------ */
/* The input handle (input.rs Handle)                                  */
/* ------------------------------------------------------------------ */

/* One registration: a stream's output chain plus its capability tracker. */
typedef struct input_reg {
	__dtfl_src_shared *shared;	/* retained; the operator shares it */
	__dtfl_push *pusher;		/* Counter over Tee; owned */
	dtfl_activator *activator;	/* owned */
} input_reg;

struct dtfl_input {
	const dtfl_type *type;		/* retained */
	dtfl_time_class *tc;		/* retained; set at first to_stream */
	unsigned depth;
	__dtfl_vec regs;		/* input_reg */
	__dtfl_batch *buffer;		/* records at now_at; owned */
	uint64_t now_at[DTFL_TIME_MAX];
	bool have_time;			/* now_at valid (tc known) */
	bool closed;
};

dtfl_input *
dtfl_input_new(const dtfl_type *type)
{
	dtfl_input *in = __dtfl_calloc(1, sizeof *in);

	in->type = dtfl_type_retain(type);
	__dtfl_vec_init(&in->regs, sizeof(input_reg));
	in->buffer = __dtfl_batch_new(0, 0);
	return in;
}

/* Flush buffered records into every registration's output at now_at. */
static void
input_flush(dtfl_input *in)
{
	size_t r, n;

	if (!in->have_time || __dtfl_batch_len(in->buffer) == 0)
		return;
	n = in->regs.len;
	for (r = 0; r < n; r++) {
		input_reg *reg = DTFL_VEC_AT(&in->regs, input_reg, r);
		__dtfl_batch *data;
		void *bundle;

		/* Share the batch for all but the last; the last takes it. */
		data = (r + 1 < n) ? __dtfl_batch_retain(in->buffer) : in->buffer;
		bundle = __dtfl_message_new(in->tc, in->now_at, data);
		__dtfl_push_push(reg->pusher, &bundle);
		if (bundle != NULL) {	/* no pusher recycles a data message */
			dtfl_message *m = bundle;

			__dtfl_batch_release(m->data);
			__dtfl_free(m);
		}
	}
	if (n == 0)
		__dtfl_batch_release(in->buffer);
	in->buffer = __dtfl_batch_new(0, 0);
}

void
dtfl_input_send(dtfl_input *in, const void *rec, size_t len)
{
	__dtfl_batch_push(in->buffer, rec, len, NULL, NULL);
	/* Bound the buffer; a full buffer flushes to the current pushers. */
	if (__dtfl_batch_bytes(in->buffer) >= DTFL_BUFFER_SIZE_BYTES)
		input_flush(in);
}

void
dtfl_input_flush(dtfl_input *in)
{
	input_flush(in);
}

dtfl_stream *
dtfl_input_to_stream(dtfl_input *in, dtfl_scope *scope)
{
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	input_reg reg;
	dtfl_stream *stream;
	size_t *addr, addr_len;
	uint64_t minimum[DTFL_TIME_MAX];

	if (!dtfl_time_is_total(tc))
		__dtfl_panic("dtfl_input_to_stream: the scope's time class is "
		    "not totally ordered");

	/* First registration pins the handle's class and current epoch. */
	if (in->tc == NULL) {
		in->tc = dtfl_time_class_retain(tc);
		in->depth = tc->depth;
		dtfl_time_minimum(tc, in->now_at);
		in->have_time = true;
	}

	/* A new registrant must not see records sent before it attached. */
	input_flush(in);

	reg.shared = __dtfl_src_shared_new(tc);
	stream = __dtfl_src_build(scope, "Input", in->type, reg.shared,
	    dtfl_scope_peers(scope), &reg.pusher, &addr, &addr_len);
	reg.activator = __dtfl_activator_new(scope->worker, addr, addr_len);
	__dtfl_free(addr);

	/*
	 * Register::update: the raw core reserved a +1 at minimum; retract
	 * it and assert the current epoch, so a handle driven forward before
	 * registering starts the operator at the right frontier (input.rs).
	 */
	dtfl_time_minimum(tc, minimum);
	__dtfl_cb_update(&reg.shared->progress, minimum, -1);
	__dtfl_cb_update(&reg.shared->progress, in->now_at, 1);

	__dtfl_vec_push(&in->regs, &reg);
	return stream;
}

const uint64_t *
dtfl_input_time(const dtfl_input *in)
{
	return in->have_time ? in->now_at : NULL;
}

/* close_epoch: flush, flush the pushers, retire the epoch's hold, activate. */
static void
input_close_epoch(dtfl_input *in)
{
	size_t r;

	input_flush(in);
	for (r = 0; r < in->regs.len; r++) {
		input_reg *reg = DTFL_VEC_AT(&in->regs, input_reg, r);

		__dtfl_push_done(reg->pusher);
		__dtfl_cb_update(&reg->shared->progress, in->now_at, -1);
		dtfl_activator_activate(reg->activator);
	}
}

void
dtfl_input_advance_to(dtfl_input *in, const uint64_t *t)
{
	size_t r;

	if (!in->have_time)
		__dtfl_panic("dtfl_input_advance_to before any stream is created");
	if (!dtfl_time_less_equal(in->tc, in->now_at, t)) {
		char from[128], to[128];

		dtfl_time_format(in->tc, in->now_at, from, sizeof from);
		dtfl_time_format(in->tc, t, to, sizeof to);
		__dtfl_panic("dtfl_input_advance_to: %s does not advance %s", to, from);
	}
	/* Only act when time actually changes (input.rs advance_to). */
	if (dtfl_time_cmp(in->tc, in->now_at, t) == 0)
		return;
	input_close_epoch(in);
	__dtfl_time_copy(in->tc, in->now_at, t);
	for (r = 0; r < in->regs.len; r++) {
		input_reg *reg = DTFL_VEC_AT(&in->regs, input_reg, r);

		__dtfl_cb_update(&reg->shared->progress, in->now_at, 1);
	}
}

void
dtfl_input_close(dtfl_input *in)
{
	size_t r;

	if (in == NULL)
		return;
	/* Drop closes the final epoch: flush, retire the hold, activate. */
	if (in->have_time && !in->closed)
		input_close_epoch(in);
	for (r = 0; r < in->regs.len; r++) {
		input_reg *reg = DTFL_VEC_AT(&in->regs, input_reg, r);

		__dtfl_push_destroy(reg->pusher);
		__dtfl_src_shared_release(reg->shared);
		dtfl_activator_free(reg->activator);
	}
	__dtfl_vec_free(&in->regs);
	__dtfl_batch_release(in->buffer);
	if (in->tc != NULL)
		dtfl_time_class_free(in->tc);
	dtfl_type_free(in->type);
	__dtfl_free(in);
}
