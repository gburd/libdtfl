/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/push_counter.c: counts records pushed into produced.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/channels/pushers/counter.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A pushing Counter updates a shared produced change batch
 * (time -> +record_count) on each data push, then forwards to its
 * pushee.  A flush (NULL) is forwarded only when produced is dirty, so
 * an idle output does not announce progress.  The produced batch belongs
 * to the operator and outlives the pusher.
 */
#include "timely/timely_int.h"

typedef struct push_counter {
	__dtfl_push base;
	__dtfl_push *pushee;		/* owned */
	__dtfl_change_batch *produced;	/* not owned */
} push_counter;

static void
counter_push(__dtfl_push *p, void **msg)
{
	push_counter *c = (push_counter *)p;

	if (*msg != NULL) {
		dtfl_message *m = *msg;
		int64_t count = (int64_t)__dtfl_batch_len(m->data);

		__dtfl_cb_update(c->produced, m->time, count);
	}
	/* only propagate NULL (flush) if dirty */
	if (*msg != NULL || !__dtfl_cb_is_empty(c->produced))
		__dtfl_push_push(c->pushee, msg);
}

static void
counter_destroy(__dtfl_push *p)
{
	push_counter *c = (push_counter *)p;

	__dtfl_push_destroy(c->pushee);
	__dtfl_free(c);
}

static const __dtfl_push_ops counter_ops = { counter_push, counter_destroy };

__dtfl_push *
__dtfl_push_counter_new(__dtfl_push *inner, __dtfl_change_batch *produced)
{
	push_counter *c = __dtfl_alloc(sizeof *c);

	c->base.ops = &counter_ops;
	c->pushee = inner;
	c->produced = produced;
	return &c->base;
}
