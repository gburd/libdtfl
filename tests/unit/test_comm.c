/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_comm.c: the parker, the typed channels, and the
 * Thread, Process and simulation allocators.
 *
 * Cross-thread tests: worker threads never call munit asserts (their
 * longjmp target is on the main thread).  A worker records the first
 * failure in its result and returns; the main thread asserts after
 * joining.  Every wait is bounded by DEADLINE_NS, and a worker that
 * hits it reports a timeout instead of hanging the suite.
 */
#include "dtfl_test.h"
#include "test_comm_msg.h"

#include <stdio.h>

#include "base/os_thread.h"
#include "comm/init_int.h"
#include "comm/sim_int.h"

#define DEADLINE_NS (UINT64_C(20) * 1000000000)	/* 20 s */
#define MS (UINT64_C(1000000))

/* Moves every pending event id out of the allocator into a vector. */
static size_t
drain_events(__dtfl_allocator *a, size_t id)
{
	size_t i, n = 0;

	for (i = 0; i < a->events.ids.len; i++)
		if (*DTFL_VEC_AT(&a->events.ids, size_t, i) == id)
			n++;
	__dtfl_vec_clear(&a->events.ids);
	return n;
}

/* ------------------------------------------------------------------ */
/* Parker                                                              */
/* ------------------------------------------------------------------ */

DTFL_TEST(parker_unpark_before_park_returns_at_once)
{
	__dtfl_parker *p = __dtfl_parker_new();
	uint64_t t0;

	(void)params; (void)data;
	__dtfl_parker_unpark(p);
	t0 = __dtfl_clock_ns();
	__dtfl_parker_park(p, UINT64_MAX);	/* would hang if lost */
	assert_true(__dtfl_clock_ns() - t0 < 1000 * MS);
	/* One token: two unparks still make only one park return early. */
	__dtfl_parker_unpark(p);
	__dtfl_parker_unpark(p);
	__dtfl_parker_park(p, 0);
	t0 = __dtfl_clock_ns();
	__dtfl_parker_park(p, 50 * MS);
	assert_true(__dtfl_clock_ns() - t0 >= 50 * MS);
	__dtfl_parker_release(p);
	return MUNIT_OK;
}

DTFL_TEST(parker_timeout_elapses)
{
	__dtfl_parker *p = __dtfl_parker_new();
	uint64_t t0, dt;

	(void)params; (void)data;
	t0 = __dtfl_clock_ns();
	__dtfl_parker_park(p, 30 * MS);
	dt = __dtfl_clock_ns() - t0;
	assert_true(dt >= 30 * MS);
	assert_true(dt < 5000 * MS);
	__dtfl_parker_park(p, 0);	/* zero timeout returns */
	__dtfl_parker_release(p);
	return MUNIT_OK;
}

typedef struct waker_arg {
	__dtfl_parker *p;
	_Atomic int flag;	/* release-store before unpark */
} waker_arg;

static void *
waker_main(void *arg)
{
	waker_arg *w = arg;
	struct timespec ts = { 0, 20 * 1000 * 1000 };

	nanosleep(&ts, NULL);
	atomic_store_explicit(&w->flag, 1, memory_order_relaxed);
	__dtfl_parker_unpark(w->p);
	return NULL;
}

DTFL_TEST(parker_cross_thread_wakes)
{
	waker_arg w;
	__dtfl_thread t;
	uint64_t t0;
	int i;

	(void)params; (void)data;
	w.p = __dtfl_parker_new();
	for (i = 0; i < 20; i++) {
		atomic_init(&w.flag, 0);
		assert_int(__dtfl_thread_spawn("waker", waker_main, &w, &t), ==, 0);
		t0 = __dtfl_clock_ns();
		/* Spurious wakeups are allowed; the flag is the truth. */
		while (atomic_load_explicit(&w.flag, memory_order_relaxed) == 0 &&
		    __dtfl_clock_ns() - t0 < DEADLINE_NS)
			__dtfl_parker_park(w.p, DEADLINE_NS);
		(void)__dtfl_thread_join(t);
		assert_int(atomic_load(&w.flag), ==, 1);
		assert_true(__dtfl_clock_ns() - t0 < DEADLINE_NS);
		__dtfl_parker_park(w.p, 0);	/* consume any leftover token */
	}
	__dtfl_parker_release(w.p);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* FIFO and MPSC                                                       */
/* ------------------------------------------------------------------ */

DTFL_TEST(fifo_order_and_compaction)
{
	__dtfl_fifo q;
	size_t i, v, next = 0, pushed = 0;

	(void)params; (void)data;
	__dtfl_fifo_init(&q, sizeof(size_t));
	for (i = 0; i < 1000; i++) {
		__dtfl_fifo_push(&q, &pushed);
		pushed++;
		if (i % 3 != 0) {
			assert_true(__dtfl_fifo_pop(&q, &v));
			assert_size(v, ==, next);
			next++;
		}
	}
	/* Compaction bounds storage by the live length, not the total. */
	assert_size(q.v.cap, <=, 4 * __dtfl_fifo_len(&q) + 8);
	while (__dtfl_fifo_pop(&q, &v))
		assert_size(v, ==, next++);
	assert_size(next, ==, pushed);
	__dtfl_fifo_free(&q);
	return MUNIT_OK;
}

static void
drop_msg_elem(void *elem, void *ctx)
{
	(void)ctx;
	test_msg_free(&test_msg_class, *(void **)elem);
}

DTFL_TEST(mpsc_close_drops_and_refuses)
{
	__dtfl_mpsc *q = __dtfl_mpsc_new(sizeof(void *), drop_msg_elem, NULL);
	void *m;

	(void)params; (void)data;
	atomic_store(&test_msg_live, 0);
	m = test_msg_new(1);
	assert_true(__dtfl_mpsc_send(q, &m));
	m = test_msg_new(2);
	assert_true(__dtfl_mpsc_send(q, &m));
	assert_true(__dtfl_mpsc_recv(q, &m));
	assert_uint64(test_msg_val(m), ==, 1);
	test_msg_free(&test_msg_class, m);
	__dtfl_mpsc_close(q);			/* drops message 2 */
	assert_long(atomic_load(&test_msg_live), ==, 0);
	m = test_msg_new(3);
	assert_false(__dtfl_mpsc_send(q, &m));	/* sender keeps it */
	test_msg_free(&test_msg_class, m);
	__dtfl_mpsc_release(q);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Thread allocator                                                    */
/* ------------------------------------------------------------------ */

DTFL_TEST(thread_allocator_fifo_and_events)
{
	__dtfl_allocator *a = __dtfl_allocator_build(__dtfl_thread_builder());
	__dtfl_push *push[1];
	__dtfl_pull *pull;
	void *m;
	uint64_t i;

	(void)params; (void)data;
	atomic_store(&test_msg_live, 0);
	assert_size(a->index, ==, 0);
	assert_size(a->peers, ==, 1);
	__dtfl_allocate(a, 7, &test_msg_class, push, &pull);
	for (i = 0; i < 5; i++)
		__dtfl_push_send(push[0], &test_msg_class, test_msg_new(i));
	/* counters.rs Pusher: one event per push. */
	assert_size(drain_events(a, 7), ==, 5);
	__dtfl_push_done(push[0]);
	assert_size(drain_events(a, 7), ==, 1);
	for (i = 0; i < 5; i++) {
		m = __dtfl_pull_recv(pull);
		assert_not_null(m);
		assert_uint64(test_msg_val(m), ==, i);
		test_msg_free(&test_msg_class, m);
	}
	assert_null(__dtfl_pull_recv(pull));
	/* counters.rs Puller: an event when a nonempty run ends. */
	assert_size(drain_events(a, 7), ==, 1);
	assert_null(__dtfl_pull_recv(pull));
	assert_size(drain_events(a, 7), ==, 0);
	/* Messages left in the channel are freed with it. */
	__dtfl_push_send(push[0], &test_msg_class, test_msg_new(99));
	__dtfl_push_destroy(push[0]);
	__dtfl_pull_destroy(pull);
	assert_long(atomic_load(&test_msg_live), ==, 0);
	/* await_events does not park while events are pending. */
	__dtfl_events_push(&a->events, 1);
	__dtfl_allocator_await_events(a, UINT64_MAX);
	__dtfl_allocator_destroy(a);
	return MUNIT_OK;
}

/* A pusher that recycles: wraps a thread pusher and plants elements. */
DTFL_TEST(thread_push_returns_recycled_or_null)
{
	__dtfl_events ev;
	__dtfl_push *push;
	__dtfl_pull *pull;
	void *m;

	(void)params; (void)data;
	__dtfl_vec_init(&ev.ids, sizeof(size_t));
	__dtfl_thread_new_from(3, &test_msg_class, &ev, &push, &pull);
	m = test_msg_new(5);
	__dtfl_push_push(push, &m);
	/* Nothing is recycled at the pin, so push hands back NULL. */
	assert_null(m);
	m = __dtfl_pull_recv(pull);
	assert_uint64(test_msg_val(m), ==, 5);
	test_msg_free(&test_msg_class, m);
	__dtfl_push_destroy(push);
	__dtfl_pull_destroy(pull);
	__dtfl_vec_free(&ev.ids);
	return MUNIT_OK;
}

DTFL_TEST(pipeline_is_local_and_unencoded)
{
	__dtfl_allocator_builder *b[2];
	__dtfl_allocator *a0, *a1;
	__dtfl_push *push;
	__dtfl_pull *pull;
	void *orig, *m;

	(void)params; (void)data;
	__dtfl_process_builders(2, b);
	a0 = __dtfl_allocator_build(b[0]);
	a1 = __dtfl_allocator_build(b[1]);
	__dtfl_pipeline(a1, 4, &test_msg_class, &push, &pull);
	orig = test_msg_new(42);
	__dtfl_push_send(push, &test_msg_class, orig);
	assert_size(drain_events(a1, 4), ==, 1);
	assert_size(a0->events.ids.len, ==, 0);
	m = __dtfl_pull_recv(pull);
	assert_ptr_equal(m, orig);	/* the same object: never encoded */
	test_msg_free(&test_msg_class, m);
	__dtfl_push_destroy(push);
	__dtfl_pull_destroy(pull);
	__dtfl_allocator_destroy(a0);
	__dtfl_allocator_destroy(a1);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Canary                                                              */
/* ------------------------------------------------------------------ */

DTFL_TEST(canary_reports_on_drop)
{
	__dtfl_vec q;
	__dtfl_canary c1, c2;

	(void)params; (void)data;
	__dtfl_vec_init(&q, sizeof(size_t));
	__dtfl_canary_init(&c1, 11, &q);
	__dtfl_canary_init(&c2, 12, &q);
	assert_size(q.len, ==, 0);
	__dtfl_canary_drop(&c2);
	__dtfl_canary_drop(&c1);
	assert_size(q.len, ==, 2);
	assert_size(*DTFL_VEC_AT(&q, size_t, 0), ==, 12);
	assert_size(*DTFL_VEC_AT(&q, size_t, 1), ==, 11);
	__dtfl_vec_free(&q);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Process allocator, many threads                                     */
/* ------------------------------------------------------------------ */

#define NCHAN 3
#define NMSG 300

typedef struct exch_arg {
	__dtfl_allocator_builder *builder;
	size_t peers;
	bool broadcast;		/* use one broadcast channel instead */
	/* Results, read by the main thread after join. */
	size_t got;		/* messages received */
	char err[256];
} exch_arg;

/*
 * Every worker sends NMSG messages to every worker on each of NCHAN
 * channels, then receives until it has peers * NCHAN * NMSG messages,
 * checking that each (sender, channel) stream arrives in order.
 */
static void *
exch_main(void *arg)
{
	exch_arg *x = arg;
	__dtfl_allocator *a = __dtfl_allocator_build(x->builder);
	size_t n = x->peers, me = a->index, c, t, s;
	__dtfl_push **push = calloc(NCHAN * n, sizeof *push);
	__dtfl_pull *pull[NCHAN];
	size_t *next = calloc(NCHAN * n, sizeof *next);
	size_t want = x->broadcast ? n * NMSG : n * NCHAN * NMSG;
	uint64_t t0 = __dtfl_clock_ns();
	unsigned k;

	if (push == NULL || next == NULL)
		abort();
	if (a->peers != n)
		snprintf(x->err, sizeof x->err, "peers %zu", a->peers);
	for (c = 0; c < NCHAN; c++) {
		if (x->broadcast && c > 0)
			break;
		if (x->broadcast)
			__dtfl_broadcast(a, 100 + c, &test_msg_class,
			    &push[c * n], &pull[c]);
		else
			__dtfl_allocate(a, 100 + c, &test_msg_class,
			    &push[c * n], &pull[c]);
	}
	for (k = 0; k < NMSG; k++)
		for (c = 0; c < NCHAN; c++) {
			if (x->broadcast) {
				if (c == 0)
					__dtfl_push_send(push[0], &test_msg_class,
					    test_msg_new(TM_PACK(me, c, k)));
				continue;
			}
			for (t = 0; t < n; t++)
				__dtfl_push_send(push[c * n + t], &test_msg_class,
				    test_msg_new(TM_PACK(me, c, k)));
		}
	for (c = 0; c < NCHAN * n; c++)
		if (push[c] != NULL)
			__dtfl_push_done(push[c]);
	while (x->got < want && x->err[0] == '\0') {
		bool any = false;

		__dtfl_allocator_receive(a);
		__dtfl_vec_clear(&a->events.ids);
		for (c = 0; c < NCHAN; c++) {
			void *m;

			if (x->broadcast && c > 0)
				break;
			while ((m = __dtfl_pull_recv(pull[c])) != NULL) {
				uint64_t v = test_msg_val(m);

				any = true;
				s = TM_SRC(v);
				if (s >= n || TM_CHAN(v) != c ||
				    TM_SEQ(v) != next[c * n + s]) {
					snprintf(x->err, sizeof x->err,
					    "worker %zu chan %zu: got src %zu "
					    "chan %zu seq %zu, want seq %zu",
					    me, c, s, TM_CHAN(v), TM_SEQ(v),
					    s < n ? next[c * n + s] : 0);
				} else {
					next[c * n + s]++;
				}
				x->got++;
				test_msg_free(&test_msg_class, m);
			}
		}
		__dtfl_allocator_release(a);
		if (!any) {
			if (__dtfl_clock_ns() - t0 > DEADLINE_NS) {
				snprintf(x->err, sizeof x->err, "worker %zu: "
				    "timeout with %zu of %zu", me, x->got, want);
				break;
			}
			/* The ArcPusher unparks us; the timeout is a backstop. */
			__dtfl_allocator_await_events(a, 100 * MS);
		}
	}
	for (c = 0; c < NCHAN * n; c++)
		__dtfl_push_destroy(push[c]);
	for (c = 0; c < NCHAN; c++)
		if (!(x->broadcast && c > 0))
			__dtfl_pull_destroy(pull[c]);
	__dtfl_allocator_destroy(a);
	free(push);
	free(next);
	return NULL;
}

static MunitResult
run_exchange(size_t n, bool broadcast)
{
	__dtfl_allocator_builder **b = calloc(n, sizeof *b);
	exch_arg *x = calloc(n, sizeof *x);
	__dtfl_thread *t = calloc(n, sizeof *t);
	size_t i;

	if (b == NULL || x == NULL || t == NULL)
		abort();
	atomic_store(&test_msg_live, 0);
	__dtfl_process_builders(n, b);
	for (i = 0; i < n; i++) {
		x[i].builder = b[i];
		x[i].peers = n;
		x[i].broadcast = broadcast;
		assert_int(__dtfl_thread_spawn("exch", exch_main, &x[i], &t[i]),
		    ==, 0);
	}
	for (i = 0; i < n; i++)
		(void)__dtfl_thread_join(t[i]);
	for (i = 0; i < n; i++) {
		if (x[i].err[0] != '\0')
			munit_errorf("%s", x[i].err);
		assert_size(x[i].got, ==,
		    broadcast ? n * NMSG : n * NCHAN * NMSG);
	}
	/* Every message was freed exactly once. */
	assert_long(atomic_load(&test_msg_live), ==, 0);
	free(b);
	free(x);
	free(t);
	return MUNIT_OK;
}

DTFL_TEST(process_all_to_all_fifo)
{
	(void)params; (void)data;
	if (run_exchange(1, false) != MUNIT_OK ||
	    run_exchange(2, false) != MUNIT_OK)
		return MUNIT_FAIL;
	return run_exchange(4, false);
}

DTFL_TEST(process_broadcast)
{
	long clones0;

	(void)params; (void)data;
	clones0 = atomic_load(&test_msg_clones);
	if (run_exchange(3, true) != MUNIT_OK)
		return MUNIT_FAIL;
	/* Broadcaster: a clone for every target but the first. */
	assert_long(atomic_load(&test_msg_clones) - clones0, ==, 3 * NMSG * 2);
	return MUNIT_OK;
}

DTFL_TEST(process_self_send_and_events)
{
	__dtfl_allocator_builder *b[2];
	__dtfl_allocator *a0, *a1;
	__dtfl_push *p0[2], *p1[2];
	__dtfl_pull *l0, *l1;
	void *m;

	(void)params; (void)data;
	atomic_store(&test_msg_live, 0);
	__dtfl_process_builders(2, b);
	a0 = __dtfl_allocator_build(b[0]);
	a1 = __dtfl_allocator_build(b[1]);
	/* Data sent before the receiver allocates waits in the rendezvous. */
	__dtfl_allocate(a0, 5, &test_msg_class, p0, &l0);
	__dtfl_push_send(p0[1], &test_msg_class, test_msg_new(10));
	__dtfl_push_send(p0[0], &test_msg_class, test_msg_new(20));
	/* Events surface only on receive(), the sender's included. */
	assert_size(a0->events.ids.len, ==, 0);
	__dtfl_allocator_receive(a0);
	assert_size(drain_events(a0, 5), ==, 1);
	m = __dtfl_pull_recv(l0);
	assert_uint64(test_msg_val(m), ==, 20);
	test_msg_free(&test_msg_class, m);
	__dtfl_allocate(a1, 5, &test_msg_class, p1, &l1);
	__dtfl_allocator_receive(a1);
	assert_size(drain_events(a1, 5), ==, 1);
	m = __dtfl_pull_recv(l1);
	assert_uint64(test_msg_val(m), ==, 10);
	test_msg_free(&test_msg_class, m);
	/* A flush raises the target's event (ArcPusher sends on done). */
	__dtfl_push_done(p1[0]);
	__dtfl_allocator_receive(a0);
	assert_size(drain_events(a0, 5), ==, 1);
	__dtfl_push_destroy(p0[0]);
	__dtfl_push_destroy(p0[1]);
	__dtfl_push_destroy(p1[1]);
	__dtfl_pull_destroy(l0);
	/* After the receiver goes, sends to it are dropped, not leaked. */
	__dtfl_allocator_destroy(a0);
	__dtfl_push_send(p1[0], &test_msg_class, test_msg_new(30));
	__dtfl_push_destroy(p1[0]);
	__dtfl_pull_destroy(l1);
	__dtfl_allocator_destroy(a1);
	assert_long(atomic_load(&test_msg_live), ==, 0);
	return MUNIT_OK;
}

DTFL_TEST(process_rendezvous_misuse_panics)
{
	__dtfl_allocator_builder *b[2];
	__dtfl_allocator *a0, *a1;
	__dtfl_push *p[2];
	__dtfl_pull *l;

	(void)params; (void)data;
	__dtfl_process_builders(2, b);
	a0 = __dtfl_allocator_build(b[0]);
	a1 = __dtfl_allocator_build(b[1]);
	__dtfl_allocate(a0, 1, &test_msg_class, p, &l);
	/* Upstream: "failed to correctly cast channel". */
	DTFL_EXPECT_PANIC(__dtfl_allocate(a1, 1, &test_msg_class2, p, &l));
	/* Upstream: "channel already consumed", while worker 1 has not. */
	{
		__dtfl_push *q[2];
		__dtfl_pull *m;

		DTFL_EXPECT_PANIC(__dtfl_allocate(a0, 1, &test_msg_class, q,
		    &m));
	}
	/* Neither panic changed the rendezvous; it tears down cleanly. */
	__dtfl_push_destroy(p[0]);
	__dtfl_push_destroy(p[1]);
	__dtfl_pull_destroy(l);
	__dtfl_allocator_destroy(a0);
	__dtfl_allocator_destroy(a1);
	return MUNIT_OK;
}

DTFL_TEST(process_unbuilt_builder_is_released)
{
	__dtfl_allocator_builder *b[3];
	__dtfl_allocator *a0;
	__dtfl_push *p[3];
	__dtfl_pull *l;

	(void)params; (void)data;
	atomic_store(&test_msg_live, 0);
	__dtfl_process_builders(3, b);
	a0 = __dtfl_allocator_build(b[0]);
	b[1]->destroy(b[1]);
	__dtfl_allocate(a0, 1, &test_msg_class, p, &l);
	/* Worker 1 never built: its messages are dropped, not leaked. */
	__dtfl_push_send(p[1], &test_msg_class, test_msg_new(1));
	__dtfl_push_send(p[2], &test_msg_class, test_msg_new(2));
	__dtfl_push_destroy(p[0]);
	__dtfl_push_destroy(p[1]);
	__dtfl_push_destroy(p[2]);
	__dtfl_pull_destroy(l);
	__dtfl_allocator_destroy(a0);
	b[2]->destroy(b[2]);
	assert_long(atomic_load(&test_msg_live), ==, 0);
	return MUNIT_OK;
}

typedef struct wake_arg {
	__dtfl_allocator_builder *builder;
	/* The receiver's parker, published for the watchdog. */
	_Atomic(__dtfl_parker *) parker;
	_Atomic int parks;	/* receiver is about to park (count) */
	_Atomic int finished;	/* receiver got the message */
	_Atomic int stop;	/* watchdog: give up */
} wake_arg;

/*
 * Worker 1: receive, and park with no timeout until a message arrives.
 * Only the ArcPusher's unpark (or the watchdog) can wake it.
 */
static void *
wake_receiver_main(void *arg)
{
	wake_arg *w = arg;
	__dtfl_allocator *a = __dtfl_allocator_build(w->builder);
	__dtfl_push *p[2];
	__dtfl_pull *l;
	void *m = NULL;

	atomic_store_explicit(&w->parker, a->parker, memory_order_release);
	__dtfl_allocate(a, 1, &test_msg_class, p, &l);
	while (atomic_load_explicit(&w->stop, memory_order_acquire) == 0) {
		__dtfl_allocator_receive(a);
		if ((m = __dtfl_pull_recv(l)) != NULL)
			break;
		__dtfl_vec_clear(&a->events.ids);
		atomic_fetch_add_explicit(&w->parks, 1, memory_order_release);
		__dtfl_allocator_await_events(a, UINT64_MAX);
	}
	if (m != NULL) {
		test_msg_free(&test_msg_class, m);
		atomic_store_explicit(&w->finished, 1, memory_order_release);
	}
	__dtfl_push_destroy(p[0]);
	__dtfl_push_destroy(p[1]);
	__dtfl_pull_destroy(l);
	__dtfl_allocator_destroy(a);
	return NULL;
}

DTFL_TEST(process_push_wakes_parked_receiver)
{
	__dtfl_allocator_builder *b[2];
	__dtfl_allocator *a0;
	__dtfl_push *p[2];
	__dtfl_pull *l;
	__dtfl_thread t;
	wake_arg w;
	struct timespec ts = { 0, 1000 * 1000 };
	uint64_t t0;
	bool lost = false;

	(void)params; (void)data;
	memset(&w, 0, sizeof w);
	__dtfl_process_builders(2, b);
	w.builder = b[1];
	a0 = __dtfl_allocator_build(b[0]);
	__dtfl_allocate(a0, 1, &test_msg_class, p, &l);
	assert_int(__dtfl_thread_spawn("wake", wake_receiver_main, &w, &t), ==, 0);
	/* Let the receiver reach its unbounded park. */
	t0 = __dtfl_clock_ns();
	while (atomic_load_explicit(&w.parks, memory_order_acquire) == 0 &&
	    __dtfl_clock_ns() - t0 < DEADLINE_NS)
		nanosleep(&ts, NULL);
	ts.tv_nsec = 50 * 1000 * 1000;
	nanosleep(&ts, NULL);
	__dtfl_push_send(p[1], &test_msg_class, test_msg_new(1));
	/* Watchdog: a lost wakeup fails the test instead of hanging it. */
	t0 = __dtfl_clock_ns();
	ts.tv_nsec = 1000 * 1000;
	while (atomic_load_explicit(&w.finished, memory_order_acquire) == 0) {
		if (__dtfl_clock_ns() - t0 > 5000 * MS) {
			lost = true;
			atomic_store_explicit(&w.stop, 1, memory_order_release);
			__dtfl_parker_unpark(atomic_load_explicit(&w.parker,
			    memory_order_acquire));
			break;
		}
		nanosleep(&ts, NULL);
	}
	(void)__dtfl_thread_join(t);
	__dtfl_push_destroy(p[0]);
	__dtfl_push_destroy(p[1]);
	__dtfl_pull_destroy(l);
	__dtfl_allocator_destroy(a0);
	if (lost)
		munit_error("push did not wake the parked receiver within 5 s");
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Broadcaster alone                                                   */
/* ------------------------------------------------------------------ */

DTFL_TEST(broadcaster_clones_all_but_first)
{
	__dtfl_events ev;
	__dtfl_push **ps = __dtfl_calloc(3, sizeof *ps);
	__dtfl_pull *ls[3];
	__dtfl_push *bc;
	void *orig = test_msg_new(77), *m;
	size_t i;

	(void)params; (void)data;
	atomic_store(&test_msg_live, 1);
	__dtfl_vec_init(&ev.ids, sizeof(size_t));
	for (i = 0; i < 3; i++)
		__dtfl_thread_new_from(1, &test_msg_class, &ev, &ps[i], &ls[i]);
	bc = __dtfl_broadcaster_new(&test_msg_class, ps, 3);
	__dtfl_push_send(bc, &test_msg_class, orig);
	m = __dtfl_pull_recv(ls[0]);
	assert_ptr_equal(m, orig);
	test_msg_free(&test_msg_class, m);
	for (i = 1; i < 3; i++) {
		m = __dtfl_pull_recv(ls[i]);
		assert_not_null(m);
		assert_ptr_not_equal(m, orig);
		assert_uint64(test_msg_val(m), ==, 77);
		test_msg_free(&test_msg_class, m);
	}
	/* A flush reaches every pusher. */
	__dtfl_vec_clear(&ev.ids);
	__dtfl_push_done(bc);
	assert_size(ev.ids.len, ==, 3);
	__dtfl_push_destroy(bc);
	for (i = 0; i < 3; i++)
		__dtfl_pull_destroy(ls[i]);
	__dtfl_vec_free(&ev.ids);
	assert_long(atomic_load(&test_msg_live), ==, 0);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Simulation allocator                                                */
/* ------------------------------------------------------------------ */

DTFL_TEST(splitmix64_reference_values)
{
	uint64_t s = 1234567;

	(void)params; (void)data;
	/* Reference output of Vigna's splitmix64.c for seed 1234567. */
	assert_uint64(__dtfl_splitmix64(&s), ==, UINT64_C(6457827717110365317));
	assert_uint64(__dtfl_splitmix64(&s), ==, UINT64_C(3203168211198807973));
	assert_uint64(__dtfl_splitmix64(&s), ==, UINT64_C(9817491932198370423));
	return MUNIT_OK;
}

#define SIM_PEERS 3
#define SIM_MSG 40

/*
 * Runs one simulated all-to-all exchange on two channels and records
 * every delivery as (receiver, value) in `trace`.  Returns the number
 * of receive rounds, or 0 on a FIFO violation (reported in err).
 */
static size_t
sim_run(uint64_t seed, __dtfl_vec *trace, char *err, size_t errlen)
{
	__dtfl_allocator_builder *b[SIM_PEERS];
	__dtfl_allocator *a[SIM_PEERS];
	__dtfl_push *p[SIM_PEERS][2][SIM_PEERS];
	__dtfl_pull *l[SIM_PEERS][2];
	size_t next[SIM_PEERS][2][SIM_PEERS];
	size_t w, c, t, got = 0, rounds = 0;
	unsigned k;

	memset(next, 0, sizeof next);
	__dtfl_sim_builders(SIM_PEERS, seed, b);
	for (w = 0; w < SIM_PEERS; w++)
		a[w] = __dtfl_allocator_build(b[w]);
	for (w = 0; w < SIM_PEERS; w++)
		for (c = 0; c < 2; c++)
			__dtfl_allocate(a[w], c, &test_msg_class, p[w][c], &l[w][c]);
	for (k = 0; k < SIM_MSG; k++)
		for (w = 0; w < SIM_PEERS; w++)
			for (c = 0; c < 2; c++)
				for (t = 0; t < SIM_PEERS; t++)
					__dtfl_push_send(p[w][c][t], &test_msg_class,
					    test_msg_new(TM_PACK(w, c, k)));
	while (got < SIM_PEERS * SIM_PEERS * 2 * SIM_MSG && rounds < 100000) {
		rounds++;
		for (w = 0; w < SIM_PEERS; w++) {
			__dtfl_allocator_receive(a[w]);
			__dtfl_vec_clear(&a[w]->events.ids);
			for (c = 0; c < 2; c++) {
				void *m;

				while ((m = __dtfl_pull_recv(l[w][c])) != NULL) {
					uint64_t v = test_msg_val(m);
					uint64_t rec[2] = { w, v };
					size_t s = TM_SRC(v);

					if (TM_SEQ(v) != next[w][c][s]++ &&
					    err[0] == '\0')
						snprintf(err, errlen, "seed %llu: "
						    "FIFO broken at worker %zu",
						    (unsigned long long)seed, w);
					__dtfl_vec_push(trace, rec);
					test_msg_free(&test_msg_class, m);
					got++;
				}
			}
		}
	}
	for (w = 0; w < SIM_PEERS; w++) {
		for (c = 0; c < 2; c++) {
			for (t = 0; t < SIM_PEERS; t++)
				__dtfl_push_destroy(p[w][c][t]);
			__dtfl_pull_destroy(l[w][c]);
		}
		__dtfl_allocator_destroy(a[w]);
	}
	return err[0] == '\0' ? rounds : 0;
}

DTFL_TEST(sim_deterministic_per_seed)
{
	__dtfl_vec t1, t2, t3;
	char err[256] = "";
	size_t r1, r2;
	uint64_t seed;
	int differ = 0;

	(void)params; (void)data;
	atomic_store(&test_msg_live, 0);
	for (seed = 1; seed <= 8; seed++) {
		__dtfl_vec_init(&t1, 2 * sizeof(uint64_t));
		__dtfl_vec_init(&t2, 2 * sizeof(uint64_t));
		__dtfl_vec_init(&t3, 2 * sizeof(uint64_t));
		r1 = sim_run(seed, &t1, err, sizeof err);
		r2 = sim_run(seed, &t2, err, sizeof err);
		(void)sim_run(seed + 1000, &t3, err, sizeof err);
		if (err[0] != '\0')
			munit_errorf("%s", err);
		assert_size(r1, >, 1);	/* delivery really was delayed */
		assert_size(r1, ==, r2);
		assert_size(t1.len, ==, SIM_PEERS * SIM_PEERS * 2 * SIM_MSG);
		assert_size(t1.len, ==, t2.len);
		assert_memory_equal(t1.len * t1.elem, t1.data, t2.data);
		if (t3.len != t1.len ||
		    memcmp(t1.data, t3.data, t1.len * t1.elem) != 0)
			differ++;
		__dtfl_vec_free(&t1);
		__dtfl_vec_free(&t2);
		__dtfl_vec_free(&t3);
	}
	/* Different seeds give different interleavings (all but surely). */
	assert_int(differ, >=, 7);
	assert_long(atomic_load(&test_msg_live), ==, 0);
	return MUNIT_OK;
}

DTFL_TEST(sim_flush_and_drop)
{
	__dtfl_allocator_builder *b[2];
	__dtfl_allocator *a0, *a1;
	__dtfl_push *p0[2];
	__dtfl_pull *l0;
	size_t i, ev = 0;

	(void)params; (void)data;
	atomic_store(&test_msg_live, 0);
	__dtfl_sim_builders(2, 9, b);
	a0 = __dtfl_allocator_build(b[0]);
	a1 = __dtfl_allocator_build(b[1]);
	__dtfl_allocate(a0, 3, &test_msg_class, p0, &l0);
	/* A flush to worker 1 raises its event on delivery. */
	__dtfl_push_done(p0[1]);
	for (i = 0; i < 1000 && ev == 0; i++) {
		__dtfl_allocator_receive(a1);
		ev = drain_events(a1, 3);
	}
	assert_size(ev, ==, 1);
	/* Undelivered messages are freed when everything is torn down. */
	__dtfl_push_send(p0[1], &test_msg_class, test_msg_new(1));
	__dtfl_push_send(p0[0], &test_msg_class, test_msg_new(2));
	__dtfl_allocator_await_events(a0, UINT64_MAX);	/* never blocks */
	__dtfl_push_destroy(p0[0]);
	__dtfl_push_destroy(p0[1]);
	__dtfl_pull_destroy(l0);
	__dtfl_allocator_destroy(a0);
	/* Worker 1 never allocated channel 3; leaving releases it. */
	__dtfl_allocator_destroy(a1);
	assert_long(atomic_load(&test_msg_live), ==, 0);
	return MUNIT_OK;
}

DTFL_TEST(sim_virtual_clock_and_rand)
{
	__dtfl_allocator_builder *b[2], *tb;
	__dtfl_allocator *a0, *a1, *t;
	void *sim;
	uint64_t r0, r1, s;

	(void)params; (void)data;
	__dtfl_sim_builders(2, 77, b);
	a0 = __dtfl_allocator_build(b[0]);
	a1 = __dtfl_allocator_build(b[1]);
	sim = __dtfl_sim_of(a0);
	assert_not_null(sim);
	assert_ptr_equal(__dtfl_sim_of(a1), sim);
	tb = __dtfl_thread_builder();
	t = __dtfl_allocator_build(tb);
	assert_null(__dtfl_sim_of(t));
	__dtfl_allocator_destroy(t);

	assert_uint64(__dtfl_sim_clock(sim), ==, 0);
	__dtfl_sim_advance(sim, 5);
	assert_uint64(__dtfl_sim_clock(sim), ==, 5);
	/* await_events never blocks: it spends the timeout virtually. */
	__dtfl_allocator_await_events(a0, 1000);
	assert_uint64(__dtfl_sim_clock(sim), ==, 1005);
	__dtfl_allocator_await_events(a1, UINT64_MAX);
	assert_uint64(__dtfl_sim_clock(sim), ==, 1005 + DTFL_SIM_IDLE_STEP_NS);
	/* With events pending it returns at once, as a real park would. */
	__dtfl_events_push(&a0->events, 1);
	__dtfl_allocator_await_events(a0, 1000);
	assert_uint64(__dtfl_sim_clock(sim), ==, 1005 + DTFL_SIM_IDLE_STEP_NS);
	__dtfl_sim_advance(sim, UINT64_MAX);
	assert_uint64(__dtfl_sim_clock(sim), ==, UINT64_MAX);	/* saturates */

	/* The generator is the seeded splitmix64 stream. */
	s = 77;
	r0 = __dtfl_sim_rand(sim);
	r1 = __dtfl_sim_rand(sim);
	assert_uint64(r0, ==, __dtfl_splitmix64(&s));
	assert_uint64(r1, ==, __dtfl_splitmix64(&s));
	__dtfl_allocator_destroy(a0);
	__dtfl_allocator_destroy(a1);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* __dtfl_comm_initialize                                              */
/* ------------------------------------------------------------------ */

DTFL_TEST(comm_initialize_modes)
{
	dtfl_config *cfg = dtfl_config_new();
	__dtfl_allocator_builder **b;
	__dtfl_comms_guard *g;
	size_t n, i;
	static const struct {
		dtfl_comm_mode mode;
		size_t threads, want;
		int rc;
		bool rendezvous;	/* build blocks for peers (one thread) */
	} cases[] = {
		{ DTFL_COMM_THREAD, 4, 1, 0, false },
		{ DTFL_COMM_PROCESS, 3, 3, 0, false },
		{ DTFL_COMM_SIM, 5, 5, 0, false },
		{ DTFL_COMM_PROCESS_BINARY, 2, 2, 0, true },
	};
	size_t k;

	(void)params; (void)data;
	for (k = 0; k < DTFL_ARRAY_LEN(cases); k++) {
		assert_int(dtfl_config_set_mode(cfg, cases[k].mode), ==, 0);
		assert_int(dtfl_config_set_threads(cfg, cases[k].threads), ==, 0);
		assert_int(__dtfl_comm_initialize(cfg, &b, &n, &g), ==,
		    cases[k].rc);
		assert_null(g);
		if (cases[k].rc != 0) {
			assert_null(b);
			continue;
		}
		assert_size(n, ==, cases[k].want);
		if (cases[k].rendezvous) {
			/*
			 * ProcessBinary builders rendezvous during build, so
			 * building them one at a time on this thread would
			 * deadlock; the exchange tests build them on separate
			 * threads.  Here just destroy them unbuilt.
			 */
			for (i = 0; i < n; i++)
				b[i]->destroy(b[i]);
			__dtfl_free(b);
			continue;
		}
		for (i = 0; i < n; i++) {
			__dtfl_allocator *a = __dtfl_allocator_build(b[i]);

			assert_size(a->index, ==, i);
			assert_size(a->peers, ==, n);
			__dtfl_allocator_destroy(a);
		}
		__dtfl_free(b);
		__dtfl_comms_guard_join(g);
	}
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(parker_unpark_before_park_returns_at_once),
	DTFL_T(parker_timeout_elapses),
	DTFL_T(parker_cross_thread_wakes),
	DTFL_T(fifo_order_and_compaction),
	DTFL_T(mpsc_close_drops_and_refuses),
	DTFL_T(thread_allocator_fifo_and_events),
	DTFL_T(thread_push_returns_recycled_or_null),
	DTFL_T(pipeline_is_local_and_unencoded),
	DTFL_T(canary_reports_on_drop),
	DTFL_T(process_all_to_all_fifo),
	DTFL_T(process_broadcast),
	DTFL_T(process_self_send_and_events),
	DTFL_T(process_push_wakes_parked_receiver),
	DTFL_T(process_rendezvous_misuse_panics),
	DTFL_T(process_unbuilt_builder_is_released),
	DTFL_T(broadcaster_clones_all_but_first),
	DTFL_T(splitmix64_reference_values),
	DTFL_T(sim_deterministic_per_seed),
	DTFL_T(sim_flush_and_drop),
	DTFL_T(sim_virtual_clock_and_rand),
	DTFL_T(comm_initialize_modes),
	DTFL_T_END
};

DTFL_TEST_MAIN("comm", tests)
