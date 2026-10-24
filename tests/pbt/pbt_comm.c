/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_comm.c: the typed Process allocator and the simulation
 * allocator against a model, under random schedules.
 *
 * One thread drives every worker of a group through a random
 * interleaving of allocate, send, flush and receive-and-pull steps
 * (the threaded exchange is in tests/unit/test_comm.c).  The model is
 * the specification every allocator shares:
 *   - every message sent is received exactly once, by its target, on
 *     its channel;
 *   - messages on one (sender, receiver, channel) arrive in send order;
 *   - a message pulled after receive() has its channel's event pending;
 *   - nothing leaks (every message is freed once).
 * The simulator runs with a drawn delay fault too (sim_int.h), which must
 * keep all four.
 */
#include "pbt.h"

#include "comm/comm_int.h"
#include "comm/sim_int.h"

#define MAXW 4
#define MAXC 3

static long live;	/* messages alive; single-threaded */

static void *
msg_new(uint64_t v)
{
	uint64_t *m = __dtfl_alloc(sizeof *m);

	*m = v;
	live++;
	return m;
}

static void *
msg_clone(const __dtfl_msg_class *mc, const void *m)
{
	(void)mc;
	return msg_new(*(const uint64_t *)m);
}

static void
msg_free(const __dtfl_msg_class *mc, void *m)
{
	(void)mc;
	live--;
	__dtfl_free(m);
}

static const __dtfl_msg_class mclass = {
	"u64", NULL, NULL, NULL, msg_clone, msg_free, NULL
};

#define PACK(s, c, seq) (((uint64_t)(s) << 48) | ((uint64_t)(c) << 32) | (seq))

typedef struct group {
	size_t n;
	__dtfl_allocator *a[MAXW];
	bool have[MAXW][MAXC];
	__dtfl_push *push[MAXW][MAXC][MAXW];
	__dtfl_pull *pull[MAXW][MAXC];
	/* Model: sent and received counts per (sender, receiver, channel). */
	uint32_t sent[MAXW][MAXW][MAXC];
	uint32_t recvd[MAXW][MAXW][MAXC];
	bool bad;
} group;

/* Destroys every endpoint and allocator; idempotent. */
static void
group_teardown(group *g)
{
	size_t w, c, t;

	for (w = 0; w < g->n; w++) {
		for (c = 0; c < MAXC; c++) {
			if (!g->have[w][c])
				continue;
			for (t = 0; t < g->n; t++)
				__dtfl_push_destroy(g->push[w][c][t]);
			__dtfl_pull_destroy(g->pull[w][c]);
		}
		__dtfl_allocator_destroy(g->a[w]);
	}
	g->n = 0;
}

static void
group_free(void *arg)
{
	group_teardown(arg);
	__dtfl_free(arg);
}

static void
ensure(group *g, size_t w, size_t c)
{
	if (!g->have[w][c]) {
		__dtfl_allocate(g->a[w], 10 + c, &mclass, g->push[w][c],
		    &g->pull[w][c]);
		g->have[w][c] = true;
	}
}

/* receive(), then pull everything on worker w, checking the model. */
static void
recv_step(group *g, size_t w)
{
	size_t c, i;

	__dtfl_allocator_receive(g->a[w]);
	for (c = 0; c < MAXC; c++) {
		bool pulled = false, event = false;
		void *m;

		if (!g->have[w][c])
			continue;
		while ((m = __dtfl_pull_recv(g->pull[w][c])) != NULL) {
			uint64_t v = *(uint64_t *)m;
			size_t s = (size_t)(v >> 48), mc = (v >> 32) & 0xffff;
			uint32_t seq = (uint32_t)v;

			msg_free(&mclass, m);
			pulled = true;
			if (s >= g->n || mc != c ||
			    seq != g->recvd[s][w][c] ||
			    seq >= g->sent[s][w][c]) {
				g->bad = true;
				return;
			}
			g->recvd[s][w][c]++;
		}
		for (i = 0; i < g->a[w]->events.ids.len; i++)
			if (*DTFL_VEC_AT(&g->a[w]->events.ids, size_t, i) ==
			    10 + c)
				event = true;
		if (pulled && !event) {
			g->bad = true;
			return;
		}
	}
	/*
	 * Consume the events of allocated channels.  An event for a channel
	 * this worker has not allocated yet stays pending, so the check
	 * above still sees it when the data is finally pulled.
	 */
	for (i = g->a[w]->events.ids.len; i-- > 0;) {
		size_t id = *DTFL_VEC_AT(&g->a[w]->events.ids, size_t, i);

		if (id >= 10 && id < 10 + MAXC && g->have[w][id - 10])
			__dtfl_vec_remove(&g->a[w]->events.ids, i);
	}
}

static void
prop_schedule(pbt_tc *tc, bool sim)
{
	__dtfl_allocator_builder *b[MAXW];
	group *g = __dtfl_calloc(1, sizeof *g);
	size_t w, c, t, s, rounds;
	int64_t steps;
	bool done;

	g->n = (size_t)pbt_int(tc, 1, MAXW);
	if (sim) {
		__dtfl_sim_faults f;

		memset(&f, 0, sizeof f);
		f.delay_max = (uint32_t)pbt_int(tc, 0, 8);
		__dtfl_sim_builders(g->n, (uint64_t)pbt_int(tc, 0, INT64_MAX),
		    &f, b);
	} else
		__dtfl_process_builders(g->n, b);
	for (w = 0; w < g->n; w++)
		g->a[w] = __dtfl_allocator_build(b[w]);
	pbt_defer(tc, group_free, g);

	for (steps = pbt_int(tc, 0, 150); steps > 0; steps--) {
		int64_t op = pbt_int(tc, 0, 3);

		w = (size_t)pbt_int(tc, 0, (int64_t)g->n - 1);
		c = (size_t)pbt_int(tc, 0, MAXC - 1);
		if (op <= 1) {
			/* Allocation order may differ between workers. */
			t = (size_t)pbt_int(tc, 0, (int64_t)g->n - 1);
			ensure(g, w, c);
			__dtfl_push_send(g->push[w][c][t], &mclass,
			    msg_new(PACK(w, c, g->sent[w][t][c]++)));
		} else if (op == 2) {
			if (g->have[w][c]) {
				t = (size_t)pbt_int(tc, 0, (int64_t)g->n - 1);
				__dtfl_push_done(g->push[w][c][t]);
			}
		} else {
			recv_step(g, w);
			PBT_ASSERT(!g->bad);
		}
	}

	/* Everyone allocates everything, then drain to quiescence. */
	for (w = 0; w < g->n; w++)
		for (c = 0; c < MAXC; c++)
			ensure(g, w, c);
	for (rounds = 0, done = false; !done && rounds < 10000; rounds++) {
		for (w = 0; w < g->n; w++) {
			recv_step(g, w);
			PBT_ASSERT(!g->bad);
		}
		done = true;
		for (s = 0; s < g->n; s++)
			for (t = 0; t < g->n; t++)
				for (c = 0; c < MAXC; c++)
					if (g->recvd[s][t][c] != g->sent[s][t][c])
						done = false;
	}
	PBT_ASSERT(done);
	group_teardown(g);
	PBT_ASSERT(live == 0);
}

static void
prop_process(pbt_tc *tc)
{
	live = 0;
	prop_schedule(tc, false);
}

static void
prop_sim(pbt_tc *tc)
{
	live = 0;
	prop_schedule(tc, true);
}

/*
 * The simulator replays: the same seed and calls give the same
 * delivery, observed as the sequence of values pulled.
 */
static void
sim_trace(uint64_t seed, const __dtfl_sim_faults *f, size_t n,
    const int64_t *plan, size_t nplan, __dtfl_vec *trace)
{
	__dtfl_allocator_builder *b[MAXW];
	__dtfl_allocator *a[MAXW];
	__dtfl_push *p[MAXW][MAXW];
	__dtfl_pull *l[MAXW];
	size_t i, w;
	void *m;

	__dtfl_sim_builders(n, seed, f, b);
	for (w = 0; w < n; w++) {
		a[w] = __dtfl_allocator_build(b[w]);
		__dtfl_allocate(a[w], 0, &mclass, p[w], &l[w]);
	}
	for (i = 0; i < nplan; i++) {
		w = (size_t)plan[i] % n;
		if (plan[i] < 0) {
			w = (size_t)(-plan[i]) % n;
			__dtfl_allocator_receive(a[w]);
			while ((m = __dtfl_pull_recv(l[w])) != NULL) {
				uint64_t rec = ((uint64_t)w << 56) ^ *(uint64_t *)m;

				__dtfl_vec_push(trace, &rec);
				msg_free(&mclass, m);
			}
		} else {
			__dtfl_push_send(p[w][(size_t)(plan[i] / MAXW) % n],
			    &mclass, msg_new((uint64_t)i));
		}
	}
	for (w = 0; w < n; w++) {
		for (i = 0; i < n; i++)
			__dtfl_push_destroy(p[w][i]);
		__dtfl_pull_destroy(l[w]);
		__dtfl_allocator_destroy(a[w]);
	}
}

static void
free_vec(void *v)
{
	__dtfl_vec_free(v);
}

static void
prop_sim_replay(pbt_tc *tc)
{
	static __dtfl_vec t1, t2;
	int64_t plan[120];
	size_t n = (size_t)pbt_int(tc, 1, MAXW), np, i;
	uint64_t seed = (uint64_t)pbt_int(tc, 0, INT64_MAX);
	__dtfl_sim_faults f;

	memset(&f, 0, sizeof f);
	f.delay_max = (uint32_t)pbt_int(tc, 0, 8);
	live = 0;
	np = (size_t)pbt_int(tc, 0, 120);
	for (i = 0; i < np; i++)
		plan[i] = pbt_int(tc, -(MAXW * MAXW), MAXW * MAXW);
	__dtfl_vec_init(&t1, sizeof(uint64_t));
	__dtfl_vec_init(&t2, sizeof(uint64_t));
	pbt_defer(tc, free_vec, &t1);
	pbt_defer(tc, free_vec, &t2);
	sim_trace(seed, &f, n, plan, np, &t1);
	sim_trace(seed, &f, n, plan, np, &t2);
	PBT_ASSERT(t1.len == t2.len);
	PBT_ASSERT(t1.len == 0 || memcmp(t1.data, t2.data, t1.len * t1.elem) == 0);
	PBT_ASSERT(live == 0);
}

static const pbt_prop props[] = {
	{ "process_schedule_model", prop_process, 300 },
	{ "sim_schedule_model", prop_sim, 300 },
	{ "sim_replay", prop_sim_replay, 200 },
};

PBT_MAIN("comm", props)
