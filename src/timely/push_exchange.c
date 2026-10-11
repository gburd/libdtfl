/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/push_exchange.c: routes records among peers by hash.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/channels/pushers/exchange.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * The Exchange pusher partitions each message's records among its
 * pushers by route(record) modulo the number of pushers, one buffer per
 * peer at the current time.  A buffer flushes when the time changes or
 * on a final flush, as a Message at that time.  The pact guarantees the
 * record count per time is preserved across the output messages, which
 * progress tracking relies on.  With a single pusher there is no
 * exchange and the message passes through.  route is the record type's
 * hash when the pact gives none (timely.h dtfl_pact_exchange).
 */
#include "timely/timely_int.h"

typedef struct exch_pusher {
	__dtfl_push base;
	const dtfl_time_class *tc;	/* retained */
	const dtfl_type *type;		/* retained */
	__dtfl_push **pushers;		/* owned; n entries */
	size_t n;
	__dtfl_batch **buffers;		/* per peer; owned */
	uint64_t (*route)(const void *, size_t, void *);
	void *ctx;
	uint64_t current[DTFL_TIME_MAX];
	bool have_current;
} exch_pusher;

static uint64_t
route_record(exch_pusher *e, const void *rec, size_t len)
{
	if (e->route != NULL)
		return e->route(rec, len, e->ctx);
	return dtfl_type_hash(e->type, rec, len);
}

/* Message::push_at: sends buffer `i` at the current time, then clears it. */
static void
flush_buffer(exch_pusher *e, size_t i)
{
	void *bundle;

	if (__dtfl_batch_len(e->buffers[i]) == 0)
		return;
	bundle = __dtfl_message_new(e->tc, e->current, e->buffers[i]);
	e->buffers[i] = __dtfl_batch_new(0, 0);		/* fresh buffer */
	__dtfl_push_send(e->pushers[i], __dtfl_message_class(), bundle);
}

static void
flush_all(exch_pusher *e)
{
	size_t i;

	for (i = 0; i < e->n; i++)
		flush_buffer(e, i);
}

static void
exch_push(__dtfl_push *p, void **msg)
{
	exch_pusher *e = (exch_pusher *)p;

	/* With one pusher there is nothing to exchange. */
	if (e->n == 1) {
		__dtfl_push_push(e->pushers[0], msg);
		return;
	}
	if (*msg != NULL) {
		dtfl_message *m = *msg;
		size_t k, cnt = __dtfl_batch_len(m->data);

		/* If the time changed, flush everything at the old time. */
		if (e->have_current) {
			if (__dtfl_words_cmp(e->current, m->time, e->tc->depth) != 0) {
				flush_all(e);
				__dtfl_time_copy(e->tc, e->current, m->time);
			}
		} else {
			__dtfl_time_copy(e->tc, e->current, m->time);
			e->have_current = true;
		}
		for (k = 0; k < cnt; k++) {
			size_t len;
			const void *rec = __dtfl_batch_rec(m->data, k, &len);
			size_t idx = (size_t)(route_record(e, rec, len) % e->n);

			__dtfl_batch_push(e->buffers[idx], rec, len, NULL, NULL);
		}
		__dtfl_message_free(m);
		*msg = NULL;
	} else {
		size_t i;

		if (e->have_current) {
			flush_all(e);
			e->have_current = false;
		}
		for (i = 0; i < e->n; i++)
			__dtfl_push_done(e->pushers[i]);
	}
}

static void
exch_destroy(__dtfl_push *p)
{
	exch_pusher *e = (exch_pusher *)p;
	size_t i;

	for (i = 0; i < e->n; i++) {
		__dtfl_push_destroy(e->pushers[i]);
		__dtfl_batch_release(e->buffers[i]);
	}
	__dtfl_free(e->pushers);
	__dtfl_free(e->buffers);
	dtfl_time_class_free(e->tc);
	dtfl_type_free(e->type);
	__dtfl_free(e);
}

static const __dtfl_push_ops exch_ops = { exch_push, exch_destroy };

__dtfl_push *
__dtfl_push_exchange_new(const dtfl_time_class *tc, const dtfl_type *type,
    __dtfl_push **pushers, size_t n,
    uint64_t (*route)(const void *, size_t, void *), void *ctx)
{
	exch_pusher *e = __dtfl_alloc(sizeof *e);
	size_t i;

	e->base.ops = &exch_ops;
	e->tc = dtfl_time_class_retain(tc);
	e->type = dtfl_type_retain(type);
	e->pushers = pushers;
	e->n = n;
	e->buffers = __dtfl_alloc(DTFL_MAX(n, 1) * sizeof(__dtfl_batch *));
	for (i = 0; i < n; i++)
		e->buffers[i] = __dtfl_batch_new(0, 0);
	e->route = route;
	e->ctx = ctx;
	e->have_current = false;
	return &e->base;
}
