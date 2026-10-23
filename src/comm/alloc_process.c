/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/alloc_process.c: typed inter-thread, intra-process channels.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/process.rs
 *   communication/src/lib.rs (promise_futures)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Messages move as pointers; nothing is encoded.  Each channel
 * identifier has one MPSC queue per receiving worker, shared by every
 * sender, exactly as upstream's per-channel Vec of mpsc channels.  An
 * MPSC queue keeps each sender's order, which gives the per (sender,
 * receiver) FIFO order timely needs.
 *
 * Shared state (one `process_shared` per group of `peers` workers):
 *   - `channels`: the rendezvous.  The first worker to allocate an
 *     identifier creates its queues; each worker takes its endpoints
 *     once; the entry goes when all have.  Protected by `mu`.
 *   - `counters[i]`: worker i's queue of channel ids with arrivals
 *     (upstream counters_send / counters_recv).
 *   - `buzzers[i]`: worker i's parker.
 * `counters` and `buzzers` are written once in __dtfl_process_builders
 * and only read afterwards, so they need no lock; the thread that
 * receives a builder is ordered after its creation by whatever handed
 * the builder over (thread creation, in practice).
 */
#include "comm/comm_int.h"

typedef struct process_entry {
	size_t identifier;
	const __dtfl_msg_class *mc;
	__dtfl_mpsc **queues;	/* [peers], one reference each */
	bool *taken;		/* [peers] */
	size_t remaining;	/* workers yet to take their endpoints */
} process_entry;

typedef struct process_shared {
	/* Builders and allocators.  acq_rel decrement; see buzzer.c. */
	atomic_size_t refs;
	size_t peers;
	__dtfl_mutex mu;
	/*
	 * process_entry *, under mu.  A linear search: entries live only
	 * between the first and last worker's allocate, so the list is
	 * short.
	 */
	__dtfl_vec channels;
	__dtfl_mpsc **counters;		/* [peers], immutable */
	__dtfl_parker **buzzers;	/* [peers], immutable */
} process_shared;

static void
entry_free(process_entry *e, size_t peers)
{
	size_t i;

	for (i = 0; i < peers; i++)
		__dtfl_mpsc_release(e->queues[i]);
	__dtfl_free(e->queues);
	__dtfl_free(e->taken);
	__dtfl_free(e);
}

static void
shared_release(process_shared *s)
{
	size_t i;

	if (atomic_fetch_sub_explicit(&s->refs, 1, memory_order_acq_rel) != 1)
		return;
	for (i = 0; i < s->channels.len; i++)
		entry_free(*DTFL_VEC_AT(&s->channels, process_entry *, i),
		    s->peers);
	__dtfl_vec_free(&s->channels);
	for (i = 0; i < s->peers; i++) {
		__dtfl_mpsc_release(s->counters[i]);
		__dtfl_parker_release(s->buzzers[i]);
	}
	__dtfl_free(s->counters);
	__dtfl_free(s->buzzers);
	__dtfl_mutex_destroy(&s->mu);
	__dtfl_free(s);
}

/* Drops a queued message the receiver will never take. */
static void
drop_message(void *elem, void *ctx)
{
	const __dtfl_msg_class *mc = ctx;

	mc->free(mc, *(void **)elem);
}

/* The push half of an intra-process channel. */
typedef struct process_pusher {
	__dtfl_push base;
	__dtfl_mpsc *target;	/* one reference */
	const __dtfl_msg_class *mc;
} process_pusher;

static void
process_pusher_push(__dtfl_push *p, void **element)
{
	process_pusher *pp = (process_pusher *)p;

	if (*element != NULL) {
		/*
		 * The remote endpoint could be shut down, and so it is not
		 * fundamentally an error to fail to send.  The message is
		 * dropped, as Rust drops the value a failed send returns.
		 */
		if (!__dtfl_mpsc_send(pp->target, element))
			pp->mc->free(pp->mc, *element);
		*element = NULL;
	}
}

static void
process_pusher_destroy(__dtfl_push *p)
{
	__dtfl_mpsc_release(((process_pusher *)p)->target);
	__dtfl_free(p);
}

static const __dtfl_push_ops process_pusher_ops = {
	process_pusher_push, process_pusher_destroy
};

/* The pull half of an intra-process channel. */
typedef struct process_puller {
	__dtfl_pull base;
	__dtfl_mpsc *source;	/* one reference; this worker receives */
} process_puller;

static void *
process_puller_pull(__dtfl_pull *p)
{
	void *m;

	return __dtfl_mpsc_recv(((process_puller *)p)->source, &m) ? m : NULL;
}

static void
process_puller_destroy(__dtfl_pull *p)
{
	process_puller *pl = (process_puller *)p;

	/* The Receiver drops: queued messages go, later sends fail. */
	__dtfl_mpsc_close(pl->source);
	__dtfl_mpsc_release(pl->source);
	__dtfl_free(pl);
}

static const __dtfl_pull_ops process_puller_ops = {
	process_puller_pull, process_puller_destroy
};

/* An allocator for inter-thread, intra-process communication. */
typedef struct process_alloc {
	__dtfl_allocator base;
	process_shared *shared;	/* one reference */
} process_alloc;

static void
process_allocate(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pushers, __dtfl_pull **puller)
{
	process_alloc *pa = (process_alloc *)a;
	process_shared *s = pa->shared;
	process_entry *e = NULL;
	__dtfl_mpsc **queues;
	size_t i, slot = 0;
	bool empty;

	/*
	 * This is race-y global initialisation of all channels for all
	 * workers, performed by the first worker that enters this critical
	 * section.
	 */
	__dtfl_mutex_lock(&s->mu);
	for (i = 0; i < s->channels.len; i++) {
		e = *DTFL_VEC_AT(&s->channels, process_entry *, i);
		if (e->identifier == identifier) {
			slot = i;
			break;
		}
		e = NULL;
	}
	if (e == NULL) {
		/* We may need to alloc a new channel ... */
		e = __dtfl_alloc(sizeof *e);
		e->identifier = identifier;
		e->mc = mc;
		e->queues = __dtfl_calloc(s->peers, sizeof *e->queues);
		e->taken = __dtfl_calloc(s->peers, sizeof *e->taken);
		e->remaining = s->peers;
		for (i = 0; i < s->peers; i++)
			e->queues[i] = __dtfl_mpsc_new(sizeof(void *),
			    drop_message, (void *)mc);
		slot = s->channels.len;
		__dtfl_vec_push(&s->channels, &e);
	}
	if (e->mc != mc) {
		__dtfl_mutex_unlock(&s->mu);
		__dtfl_panic("failed to correctly cast channel %zu", identifier);
	}
	if (e->taken[a->index]) {
		__dtfl_mutex_unlock(&s->mu);
		__dtfl_panic("channel %zu already consumed", identifier);
	}
	e->taken[a->index] = true;
	/* One reference per pusher, and one for our puller. */
	for (i = 0; i < s->peers; i++)
		(void)__dtfl_mpsc_retain(e->queues[i]);
	(void)__dtfl_mpsc_retain(e->queues[a->index]);
	/*
	 * Copy the queue pointers: once we unlock, the last worker to take
	 * its endpoints may free the entry.
	 */
	queues = __dtfl_memdup(e->queues, s->peers * sizeof *queues);
	empty = --e->remaining == 0;
	if (empty)
		__dtfl_vec_swap_remove(&s->channels, slot);
	__dtfl_mutex_unlock(&s->mu);

	/* sends: one per target worker; recv: this worker's queue. */
	for (i = 0; i < s->peers; i++) {
		process_pusher *pp = __dtfl_alloc(sizeof *pp);

		pp->base.ops = &process_pusher_ops;
		pp->target = queues[i];
		pp->mc = mc;
		pushers[i] = __dtfl_arc_pusher_new(&pp->base, identifier,
		    s->counters[i], s->buzzers[i]);
	}
	{
		process_puller *pl = __dtfl_alloc(sizeof *pl);

		pl->base.ops = &process_puller_ops;
		pl->source = queues[a->index];
		*puller = __dtfl_count_puller_new(&pl->base, identifier,
		    &a->events);
	}
	__dtfl_free(queues);
	/* The entry's own references go with the last worker's take. */
	if (empty)
		entry_free(e, s->peers);
}

static void
process_receive(__dtfl_allocator *a)
{
	process_alloc *pa = (process_alloc *)a;

	__dtfl_mpsc_drain(pa->shared->counters[a->index], &a->events.ids);
}

static void
process_destroy(__dtfl_allocator *a)
{
	process_alloc *pa = (process_alloc *)a;

	/* Our counters Receiver drops; late ArcPusher sends are ignored. */
	__dtfl_mpsc_close(pa->shared->counters[a->index]);
	shared_release(pa->shared);
	__dtfl_allocator_fini(a);
	__dtfl_free(pa);
}

static const __dtfl_allocator_ops process_ops = {
	.allocate = process_allocate,
	.receive = process_receive,
	.await_events = __dtfl_allocator_park_if_idle,
	.destroy = process_destroy,
};

typedef struct process_builder {
	__dtfl_allocator_builder base;
	process_shared *shared;	/* one reference */
	size_t index;
} process_builder;

static __dtfl_allocator *
process_build(__dtfl_allocator_builder *b)
{
	process_builder *pb = (process_builder *)b;
	process_alloc *pa = __dtfl_alloc(sizeof *pa);

	__dtfl_allocator_init(&pa->base, &process_ops, pb->index,
	    pb->shared->peers, __dtfl_parker_retain(pb->shared->buzzers[pb->index]));
	pa->shared = pb->shared;	/* the builder's reference moves */
	__dtfl_free(pb);
	return &pa->base;
}

static void
process_builder_destroy(__dtfl_allocator_builder *b)
{
	process_builder *pb = (process_builder *)b;

	/* An unbuilt worker never receives: refuse sends to it. */
	__dtfl_mpsc_close(pb->shared->counters[pb->index]);
	shared_release(pb->shared);
	__dtfl_free(pb);
}

void
__dtfl_process_builders(size_t peers, __dtfl_allocator_builder **builders)
{
	__dtfl_process_builders_parked(peers, NULL, builders);
}

void
__dtfl_process_builders_parked(size_t peers, __dtfl_parker *const *parkers,
    __dtfl_allocator_builder **builders)
{
	process_shared *s = __dtfl_alloc(sizeof *s);
	size_t i;

	DTFL_ASSERT(peers > 0);
	atomic_init(&s->refs, peers);
	s->peers = peers;
	__dtfl_mutex_init(&s->mu);
	__dtfl_vec_init(&s->channels, sizeof(process_entry *));
	s->counters = __dtfl_calloc(peers, sizeof *s->counters);
	s->buzzers = __dtfl_calloc(peers, sizeof *s->buzzers);
	for (i = 0; i < peers; i++) {
		process_builder *pb = __dtfl_alloc(sizeof *pb);

		s->counters[i] = __dtfl_mpsc_new(sizeof(size_t), NULL, NULL);
		s->buzzers[i] = parkers != NULL ?
		    __dtfl_parker_retain(parkers[i]) : __dtfl_parker_new();
		pb->base.build = process_build;
		pb->base.destroy = process_builder_destroy;
		pb->shared = s;
		pb->index = i;
		builders[i] = &pb->base;
	}
}
