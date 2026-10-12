/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/event.c: capture events and the event link.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/capture/event.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * EventLink is a shared singly linked list of Event<T, C> values.  An
 * EventPusher appends to the tail; an EventIterator advances a read
 * cursor.  The sync variant guards the chain with a mutex so a capture
 * on one thread is replayed on another (PLAN section 5).  Drop frees the
 * chain iteratively, as upstream's Drop avoids a deep recursive drop
 * (avoid_stack_overflow_in_drop).
 *
 * EventWriter / EventReader frame events as a u64 LE length then the
 * bincode-1 fixint little-endian Event<T, C> (PLAN D9).  The tag is a
 * u32; Progress is a u64 count then (time words, i64 delta) pairs;
 * Messages is the time words then the batch encoding (batch_int.h).
 * Only libdtfl reads these bytes, so the T and C encodings are libdtfl's.
 */
#include "timely/capture_int.h"

#include "timely/log_codec_int.h"

/* ------------------------------------------------------------------ */
/* Event                                                               */
/* ------------------------------------------------------------------ */

void
__dtfl_event_clear(dtfl_event *e)
{
	if (e->tag == DTFL_EVENT_PROGRESS)
		__dtfl_cb_free(&e->u.progress);
	else
		__dtfl_batch_release(e->u.messages.data);
}

/* ------------------------------------------------------------------ */
/* Event link (link / link_sync)                                       */
/* ------------------------------------------------------------------ */

typedef struct dtfl_event_node {
	dtfl_event event;
	struct dtfl_event_node *next;
} dtfl_event_node;

static dtfl_event_link *
link_new(const dtfl_time_class *tc, const dtfl_type *type, bool sync)
{
	dtfl_event_link *l = __dtfl_calloc(1, sizeof *l);

	l->tc = dtfl_time_class_retain(tc);
	l->type = dtfl_type_retain(type);
	l->sync = sync;
	if (sync)
		pthread_mutex_init(&l->lock, NULL);
	/*
	 * A sentinel head carries no event, matching upstream's first link
	 * element.  read starts at the sentinel; next advances past it.
	 */
	l->head = __dtfl_calloc(1, sizeof *l->head);
	l->tail = l->head;
	l->read = l->head;
	return l;
}

dtfl_event_link *
dtfl_event_link_new(const dtfl_time_class *tc, const dtfl_type *type)
{
	return link_new(tc, type, false);
}

dtfl_event_link *
dtfl_event_link_new_sync(const dtfl_time_class *tc, const dtfl_type *type)
{
	return link_new(tc, type, true);
}

void
dtfl_event_link_free(dtfl_event_link *l)
{
	dtfl_event_node *n;

	if (l == NULL)
		return;
	/* Iterative drop over the chain (avoid_stack_overflow_in_drop). */
	n = l->head;
	while (n != NULL) {
		dtfl_event_node *next = n->next;

		if (n != l->head)	/* the sentinel carries no event */
			__dtfl_event_clear(&n->event);
		__dtfl_free(n);
		n = next;
	}
	if (l->sync)
		pthread_mutex_destroy(&l->lock);
	dtfl_time_class_free(l->tc);
	dtfl_type_free(l->type);
	__dtfl_free(l);
}

void
__dtfl_event_link_push(dtfl_event_link *l, dtfl_event *e)
{
	dtfl_event_node *node = __dtfl_calloc(1, sizeof *node);

	node->event = *e;
	node->next = NULL;
	if (l->sync)
		pthread_mutex_lock(&l->lock);
	l->tail->next = node;
	l->tail = node;
	if (l->sync)
		pthread_mutex_unlock(&l->lock);
}

const dtfl_event *
__dtfl_event_link_next(dtfl_event_link *l)
{
	dtfl_event_node *next;

	if (l->sync)
		pthread_mutex_lock(&l->lock);
	next = l->read->next;
	if (next != NULL)
		l->read = next;
	if (l->sync)
		pthread_mutex_unlock(&l->lock);
	return next != NULL ? &next->event : NULL;
}

/* ------------------------------------------------------------------ */
/* The event-pusher adaptor over a link                                */
/* ------------------------------------------------------------------ */

/*
 * dtfl_event_pusher built over an event link: progress and messages
 * callbacks append events.  The pusher does not own the link.
 */
static void
link_push_progress(void *ctx, const uint64_t *times, const int64_t *diffs,
    size_t n)
{
	dtfl_event_link *l = ctx;
	dtfl_event e;
	size_t i;

	e.tag = DTFL_EVENT_PROGRESS;
	e.depth = l->tc->depth;
	__dtfl_cb_init(&e.u.progress, l->tc->depth);
	for (i = 0; i < n; i++)
		__dtfl_cb_update(&e.u.progress, times + (size_t)e.depth * i,
		    diffs[i]);
	__dtfl_event_link_push(l, &e);
}

static void
link_push_messages(void *ctx, const uint64_t *time, const dtfl_message *msg)
{
	dtfl_event_link *l = ctx;
	dtfl_event e;

	e.tag = DTFL_EVENT_MESSAGES;
	e.depth = l->tc->depth;
	__dtfl_time_copy(l->tc, e.u.messages.time, time);
	e.u.messages.data = __dtfl_batch_retain(msg->data);
	__dtfl_event_link_push(l, &e);
}

dtfl_event_pusher
dtfl_event_link_pusher(dtfl_event_link *l)
{
	dtfl_event_pusher p;

	p.progress = link_push_progress;
	p.messages = link_push_messages;
	p.ctx = l;
	return p;
}

/* ------------------------------------------------------------------ */
/* Wire codec (PLAN D9)                                                */
/* ------------------------------------------------------------------ */

static void
bc_i64(__dtfl_vec *o, int64_t v)
{
	__dtfl_bc_u64(o, (uint64_t)v);
}

/* Serializes just the Event body (no length prefix) into `body`. */
static void
encode_body(const dtfl_time_class *tc, const dtfl_event *e, __dtfl_vec *body)
{
	unsigned d = tc->depth, i;

	if (e->tag == DTFL_EVENT_PROGRESS) {
		size_t k, n;
		__dtfl_change_batch cb;

		__dtfl_bc_u32(body, DTFL_EVENT_PROGRESS);
		/* Copy to iterate (compaction mutates); the source is const. */
		__dtfl_cb_init(&cb, d);
		__dtfl_cb_extend(&cb, &e->u.progress);
		n = __dtfl_cb_iter(&cb);
		__dtfl_bc_u64(body, n);
		for (k = 0; k < n; k++) {
			const uint64_t *key = __dtfl_cb_key(&cb, k);

			for (i = 0; i < d; i++)
				__dtfl_bc_u64(body, key[i]);
			bc_i64(body, __dtfl_cb_delta(&cb, k));
		}
		__dtfl_cb_free(&cb);
	} else {
		size_t blen = __dtfl_batch_encoded_len(e->u.messages.data);
		unsigned char *slot;

		__dtfl_bc_u32(body, DTFL_EVENT_MESSAGES);
		for (i = 0; i < d; i++)
			__dtfl_bc_u64(body, e->u.messages.time[i]);
		slot = __dtfl_vec_push_n(body, NULL, blen);
		__dtfl_batch_encode(e->u.messages.data, slot);
	}
}

void
__dtfl_event_encode(const dtfl_time_class *tc, const dtfl_event *e,
    __dtfl_vec *out)
{
	__dtfl_vec body;
	unsigned char *slot;

	__dtfl_vec_init(&body, 1);
	encode_body(tc, e, &body);
	__dtfl_bc_u64(out, body.len);
	slot = __dtfl_vec_push_n(out, NULL, body.len);
	if (body.len > 0)
		memcpy(slot, body.data, body.len);
	__dtfl_vec_free(&body);
}

static uint64_t
get_u64_le(const unsigned char *p)
{
	uint64_t v = 0;
	size_t i;

	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static uint32_t
get_u32_le(const unsigned char *p)
{
	uint32_t v = 0;
	size_t i;

	for (i = 0; i < 4; i++)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}

bool
__dtfl_event_decode(const dtfl_time_class *tc, const dtfl_type *type,
    const unsigned char *p, size_t len, dtfl_event *e, size_t *consumed)
{
	unsigned d = tc->depth, i;
	uint64_t body_len;
	const unsigned char *body;
	uint32_t tag;

	(void)type;
	if (len < 8)
		return false;
	body_len = get_u64_le(p);
	if (len < 8 + body_len)
		return false;
	body = p + 8;
	if (body_len < 4)
		return false;
	tag = get_u32_le(body);
	if (tag == DTFL_EVENT_PROGRESS) {
		uint64_t count, k;
		size_t off = 4;

		if (body_len < off + 8)
			return false;
		count = get_u64_le(body + off);
		off += 8;
		e->tag = DTFL_EVENT_PROGRESS;
		e->depth = d;
		__dtfl_cb_init(&e->u.progress, d);
		for (k = 0; k < count; k++) {
			uint64_t key[DTFL_TIME_MAX];
			int64_t delta;

			if (body_len < off + (size_t)(d + 1) * 8) {
				__dtfl_cb_free(&e->u.progress);
				return false;
			}
			for (i = 0; i < d; i++)
				key[i] = get_u64_le(body + off + (size_t)i * 8);
			delta = (int64_t)get_u64_le(body + off + (size_t)d * 8);
			off += (size_t)(d + 1) * 8;
			__dtfl_cb_update(&e->u.progress, key, delta);
		}
	} else if (tag == DTFL_EVENT_MESSAGES) {
		size_t off = 4;
		__dtfl_batch *data;

		if (body_len < off + (size_t)d * 8)
			return false;
		e->tag = DTFL_EVENT_MESSAGES;
		e->depth = d;
		for (i = 0; i < d; i++)
			e->u.messages.time[i] = get_u64_le(body + off + (size_t)i * 8);
		off += (size_t)d * 8;
		data = __dtfl_batch_decode(body + off, (size_t)body_len - off);
		if (data == NULL)
			return false;
		e->u.messages.data = data;
	} else {
		return false;
	}
	*consumed = 8 + (size_t)body_len;
	return true;
}
