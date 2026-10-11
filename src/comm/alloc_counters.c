/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/alloc_counters.c: Push and Pull wrappers that maintain the
 * worker's channel events.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/counters.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 */
#include "comm/comm_int.h"

/* The push half of an intra-thread channel. */
typedef struct count_pusher {
	__dtfl_push base;
	size_t index;
	__dtfl_events *events;	/* the worker's; outlives the pusher */
	__dtfl_push *pusher;	/* owned */
} count_pusher;

static void
count_pusher_push(__dtfl_push *p, void **element)
{
	count_pusher *c = (count_pusher *)p;

	/*
	 * Upstream notes a less chatty version (count, report on flush) is
	 * possible but can be late in moving information along.
	 */
	__dtfl_events_push(c->events, c->index);
	c->pusher->ops->push(c->pusher, element);
}

static void
count_pusher_destroy(__dtfl_push *p)
{
	count_pusher *c = (count_pusher *)p;

	__dtfl_push_destroy(c->pusher);
	__dtfl_free(c);
}

static const __dtfl_push_ops count_pusher_ops = {
	count_pusher_push, count_pusher_destroy
};

__dtfl_push *
__dtfl_count_pusher_new(__dtfl_push *inner, size_t identifier,
    __dtfl_events *events)
{
	count_pusher *c = __dtfl_alloc(sizeof *c);

	c->base.ops = &count_pusher_ops;
	c->index = identifier;
	c->events = events;
	c->pusher = inner;
	return &c->base;
}

/* The push half of an inter-thread channel. */
typedef struct arc_pusher {
	__dtfl_push base;
	size_t index;
	__dtfl_mpsc *events;	/* the target worker's id queue; retained */
	__dtfl_push *pusher;	/* owned */
	__dtfl_parker *buzzer;	/* the target worker's parker; retained */
} arc_pusher;

static void
arc_pusher_push(__dtfl_push *p, void **element)
{
	arc_pusher *c = (arc_pusher *)p;
	size_t id = c->index;

	/*
	 * These three calls should happen in this order, to ensure that
	 * we first enqueue data, second enqueue interest in the channel,
	 * and finally awaken the thread.  Other orders are defective when
	 * multiple threads are involved.
	 */
	c->pusher->ops->push(c->pusher, element);
	/* A send to a departed worker fails; not an error in shutdown. */
	(void)__dtfl_mpsc_send(c->events, &id);
	__dtfl_parker_unpark(c->buzzer);
}

static void
arc_pusher_destroy(__dtfl_push *p)
{
	arc_pusher *c = (arc_pusher *)p;

	__dtfl_push_destroy(c->pusher);
	__dtfl_mpsc_release(c->events);
	__dtfl_parker_release(c->buzzer);
	__dtfl_free(c);
}

static const __dtfl_push_ops arc_pusher_ops = {
	arc_pusher_push, arc_pusher_destroy
};

__dtfl_push *
__dtfl_arc_pusher_new(__dtfl_push *inner, size_t identifier,
    __dtfl_mpsc *ids, __dtfl_parker *parker)
{
	arc_pusher *c = __dtfl_alloc(sizeof *c);

	c->base.ops = &arc_pusher_ops;
	c->index = identifier;
	c->events = __dtfl_mpsc_retain(ids);
	c->pusher = inner;
	c->buzzer = __dtfl_parker_retain(parker);
	return &c->base;
}

/* The pull half of a channel. */
typedef struct count_puller {
	__dtfl_pull base;
	size_t index;
	size_t count;
	__dtfl_events *events;	/* the worker's; outlives the puller */
	__dtfl_pull *puller;	/* owned */
} count_puller;

static void *
count_puller_pull(__dtfl_pull *p)
{
	count_puller *c = (count_puller *)p;
	void *result = c->puller->ops->pull(c->puller);

	if (result == NULL) {
		if (c->count != 0) {
			__dtfl_events_push(c->events, c->index);
			c->count = 0;
		}
	} else {
		c->count++;
	}
	return result;
}

static void
count_puller_destroy(__dtfl_pull *p)
{
	count_puller *c = (count_puller *)p;

	__dtfl_pull_destroy(c->puller);
	__dtfl_free(c);
}

static const __dtfl_pull_ops count_puller_ops = {
	count_puller_pull, count_puller_destroy
};

__dtfl_pull *
__dtfl_count_puller_new(__dtfl_pull *inner, size_t identifier,
    __dtfl_events *events)
{
	count_puller *c = __dtfl_alloc(sizeof *c);

	c->base.ops = &count_puller_ops;
	c->index = identifier;
	c->count = 0;
	c->events = events;
	c->puller = inner;
	return &c->base;
}
