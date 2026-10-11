/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/queue.c: the FIFO (VecDeque) and the many-producer,
 * one-consumer queue (std::sync::mpsc::channel) that the typed
 * allocators are built from.  See comm_int.h for the contracts.
 */
#include "comm/comm_int.h"

void
__dtfl_fifo_init(__dtfl_fifo *q, size_t elem)
{
	__dtfl_vec_init(&q->v, elem);
	q->head = 0;
}

void
__dtfl_fifo_push(__dtfl_fifo *q, const void *elem)
{
	/*
	 * Reclaim the consumed prefix once it is at least half the vector,
	 * so a queue that never quite empties stays O(live) in size.
	 */
	if (q->head > 0 && q->head >= q->v.len / 2) {
		memmove(q->v.data, q->v.data + q->head * q->v.elem,
		    (q->v.len - q->head) * q->v.elem);
		q->v.len -= q->head;
		q->head = 0;
	}
	__dtfl_vec_push(&q->v, elem);
}

bool
__dtfl_fifo_pop(__dtfl_fifo *q, void *out)
{
	if (q->head == q->v.len)
		return false;
	memcpy(out, q->v.data + q->head * q->v.elem, q->v.elem);
	if (++q->head == q->v.len) {
		q->head = 0;
		q->v.len = 0;
	}
	return true;
}

void
__dtfl_fifo_free(__dtfl_fifo *q)
{
	__dtfl_vec_free(&q->v);
	q->head = 0;
}

struct __dtfl_mpsc {
	/*
	 * Senders plus the receiver.  acq_rel on decrement: the thread
	 * that frees sees every other holder's accesses.
	 */
	atomic_size_t refs;
	pthread_mutex_t mu;
	__dtfl_fifo q;		/* under mu */
	bool closed;		/* under mu */
	void (*drop)(void *elem, void *ctx);
	void *ctx;
};

__dtfl_mpsc *
__dtfl_mpsc_new(size_t elem, void (*drop)(void *, void *), void *ctx)
{
	__dtfl_mpsc *q = __dtfl_alloc(sizeof *q);

	atomic_init(&q->refs, 1);
	if (pthread_mutex_init(&q->mu, NULL) != 0)
		__dtfl_panic("mpsc: cannot initialize mutex");
	__dtfl_fifo_init(&q->q, elem);
	q->closed = false;
	q->drop = drop;
	q->ctx = ctx;
	return q;
}

__dtfl_mpsc *
__dtfl_mpsc_retain(__dtfl_mpsc *q)
{
	/* relaxed: the caller already holds a reference. */
	atomic_fetch_add_explicit(&q->refs, 1, memory_order_relaxed);
	return q;
}

/* Drops every element of `v` with the queue's drop function. */
static void
drop_all(__dtfl_mpsc *q, __dtfl_vec *v)
{
	size_t i;

	if (q->drop != NULL)
		for (i = 0; i < v->len; i++)
			q->drop(__dtfl_vec_at(v, i), q->ctx);
}

void
__dtfl_mpsc_release(__dtfl_mpsc *q)
{
	__dtfl_vec rest;

	if (q == NULL)
		return;
	if (atomic_fetch_sub_explicit(&q->refs, 1, memory_order_acq_rel) != 1)
		return;
	/* Last holder: no lock needed. */
	__dtfl_vec_init(&rest, q->q.v.elem);
	__dtfl_mpsc_drain(q, &rest);
	drop_all(q, &rest);
	__dtfl_vec_free(&rest);
	__dtfl_fifo_free(&q->q);
	pthread_mutex_destroy(&q->mu);
	__dtfl_free(q);
}

bool
__dtfl_mpsc_send(__dtfl_mpsc *q, const void *elem)
{
	bool ok;

	pthread_mutex_lock(&q->mu);
	ok = !q->closed;
	if (ok)
		__dtfl_fifo_push(&q->q, elem);
	pthread_mutex_unlock(&q->mu);
	return ok;
}

bool
__dtfl_mpsc_recv(__dtfl_mpsc *q, void *out)
{
	bool ok;

	pthread_mutex_lock(&q->mu);
	ok = __dtfl_fifo_pop(&q->q, out);
	pthread_mutex_unlock(&q->mu);
	return ok;
}

void
__dtfl_mpsc_drain(__dtfl_mpsc *q, __dtfl_vec *out)
{
	DTFL_ASSERT(out->elem == q->q.v.elem);
	pthread_mutex_lock(&q->mu);
	__dtfl_vec_push_n(out, q->q.v.data + q->q.head * q->q.v.elem,
	    __dtfl_fifo_len(&q->q));
	q->q.head = 0;
	q->q.v.len = 0;
	pthread_mutex_unlock(&q->mu);
}

void
__dtfl_mpsc_close(__dtfl_mpsc *q)
{
	__dtfl_vec rest;

	__dtfl_vec_init(&rest, q->q.v.elem);
	pthread_mutex_lock(&q->mu);
	q->closed = true;
	pthread_mutex_unlock(&q->mu);
	/* No send succeeds after closed is set, so this drains the rest. */
	__dtfl_mpsc_drain(q, &rest);
	/* User free functions run outside the lock. */
	drop_all(q, &rest);
	__dtfl_vec_free(&rest);
}
