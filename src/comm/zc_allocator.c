/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/allocator.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/zc_allocator.c: the TcpAllocator, a zero-copy allocator for
 * inter-process communication over TCP.
 *
 * A TcpAllocator wraps an inner typed Process allocator for the workers
 * in its own process and routes messages to workers in other processes
 * through per-process SendEndpoints and the network send/recv threads.
 *
 *   index = my_process * threads + local_index   (global worker index)
 *   peers = threads * processes                  (all workers)
 *
 *   sends[p]  -> the queue drained by the send thread to process p
 *   recvs[p]  <- the queue filled by the recv thread from process p
 * both indexed by remote process with my_process skipped, so sends and
 * recvs have `processes - 1` entries.
 *
 * allocate() hands process-local targets the inner typed pushers and
 * remote targets a Pusher that frames the message into the owning
 * process's SendEndpoint; the puller is a PullerInner over the inner
 * typed puller and this channel's inter-process byte FIFO.
 *
 * Events.  Upstream keeps one event list (the inner allocator's) that
 * both the inner endpoints and the TCP receive path push to.  Here the
 * TcpAllocator owns the worker-visible event list; receive() drains the
 * inner allocator's list into it (before and after inner.receive()), so
 * inner and TCP activity both reach the worker.
 *
 * Threads.  The allocator and everything it hands out belong to one
 * worker thread.  The SendEndpoints' MergeQueues and the recv queues are
 * the only cross-thread state (zc_bytes_exchange.c); the network threads
 * at the far end of each are started by zc_initialize.c.
 */
#include "comm/zc_int.h"

#include <sched.h>

/* ------------------------------------------------------------------ */
/* TcpAllocator                                                        */
/* ------------------------------------------------------------------ */

typedef struct to_local_entry {
	size_t channel;
	__dtfl_bytes_fifo *fifo;	/* one reference */
} to_local_entry;

typedef struct tcp_alloc {
	__dtfl_allocator base;
	__dtfl_allocator *inner;	/* process-local typed allocator */
	__dtfl_zc_canaries *canaries;	/* one reference */
	bool has_bound;			/* channel_id_bound is Some */
	size_t channel_id_bound;
	__dtfl_vec staged;		/* __dtfl_bytes */
	__dtfl_send_endpoint **sends;	/* [processes-1]; sends[p] -> proc p */
	__dtfl_merge_queue **recvs;	/* [processes-1]; recvs[p] <- proc p */
	size_t nremote;			/* processes - 1 */
	__dtfl_vec to_local;		/* to_local_entry, sorted */
} tcp_alloc;

/* Vector sorted by channel; binary search (channels increase, so append). */static size_t
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
	base = (to_local_entry *)(void *)m->data;
	memmove(base + at + 1, base + at, (m->len - 1 - at) * sizeof e);
	base[at] = e;
	return e.fifo;
}

/*
 * Allocate::allocate.  Identifiers must strictly increase.  Returns one
 * pusher per target worker (process-local ones share the inner typed
 * channel; remote ones frame into the owning process's SendEndpoint)
 * and a PullerInner over the inner typed puller and this channel's byte
 * FIFO.
 */
static void
tcp_allocate(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pushers, __dtfl_pull **puller)
{
	tcp_alloc *p = (tcp_alloc *)a;
	__dtfl_allocator *in = p->inner;
	size_t inner_peers = in->peers;
	size_t my_proc = a->index / inner_peers;
	__dtfl_push **inner_sends;
	__dtfl_pull *inner_recv;
	__dtfl_bytes_fifo *fifo;
	__dtfl_message_header header;
	size_t target, next_inner = 0, at;
	bool found;

	/* Assume and enforce in-order identifier allocation. */
	if (p->has_bound && !(p->channel_id_bound < identifier))
		__dtfl_panic("Tcp: channel %zu allocated after %zu",
		    identifier, p->channel_id_bound);
	p->has_bound = true;
	p->channel_id_bound = identifier;

	/* Inner exchange allocations (process-local, typed). */
	inner_sends = __dtfl_calloc(inner_peers, sizeof *inner_sends);
	in->ops->allocate(in, identifier, mc, inner_sends, &inner_recv);

	for (target = 0; target < a->peers; target++) {
		size_t process_id = target / inner_peers;

		if (process_id == my_proc) {
			pushers[target] = inner_sends[next_inner++];
		} else {
			header.channel = identifier;
			header.source = a->index;
			header.target_lower = target;
			header.target_upper = target + 1;
			header.length = 0;
			header.seqno = 0;
			/* Skip our own process in the sends[] indexing. */
			if (process_id > my_proc)
				process_id -= 1;
			pushers[target] = __dtfl_zc_pusher_new(&header,
			    p->sends[process_id], mc);
		}
	}
	__dtfl_free(inner_sends);

	at = to_local_find(&p->to_local, identifier, &found);
	fifo = found ? DTFL_VEC_AT(&p->to_local, to_local_entry, at)->fifo :
	    to_local_insert(&p->to_local, at, identifier);
	*puller = __dtfl_zc_puller_inner_new(inner_recv, fifo, mc, identifier,
	    &a->events, p->canaries);
}

/* Drains the inner allocator's event list into the worker-visible one. */
static void
drain_inner_events(tcp_alloc *p)
{
	__dtfl_vec *src = &p->inner->events.ids;
	size_t i;

	for (i = 0; i < src->len; i++)
		__dtfl_events_push(&p->base.events,
		    *DTFL_VEC_AT(src, size_t, i));
	__dtfl_vec_clear(src);
}

/*
 * Moves network arrivals into the per-channel FIFOs, draining each
 * drained view (an integral number of framed messages; none splits
 * across views) and the inner allocator's events.
 */
static void
tcp_receive(__dtfl_allocator *a)
{
	tcp_alloc *p = (tcp_alloc *)a;
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
		__dtfl_bytes_fifo_release(DTFL_VEC_AT(&p->to_local,
		    to_local_entry, at)->fifo);
		__dtfl_vec_remove(&p->to_local, at);
	}
	__dtfl_vec_clear(dropped);

	/* Catch inner-puller reports from the previous step. */
	drain_inner_events(p);
	p->inner->ops->receive(p->inner);
	drain_inner_events(p);

	for (i = 0; i < p->nremote; i++)
		__dtfl_merge_queue_drain_into(p->recvs[i], &p->staged);

	for (i = 0; i < p->staged.len; i++) {
		bytes = *DTFL_VEC_AT(&p->staged, __dtfl_bytes, i);
		while (bytes.len > 0) {
			/*
			 * A short frame is a contract violation (upstream
			 * prints and spins); networking.c's convention panics.
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
				__dtfl_bytes_fifo_push(to_local_insert(
				    &p->to_local, at, header.channel), &peel);
			} else {
				/* A channel already dropped here. */
				__dtfl_bytes_release(&peel);
			}
		}
		__dtfl_bytes_release(&bytes);
	}
	__dtfl_vec_clear(&p->staged);
}

/* Publishes outgoing byte ledgers; the inner allocator has none. */
static void
tcp_release(__dtfl_allocator *a)
{
	tcp_alloc *p = (tcp_alloc *)a;
	size_t i;

	if (p->inner->ops->release != NULL)
		p->inner->ops->release(p->inner);
	for (i = 0; i < p->nremote; i++)
		__dtfl_send_endpoint_publish(p->sends[i]);
}

/* Parks unless events are already pending (delegates to the inner). */
static void
tcp_await_events(__dtfl_allocator *a, uint64_t timeout_ns)
{
	tcp_alloc *p = (tcp_alloc *)a;

	if (a->events.ids.len == 0)
		p->inner->ops->await_events(p->inner, timeout_ns);
}

/*
 * Drops the allocator.  Pushers and pullers still alive keep their
 * endpoints, FIFOs and canaries (each is counted); a puller must not
 * pull after this (it reports to this allocator's events).  Dropping
 * each SendEndpoint flushes and closes the queue to its send thread;
 * dropping each recv queue closes the receive side.  The send threads
 * then see every source complete and exit; the comms guard joins them.
 */
static void
tcp_destroy(__dtfl_allocator *a)
{
	tcp_alloc *p = (tcp_alloc *)a;
	size_t i;

	for (i = 0; i < p->nremote; i++) {
		__dtfl_send_endpoint_release(p->sends[i]);
		__dtfl_merge_queue_drop(p->recvs[i], false);
	}
	__dtfl_free(p->sends);
	__dtfl_free(p->recvs);
	for (i = 0; i < p->to_local.len; i++)
		__dtfl_bytes_fifo_release(DTFL_VEC_AT(&p->to_local,
		    to_local_entry, i)->fifo);
	__dtfl_vec_free(&p->to_local);
	for (i = 0; i < p->staged.len; i++)
		__dtfl_bytes_release(DTFL_VEC_AT(&p->staged, __dtfl_bytes, i));
	__dtfl_vec_free(&p->staged);
	if (--p->canaries->rc == 0) {
		__dtfl_vec_free(&p->canaries->dropped);
		__dtfl_free(p->canaries);
	}
	p->inner->ops->destroy(p->inner);
	__dtfl_vec_free(&a->events.ids);
	__dtfl_parker_release(a->parker);
	__dtfl_free(p);
}

static const __dtfl_allocator_ops tcp_ops = {
	.allocate = tcp_allocate,
	.receive = tcp_receive,
	.release = tcp_release,
	.await_events = tcp_await_events,
	.destroy = tcp_destroy,
};

/* MergeQueue buzz: wakes the worker (recv data landed in a recv queue). */
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

/* ------------------------------------------------------------------ */
/* TcpBuilder                                                          */
/* ------------------------------------------------------------------ */

/*
 * One worker's TcpBuilder.  `inner` builds the process-local typed
 * allocator.  `promises[p]` posts this worker's recv-queue writer to
 * recv thread p; `futures[p]` receives this worker's send-queue writer
 * from send thread p (p over the processes - 1 remote processes).
 */
typedef struct tcp_builder {
	__dtfl_allocator_builder base;
	__dtfl_allocator_builder *inner;
	size_t index;			/* global worker index */
	size_t peers;			/* total workers */
	size_t nremote;			/* processes - 1 */
	__dtfl_mpsc **promises;		/* [nremote]; to recv threads */
	__dtfl_mpsc **futures;		/* [nremote]; from send threads */
	__dtfl_spill_fn *spill;		/* one reference, or NULL */
} tcp_builder;

/*
 * TcpBuilder::build, on the worker thread: create the recv queues (keep
 * the readers, post the writers to the recv threads), then receive the
 * send-queue writers from the send threads and wrap each in a
 * SendEndpoint.  Then build the inner allocator.
 */
static __dtfl_allocator *
tcp_builder_build(__dtfl_allocator_builder *bb)
{
	tcp_builder *b = (tcp_builder *)bb;
	tcp_alloc *p = __dtfl_calloc(1, sizeof *p);
	__dtfl_allocator *inner = __dtfl_allocator_build(b->inner);
	size_t i;

	/*
	 * The inner allocator owns the parker the worker parks on; the TCP
	 * allocator borrows a reference for its own head.  Both parkers are
	 * woken by their respective queues (process queues buzz the inner
	 * parker; recv-thread extends buzz... the inner, since recv data
	 * lands in queues whose buzz the recv thread owns).  await_events
	 * delegates to the inner, so one parker is enough.
	 */
	__dtfl_allocator_init(&p->base, &tcp_ops, b->index, b->peers,
	    __dtfl_parker_retain(inner->parker));
	p->inner = inner;
	p->canaries = __dtfl_alloc(sizeof *p->canaries);
	p->canaries->rc = 1;
	__dtfl_vec_init(&p->canaries->dropped, sizeof(size_t));
	__dtfl_vec_init(&p->staged, sizeof(__dtfl_bytes));
	__dtfl_vec_init(&p->to_local, sizeof(to_local_entry));
	p->nremote = b->nremote;
	p->sends = __dtfl_calloc(DTFL_MAX(b->nremote, 1), sizeof *p->sends);
	p->recvs = __dtfl_calloc(DTFL_MAX(b->nremote, 1), sizeof *p->recvs);

	/*
	 * Fulfill puller obligations: for each recv thread, create a queue
	 * pair, keep the reader (recv thread extends the writer), post the
	 * writer.  The recv thread owns the queue's buzz, so it parks the
	 * worker's (inner) parker on the empty-to-nonempty transition.
	 */
	for (i = 0; i < b->nremote; i++) {
		__dtfl_merge_queue *writer, *reader;
		__dtfl_spill_policy *wp, *rp;
		__dtfl_mq_handoff h;

		__dtfl_spill_fn_make(b->spill, &wp, &rp);
		__dtfl_merge_queue_new_pair(buzz_parker,
		    __dtfl_parker_retain(inner->parker), drop_parker, wp, rp,
		    &writer, &reader);
		p->recvs[i] = reader;
		h.queue = writer;
		if (!__dtfl_mpsc_send(b->promises[i], &h))
			__dtfl_merge_queue_drop(writer, false);
		__dtfl_mpsc_release(b->promises[i]);
	}

	/* Extract pusher commitments from each send thread. */
	for (i = 0; i < b->nremote; i++) {
		__dtfl_mq_handoff h;

		while (!__dtfl_mpsc_recv(b->futures[i], &h)) {
			/* The send thread posts at startup; a brief spin. */
			sched_yield();
		}
		p->sends[i] = __dtfl_send_endpoint_new(h.queue);
		__dtfl_mpsc_release(b->futures[i]);
	}

	__dtfl_free(b->promises);
	__dtfl_free(b->futures);
	__dtfl_spill_fn_release(b->spill);
	__dtfl_free(b);
	return &p->base;
}

/* Drops an unbuilt builder: the network threads see the queues close. */
static void
tcp_builder_destroy(__dtfl_allocator_builder *bb)
{
	tcp_builder *b = (tcp_builder *)bb;
	size_t i;

	b->inner->destroy(b->inner);
	for (i = 0; i < b->nremote; i++) {
		__dtfl_mpsc_release(b->promises[i]);
		__dtfl_mpsc_release(b->futures[i]);
	}
	__dtfl_free(b->promises);
	__dtfl_free(b->futures);
	__dtfl_spill_fn_release(b->spill);
	__dtfl_free(b);
}

/*
 * new_vector: build `threads` TcpBuilders, one per local worker, and the
 * mpsc channels the network threads use.  `inner` holds the typed
 * Process builders (one per local worker; ownership moves in).  Fills
 * *net_promises and *net_futures, each a [nremote][threads] grid of
 * __dtfl_mpsc * for the recv and send threads respectively (the caller
 * owns the grids and the references).
 */
void
__dtfl_tcp_builders(__dtfl_allocator_builder **inner, size_t threads,
    size_t my_process, size_t processes, __dtfl_spill_fn *spill,
    __dtfl_allocator_builder **builders, __dtfl_mpsc ***net_promises,
    __dtfl_mpsc ***net_futures)
{
	size_t nremote = processes - 1;
	size_t peers = threads * processes;
	/* worker_promises[t][p] = net_promises[p][t]; worker_futures sym. */
	__dtfl_mpsc **promises = __dtfl_calloc(DTFL_MAX(nremote, 1) * threads,
	    sizeof *promises);
	__dtfl_mpsc **futures = __dtfl_calloc(DTFL_MAX(nremote, 1) * threads,
	    sizeof *futures);
	size_t t, pr;

	DTFL_ASSERT(processes >= 1 && threads >= 1);

	/* One mpsc per (remote process, local worker) in each direction. */
	for (pr = 0; pr < nremote; pr++)
		for (t = 0; t < threads; t++) {
			promises[pr * threads + t] =
			    __dtfl_mpsc_new(sizeof(__dtfl_mq_handoff), NULL,
			    NULL);
			futures[pr * threads + t] =
			    __dtfl_mpsc_new(sizeof(__dtfl_mq_handoff), NULL,
			    NULL);
		}

	for (t = 0; t < threads; t++) {
		tcp_builder *b = __dtfl_alloc(sizeof *b);

		b->base.build = tcp_builder_build;
		b->base.destroy = tcp_builder_destroy;
		b->inner = inner[t];
		b->index = my_process * threads + t;
		b->peers = peers;
		b->nremote = nremote;
		b->spill = __dtfl_spill_fn_retain(spill);
		b->promises = __dtfl_calloc(DTFL_MAX(nremote, 1),
		    sizeof *b->promises);
		b->futures = __dtfl_calloc(DTFL_MAX(nremote, 1),
		    sizeof *b->futures);
		for (pr = 0; pr < nremote; pr++) {
			/* The worker and a network thread each hold a ref. */
			b->promises[pr] =
			    __dtfl_mpsc_retain(promises[pr * threads + t]);
			b->futures[pr] =
			    __dtfl_mpsc_retain(futures[pr * threads + t]);
		}
		builders[t] = &b->base;
	}

	/*
	 * Hand the network-thread references out as [nremote][threads]
	 * grids.  net_promises[pr] goes to recv thread pr; net_futures[pr]
	 * to send thread pr.
	 */
	for (pr = 0; pr < nremote; pr++)
		for (t = 0; t < threads; t++) {
			net_promises[pr][t] = promises[pr * threads + t];
			net_futures[pr][t] = futures[pr * threads + t];
		}
	__dtfl_free(promises);
	__dtfl_free(futures);
}
