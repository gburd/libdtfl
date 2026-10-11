/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/allocator_process.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/zc_allocator_process.c: the ProcessBinary allocator, a
 * serializing allocator for inter-thread, intra-process communication.
 *
 * Each pair of workers (i, j) shares one MergeQueue pair from i to j:
 * worker i's sends[j] stages and publishes into the writer, worker j's
 * recvs[i] drains the reader.  Builders exchange the writers through a
 * rendezvous (upstream's promise_futures mpsc channels): builder j
 * creates every queue into j, keeps the readers, and posts the writer
 * for i -> j in slot [i][j]; builder i then takes slots [i][*].
 *
 * Threads.  The rendezvous is the only state builders share, and its
 * mutex guards the slots.  Once built, an allocator and everything it
 * hands out belong to its worker thread; the MergeQueues document their
 * own synchronization (zc_bytes_exchange.c).  Each queue into worker j
 * buzzes j's parker, which the queue retains until both handles drop.
 */
#include "comm/zc_int.h"

/* ------------------------------------------------------------------ */
/* Rendezvous and builders                                             */
/* ------------------------------------------------------------------ */

typedef struct rendezvous {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	size_t peers;
	size_t refs;		/* unbuilt builders; guarded */
	bool abandoned;		/* a builder died unbuilt; guarded */
	__dtfl_merge_queue **slot;	/* [from * peers + to]; guarded */
} rendezvous;

typedef struct process_builder {
	__dtfl_allocator_builder base;
	rendezvous *rv;
	size_t index;
} process_builder;

/* Drops one builder's reference; the last frees untaken writers. */
static void
rendezvous_release(rendezvous *rv)
{
	size_t i, left;

	(void)pthread_mutex_lock(&rv->lock);
	left = --rv->refs;
	(void)pthread_mutex_unlock(&rv->lock);
	if (left > 0)
		return;
	for (i = 0; i < rv->peers * rv->peers; i++)
		__dtfl_merge_queue_drop(rv->slot[i], false);
	__dtfl_free(rv->slot);
	(void)pthread_cond_destroy(&rv->cond);
	(void)pthread_mutex_destroy(&rv->lock);
	__dtfl_free(rv);
}

/* ------------------------------------------------------------------ */
/* Channel map (HashMap<usize, Rc<RefCell<VecDeque<Bytes>>>>)          */
/* ------------------------------------------------------------------ */

typedef struct to_local_entry {
	size_t channel;
	__dtfl_bytes_fifo *fifo;	/* one reference */
} to_local_entry;

/*
 * A vector sorted by channel with binary search.  Channels are
 * allocated in increasing order, so inserts append; a hash map is the
 * upgrade if many channels are live and arrive out of order.
 */
static size_t
to_local_find(const __dtfl_vec *m, size_t channel, bool *found)
{
	size_t lo = 0, hi = m->len, mid;

	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		if (DTFL_VEC_AT(m, to_local_entry, mid)->channel < channel)
			lo = mid + 1;
		else
			hi = mid;
	}
	*found = lo < m->len &&
	    DTFL_VEC_AT(m, to_local_entry, lo)->channel == channel;
	return lo;
}

/* Inserts a new, empty FIFO for `channel` at position `at`. */
static __dtfl_bytes_fifo *
to_local_insert(__dtfl_vec *m, size_t at, size_t channel)
{
	to_local_entry e = { channel, __dtfl_bytes_fifo_new() };
	to_local_entry *base;

	__dtfl_vec_push(m, &e);
	base = (to_local_entry *)m->data;
	memmove(base + at + 1, base + at, (m->len - 1 - at) * sizeof e);
	base[at] = e;
	return e.fifo;
}

/* ------------------------------------------------------------------ */
/* ProcessAllocator                                                    */
/* ------------------------------------------------------------------ */

typedef struct zc_process {
	__dtfl_allocator base;
	__dtfl_zc_canaries *canaries;	/* one reference */
	bool has_bound;			/* channel_id_bound is Some */
	size_t channel_id_bound;
	__dtfl_vec staged;		/* __dtfl_bytes */
	__dtfl_send_endpoint **sends;	/* sends[x] -> goes to thread x */
	__dtfl_merge_queue **recvs;	/* recvs[x] <- from thread x */
	__dtfl_vec to_local;		/* to_local_entry, sorted */
} zc_process;

/* MergeQueue buzz: unparks the receiving worker. */
static void
buzz_parker(void *ctx)
{
	__dtfl_parker_unpark(ctx);
}

/* MergeQueue ctx_drop: releases the queue's parker reference. */
static void
drop_parker(void *ctx)
{
	__dtfl_parker_release(ctx);
}

/*
 * Allocate::allocate.  Identifiers must strictly increase.  Returns one
 * pusher per target worker and a puller over this worker's FIFO for the
 * channel, which receive() may already have created.
 */
static void
zc_process_allocate(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pushers, __dtfl_pull **puller)
{
	zc_process *p = (zc_process *)a;
	__dtfl_message_header header;
	__dtfl_bytes_fifo *fifo;
	size_t target, at;
	bool found;

	/* Assume and enforce in-order identifier allocation. */
	if (p->has_bound && !(p->channel_id_bound < identifier))
		__dtfl_panic("ProcessBinary: channel %zu allocated after %zu",
		    identifier, p->channel_id_bound);
	p->has_bound = true;
	p->channel_id_bound = identifier;

	for (target = 0; target < a->peers; target++) {
		header.channel = identifier;
		header.source = a->index;
		header.target_lower = target;
		header.target_upper = target + 1;
		header.length = 0;
		header.seqno = 0;
		pushers[target] = __dtfl_zc_pusher_new(&header,
		    p->sends[target], mc);
	}

	at = to_local_find(&p->to_local, identifier, &found);
	fifo = found ? DTFL_VEC_AT(&p->to_local, to_local_entry, at)->fifo :
	    to_local_insert(&p->to_local, at, identifier);
	*puller = __dtfl_zc_puller_new(fifo, mc, identifier, &a->events,
	    p->canaries);
}

/*
 * Moves arrivals into the per-channel FIFOs.  Each drained view holds
 * an integral number of messages; none splits across views.
 */
static void
zc_process_receive(__dtfl_allocator *a)
{
	zc_process *p = (zc_process *)a;
	__dtfl_vec *dropped = &p->canaries->dropped;
	__dtfl_message_header header;
	__dtfl_bytes bytes, peel, head;
	size_t i, at;
	bool found;

	/* Retire channels whose Puller has been dropped. */
	for (i = 0; i < dropped->len; i++) {
		size_t ch = *DTFL_VEC_AT(dropped, size_t, i);

		at = to_local_find(&p->to_local, ch, &found);
		if (!found)
			__dtfl_panic("non-existent channel dropped");
		/*
		 * The FIFO may be non-empty if the dataflow was forcibly
		 * dropped: every worker drops it too, without waiting on it.
		 */
		__dtfl_bytes_fifo_release(DTFL_VEC_AT(&p->to_local, to_local_entry,
		    at)->fifo);
		__dtfl_vec_remove(&p->to_local, at);
	}
	__dtfl_vec_clear(dropped);

	for (i = 0; i < a->peers; i++)
		__dtfl_merge_queue_drain_into(p->recvs[i], &p->staged);

	for (i = 0; i < p->staged.len; i++) {
		bytes = *DTFL_VEC_AT(&p->staged, __dtfl_bytes, i);
		while (bytes.len > 0) {
			/*
			 * Upstream prints "failed to read full header!" and
			 * spins forever; a short frame is a contract violation.
			 */
			if (!__dtfl_header_try_read(&header, bytes.ptr,
			    bytes.len))
				__dtfl_panic("failed to read full header!");

			/* Get the header and payload, ditch the header. */
			__dtfl_bytes_extract_to(&bytes,
			    __dtfl_header_required_bytes(&header), &peel);
			__dtfl_bytes_extract_to(&peel,
			    __dtfl_header_header_bytes(&header), &head);
			__dtfl_bytes_release(&head);

			/* Safe to count even if the channel was dropped. */
			__dtfl_events_push(&a->events, header.channel);

			at = to_local_find(&p->to_local, header.channel,
			    &found);
			if (found) {
				__dtfl_bytes_fifo_push(DTFL_VEC_AT(&p->to_local,
				    to_local_entry, at)->fifo, &peel);
			} else if (!p->has_bound ||
			    p->channel_id_bound < header.channel) {
				/* Data may arrive before allocation. */
				__dtfl_bytes_fifo_push(to_local_insert(&p->to_local,
				    at, header.channel), &peel);
			} else {
				/* A channel already dropped here. */
				__dtfl_bytes_release(&peel);
			}
		}
		__dtfl_bytes_release(&bytes);
	}
	__dtfl_vec_clear(&p->staged);
}

/* Publishes outgoing byte ledgers. */
static void
zc_process_release(__dtfl_allocator *a)
{
	zc_process *p = (zc_process *)a;
	size_t i;

	for (i = 0; i < a->peers; i++)
		__dtfl_send_endpoint_publish(p->sends[i]);
}

/* Parks unless events are already pending. */
static void
zc_process_await_events(__dtfl_allocator *a, uint64_t timeout_ns)
{
	if (a->events.ids.len == 0)
		__dtfl_parker_park(a->parker, timeout_ns);
}

/*
 * Drops the allocator.  Pushers and pullers still alive keep their
 * endpoints, FIFOs and canaries (each is counted), but a puller must
 * not pull after this, because it reports to the allocator's events.
 */
static void
zc_process_destroy(__dtfl_allocator *a)
{
	zc_process *p = (zc_process *)a;
	size_t i;

	for (i = 0; i < a->peers; i++) {
		__dtfl_send_endpoint_release(p->sends[i]);
		__dtfl_merge_queue_drop(p->recvs[i], false);
	}
	__dtfl_free(p->sends);
	__dtfl_free(p->recvs);
	for (i = 0; i < p->to_local.len; i++)
		__dtfl_bytes_fifo_release(DTFL_VEC_AT(&p->to_local, to_local_entry,
		    i)->fifo);
	__dtfl_vec_free(&p->to_local);
	for (i = 0; i < p->staged.len; i++)
		__dtfl_bytes_release(DTFL_VEC_AT(&p->staged, __dtfl_bytes, i));
	__dtfl_vec_free(&p->staged);
	if (--p->canaries->rc == 0) {
		__dtfl_vec_free(&p->canaries->dropped);
		__dtfl_free(p->canaries);
	}
	__dtfl_vec_free(&a->events.ids);
	__dtfl_parker_release(a->parker);
	__dtfl_free(p);
}

static const __dtfl_allocator_ops zc_process_ops = {
	.allocate = zc_process_allocate,
	.receive = zc_process_receive,
	.release = zc_process_release,
	.await_events = zc_process_await_events,
	.destroy = zc_process_destroy,
};

/*
 * ProcessBuilder::build, on the worker thread: creates the queues into
 * this worker, posts their writers, then waits for the writers out of
 * this worker.  Blocks until every peer builder has posted, or panics if
 * one was destroyed unbuilt (upstream's recv() error).
 */
static __dtfl_allocator *
process_builder_build(__dtfl_allocator_builder *bb)
{
	process_builder *b = (process_builder *)bb;
	rendezvous *rv = b->rv;
	size_t peers = rv->peers, me = b->index, r;
	zc_process *p = __dtfl_calloc(1, sizeof *p);
	__dtfl_merge_queue *writer, *reader;
	bool abandoned = false;

	p->base.ops = &zc_process_ops;
	p->base.index = me;
	p->base.peers = peers;
	__dtfl_vec_init(&p->base.events.ids, sizeof(size_t));
	p->base.parker = __dtfl_parker_new();
	p->canaries = __dtfl_alloc(sizeof *p->canaries);
	p->canaries->rc = 1;
	__dtfl_vec_init(&p->canaries->dropped, sizeof(size_t));
	__dtfl_vec_init(&p->staged, sizeof(__dtfl_bytes));
	__dtfl_vec_init(&p->to_local, sizeof(to_local_entry));
	p->sends = __dtfl_calloc(peers, sizeof *p->sends);
	p->recvs = __dtfl_calloc(peers, sizeof *p->recvs);

	/* Fulfill puller obligations: queues r -> me, buzzing my parker. */
	for (r = 0; r < peers; r++) {
		__dtfl_merge_queue_new_pair(buzz_parker,
		    __dtfl_parker_retain(p->base.parker), drop_parker, NULL,
		    NULL, &writer, &reader);
		p->recvs[r] = reader;
		(void)pthread_mutex_lock(&rv->lock);
		rv->slot[r * peers + me] = writer;
		(void)pthread_cond_broadcast(&rv->cond);
		(void)pthread_mutex_unlock(&rv->lock);
	}

	/* Extract pusher commitments: queues me -> r. */
	(void)pthread_mutex_lock(&rv->lock);
	for (r = 0; r < peers && !abandoned; r++) {
		while (rv->slot[me * peers + r] == NULL && !rv->abandoned)
			(void)pthread_cond_wait(&rv->cond, &rv->lock);
		if (rv->slot[me * peers + r] == NULL) {
			abandoned = true;
			break;
		}
		p->sends[r] =
		    __dtfl_send_endpoint_new(rv->slot[me * peers + r]);
		rv->slot[me * peers + r] = NULL;
	}
	(void)pthread_mutex_unlock(&rv->lock);
	if (abandoned)
		__dtfl_panic("Failed to receive MergeQueue");

	rendezvous_release(rv);
	__dtfl_free(b);
	return &p->base;
}

/* Drops an unbuilt builder; peers waiting on its queues panic. */
static void
process_builder_destroy(__dtfl_allocator_builder *bb)
{
	process_builder *b = (process_builder *)bb;
	rendezvous *rv = b->rv;

	(void)pthread_mutex_lock(&rv->lock);
	rv->abandoned = true;
	(void)pthread_cond_broadcast(&rv->cond);
	(void)pthread_mutex_unlock(&rv->lock);
	rendezvous_release(rv);
	__dtfl_free(b);
}

/* ProcessBuilder::new_vector: `peers` builders sharing one rendezvous. */
void
__dtfl_process_binary_builders(size_t peers,
    __dtfl_allocator_builder **builders)
{
	rendezvous *rv = __dtfl_alloc(sizeof *rv);
	size_t i;

	DTFL_ASSERT(peers > 0);
	if (pthread_mutex_init(&rv->lock, NULL) != 0 ||
	    pthread_cond_init(&rv->cond, NULL) != 0)
		__dtfl_panic("ProcessBinary: rendezvous init failed");
	rv->peers = peers;
	rv->refs = peers;
	rv->abandoned = false;
	rv->slot = __dtfl_calloc(__dtfl_mul_size(peers, peers),
	    sizeof *rv->slot);
	for (i = 0; i < peers; i++) {
		process_builder *b = __dtfl_alloc(sizeof *b);

		b->base.build = process_builder_build;
		b->base.destroy = process_builder_destroy;
		b->rv = rv;
		b->index = i;
		builders[i] = &b->base;
	}
}
