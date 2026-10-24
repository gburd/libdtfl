/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/pact.c: parallelization contracts.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/channels/pact.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A pact allocates the channel for an operator input and returns the
 * pusher the source stream pushes into and the puller the operator
 * reads.  Pipeline uses the worker's local channel; Exchange allocates
 * the full peers-wide channel and wraps the senders in an Exchange
 * pusher that routes by the pact's hash.  Both wrap endpoints in a
 * LogPusher (which stamps from and seq and logs MessagesEvent) and a
 * LogPuller (which logs on pull), as upstream's LogPusher / LogPuller.
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

/* ------------------------------------------------------------------ */
/* LogPusher (pact.rs push_pull)                                       */
/* ------------------------------------------------------------------ */

typedef struct log_pusher {
	__dtfl_push base;
	__dtfl_push *pusher;	/* owned */
	__dtfl_worker *w;
	size_t channel;
	size_t counter;
	size_t source;
	size_t target;
} log_pusher;

static void
log_pusher_push(__dtfl_push *p, void **msg)
{
	log_pusher *lp = (log_pusher *)p;

	if (*msg != NULL) {
		dtfl_message *m = *msg;

		m->seq = lp->counter;
		m->from = lp->source;
		lp->counter++;
		__dtfl_worker_log_messages(lp->w, true, lp->channel, lp->source,
		    lp->target, m->seq, (int64_t)__dtfl_batch_len(m->data));
	}
	__dtfl_push_push(lp->pusher, msg);
}

static void
log_pusher_destroy(__dtfl_push *p)
{
	log_pusher *lp = (log_pusher *)p;

	__dtfl_push_destroy(lp->pusher);
	__dtfl_free(lp);
}

static const __dtfl_push_ops log_pusher_ops = {
	log_pusher_push, log_pusher_destroy
};

static __dtfl_push *
log_pusher_new(__dtfl_push *inner, __dtfl_worker *w, size_t source,
    size_t target, size_t channel)
{
	log_pusher *lp = __dtfl_alloc(sizeof *lp);

	lp->base.ops = &log_pusher_ops;
	lp->pusher = inner;
	lp->w = w;
	lp->channel = channel;
	lp->counter = 0;
	lp->source = source;
	lp->target = target;
	return &lp->base;
}

/* ------------------------------------------------------------------ */
/* LogPuller (pact.rs push_pull)                                       */
/* ------------------------------------------------------------------ */

typedef struct log_puller {
	__dtfl_pull base;
	__dtfl_pull *puller;	/* owned */
	__dtfl_worker *w;
	size_t channel;
	size_t index;
} log_puller;

static void *
log_puller_pull(__dtfl_pull *p)
{
	log_puller *lp = (log_puller *)p;
	dtfl_message *m = __dtfl_pull_recv(lp->puller);

	if (m != NULL)
		__dtfl_worker_log_messages(lp->w, false, lp->channel, m->from,
		    lp->index, m->seq, (int64_t)__dtfl_batch_len(m->data));
	return m;
}

static void
log_puller_destroy(__dtfl_pull *p)
{
	log_puller *lp = (log_puller *)p;

	__dtfl_pull_destroy(lp->puller);
	__dtfl_free(lp);
}

static const __dtfl_pull_ops log_puller_ops = {
	log_puller_pull, log_puller_destroy
};

static __dtfl_pull *
log_puller_new(__dtfl_pull *inner, __dtfl_worker *w, size_t index, size_t channel)
{
	log_puller *lp = __dtfl_alloc(sizeof *lp);

	lp->base.ops = &log_puller_ops;
	lp->puller = inner;
	lp->w = w;
	lp->channel = channel;
	lp->index = index;
	return &lp->base;
}

/* ------------------------------------------------------------------ */
/* Public pact constructors                                            */
/* ------------------------------------------------------------------ */

dtfl_pact
dtfl_pact_pipeline(void)
{
	dtfl_pact p = { DTFL_PACT_PIPELINE, NULL, NULL };

	return p;
}

dtfl_pact
dtfl_pact_exchange(uint64_t (*route)(const void *, size_t, void *), void *ctx)
{
	dtfl_pact p = { DTFL_PACT_EXCHANGE, route, ctx };

	return p;
}

/* ------------------------------------------------------------------ */
/* connect                                                             */
/* ------------------------------------------------------------------ */

void
__dtfl_pact_connect(dtfl_scope *s, const dtfl_pact *pact, size_t identifier,
    const size_t *path, size_t path_len, const dtfl_time_class *tc,
    const dtfl_type *type, __dtfl_push **pusher, __dtfl_pull **puller)
{
	__dtfl_worker *w = s->worker;
	size_t index = w->allocator->index;
	size_t peers = w->allocator->peers;
	const __dtfl_msg_class *mc = __dtfl_message_class();

	if (pact->kind == DTFL_PACT_PIPELINE) {
		__dtfl_push *inner;
		__dtfl_pull *recv;

		__dtfl_worker_pipeline(w, identifier, path, path_len, mc, &inner, &recv);
		*pusher = log_pusher_new(inner, w, index, index, identifier);
		*puller = log_puller_new(recv, w, index, identifier);
	} else {
		__dtfl_push **senders = __dtfl_alloc(DTFL_MAX(peers, 1) *
		    sizeof(__dtfl_push *));
		__dtfl_pull *recv;
		size_t i;

		__dtfl_worker_allocate(w, identifier, path, path_len, mc, senders, &recv);
		for (i = 0; i < peers; i++)
			senders[i] = log_pusher_new(senders[i], w, index, i, identifier);
		*pusher = __dtfl_push_exchange_new(tc, type, senders, peers,
		    pact->route, pact->ctx);
		__dtfl_push_exchange_set_pool(*pusher, &w->buf_pool);
		*puller = log_puller_new(recv, w, index, identifier);
	}
}
