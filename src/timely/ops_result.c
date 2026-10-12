/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_result.c: stream helpers for Result records.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/vec/result.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A Result record is a tag byte (0 Ok, 1 Err) then the value.  ok and
 * err are flat maps that keep one arm and emit its unwrapped value.
 * map_ok, map_err, and_then and unwrap_or_else are maps (one output per
 * input) that transform one arm's value and leave the record a Result,
 * except unwrap_or_else which flattens both arms to a plain value.  The
 * value transform writes into a scratch buffer and the operator applies
 * the tag, so a callback never sees the tag byte.  All are pipeline
 * operators (the input frontier is not needed).
 */
#include "timely/ops_int.h"

#define RESULT_OK 0
#define RESULT_ERR 1

/* Scratch for a value transform's output; a value larger than this is a
 * programming error for these helpers (the pinned uses are small). */
#define RESULT_SCRATCH 4096

/* ------------------------------------------------------------------ */
/* ok / err: keep one arm, drop the tag                                */
/* ------------------------------------------------------------------ */

typedef struct arm_ctx {
	uint8_t keep;	/* RESULT_OK or RESULT_ERR */
} arm_ctx;

static bool
arm_logic(dtfl_op *op, void *vctx)
{
	arm_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const unsigned char *rec = dtfl_message_record(m, i, &len);

			if (len >= 1 && rec[0] == c->keep)
				dtfl_session_give(s, rec + 1, len - 1);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static dtfl_stream *
build_arm(dtfl_stream *st, const dtfl_type *out_type, uint8_t keep,
    const char *name)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st), name);
	arm_ctx *c = __dtfl_alloc(sizeof *c);
	dtfl_stream *out;

	c->keep = keep;
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, out_type);
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	dtfl_op_builder_build(b, arm_logic, c, __dtfl_free);
	return out;
}

dtfl_stream *
dtfl_stream_ok(dtfl_stream *st, const dtfl_type *ok_type)
{
	return build_arm(st, ok_type, RESULT_OK, "Ok");
}

dtfl_stream *
dtfl_stream_err(dtfl_stream *st, const dtfl_type *err_type)
{
	return build_arm(st, err_type, RESULT_ERR, "Err");
}

/* ------------------------------------------------------------------ */
/* map_ok / map_err: transform one arm, keep the Result record         */
/* ------------------------------------------------------------------ */

typedef struct map_arm_ctx {
	dtfl_val_fn fn;
	void *user;
	void (*user_free)(void *);
	uint8_t arm;	/* the arm the transform applies to */
} map_arm_ctx;

static bool
map_arm_logic(dtfl_op *op, void *vctx)
{
	map_arm_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const unsigned char *rec = dtfl_message_record(m, i, &len);

			if (len < 1)
				continue;
			if (rec[0] == c->arm) {
				unsigned char scratch[RESULT_SCRATCH];
				size_t out_len = c->fn(c->user, rec + 1, len - 1,
				    scratch, sizeof scratch);
				unsigned char *dst = __dtfl_session_reserve(s,
				    1 + out_len);

				dst[0] = c->arm;
				if (out_len > 0)
					memcpy(dst + 1, scratch, out_len);
			} else {
				dtfl_session_give(s, rec, len);	/* pass through */
			}
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
map_arm_free(void *vctx)
{
	map_arm_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

static dtfl_stream *
build_map_arm(dtfl_stream *st, dtfl_val_fn fn, dtfl_ctx ctx, uint8_t arm,
    const char *name)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st), name);
	map_arm_ctx *c = __dtfl_alloc(sizeof *c);
	dtfl_stream *out;

	c->fn = fn;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	c->arm = arm;
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, dtfl_stream_type(st));
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	dtfl_op_builder_build(b, map_arm_logic, c, map_arm_free);
	return out;
}

dtfl_stream *
dtfl_stream_map_ok(dtfl_stream *st, dtfl_val_fn fn, dtfl_ctx ctx)
{
	return build_map_arm(st, fn, ctx, RESULT_OK, "MapOk");
}

dtfl_stream *
dtfl_stream_map_err(dtfl_stream *st, dtfl_val_fn fn, dtfl_ctx ctx)
{
	return build_map_arm(st, fn, ctx, RESULT_ERR, "MapErr");
}

/* ------------------------------------------------------------------ */
/* and_then: Ok -> new Result; Err passes through                      */
/* ------------------------------------------------------------------ */

typedef struct and_then_ctx {
	dtfl_and_then_fn fn;
	void *user;
	void (*user_free)(void *);
} and_then_ctx;

static bool
and_then_logic(dtfl_op *op, void *vctx)
{
	and_then_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const unsigned char *rec = dtfl_message_record(m, i, &len);

			if (len < 1)
				continue;
			if (rec[0] == RESULT_OK) {
				unsigned char scratch[RESULT_SCRATCH];
				bool is_ok = true;
				size_t out_len = c->fn(c->user, rec + 1, len - 1,
				    scratch, sizeof scratch, &is_ok);
				unsigned char *dst = __dtfl_session_reserve(s,
				    1 + out_len);

				dst[0] = is_ok ? RESULT_OK : RESULT_ERR;
				if (out_len > 0)
					memcpy(dst + 1, scratch, out_len);
			} else {
				dtfl_session_give(s, rec, len);	/* Err passes through */
			}
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
and_then_free(void *vctx)
{
	and_then_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_and_then(dtfl_stream *st, dtfl_and_then_fn fn, dtfl_ctx ctx)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st), "AndThen");
	and_then_ctx *c = __dtfl_alloc(sizeof *c);
	dtfl_stream *out;

	c->fn = fn;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, dtfl_stream_type(st));
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	dtfl_op_builder_build(b, and_then_logic, c, and_then_free);
	return out;
}

/* ------------------------------------------------------------------ */
/* unwrap_or_else: Ok unwrapped; Err -> value                          */
/* ------------------------------------------------------------------ */

typedef struct unwrap_ctx {
	dtfl_val_fn fn;
	void *user;
	void (*user_free)(void *);
} unwrap_ctx;

static bool
unwrap_logic(dtfl_op *op, void *vctx)
{
	unwrap_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const unsigned char *rec = dtfl_message_record(m, i, &len);

			if (len < 1)
				continue;
			if (rec[0] == RESULT_OK) {
				dtfl_session_give(s, rec + 1, len - 1);
			} else {
				unsigned char scratch[RESULT_SCRATCH];
				size_t out_len = c->fn(c->user, rec + 1, len - 1,
				    scratch, sizeof scratch);

				dtfl_session_give(s, scratch, out_len);
			}
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
unwrap_free(void *vctx)
{
	unwrap_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_unwrap_or_else(dtfl_stream *st, const dtfl_type *out_type,
    dtfl_val_fn fn, dtfl_ctx ctx)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(st),
	    "UnwrapOrElse");
	unwrap_ctx *c = __dtfl_alloc(sizeof *c);
	dtfl_stream *out;

	c->fn = fn;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, out_type);
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	dtfl_op_builder_build(b, unwrap_logic, c, unwrap_free);
	return out;
}
