/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/push_pull.rs
 *   communication/src/allocator/counters.rs
 *   communication/src/allocator/canary.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/zc_push_pull.c: Push and Pull over serialized data.
 *
 * The Pusher has a fixed MessageHeader and a shared SendEndpoint, from
 * which it reserves space to write the header and the encoded message.
 * The Puller pops payload views from a worker-local FIFO and decodes
 * them.  As in allocator_process.rs, the Puller is wrapped in the
 * counters.rs Puller (reports the channel to the worker's events when a
 * run of messages ends) and owns a Canary (reports the channel when the
 * puller drops).  Only Puller is ported; PullerInner (used by the Tcp
 * allocator) lands with it.  All of this belongs to one worker thread.
 */
#include "comm/zc_int.h"

/* ------------------------------------------------------------------ */
/* Worker-local FIFO of payload views (Rc<RefCell<VecDeque<Bytes>>>)   */
/* ------------------------------------------------------------------ */

/* An empty FIFO with one reference. */
__dtfl_bytes_fifo *
__dtfl_bytes_fifo_new(void)
{
	__dtfl_bytes_fifo *f = __dtfl_alloc(sizeof *f);

	f->rc = 1;
	__dtfl_vec_init(&f->items, sizeof(__dtfl_bytes));
	f->head = 0;
	return f;
}

/* Rc::clone: one more worker-local reference. */
__dtfl_bytes_fifo *
__dtfl_bytes_fifo_retain(__dtfl_bytes_fifo *f)
{
	f->rc++;
	return f;
}

/* Drops one reference; the last frees any undelivered views. */
void
__dtfl_bytes_fifo_release(__dtfl_bytes_fifo *f)
{
	size_t i;

	if (f == NULL || --f->rc > 0)
		return;
	for (i = f->head; i < f->items.len; i++)
		__dtfl_bytes_release(DTFL_VEC_AT(&f->items, __dtfl_bytes, i));
	__dtfl_vec_free(&f->items);
	__dtfl_free(f);
}

/* push_back: moves *b in (b is zeroed). */
void
__dtfl_bytes_fifo_push(__dtfl_bytes_fifo *f, __dtfl_bytes *b)
{
	/* Reuse the consumed prefix once it is at least half the vector. */
	if (f->head > 0 && f->head >= f->items.len / 2) {
		memmove(f->items.data, __dtfl_vec_at(&f->items, f->head),
		    (f->items.len - f->head) * sizeof(__dtfl_bytes));
		f->items.len -= f->head;
		f->head = 0;
	}
	__dtfl_vec_push(&f->items, b);
	memset(b, 0, sizeof *b);
}

/* pop_front: moves the oldest view to *out; false if empty. */
bool
__dtfl_bytes_fifo_pop(__dtfl_bytes_fifo *f, __dtfl_bytes *out)
{
	if (f->head == f->items.len)
		return false;
	*out = *DTFL_VEC_AT(&f->items, __dtfl_bytes, f->head);
	f->head++;
	if (f->head == f->items.len) {
		f->items.len = 0;
		f->head = 0;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* Pusher                                                              */
/* ------------------------------------------------------------------ */

typedef struct zc_pusher {
	__dtfl_push base;
	__dtfl_message_header header;	/* template; seqno counts up */
	__dtfl_send_endpoint *sender;	/* one reference */
	const __dtfl_msg_class *mc;
} zc_pusher;

/*
 * Push::push.  A message is encoded behind a header with the next
 * sequence number.  As upstream, the element is left in *msg after
 * encoding: the caller gets it back as the recycled element.  done()
 * (msg == NULL) has nothing to flush, because make_valid already sent
 * the bytes.
 */
static void
zc_pusher_push(__dtfl_push *pp, void **msg)
{
	zc_pusher *p = (zc_pusher *)pp;
	__dtfl_message_header header;
	unsigned char *bytes;
	size_t avail, required;

	if (msg == NULL || *msg == NULL)
		return;
	header = p->header;
	p->header.seqno++;
	header.length = p->mc->encoded_len(p->mc, *msg);
	DTFL_ASSERT(header.length > 0);
	required = __dtfl_header_required_bytes(&header);
	DTFL_ASSERT(required > header.length);

	bytes = __dtfl_send_endpoint_reserve(p->sender, required, &avail);
	DTFL_ASSERT(avail >= required);
	__dtfl_header_write_to(&header, bytes);
	p->mc->encode(p->mc, *msg, bytes + __dtfl_header_header_bytes(&header));
	__dtfl_send_endpoint_make_valid(p->sender, required);
}

/* Drop: releases this pusher's endpoint reference. */
static void
zc_pusher_destroy(__dtfl_push *pp)
{
	zc_pusher *p = (zc_pusher *)pp;

	__dtfl_send_endpoint_release(p->sender);
	__dtfl_free(p);
}

static const __dtfl_push_ops zc_pusher_ops = {
	zc_pusher_push, zc_pusher_destroy
};

/* Pusher::new: retains `sender`. */
__dtfl_push *
__dtfl_zc_pusher_new(const __dtfl_message_header *header,
    __dtfl_send_endpoint *sender, const __dtfl_msg_class *mc)
{
	zc_pusher *p = __dtfl_alloc(sizeof *p);

	p->base.ops = &zc_pusher_ops;
	p->header = *header;
	p->sender = __dtfl_send_endpoint_retain(sender);
	p->mc = mc;
	return &p->base;
}

/* ------------------------------------------------------------------ */
/* Puller (with counters::Puller and Canary)                           */
/* ------------------------------------------------------------------ */

typedef struct zc_puller {
	__dtfl_pull base;
	__dtfl_bytes_fifo *receiver;	/* one reference */
	const __dtfl_msg_class *mc;
	size_t index;			/* channel identifier */
	size_t count;			/* messages since last report */
	__dtfl_events *events;		/* the allocator's; borrowed */
	__dtfl_zc_canaries *canaries;	/* one reference */
} zc_puller;

/*
 * Pull::pull: the next decoded message (owned by the caller) or NULL.
 * When the FIFO runs dry after delivering messages, the channel is
 * reported to the worker's events (counters.rs).
 */
static void *
zc_puller_pull(__dtfl_pull *pp)
{
	zc_puller *p = (zc_puller *)pp;
	__dtfl_bytes b;
	void *msg;

	if (!__dtfl_bytes_fifo_pop(p->receiver, &b)) {
		if (p->count != 0) {
			__dtfl_events_push(p->events, p->index);
			p->count = 0;
		}
		return NULL;
	}
	msg = p->mc->decode(p->mc, &b);
	__dtfl_bytes_release(&b);
	/* from_bytes cannot fail upstream: a bad frame is a contract bug. */
	if (msg == NULL)
		__dtfl_panic("%s: failed to decode a message on channel %zu",
		    p->mc->name, p->index);
	p->count++;
	return msg;
}

/* Drop: the Canary reports the channel so the allocator can retire it. */
static void
zc_puller_destroy(__dtfl_pull *pp)
{
	zc_puller *p = (zc_puller *)pp;
	__dtfl_zc_canaries *c = p->canaries;

	__dtfl_vec_push(&c->dropped, &p->index);
	if (--c->rc == 0) {
		__dtfl_vec_free(&c->dropped);
		__dtfl_free(c);
	}
	__dtfl_bytes_fifo_release(p->receiver);
	__dtfl_free(p);
}

static const __dtfl_pull_ops zc_puller_ops = {
	zc_puller_pull, zc_puller_destroy
};

/* Puller::new wrapped in counters::Puller; retains receiver and canaries. */
__dtfl_pull *
__dtfl_zc_puller_new(__dtfl_bytes_fifo *receiver, const __dtfl_msg_class *mc,
    size_t identifier, __dtfl_events *events, __dtfl_zc_canaries *canaries)
{
	zc_puller *p = __dtfl_alloc(sizeof *p);

	p->base.ops = &zc_puller_ops;
	p->receiver = __dtfl_bytes_fifo_retain(receiver);
	p->mc = mc;
	p->index = identifier;
	p->count = 0;
	p->events = events;
	canaries->rc++;
	p->canaries = canaries;
	return &p->base;
}
