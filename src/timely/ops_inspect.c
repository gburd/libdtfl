/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_inspect.c: inspect, inspect_batch and inspect_core.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/inspect.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * inspect_core is a frontier-aware unary operator: it reports each
 * frontier change (Err arm, msg NULL) and each input message (Ok arm),
 * passing the records through unchanged.  inspect_batch is inspect_core
 * ignoring the frontier changes; inspect is inspect_batch calling the
 * closure once per record.  All three preserve the stream, so the output
 * type is the input type.
 */
#include "timely/ops_int.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* inspect_core                                                        */
/* ------------------------------------------------------------------ */

typedef struct inspect_core_ctx {
	dtfl_inspect_core_fn fn;
	void *user;
	void (*user_free)(void *);
	const dtfl_time_class *tc;	/* not retained */
	unsigned words;			/* words per frontier element */
	uint64_t *prev;			/* previous frontier, words*cap */
	size_t prev_n, prev_cap;
	bool have_prev;			/* initialised the comparison */
} inspect_core_ctx;

static bool
frontier_changed(inspect_core_ctx *c, const uint64_t *f, size_t n)
{
	if (!c->have_prev || n != c->prev_n)
		return true;
	return memcmp(c->prev, f, (size_t)c->words * n * sizeof(uint64_t)) != 0;
}

static void
frontier_store(inspect_core_ctx *c, const uint64_t *f, size_t n)
{
	size_t need = (size_t)c->words * n;

	if (need > c->prev_cap) {
		c->prev = __dtfl_realloc(c->prev, need * sizeof(uint64_t));
		c->prev_cap = need;
	}
	if (need > 0)
		memcpy(c->prev, f, need * sizeof(uint64_t));
	c->prev_n = n;
	c->have_prev = true;
}

static bool
inspect_core_logic(dtfl_op *op, void *vctx)
{
	inspect_core_ctx *c = vctx;
	dtfl_message *m;
	const uint64_t *f;
	size_t fn;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* Err arm: report the frontier only when it changes. */
	f = dtfl_op_frontier(op, 0, &fn);
	if (frontier_changed(c, f, fn)) {
		frontier_store(c, f, fn);
		c->fn(c->user, NULL, f, fn);
	}

	/* Ok arm: each message, records passed through unchanged. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);

		c->fn(c->user, m, NULL, 0);
		dtfl_session_give_message(s, m);
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
inspect_core_free(void *vctx)
{
	inspect_core_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c->prev);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_inspect_core(dtfl_stream *st, dtfl_inspect_core_fn fn, dtfl_ctx ctx)
{
	dtfl_scope *scope = dtfl_stream_scope(st);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "InspectBatch");
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	dtfl_stream *out;
	inspect_core_ctx *c = __dtfl_calloc(1, sizeof *c);

	c->fn = fn;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	c->tc = tc;
	c->words = DTFL_MAX(dtfl_time_depth(tc), 1u);
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, dtfl_stream_type(st));
	dtfl_op_builder_build(b, inspect_core_logic, c, inspect_core_free);
	return out;
}

/* ------------------------------------------------------------------ */
/* inspect_batch (inspect_core ignoring frontier changes)             */
/* ------------------------------------------------------------------ */

typedef struct inspect_batch_ctx {
	dtfl_inspect_batch_fn fn;
	void *user;
	void (*user_free)(void *);
} inspect_batch_ctx;

static void
inspect_batch_adapt(void *vctx, const dtfl_message *msg, const uint64_t *frontier,
    size_t nfrontier)
{
	inspect_batch_ctx *c = vctx;

	(void)frontier;
	(void)nfrontier;
	if (msg != NULL)
		c->fn(c->user, dtfl_message_time(msg), msg);
}

static void
inspect_batch_free(void *vctx)
{
	inspect_batch_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_inspect_batch(dtfl_stream *st, dtfl_inspect_batch_fn fn,
    dtfl_ctx ctx)
{
	inspect_batch_ctx *c = __dtfl_alloc(sizeof *c);

	c->fn = fn;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	return dtfl_stream_inspect_core(st, inspect_batch_adapt,
	    DTFL_CTX(c, inspect_batch_free));
}

/* ------------------------------------------------------------------ */
/* inspect (inspect_batch calling the closure once per record)        */
/* ------------------------------------------------------------------ */

typedef struct inspect_ctx {
	dtfl_inspect_fn fn;
	void *user;
	void (*user_free)(void *);
} inspect_ctx;

static void
inspect_adapt(void *vctx, const uint64_t *time, const dtfl_message *msg)
{
	inspect_ctx *c = vctx;
	size_t i, n = dtfl_message_len(msg);

	for (i = 0; i < n; i++) {
		size_t len;
		const void *rec = dtfl_message_record(msg, i, &len);

		c->fn(c->user, time, rec, len);
	}
}

static void
inspect_free(void *vctx)
{
	inspect_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_inspect(dtfl_stream *st, dtfl_inspect_fn fn, dtfl_ctx ctx)
{
	inspect_ctx *c = __dtfl_alloc(sizeof *c);

	c->fn = fn;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	return dtfl_stream_inspect_batch(st, inspect_adapt,
	    DTFL_CTX(c, inspect_free));
}
