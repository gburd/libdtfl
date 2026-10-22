/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/alloc_sim.c: the simulation allocator (PLAN D8).  See
 * sim_int.h for the delivery model.
 *
 * Not a port: upstream has no simulator.  The channel rendezvous and
 * endpoint protocol follow process.rs so that code built on the
 * Process allocator runs unchanged on this one.
 *
 * Everything here belongs to the one simulation thread; counts are
 * plain integers and nothing is locked.
 *
 * Delay faults: each in-flight entry carries the receive turn from
 * which it may be delivered.  receive() only ever delivers a prefix of
 * a FIFO, and the prefix stops at the first entry that is not ready, so
 * a delay never reorders one (sender, receiver, channel) FIFO.
 */
#include "comm/sim_int.h"

typedef struct sim_shared sim_shared;

typedef struct sim_channel {
	sim_shared *shared;
	size_t identifier;
	const __dtfl_msg_class *mc;
	/* sim_entry: in flight from s to r at [s * peers + r] */
	__dtfl_fifo *inflight;
	__dtfl_fifo *inbox;	/* void *: delivered to r, not yet pulled */
	bool *taken;		/* r has allocated this channel */
	bool *closed;		/* r's puller is gone */
	size_t untaken;		/* workers yet to allocate */
	size_t endpoints;	/* live pushers and pullers */
} sim_channel;

struct sim_shared {
	size_t refs;		/* builders, allocators and channels */
	size_t peers;
	uint64_t rng;		/* splitmix64 state */
	uint64_t clock_ns;	/* virtual time */
	uint64_t turn;		/* receive() calls so far, by any worker */
	__dtfl_sim_faults faults;
	__dtfl_vec channels;	/* sim_channel *, in creation order */
};

/* A message in flight (NULL = flush) and the turn it becomes ready. */
typedef struct sim_entry {
	void *m;
	uint64_t ready;
} sim_entry;

/* One delivery candidate during receive(): a queue and its budget. */
typedef struct sim_cand {
	sim_channel *ch;
	__dtfl_fifo *q;
	uint64_t budget;
} sim_cand;

uint64_t
__dtfl_splitmix64(uint64_t *state)
{
	uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));

	z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
	return z ^ (z >> 31);
}

/* A value in [0, n); n > 0.  The modulo bias is irrelevant here. */
static uint64_t
rng_below(sim_shared *s, uint64_t n)
{
	return __dtfl_sim_rand(s) % n;
}

static void
shared_release(sim_shared *s)
{
	if (--s->refs > 0)
		return;
	DTFL_ASSERT(s->channels.len == 0);
	__dtfl_vec_free(&s->channels);
	__dtfl_free(s);
}

/* Frees every message left in q (flush markers are NULL). */
static void
fifo_drop(__dtfl_fifo *q, const __dtfl_msg_class *mc)
{
	void *m;

	while (__dtfl_fifo_pop(q, &m))
		if (m != NULL)
			mc->free(mc, m);
	__dtfl_fifo_free(q);
}

/* As fifo_drop, for a FIFO of sim_entry. */
static void
inflight_drop(__dtfl_fifo *q, const __dtfl_msg_class *mc)
{
	sim_entry e;

	while (__dtfl_fifo_pop(q, &e))
		if (e.m != NULL)
			mc->free(mc, e.m);
	__dtfl_fifo_free(q);
}

/*
 * How many entries at the head of q are ready to deliver at the current
 * turn: all of them without delay faults, else up to the first entry
 * still held back.
 */
static size_t
ready_prefix(const sim_shared *s, const __dtfl_fifo *q)
{
	size_t i, n = __dtfl_fifo_len(q);

	if (s->faults.delay_max == 0)
		return n;
	for (i = 0; i < n; i++) {
		const sim_entry *e = DTFL_VEC_AT(&q->v, sim_entry, q->head + i);

		if (e->ready > s->turn)
			break;
	}
	return i;
}

/* Frees the channel once every worker took it and no endpoint is left. */
static void
channel_maybe_free(sim_channel *c)
{
	sim_shared *s = c->shared;
	size_t i, n = s->peers;

	if (c->untaken > 0 || c->endpoints > 0)
		return;
	for (i = 0; i < s->channels.len; i++)
		if (*DTFL_VEC_AT(&s->channels, sim_channel *, i) == c) {
			__dtfl_vec_remove(&s->channels, i);
			break;
		}
	for (i = 0; i < n * n; i++)
		inflight_drop(&c->inflight[i], c->mc);
	for (i = 0; i < n; i++)
		fifo_drop(&c->inbox[i], c->mc);
	__dtfl_free(c->inflight);
	__dtfl_free(c->inbox);
	__dtfl_free(c->taken);
	__dtfl_free(c->closed);
	__dtfl_free(c);
	shared_release(s);
}

/* The push half: from `source` to `target` on one channel. */
typedef struct sim_pusher {
	__dtfl_push base;
	sim_channel *ch;
	size_t source, target;
} sim_pusher;

static void
sim_pusher_push(__dtfl_push *p, void **element)
{
	sim_pusher *sp = (sim_pusher *)p;
	sim_channel *c = sp->ch;
	sim_shared *s = c->shared;
	sim_entry e = { *element, 0 };

	/* Data and flushes alike enter the FIFO; see sim_int.h. */
	if (*element != NULL && c->closed[sp->target]) {
		c->mc->free(c->mc, *element);
	} else {
		if (s->faults.delay_max > 0)
			e.ready = s->turn + rng_below(s,
			    (uint64_t)s->faults.delay_max + 1);
		__dtfl_fifo_push(&c->inflight[sp->source * s->peers +
		    sp->target], &e);
	}
	*element = NULL;
}

static void
sim_pusher_destroy(__dtfl_push *p)
{
	sim_channel *c = ((sim_pusher *)p)->ch;

	__dtfl_free(p);
	c->endpoints--;
	channel_maybe_free(c);
}

static const __dtfl_push_ops sim_pusher_ops = {
	sim_pusher_push, sim_pusher_destroy
};

/* The pull half: worker `index`'s delivered messages. */
typedef struct sim_puller {
	__dtfl_pull base;
	sim_channel *ch;
	size_t index;
} sim_puller;

static void *
sim_puller_pull(__dtfl_pull *p)
{
	sim_puller *sp = (sim_puller *)p;
	void *m;

	return __dtfl_fifo_pop(&sp->ch->inbox[sp->index], &m) ? m : NULL;
}

static void
sim_puller_destroy(__dtfl_pull *p)
{
	sim_puller *sp = (sim_puller *)p;
	sim_channel *c = sp->ch;
	void *m;

	/* As a dropped Receiver: queued messages go, later ones are freed. */
	c->closed[sp->index] = true;
	while (__dtfl_fifo_pop(&c->inbox[sp->index], &m))
		c->mc->free(c->mc, m);
	__dtfl_free(sp);
	c->endpoints--;
	channel_maybe_free(c);
}

static const __dtfl_pull_ops sim_puller_ops = {
	sim_puller_pull, sim_puller_destroy
};

typedef struct sim_alloc {
	__dtfl_allocator base;
	sim_shared *shared;	/* one reference */
	__dtfl_vec cands;	/* sim_cand scratch for receive() */
} sim_alloc;

static sim_channel *
channel_new(sim_shared *s, size_t identifier, const __dtfl_msg_class *mc)
{
	sim_channel *c = __dtfl_alloc(sizeof *c);
	size_t i, n = s->peers;

	c->shared = s;
	s->refs++;
	c->identifier = identifier;
	c->mc = mc;
	c->inflight = __dtfl_calloc(n * n, sizeof *c->inflight);
	c->inbox = __dtfl_calloc(n, sizeof *c->inbox);
	c->taken = __dtfl_calloc(n, sizeof *c->taken);
	c->closed = __dtfl_calloc(n, sizeof *c->closed);
	for (i = 0; i < n * n; i++)
		__dtfl_fifo_init(&c->inflight[i], sizeof(sim_entry));
	for (i = 0; i < n; i++)
		__dtfl_fifo_init(&c->inbox[i], sizeof(void *));
	c->untaken = n;
	c->endpoints = 0;
	__dtfl_vec_push(&s->channels, &c);
	return c;
}

static void
sim_allocate(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pushers, __dtfl_pull **puller)
{
	sim_shared *s = ((sim_alloc *)a)->shared;
	sim_channel *c = NULL;
	sim_puller *pl;
	size_t i;

	for (i = 0; i < s->channels.len; i++) {
		c = *DTFL_VEC_AT(&s->channels, sim_channel *, i);
		if (c->identifier == identifier)
			break;
		c = NULL;
	}
	if (c == NULL)
		c = channel_new(s, identifier, mc);
	if (c->mc != mc)
		__dtfl_panic("failed to correctly cast channel %zu", identifier);
	if (c->taken[a->index])
		__dtfl_panic("channel %zu already consumed", identifier);
	c->taken[a->index] = true;
	c->untaken--;
	for (i = 0; i < s->peers; i++) {
		sim_pusher *sp = __dtfl_alloc(sizeof *sp);

		sp->base.ops = &sim_pusher_ops;
		sp->ch = c;
		sp->source = a->index;
		sp->target = i;
		pushers[i] = &sp->base;
	}
	pl = __dtfl_alloc(sizeof *pl);
	pl->base.ops = &sim_puller_ops;
	pl->ch = c;
	pl->index = a->index;
	c->endpoints += s->peers + 1;
	*puller = __dtfl_count_puller_new(&pl->base, identifier, &a->events);
}

/*
 * Delivers a pseudo-random part of what is in flight to this worker:
 * each (channel, sender) FIFO gets a budget uniform in [0, ready], where
 * ready is its deliverable prefix (ready_prefix), and the budgets are
 * spent one head at a time on a uniformly chosen candidate, which keeps
 * every FIFO in order while interleaving them.
 */
static void
sim_receive(__dtfl_allocator *a)
{
	sim_alloc *sa = (sim_alloc *)a;
	sim_shared *s = sa->shared;
	size_t i, j, r = a->index;
	uint64_t total = 0;

	s->turn++;
	__dtfl_vec_clear(&sa->cands);
	for (i = 0; i < s->channels.len; i++) {
		sim_channel *c = *DTFL_VEC_AT(&s->channels, sim_channel *, i);

		for (j = 0; j < s->peers; j++) {
			__dtfl_fifo *q = &c->inflight[j * s->peers + r];
			sim_cand cand = { c, q, 0 };
			size_t ready;

			if (__dtfl_fifo_len(q) == 0)
				continue;
			ready = ready_prefix(s, q);
			if (ready == 0)
				continue;
			cand.budget = rng_below(s, ready + 1);
			if (cand.budget > 0) {
				__dtfl_vec_push(&sa->cands, &cand);
				total += cand.budget;
			}
		}
	}
	while (total > 0) {
		size_t k = (size_t)rng_below(s, sa->cands.len);
		sim_cand *cand = DTFL_VEC_AT(&sa->cands, sim_cand, k);
		sim_channel *c = cand->ch;
		sim_entry e;
		void *m;

		if (!__dtfl_fifo_pop(cand->q, &e))
			__dtfl_panic("sim: delivery budget exceeds queue");
		m = e.m;
		if (m != NULL) {
			if (c->closed[r])
				c->mc->free(c->mc, m);
			else
				__dtfl_fifo_push(&c->inbox[r], &m);
		}
		/* As ArcPusher: every push, data or flush, raises the event. */
		__dtfl_events_push(&a->events, c->identifier);
		total--;
		if (--cand->budget == 0)
			__dtfl_vec_swap_remove(&sa->cands, k);
	}
}

/*
 * One thread runs every worker, so parking would deadlock.  Idle time
 * passes on the virtual clock instead (see sim_int.h).
 */
static void
sim_await_events(__dtfl_allocator *a, uint64_t timeout_ns)
{
	if (a->events.ids.len > 0)
		return;
	__dtfl_sim_advance(((sim_alloc *)a)->shared,
	    timeout_ns == UINT64_MAX ? DTFL_SIM_IDLE_STEP_NS : timeout_ns);
}

/*
 * Worker `index` is leaving.  A channel it never allocated would wait
 * for it forever: take it now, as a worker that dropped its puller at
 * once.  Backwards, because channel_maybe_free removes from the list.
 */
static void
forsake_channels(sim_shared *s, size_t index)
{
	size_t i;

	for (i = s->channels.len; i-- > 0;) {
		sim_channel *c = *DTFL_VEC_AT(&s->channels, sim_channel *, i);

		if (c->taken[index])
			continue;
		c->taken[index] = true;
		c->closed[index] = true;
		c->untaken--;
		channel_maybe_free(c);
	}
}

static void
sim_destroy(__dtfl_allocator *a)
{
	sim_alloc *sa = (sim_alloc *)a;

	forsake_channels(sa->shared, a->index);
	__dtfl_vec_free(&sa->cands);
	shared_release(sa->shared);
	__dtfl_allocator_fini(a);
	__dtfl_free(sa);
}

static const __dtfl_allocator_ops sim_ops = {
	.allocate = sim_allocate,
	.receive = sim_receive,
	.await_events = sim_await_events,
	.destroy = sim_destroy,
};

typedef struct sim_builder {
	__dtfl_allocator_builder base;
	sim_shared *shared;	/* one reference */
	size_t index;
} sim_builder;

static __dtfl_allocator *
sim_build(__dtfl_allocator_builder *b)
{
	sim_builder *sb = (sim_builder *)b;
	sim_alloc *sa = __dtfl_alloc(sizeof *sa);

	__dtfl_allocator_init(&sa->base, &sim_ops, sb->index,
	    sb->shared->peers, __dtfl_parker_new());
	sa->shared = sb->shared;	/* the builder's reference moves */
	__dtfl_vec_init(&sa->cands, sizeof(sim_cand));
	__dtfl_free(sb);
	return &sa->base;
}

static void
sim_builder_destroy(__dtfl_allocator_builder *b)
{
	sim_builder *sb = (sim_builder *)b;

	forsake_channels(sb->shared, sb->index);
	shared_release(sb->shared);
	__dtfl_free(sb);
}

void
__dtfl_sim_builders(size_t peers, uint64_t seed,
    const __dtfl_sim_faults *faults, __dtfl_allocator_builder **builders)
{
	sim_shared *s = __dtfl_alloc(sizeof *s);
	size_t i;

	DTFL_ASSERT(peers > 0);
	s->refs = peers;
	s->peers = peers;
	s->rng = seed;
	s->clock_ns = 0;
	s->turn = 0;
	if (faults != NULL)
		s->faults = *faults;
	else
		memset(&s->faults, 0, sizeof s->faults);
	__dtfl_vec_init(&s->channels, sizeof(sim_channel *));
	for (i = 0; i < peers; i++) {
		sim_builder *sb = __dtfl_alloc(sizeof *sb);

		sb->base.build = sim_build;
		sb->base.destroy = sim_builder_destroy;
		sb->shared = s;
		sb->index = i;
		builders[i] = &sb->base;
	}
}

void *
__dtfl_sim_of(__dtfl_allocator *a)
{
	return a->ops == &sim_ops ? ((sim_alloc *)a)->shared : NULL;
}

const __dtfl_sim_faults *
__dtfl_sim_faults_of(void *sim)
{
	return &((sim_shared *)sim)->faults;
}

uint64_t
__dtfl_sim_rand(void *sim)
{
	return __dtfl_splitmix64(&((sim_shared *)sim)->rng);
}

uint64_t
__dtfl_sim_clock(void *sim)
{
	return ((sim_shared *)sim)->clock_ns;
}

void
__dtfl_sim_advance(void *sim, uint64_t ns)
{
	sim_shared *s = sim;

	s->clock_ns = ns > UINT64_MAX - s->clock_ns ? UINT64_MAX :
	    s->clock_ns + ns;
}
