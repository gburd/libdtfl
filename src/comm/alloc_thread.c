/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/alloc_thread.c: intra-thread communication.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/thread.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A channel is a pair of FIFOs shared by its pusher and puller (the
 * Rc<RefCell<(VecDeque<T>, VecDeque<T>)>>): queued messages, and
 * recycled ones that a push hands back.  Nothing refills the recycle
 * FIFO at the pin (the puller's return path is commented out
 * upstream), so it stays empty; it is kept so the push protocol reads
 * as upstream.  Both halves belong to one worker; the count is plain.
 */
#include "comm/comm_int.h"

typedef struct thread_shared {
	size_t refs;			/* pusher + puller; worker-local */
	__dtfl_fifo queue;		/* void *: messages in flight */
	__dtfl_fifo recycle;		/* void *: elements to hand back */
	const __dtfl_msg_class *mc;	/* frees what is left at the end */
} thread_shared;

/* Drops one reference; the last frees any messages still queued. */
static void
shared_release(thread_shared *s)
{
	void *m;
	int q;

	if (--s->refs > 0)
		return;
	for (q = 0; q < 2; q++) {
		__dtfl_fifo *f = q == 0 ? &s->queue : &s->recycle;

		while (__dtfl_fifo_pop(f, &m)) {
			DTFL_ASSERT(s->mc != NULL);
			s->mc->free(s->mc, m);
		}
		__dtfl_fifo_free(f);
	}
	__dtfl_free(s);
}

/* The push half of an intra-thread channel. */
typedef struct thread_pusher {
	__dtfl_push base;
	thread_shared *target;
} thread_pusher;

static void
thread_pusher_push(__dtfl_push *p, void **element)
{
	thread_shared *s = ((thread_pusher *)p)->target;

	if (*element != NULL)
		__dtfl_fifo_push(&s->queue, element);
	if (!__dtfl_fifo_pop(&s->recycle, element))
		*element = NULL;
}

static void
thread_pusher_destroy(__dtfl_push *p)
{
	shared_release(((thread_pusher *)p)->target);
	__dtfl_free(p);
}

static const __dtfl_push_ops thread_pusher_ops = {
	thread_pusher_push, thread_pusher_destroy
};

/* The pull half of an intra-thread channel. */
typedef struct thread_puller {
	__dtfl_pull base;
	thread_shared *source;
} thread_puller;

static void *
thread_puller_pull(__dtfl_pull *p)
{
	thread_shared *s = ((thread_puller *)p)->source;
	void *m;

	return __dtfl_fifo_pop(&s->queue, &m) ? m : NULL;
}

static void
thread_puller_destroy(__dtfl_pull *p)
{
	shared_release(((thread_puller *)p)->source);
	__dtfl_free(p);
}

static const __dtfl_pull_ops thread_puller_ops = {
	thread_puller_pull, thread_puller_destroy
};

void
__dtfl_thread_new_from(size_t identifier, const __dtfl_msg_class *mc,
    __dtfl_events *events, __dtfl_push **pusher, __dtfl_pull **puller)
{
	thread_shared *s = __dtfl_alloc(sizeof *s);
	thread_pusher *tp = __dtfl_alloc(sizeof *tp);
	thread_puller *tl = __dtfl_alloc(sizeof *tl);

	s->refs = 2;
	__dtfl_fifo_init(&s->queue, sizeof(void *));
	__dtfl_fifo_init(&s->recycle, sizeof(void *));
	s->mc = mc;
	tp->base.ops = &thread_pusher_ops;
	tp->target = s;
	tl->base.ops = &thread_puller_ops;
	tl->source = s;
	*pusher = __dtfl_count_pusher_new(&tp->base, identifier, events);
	*puller = __dtfl_count_puller_new(&tl->base, identifier, events);
}

/* An allocator for intra-thread communication. */
static void
thread_allocate(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pushers, __dtfl_pull **puller)
{
	__dtfl_thread_new_from(identifier, mc, &a->events, &pushers[0], puller);
}

static void
thread_destroy(__dtfl_allocator *a)
{
	__dtfl_allocator_fini(a);
	__dtfl_free(a);
}

static const __dtfl_allocator_ops thread_ops = {
	.allocate = thread_allocate,
	.await_events = __dtfl_allocator_park_if_idle,
	.destroy = thread_destroy,
};

/* Builder for the single-threaded allocator. */
static __dtfl_allocator *
thread_build(__dtfl_allocator_builder *b)
{
	__dtfl_allocator *a = __dtfl_alloc(sizeof *a);

	__dtfl_free(b);
	__dtfl_allocator_init(a, &thread_ops, 0, 1, __dtfl_parker_new());
	return a;
}

static void
thread_builder_destroy(__dtfl_allocator_builder *b)
{
	__dtfl_free(b);
}

__dtfl_allocator_builder *
__dtfl_thread_builder(void)
{
	__dtfl_allocator_builder *b = __dtfl_alloc(sizeof *b);

	b->build = thread_build;
	b->destroy = thread_builder_destroy;
	return b;
}
