/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/sync.c: barrier and sequencer synchronization.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/synchronization/barrier.rs
 *   timely/src/synchronization/sequence.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A Barrier is a reusable all-workers rendezvous: a dataflow with one
 * unit input and a probe on it.  wait() advances the input one round and
 * steps the worker until the probe passes that round, which happens only
 * once every worker has advanced, without blocking dataflow execution.
 *
 * A Sequencer is a shared totally ordered log.  Each worker pushes
 * elements into a local send queue; a source operator timestamps them
 * with the elapsed time and broadcasts a (worker, counter, element)
 * tuple to every worker; an exchange routes each tuple to its target
 * worker's sink, which sorts by (time, worker, counter) and delivers,
 * in frontier order, into a recv queue every worker drains the same way.
 * A catchup activator keeps the source firing until its capability
 * passes the greatest time the sink has seen, so every worker forms the
 * same sequence even when traffic goes quiet (sequence.rs).
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

#include "dtfl/timely_ops.h"

#include <stdlib.h>

#include "base/base_int.h"

static void
put_u64le(unsigned char *p, uint64_t v)
{
	size_t i;

	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t
get_u64le(const unsigned char *p)
{
	uint64_t v = 0;
	size_t i;

	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

/* ------------------------------------------------------------------ */
/* Barrier (barrier.rs)                                                */
/* ------------------------------------------------------------------ */

struct dtfl_barrier {
	dtfl_worker *worker;
	dtfl_input *input;	/* unit input; owned */
	dtfl_probe *probe;	/* owned */
	uint64_t round;		/* the input's current time */
};

typedef struct barrier_build {
	dtfl_input *input;
	dtfl_probe *probe;
} barrier_build;

static void
barrier_dataflow(dtfl_scope *scope, void *arg)
{
	barrier_build *b = arg;
	dtfl_stream *s = dtfl_input_to_stream(b->input, scope);

	dtfl_stream_probe_with(s, b->probe);
}

dtfl_barrier *
dtfl_barrier_new(dtfl_worker *w)
{
	dtfl_barrier *b = __dtfl_alloc(sizeof *b);
	dtfl_time_class *tc = dtfl_time_u64();
	barrier_build build;

	b->worker = w;
	b->input = dtfl_input_new(dtfl_type_unit());
	b->probe = dtfl_probe_new();
	b->round = 0;
	build.input = b->input;
	build.probe = b->probe;
	dtfl_worker_dataflow(w, "Barrier", tc, barrier_dataflow, &build);
	dtfl_time_class_free(tc);
	return b;
}

void
dtfl_barrier_wait(dtfl_barrier *b)
{
	/* advance: move the input one round forward. */
	b->round++;
	dtfl_input_advance_to(b->input, &b->round);

	/* reached: the probe is no longer behind the input's time. */
	while (dtfl_probe_less_than(b->probe, &b->round))
		dtfl_worker_step(b->worker);
}

void
dtfl_barrier_free(dtfl_barrier *b)
{
	if (b == NULL)
		return;
	dtfl_input_close(b->input);
	dtfl_probe_free(b->probe);
	__dtfl_free(b);
}

/* ------------------------------------------------------------------ */
/* Sequencer (sequence.rs)                                             */
/* ------------------------------------------------------------------ */

/* A queued element: owned bytes. */
typedef struct seq_elem {
	unsigned char *bytes;
	size_t len;
} seq_elem;

/* A deque of elements, grown by doubling (upstream VecDeque). */
typedef struct seq_queue {
	seq_elem *items;
	size_t head, len, cap;
} seq_queue;

static void
queue_push(seq_queue *q, const void *bytes, size_t len)
{
	if (q->head + q->len == q->cap) {
		if (q->head > 0) {
			memmove(q->items, q->items + q->head,
			    q->len * sizeof(seq_elem));
			q->head = 0;
		} else {
			size_t ncap = q->cap ? q->cap * 2 : 8;

			q->items = __dtfl_realloc(q->items, ncap * sizeof(seq_elem));
			q->cap = ncap;
		}
	}
	q->items[q->head + q->len].bytes = __dtfl_memdup(bytes, len);
	q->items[q->head + q->len].len = len;
	q->len++;
}

/* Pops the front element into *out (caller frees out->bytes), or false. */
static bool
queue_pop(seq_queue *q, seq_elem *out)
{
	if (q->len == 0)
		return false;
	*out = q->items[q->head];
	q->head++;
	q->len--;
	if (q->len == 0)
		q->head = 0;
	return true;
}

static void
queue_free(seq_queue *q)
{
	size_t i;

	for (i = 0; i < q->len; i++)
		__dtfl_free(q->items[q->head + i].bytes);
	__dtfl_free(q->items);
}

/*
 * Shared state between the sequencer handle and its two operators.  All
 * live on one worker thread, so a plain refcount and no locks suffice
 * (the activator alone crosses into other operators, as upstream's Rc).
 */
typedef struct seq_shared {
	int refs;
	seq_queue send;			/* proposed elements */
	seq_queue recv;			/* sequenced elements */
	dtfl_activator *source_act;	/* the source's activator; owned */
	uint64_t catchup_until;		/* elapsed time to keep firing to */
	bool have_catchup;
	size_t counter;			/* per-worker monotonic counter */
	size_t peers;			/* worker count (captured at creation) */
	uint64_t start_ns;		/* timer origin */
} seq_shared;

static seq_shared *
shared_retain(seq_shared *s)
{
	s->refs++;
	return s;
}

static void
shared_release(seq_shared *s)
{
	if (--s->refs > 0)
		return;
	queue_free(&s->send);
	queue_free(&s->recv);
	dtfl_activator_free(s->source_act);
	__dtfl_free(s);
}

struct dtfl_sequencer {
	dtfl_worker *worker;
	const dtfl_type *type;	/* retained */
	seq_shared *shared;	/* retained */
	unsigned char *last_bytes;	/* the element returned by _next; owned */
};

/* A received tuple pending delivery: (time, worker, counter) and bytes. */
typedef struct recvd_item {
	uint64_t time;
	uint64_t worker;
	uint64_t counter;
	unsigned char *bytes;	/* owned */
	size_t len;
} recvd_item;

/* ---- source operator ---- */

typedef struct source_ctx {
	seq_shared *shared;	/* retained */
	dtfl_cap *cap;		/* the held output capability; owned */
	bool have_cap;
} source_ctx;

static bool
source_logic(dtfl_op *op, void *vctx)
{
	source_ctx *c = vctx;
	size_t peers = c->shared->peers;
	uint64_t elapsed;
	seq_elem e;

	if (!c->have_cap) {
		/* Install the source activator now that we have the handle. */
		if (c->shared->source_act == NULL)
			c->shared->source_act = dtfl_op_activator(op);
		c->cap = dtfl_op_initial_cap(op, 0);
		c->have_cap = (c->cap != NULL);
	}
	if (!c->have_cap)
		return false;

	/* Downgrade the capability to the current elapsed time. */
	elapsed = __dtfl_clock_ns() - c->shared->start_ns;
	dtfl_cap_downgrade(c->cap, &elapsed);

	/* Drain the send queue, broadcasting each element to every worker. */
	{
		dtfl_session *s = dtfl_op_session(op, 0, c->cap);

		while (queue_pop(&c->shared->send, &e)) {
			size_t w;

			for (w = 0; w < peers; w++) {
				unsigned char hdr[16];
				unsigned char *dst;

				put_u64le(hdr, (uint64_t)w);
				put_u64le(hdr + 8, (uint64_t)c->shared->counter);
				dst = __dtfl_session_reserve(s, 16 + e.len);
				memcpy(dst, hdr, 16);
				if (e.len > 0)
					memcpy(dst + 16, e.bytes, e.len);
			}
			c->shared->counter++;
			__dtfl_free(e.bytes);
		}
		dtfl_session_close(s);
	}

	/* Catchup: keep firing until the capability passes the sink's time. */
	if (c->shared->have_catchup) {
		if (elapsed < c->shared->catchup_until) {
			dtfl_activator *a = dtfl_op_activator(op);

			dtfl_activator_activate(a);
			dtfl_activator_free(a);
		} else {
			c->shared->have_catchup = false;
		}
	}
	return true;	/* a source never completes while the handle lives */
}

static void
source_free(void *vctx)
{
	source_ctx *c = vctx;

	if (c->have_cap && c->cap != NULL)
		dtfl_cap_drop(c->cap);
	shared_release(c->shared);
	__dtfl_free(c);
}

/* ---- sink operator ---- */

typedef struct sink_ctx {
	seq_shared *shared;	/* retained */
	recvd_item *recvd;	/* owned; pending, sorted by (time, worker, counter) */
	size_t nrecvd, caprecvd;
} sink_ctx;

static int
recvd_cmp(const void *va, const void *vb)
{
	const recvd_item *a = va, *b = vb;

	if (a->time != b->time)
		return a->time < b->time ? -1 : 1;
	if (a->worker != b->worker)
		return a->worker < b->worker ? -1 : 1;
	if (a->counter != b->counter)
		return a->counter < b->counter ? -1 : 1;
	return 0;
}

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	sink_ctx *c = vctx;
	dtfl_message *m;
	size_t i, count;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* Collect every tuple from this round's messages. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		size_t n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const unsigned char *rec = dtfl_message_record(m, i, &len);
			recvd_item it;

			if (len < 16)
				continue;
			it.time = t[0];
			it.worker = get_u64le(rec);
			it.counter = get_u64le(rec + 8);
			it.len = len - 16;
			it.bytes = __dtfl_memdup(rec + 16, it.len);
			if (c->nrecvd == c->caprecvd) {
				c->caprecvd = c->caprecvd ? c->caprecvd * 2 : 16;
				c->recvd = __dtfl_realloc(c->recvd,
				    c->caprecvd * sizeof(recvd_item));
			}
			c->recvd[c->nrecvd++] = it;
		}
	}

	/* Sort by (time, worker, counter) (qsort(NULL, 0) trips UBSan). */
	if (c->nrecvd > 1)
		qsort(c->recvd, c->nrecvd, sizeof(recvd_item), recvd_cmp);

	/* Keep the source firing until its time passes the greatest seen. */
	if (c->nrecvd > 0) {
		c->shared->catchup_until = c->recvd[c->nrecvd - 1].time;
		c->shared->have_catchup = true;
		if (c->shared->source_act != NULL)
			dtfl_activator_activate(c->shared->source_act);
	}

	/* Deliver tuples whose time the input frontier has passed. */
	count = 0;
	for (i = 0; i < c->nrecvd; i++) {
		if (!dtfl_op_frontier_less_equal(op, 0, &c->recvd[i].time))
			count++;
		else
			break;	/* sorted: once the frontier holds one, it holds the rest */
	}
	for (i = 0; i < count; i++) {
		queue_push(&c->shared->recv, c->recvd[i].bytes, c->recvd[i].len);
		__dtfl_free(c->recvd[i].bytes);
	}
	if (count > 0) {
		memmove(c->recvd, c->recvd + count,
		    (c->nrecvd - count) * sizeof(recvd_item));
		c->nrecvd -= count;
	}
	return true;
}

static void
sink_free(void *vctx)
{
	sink_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->nrecvd; i++)
		__dtfl_free(c->recvd[i].bytes);
	__dtfl_free(c->recvd);
	shared_release(c->shared);
	__dtfl_free(c);
}

/* Route a tuple to its target worker (the leading u64). */
static uint64_t
seq_route(const void *rec, size_t len, void *ctx)
{
	(void)ctx;
	if (len < 8)
		return 0;
	return get_u64le(rec);
}

/* ---- the dataflow ---- */

typedef struct seq_build {
	seq_shared *shared;
	const dtfl_type *type;
} seq_build;

static void
seq_dataflow(dtfl_scope *scope, void *arg)
{
	seq_build *sb = arg;
	const dtfl_type *tuple = dtfl_type_bytes();	/* opaque (w, c, elem) tuple */
	dtfl_op_builder *src = dtfl_op_builder_new(scope, "SequenceInput");
	dtfl_stream *stream;
	source_ctx *sc = __dtfl_calloc(1, sizeof *sc);
	dtfl_op_builder *snk;
	sink_ctx *kc;

	(void)sb->type;
	sc->shared = shared_retain(sb->shared);
	stream = dtfl_op_builder_output(src, tuple);
	dtfl_op_builder_build(src, source_logic, sc, source_free);

	snk = dtfl_op_builder_new(scope, "SequenceOutput");
	kc = __dtfl_calloc(1, sizeof *kc);
	kc->shared = shared_retain(sb->shared);
	dtfl_op_builder_input(snk, stream, dtfl_pact_exchange(seq_route, NULL));
	dtfl_op_builder_build(snk, sink_logic, kc, sink_free);
}

dtfl_sequencer *
dtfl_sequencer_new(dtfl_worker *w, const dtfl_type *type)
{
	dtfl_sequencer *s = __dtfl_calloc(1, sizeof *s);
	dtfl_time_class *tc = dtfl_time_duration();
	seq_build build;

	s->worker = w;
	s->type = dtfl_type_retain(type);
	s->shared = __dtfl_calloc(1, sizeof *s->shared);
	s->shared->refs = 1;
	s->shared->peers = dtfl_worker_peers(w);
	s->shared->start_ns = __dtfl_clock_ns();
	build.shared = s->shared;
	build.type = type;
	dtfl_worker_dataflow(w, "Sequencer", tc, seq_dataflow, &build);
	dtfl_time_class_free(tc);
	return s;
}

void
dtfl_sequencer_push(dtfl_sequencer *s, const void *rec, size_t len)
{
	queue_push(&s->shared->send, rec, len);
	if (s->shared->source_act != NULL)
		dtfl_activator_activate(s->shared->source_act);
}

const void *
dtfl_sequencer_next(dtfl_sequencer *s, size_t *len)
{
	seq_elem e;

	/* Keep one element alive for the caller to read until the next call. */
	__dtfl_free(s->last_bytes);
	s->last_bytes = NULL;
	if (!queue_pop(&s->shared->recv, &e))
		return NULL;
	s->last_bytes = e.bytes;
	*len = e.len;
	return e.bytes;
}

void
dtfl_sequencer_free(dtfl_sequencer *s)
{
	if (s == NULL)
		return;
	/* Activate the source so it drops its capability (upstream Drop). */
	if (s->shared->source_act != NULL)
		dtfl_activator_activate(s->shared->source_act);
	__dtfl_free(s->last_bytes);
	shared_release(s->shared);
	dtfl_type_free(s->type);
	__dtfl_free(s);
}
