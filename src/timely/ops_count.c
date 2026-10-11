/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_count.c: count.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/vec/count.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * count is Accumulate::count: it sums the number of records at each time
 * and emits that u64 when the input frontier passes the time.  Upstream
 * does this with a notificator; here each open time keeps a retained
 * capability, and on each schedule a time whose capability the input
 * frontier no longer covers is emitted and released (the notificator's
 * for_each over times the frontier has passed).  The output type is u64.
 */
#include "timely/ops_int.h"

#include <string.h>

/* A time accumulating records, holding a capability until it is emitted. */
typedef struct count_slot {
	dtfl_cap *cap;		/* capability at the time; owned */
	uint64_t count;		/* records seen at the time */
} count_slot;

typedef struct count_ctx {
	const dtfl_time_class *tc;	/* not retained */
	unsigned words;
	count_slot *slots;		/* open times; linear scan (few open) */
	size_t n, cap;
} count_ctx;

/* Finds the slot for `time`, or creates one with a retained capability. */
static count_slot *
count_slot_for(count_ctx *c, dtfl_op *op, size_t input, const uint64_t *time)
{
	size_t i;
	size_t bytes = (size_t)c->words * sizeof(uint64_t);

	for (i = 0; i < c->n; i++)
		if (memcmp(dtfl_cap_time(c->slots[i].cap), time, bytes) == 0)
			return &c->slots[i];
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 4;
		c->slots = __dtfl_realloc(c->slots, c->cap * sizeof(*c->slots));
	}
	c->slots[c->n].cap = dtfl_op_input_cap_delayed(op, input, 0, time);
	c->slots[c->n].count = 0;
	return &c->slots[c->n++];
}

static bool
count_logic(dtfl_op *op, void *vctx)
{
	count_ctx *c = vctx;
	dtfl_message *m;
	size_t i;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		count_slot *slot = count_slot_for(c, op, 0, dtfl_message_time(m));

		slot->count += dtfl_message_len(m);
	}

	/* Emit each time the input frontier has passed, then release it. */
	for (i = 0; i < c->n;) {
		count_slot *slot = &c->slots[i];
		const uint64_t *t = dtfl_cap_time(slot->cap);

		if (dtfl_op_frontier_less_equal(op, 0, t)) {
			i++;
			continue;
		}
		{
			dtfl_session *s = dtfl_op_session(op, 0, slot->cap);
			unsigned char buf[8];

			dtfl_put_u64(buf, slot->count);
			dtfl_session_give(s, buf, sizeof buf);
			dtfl_session_close(s);
		}
		dtfl_cap_drop(slot->cap);
		c->slots[i] = c->slots[--c->n];	/* fill the hole */
	}
	return false;
}

static void
count_free(void *vctx)
{
	count_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->n; i++)
		dtfl_cap_drop(c->slots[i].cap);
	__dtfl_free(c->slots);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_count(dtfl_stream *st)
{
	dtfl_scope *scope = dtfl_stream_scope(st);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Count");
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	dtfl_stream *out;
	count_ctx *c = __dtfl_calloc(1, sizeof *c);

	c->tc = tc;
	c->words = DTFL_MAX(dtfl_time_depth(tc), 1u);
	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, count_logic, c, count_free);
	return out;
}
