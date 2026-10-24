/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/alloc_generic.c: dispatch to the allocator in use.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/generic.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Upstream's Allocator and AllocatorBuilder enums become the ops
 * vtables of __dtfl_allocator and __dtfl_allocator_builder; these
 * functions are the enum's match arms.
 */
#include "comm/comm_int.h"

void
__dtfl_allocate(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pushers, __dtfl_pull **puller)
{
	a->ops->allocate(a, identifier, mc, pushers, puller);
}

void
__dtfl_broadcast(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pusher, __dtfl_pull **puller)
{
	__dtfl_push **pushers;

	if (a->ops->broadcast != NULL) {
		a->ops->broadcast(a, identifier, mc, pusher, puller);
		return;
	}
	/* The Allocate trait's default: allocate, wrap in a Broadcaster. */
	pushers = __dtfl_calloc(a->peers, sizeof *pushers);
	a->ops->allocate(a, identifier, mc, pushers, puller);
	*pusher = __dtfl_broadcaster_new(mc, pushers, a->peers);
}

void
__dtfl_pipeline(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pusher, __dtfl_pull **puller)
{
	__dtfl_thread_new_from(identifier, mc, &a->events, pusher, puller);
}

void
__dtfl_allocator_receive(__dtfl_allocator *a)
{
	if (a->ops->receive != NULL)
		a->ops->receive(a);
}

void
__dtfl_allocator_release(__dtfl_allocator *a)
{
	if (a->ops->release != NULL)
		a->ops->release(a);
}

void
__dtfl_allocator_await_events(__dtfl_allocator *a, uint64_t timeout_ns)
{
	if (a->ops->await_events != NULL)
		a->ops->await_events(a, timeout_ns);
}

void
__dtfl_allocator_destroy(__dtfl_allocator *a)
{
	if (a != NULL)
		a->ops->destroy(a);
}

__dtfl_allocator *
__dtfl_allocator_build(__dtfl_allocator_builder *b)
{
	return b->build(b);
}
