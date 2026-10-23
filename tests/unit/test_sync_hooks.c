/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_sync_hooks.c: dtfl_set_sync_hooks (ADR-0013).
 *
 * The hooks below wrap pthreads and futex-free waits and count every
 * call.  A dataflow with an exchange on four worker threads must still
 * produce the right answer with them installed, and the counters must
 * show that libdtfl's locks, parks and clock reads all went through
 * them.  Each munit test runs in a forked child, so each starts with the
 * hooks unlatched.
 */
#define _POSIX_C_SOURCE 200809L

#include "dtfl_test.h"

#include "dtfl/config.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

static atomic_long n_mutex_init, n_lock, n_unlock, n_mutex_destroy;
static atomic_long n_cond_init, n_cond_wait, n_signal, n_cond_destroy;
static atomic_long n_wait, n_wake, n_yield, n_sleep, n_clock, n_self;
static atomic_long live_mutexes, held;

static int
h_mutex_init(void **m, void *ctx)
{
	pthread_mutex_t *pm = malloc(sizeof *pm);

	(void)ctx;
	atomic_fetch_add(&n_mutex_init, 1);
	atomic_fetch_add(&live_mutexes, 1);
	pthread_mutex_init(pm, NULL);
	*m = pm;
	return 0;
}

static void
h_mutex_destroy(void *m, void *ctx)
{
	(void)ctx;
	atomic_fetch_add(&n_mutex_destroy, 1);
	atomic_fetch_sub(&live_mutexes, 1);
	pthread_mutex_destroy(m);
	free(m);
}

static void
h_lock(void *m, void *ctx)
{
	(void)ctx;
	atomic_fetch_add(&n_lock, 1);
	pthread_mutex_lock(m);
	atomic_fetch_add(&held, 1);
}

static void
h_unlock(void *m, void *ctx)
{
	(void)ctx;
	atomic_fetch_add(&n_unlock, 1);
	atomic_fetch_sub(&held, 1);
	pthread_mutex_unlock(m);
}

static int
h_cond_init(void **c, void *ctx)
{
	pthread_cond_t *pc = malloc(sizeof *pc);

	(void)ctx;
	atomic_fetch_add(&n_cond_init, 1);
	pthread_cond_init(pc, NULL);
	*c = pc;
	return 0;
}

static void
h_cond_destroy(void *c, void *ctx)
{
	(void)ctx;
	atomic_fetch_add(&n_cond_destroy, 1);
	pthread_cond_destroy(c);
	free(c);
}

static void
h_cond_wait(void *c, void *m, uint64_t timeout_ns, void *ctx)
{
	struct timespec ts;

	(void)ctx;
	atomic_fetch_add(&n_cond_wait, 1);
	atomic_fetch_sub(&held, 1);
	if (timeout_ns == UINT64_MAX) {
		pthread_cond_wait(c, m);
	} else {
		uint64_t ns;

		clock_gettime(CLOCK_REALTIME, &ts);
		ns = (uint64_t)ts.tv_nsec + (timeout_ns > 1000000000000ull ?
		    1000000000000ull : timeout_ns);
		ts.tv_sec += (time_t)(ns / 1000000000ull);
		ts.tv_nsec = (long)(ns % 1000000000ull);
		pthread_cond_timedwait(c, m, &ts);
	}
	atomic_fetch_add(&held, 1);
}

static void
h_signal(void *c, void *ctx)
{
	(void)ctx;
	atomic_fetch_add(&n_signal, 1);
	pthread_cond_signal(c);
}

static void
h_broadcast(void *c, void *ctx)
{
	(void)ctx;
	atomic_fetch_add(&n_signal, 1);
	pthread_cond_broadcast(c);
}

/*
 * wait/wake without futex: a poll on the word with a short sleep.  Slow
 * but correct, since wait may return early and the caller rereads.
 */
static void
h_wait(const uint32_t *word, uint32_t expect, uint64_t timeout_ns, void *ctx)
{
	struct timespec ts = { 0, 100000 };	/* 100 us */

	(void)ctx;
	atomic_fetch_add(&n_wait, 1);
	if (timeout_ns < 100000)
		ts.tv_nsec = (long)timeout_ns;
	if (atomic_load((_Atomic uint32_t *)(uintptr_t)word) == expect)
		nanosleep(&ts, NULL);
}

static void
h_wake(const uint32_t *word, void *ctx)
{
	(void)word; (void)ctx;
	atomic_fetch_add(&n_wake, 1);
}

static void
h_yield(void *ctx)
{
	(void)ctx;
	atomic_fetch_add(&n_yield, 1);
	sched_yield();
}

static void
h_sleep(uint64_t ns, void *ctx)
{
	struct timespec ts;

	(void)ctx;
	atomic_fetch_add(&n_sleep, 1);
	ts.tv_sec = (time_t)(ns / 1000000000ull);
	ts.tv_nsec = (long)(ns % 1000000000ull);
	nanosleep(&ts, NULL);
}

static uint64_t
h_clock(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	atomic_fetch_add(&n_clock, 1);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uintptr_t
h_self(void *ctx)
{
	(void)ctx;
	atomic_fetch_add(&n_self, 1);
	return (uintptr_t)pthread_self();
}

static const dtfl_sync_hooks all_hooks = {
	h_mutex_init, h_mutex_destroy, h_lock, h_unlock,
	h_cond_init, h_cond_destroy, h_cond_wait, h_signal, h_broadcast,
	h_wait, h_wake, h_yield, h_sleep, h_clock, h_self, NULL
};

/* ------------------------------------------------------------------ */
/* The dataflow: every worker sends 0..N, exchanged by value; worker w */
/* sums what it receives.  The totals across workers must be peers *   */
/* sum(0..N), and each record must land on worker (value % peers).     */
/* ------------------------------------------------------------------ */

#define N 2000
#define MAXW 8

static atomic_ullong sums[MAXW];
static atomic_long misrouted;

typedef struct wctx {
	dtfl_input *input;
	dtfl_probe *probe;
	size_t index, peers;
} wctx;

static uint64_t
route(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static void
see(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	wctx *c = ctx;
	uint64_t v = dtfl_get_u64(rec);

	(void)time; (void)len;
	if (v % c->peers != c->index)
		atomic_fetch_add(&misrouted, 1);
	atomic_fetch_add(&sums[c->index], v);
}

static void
build(dtfl_scope *scope, void *arg)
{
	wctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);

	s = dtfl_stream_exchange(s, route, NULL);
	s = dtfl_stream_inspect(s, see, DTFL_CTX(c, NULL));
	dtfl_stream_probe_with(s, c->probe);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	wctx *c = calloc(1, sizeof *c);
	uint64_t i, t = 1;

	(void)ctx;
	c->index = dtfl_worker_index(w);
	c->peers = dtfl_worker_peers(w);
	c->input = dtfl_input_new(dtfl_type_u64());
	c->probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "sync", tc, build, c);
	/*
	 * Worker 0 starts late, so the others have nothing to do and park
	 * (step_or_park) until its records and frontier arrive.
	 */
	if (c->index == 0 && c->peers > 1) {
		struct timespec late = { 0, 20000000 };	/* 20 ms */

		nanosleep(&late, NULL);
	}
	for (i = 0; i < N; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, i);
		dtfl_input_send(c->input, buf, sizeof buf);
	}
	dtfl_input_advance_to(c->input, &t);
	while (dtfl_probe_less_than(c->probe, dtfl_input_time(c->input)))
		(void)dtfl_worker_step_or_park(w, UINT64_C(5000000));
	dtfl_input_close(c->input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c->probe);
	dtfl_time_class_free(tc);
	free(c);
}

static void
run(dtfl_comm_mode mode, size_t workers)
{
	dtfl_config *cfg = dtfl_config_new();
	unsigned long long total = 0;
	size_t i;

	for (i = 0; i < MAXW; i++)
		atomic_store(&sums[i], 0);
	atomic_store(&misrouted, 0);
	assert_int(dtfl_config_set_mode(cfg, mode), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, workers), ==, 0);
	assert_int(dtfl_execute(cfg, worker, NULL, NULL), ==, 0);
	dtfl_config_free(cfg);
	for (i = 0; i < workers; i++)
		total += atomic_load(&sums[i]);
	assert_long(atomic_load(&misrouted), ==, 0);
	assert_ullong(total, ==, (unsigned long long)workers * N * (N - 1) / 2);
}

static void
reset_counts(void)
{
	atomic_store(&n_mutex_init, 0); atomic_store(&n_lock, 0);
	atomic_store(&n_unlock, 0); atomic_store(&n_mutex_destroy, 0);
	atomic_store(&n_cond_init, 0); atomic_store(&n_cond_wait, 0);
	atomic_store(&n_signal, 0); atomic_store(&n_cond_destroy, 0);
	atomic_store(&n_wait, 0); atomic_store(&n_wake, 0);
	atomic_store(&n_yield, 0); atomic_store(&n_sleep, 0);
	atomic_store(&n_clock, 0); atomic_store(&n_self, 0);
	atomic_store(&live_mutexes, 0); atomic_store(&held, 0);
}

/* Every lock pairs with an unlock, and every mutex made is destroyed. */
static void
assert_balanced(void)
{
	assert_long(atomic_load(&n_lock), ==, atomic_load(&n_unlock));
	assert_long(atomic_load(&held), ==, 0);
	assert_long(atomic_load(&live_mutexes), ==, 0);
	assert_long(atomic_load(&n_mutex_init), ==,
	    atomic_load(&n_mutex_destroy));
	assert_long(atomic_load(&n_cond_init), ==,
	    atomic_load(&n_cond_destroy));
}

DTFL_TEST(sync_hooks_carry_process_exchange)
{
	(void)params; (void)data;
	reset_counts();
	assert_int(dtfl_set_sync_hooks(&all_hooks), ==, 0);
	run(DTFL_COMM_PROCESS, 4);
	assert_balanced();
	/* The Process channel table and SyncActivations lock through us. */
	assert_long(atomic_load(&n_mutex_init), >, 0);
	assert_long(atomic_load(&n_lock), >, 0);
	/* Idle workers parked on the word hooks, timed by our clock. */
	assert_long(atomic_load(&n_clock), >, 0);
	assert_long(atomic_load(&n_wait), >, 0);
	return MUNIT_OK;
}

DTFL_TEST(sync_hooks_carry_process_binary_exchange)
{
	(void)params; (void)data;
	reset_counts();
	assert_int(dtfl_set_sync_hooks(&all_hooks), ==, 0);
	run(DTFL_COMM_PROCESS_BINARY, 4);
	assert_balanced();
	/* The ProcessBinary rendezvous waits on a hooked condition. */
	assert_long(atomic_load(&n_cond_init), >, 0);
	assert_long(atomic_load(&n_lock), >, 0);
	return MUNIT_OK;
}

/*
 * Mutex and condition hooks with no wait/wake: the parker must build on
 * the host's condition rather than reach for futex(2).
 */
DTFL_TEST(sync_hooks_parker_uses_host_condition)
{
	dtfl_sync_hooks h = all_hooks;

	(void)params; (void)data;
	h.wait = NULL;
	h.wake = NULL;
	reset_counts();
	assert_int(dtfl_set_sync_hooks(&h), ==, 0);
	run(DTFL_COMM_PROCESS, 4);
	assert_balanced();
	assert_long(atomic_load(&n_wait), ==, 0);
	/* Four parkers, each a hooked condition, and idle workers wait. */
	assert_long(atomic_load(&n_cond_init), >=, 4);
	assert_long(atomic_load(&n_cond_wait), >, 0);
	assert_long(atomic_load(&n_signal), >, 0);
	return MUNIT_OK;
}

/* The debug owner check asks the self hook for the thread identity. */
DTFL_TEST(sync_hooks_self_names_the_owner)
{
	dtfl_worker *w;

	(void)params; (void)data;
	reset_counts();
	assert_int(dtfl_set_sync_hooks(&all_hooks), ==, 0);
	w = dtfl_worker_new_standalone();
	while (dtfl_worker_step(w))
		;
	dtfl_worker_free(w);
	assert_long(atomic_load(&n_self), >, 0);
	assert_balanced();
	return MUNIT_OK;
}

DTFL_TEST(sync_hooks_refused_after_first_use)
{
	(void)params; (void)data;
	/* The first lock latches the hooks. */
	run(DTFL_COMM_PROCESS, 2);
	assert_int(dtfl_set_sync_hooks(&all_hooks), ==, DTFL_ESTATE);
	assert_int(dtfl_set_sync_hooks(NULL), ==, DTFL_ESTATE);
	return MUNIT_OK;
}

DTFL_TEST(sync_hooks_reject_partial_groups)
{
	dtfl_sync_hooks h;

	(void)params; (void)data;
	h = all_hooks;
	h.mutex_unlock = NULL;
	assert_int(dtfl_set_sync_hooks(&h), ==, DTFL_EINVAL);
	h = all_hooks;
	h.cond_broadcast = NULL;
	assert_int(dtfl_set_sync_hooks(&h), ==, DTFL_EINVAL);
	/* Conditions without mutexes cannot wait on a hooked mutex. */
	memset(&h, 0, sizeof h);
	h.cond_init = h_cond_init; h.cond_destroy = h_cond_destroy;
	h.cond_wait = h_cond_wait; h.cond_signal = h_signal;
	h.cond_broadcast = h_broadcast;
	assert_int(dtfl_set_sync_hooks(&h), ==, DTFL_EINVAL);
	memset(&h, 0, sizeof h);
	h.wait = h_wait;
	assert_int(dtfl_set_sync_hooks(&h), ==, DTFL_EINVAL);
	/* Single optional members are fine; nothing has latched yet. */
	memset(&h, 0, sizeof h);
	h.clock = h_clock;
	assert_int(dtfl_set_sync_hooks(&h), ==, 0);
	assert_int(dtfl_set_sync_hooks(NULL), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(sync_hooks_carry_process_exchange),
	DTFL_T(sync_hooks_carry_process_binary_exchange),
	DTFL_T(sync_hooks_parker_uses_host_condition),
	DTFL_T(sync_hooks_self_names_the_owner),
	DTFL_T(sync_hooks_refused_after_first_use),
	DTFL_T(sync_hooks_reject_partial_groups),
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

DTFL_TEST_MAIN("sync_hooks", tests)
