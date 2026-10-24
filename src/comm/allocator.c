/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/allocator.c: Push/Pull helpers, the Broadcaster, and the
 * parts of Allocate every allocator shares.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/mod.rs
 *   communication/src/lib.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 */
#include "comm/comm_int.h"

void
__dtfl_push_send(__dtfl_push *p, const __dtfl_msg_class *mc, void *msg)
{
	void *m = msg;

	p->ops->push(p, &m);
	/* Rust drops whatever the pusher handed back. */
	if (m != NULL) {
		DTFL_ASSERT(mc != NULL);
		mc->free(mc, m);
	}
}

void
__dtfl_push_done(__dtfl_push *p)
{
	void *m = NULL;

	p->ops->push(p, &m);
	if (m != NULL)
		__dtfl_panic("pusher returned an element from done()");
}

void
__dtfl_push_destroy(__dtfl_push *p)
{
	if (p != NULL)
		p->ops->destroy(p);
}

void
__dtfl_pull_destroy(__dtfl_pull *p)
{
	if (p != NULL)
		p->ops->destroy(p);
}

/*
 * An adapter to broadcast any pushed element.  Owns its pushers.
 * Upstream keeps `spare` between pushes so clone_from can reuse its
 * allocation; a class clone always allocates, so a recycled spare is
 * freed at once instead.
 */
typedef struct broadcaster {
	__dtfl_push base;
	const __dtfl_msg_class *mc;
	__dtfl_push **pushers;
	size_t n;
} broadcaster;

static void
broadcaster_push(__dtfl_push *p, void **msg)
{
	broadcaster *b = (broadcaster *)p;
	void *spare;
	size_t i;

	/* Push defensive copies to pushers after the first. */
	for (i = 1; i < b->n; i++) {
		spare = *msg != NULL ? b->mc->clone(b->mc, *msg) : NULL;
		b->pushers[i]->ops->push(b->pushers[i], &spare);
		if (spare != NULL)
			b->mc->free(b->mc, spare);
	}
	/* Push the element itself at the first pusher. */
	if (b->n > 0)
		b->pushers[0]->ops->push(b->pushers[0], msg);
}

static void
broadcaster_destroy(__dtfl_push *p)
{
	broadcaster *b = (broadcaster *)p;
	size_t i;

	for (i = 0; i < b->n; i++)
		__dtfl_push_destroy(b->pushers[i]);
	__dtfl_free(b->pushers);
	__dtfl_free(b);
}

static const __dtfl_push_ops broadcaster_ops = {
	broadcaster_push, broadcaster_destroy
};

__dtfl_push *
__dtfl_broadcaster_new(const __dtfl_msg_class *mc, __dtfl_push **pushers,
    size_t n)
{
	broadcaster *b = __dtfl_alloc(sizeof *b);

	b->base.ops = &broadcaster_ops;
	b->mc = mc;
	b->pushers = pushers;
	b->n = n;
	return &b->base;
}

void
__dtfl_allocator_init(__dtfl_allocator *a, const __dtfl_allocator_ops *ops,
    size_t index, size_t peers, __dtfl_parker *parker)
{
	DTFL_ASSERT(index < peers);
	a->ops = ops;
	a->index = index;
	a->peers = peers;
	__dtfl_vec_init(&a->events.ids, sizeof(size_t));
	a->parker = parker;
}

void
__dtfl_allocator_fini(__dtfl_allocator *a)
{
	__dtfl_vec_free(&a->events.ids);
	__dtfl_parker_release(a->parker);
	a->parker = NULL;
}

void
__dtfl_allocator_park_if_idle(__dtfl_allocator *a, uint64_t timeout_ns)
{
	if (a->events.ids.len == 0)
		__dtfl_parker_park(a->parker, timeout_ns);
}
