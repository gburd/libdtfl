/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/pull_counter.c: counts records pulled into consumed.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/channels/pullers/counter.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A pulling Counter wraps a puller and records each message's record
 * count into a shared consumed change batch (time -> +record_count).
 * Upstream records this through a ConsumedGuard that updates on drop, so
 * the count lands when the message is released, not when it is read.
 * Here the guard is the held message: pulling the next one, or finishing
 * the handle, records the previous message's consumption and frees it.
 * The consumed batch belongs to the operator and outlives the counter.
 */
#include "timely/timely_int.h"

struct __dtfl_pull_counter {
	__dtfl_pull *pullable;		/* owned */
	__dtfl_change_batch *consumed;	/* not owned */
	dtfl_message *held;		/* current message; owned until released */
};

__dtfl_pull_counter *
__dtfl_pull_counter_new(__dtfl_pull *inner, __dtfl_change_batch *consumed)
{
	__dtfl_pull_counter *c = __dtfl_alloc(sizeof *c);

	c->pullable = inner;
	c->consumed = consumed;
	c->held = NULL;
	return c;
}

/* Records the held message's consumption and frees it (the guard drop). */
static void
release_held(__dtfl_pull_counter *c)
{
	if (c->held != NULL) {
		int64_t count = (int64_t)__dtfl_batch_len(c->held->data);

		__dtfl_cb_update(c->consumed, c->held->time, count);
		__dtfl_message_free(c->held);
		c->held = NULL;
	}
}

dtfl_message *
__dtfl_pull_counter_next(__dtfl_pull_counter *c)
{
	release_held(c);
	c->held = __dtfl_pull_recv(c->pullable);
	return c->held;
}

void
__dtfl_pull_counter_finish(__dtfl_pull_counter *c)
{
	release_held(c);
}

void
__dtfl_pull_counter_free(__dtfl_pull_counter *c)
{
	if (c == NULL)
		return;
	release_held(c);
	__dtfl_pull_destroy(c->pullable);
	__dtfl_free(c);
}

void
__dtfl_pull_counter_set_consumed(__dtfl_pull_counter *c,
    __dtfl_change_batch *consumed)
{
	c->consumed = consumed;
}
