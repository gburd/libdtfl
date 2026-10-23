/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_execute_worker.c: dtfl_execute_worker (ADR-0013),
 * workers that are separate processes created by the host.
 *
 * Each test forks N children.  Each child joins one execution as worker
 * i of N with dtfl_execute_worker and runs the same dataflow: every
 * worker sends 0..M, exchanged by value, and counts and sums what it
 * receives, checking that each record was routed to it.  The children
 * report through a pipe and the parent checks the totals.
 *
 * Two transports carry the bytes:
 *   - dtfl_transport_socket over a socketpair mesh made before fork;
 *   - a host transport written here: one single-producer ring per
 *     ordered pair of workers in a MAP_SHARED mapping made before fork,
 *     woken through a word wait (futex on Linux, a short sleep
 *     elsewhere).  It stands in for PostgreSQL's shm_mq in a DSM
 *     segment, woken by a latch.
 *
 * Each child also checks that libdtfl started no thread: a counting
 * spawn hook must stay at zero and, on Linux, /proc/self/task must
 * still hold one entry when the execution returns.
 */
#if defined(__linux__)
#define _GNU_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "dtfl_test.h"

#include "dtfl/config.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#define M 3000		/* records each worker sends */
#define MAXN 8

/* ------------------------------------------------------------------ */
/* The dataflow                                                        */
/* ------------------------------------------------------------------ */

typedef struct wctx {
	dtfl_input *input;
	dtfl_probe *probe;
	size_t index, peers;
	uint64_t count, sum, misrouted;
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
		c->misrouted++;
	c->count++;
	c->sum += v;
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

/* Three rounds, so frontiers advance across processes more than once. */
static void
worker(dtfl_worker *w, void *arg)
{
	wctx *c = arg;
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t i, round;

	c->index = dtfl_worker_index(w);
	c->peers = dtfl_worker_peers(w);
	c->input = dtfl_input_new(dtfl_type_u64());
	c->probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "join", tc, build, c);
	for (round = 0; round < 3; round++) {
		uint64_t next = round + 1;

		for (i = round; i < M; i += 3) {
			unsigned char buf[8];

			dtfl_put_u64(buf, i);
			dtfl_input_send(c->input, buf, sizeof buf);
		}
		dtfl_input_advance_to(c->input, &next);
		while (dtfl_probe_less_than(c->probe,
		    dtfl_input_time(c->input)))
			(void)dtfl_worker_step_or_park(w, UINT64_MAX);
	}
	dtfl_input_close(c->input);
	while (dtfl_worker_step_or_park(w, UINT64_MAX))
		;
	dtfl_probe_free(c->probe);
	dtfl_time_class_free(tc);
}

/* ------------------------------------------------------------------ */
/* No threads: a spawn hook that counts, and /proc/self/task           */
/* ------------------------------------------------------------------ */

static atomic_int spawns;

static int
refuse_spawn(void *(*start)(void *), void *arg, void **out, void *ctx)
{
	(void)start; (void)arg; (void)out; (void)ctx;
	atomic_fetch_add(&spawns, 1);
	return DTFL_EIO;
}

static int
refuse_join(void *h, void **ret, void *ctx)
{
	(void)h; (void)ret; (void)ctx;
	return DTFL_EIO;
}

/* Threads in this process, or -1 where /proc is not there. */
static int
task_count(void)
{
	DIR *d = opendir("/proc/self/task");
	struct dirent *e;
	int n = 0;

	if (d == NULL)
		return -1;
	while ((e = readdir(d)) != NULL)
		if (e->d_name[0] != '.')
			n++;
	closedir(d);
	return n;
}

/* What a child reports to the parent. */
typedef struct report {
	int rc;
	int spawns;
	int tasks;
	uint64_t count, sum, misrouted;
	char panic[256];	/* a libdtfl panic in the child, or "" */
} report;

static int report_fd = -1;

/* A child that panics reports the message, so the parent can show it. */
static void
child_panic(const char *msg, void *ctx)
{
	report r;

	(void)ctx;
	memset(&r, 0, sizeof r);
	r.rc = -1;
	snprintf(r.panic, sizeof r.panic, "%s", msg);
	if (report_fd >= 0)
		(void)!write(report_fd, &r, sizeof r);
	_exit(4);
}

static void
child_finish(int fd, int rc, const wctx *c)
{
	report r;

	memset(&r, 0, sizeof r);
	r.rc = rc;
	r.spawns = atomic_load(&spawns);
	r.tasks = task_count();
	r.count = c->count;
	r.sum = c->sum;
	r.misrouted = c->misrouted;
	if (write(fd, &r, sizeof r) != (ssize_t)sizeof r)
		_exit(3);
	_exit(0);
}

/* Waits for n children, reads their reports and checks the totals. */
static void
collect(pid_t *pids, int *rd, size_t n)
{
	uint64_t count = 0, sum = 0;
	size_t i;

	for (i = 0; i < n; i++) {
		report r;
		int status;

		assert_int((int)read(rd[i], &r, sizeof r), ==, (int)sizeof r);
		close(rd[i]);
		if (r.panic[0] != '\0')
			munit_errorf("worker %zu panicked: %s", i, r.panic);
		assert_int(waitpid(pids[i], &status, 0), ==, pids[i]);
		assert_true(WIFEXITED(status));
		assert_int(WEXITSTATUS(status), ==, 0);
		assert_int(r.rc, ==, 0);
		assert_int(r.spawns, ==, 0);
		if (r.tasks >= 0)
			assert_int(r.tasks, ==, 1);
		assert_ullong(r.misrouted, ==, 0);
		count += r.count;
		sum += r.sum;
	}
	assert_ullong(count, ==, (uint64_t)n * M);
	assert_ullong(sum, ==, (uint64_t)n * ((uint64_t)M * (M - 1) / 2));
}

/* A child gives up rather than hang the suite. */
static void
child_setup(int fd)
{
	dtfl_thread_hooks th = { refuse_spawn, refuse_join, NULL };

	alarm(60);
	report_fd = fd;
	dtfl_set_panic_hook(child_panic, NULL);
	(void)dtfl_set_thread_hooks(&th);
}

/* ------------------------------------------------------------------ */
/* Over dtfl_transport_socket                                          */
/* ------------------------------------------------------------------ */

static void
run_sockets(size_t n)
{
	int mesh[MAXN][MAXN], rd[MAXN];
	pid_t pids[MAXN];
	size_t i, j;

	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			mesh[i][j] = -1;
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++) {
			int sv[2];

			assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), ==, 0);
			mesh[i][j] = sv[0];
			mesh[j][i] = sv[1];
		}
	for (i = 0; i < n; i++) {
		int p[2];

		assert_int(pipe(p), ==, 0);
		pids[i] = fork();
		assert_int(pids[i], >=, 0);
		if (pids[i] == 0) {
			dtfl_transport t;
			wctx c;
			int rc;

			close(p[0]);
			/* Keep only this worker's ends. */
			for (j = 0; j < n; j++) {
				size_t k;

				for (k = 0; k < n; k++)
					if (j != i && mesh[j][k] >= 0)
						close(mesh[j][k]);
			}
			child_setup(p[1]);
			memset(&c, 0, sizeof c);
			rc = dtfl_transport_socket(mesh[i], n, i, &t);
			if (rc == 0) {
				rc = dtfl_execute_worker(NULL, i, n, &t, worker,
				    &c);
				t.destroy(t.ctx);
			}
			child_finish(p[1], rc, &c);
		}
		close(p[1]);
		rd[i] = p[0];
	}
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			if (mesh[i][j] >= 0)
				close(mesh[i][j]);
	collect(pids, rd, n);
}

DTFL_TEST(execute_worker_two_processes_socketpair)
{
	(void)params; (void)data;
	run_sockets(2);
	return MUNIT_OK;
}

DTFL_TEST(execute_worker_four_processes_socketpair)
{
	(void)params; (void)data;
	run_sockets(4);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Over a host transport: rings in MAP_SHARED memory                   */
/* ------------------------------------------------------------------ */

/*
 * One ring per ordered pair (from, to).  A single producer and a single
 * consumer: head is written only by the reader, tail only by the
 * writer, each with release order after touching the data and read
 * with acquire by the other side.  `closed` is the writer's end of
 * stream.  Each worker has a doorbell word that writers and readers
 * bump and wake: a writer after adding bytes, a reader after making
 * room.  Small on purpose, so writes are often short.
 */
#define RING 4096

typedef struct ring {
	_Atomic uint64_t head;	/* bytes consumed */
	_Atomic uint64_t tail;	/* bytes produced */
	atomic_bool closed;
	unsigned char buf[RING];
} ring;

typedef struct shm {
	_Atomic uint32_t bell[MAXN];
	ring rings[MAXN][MAXN];	/* [from][to] */
} shm;

typedef struct shm_transport {
	shm *s;
	size_t self, n;
	uint32_t seen;		/* our bell's value at the last wait */
} shm_transport;

static void
ring_bell(shm *s, size_t who)
{
	atomic_fetch_add_explicit(&s->bell[who], 1, memory_order_seq_cst);
#if defined(__linux__)
	/* Shared (not PRIVATE): the waiter is another process. */
	(void)syscall(SYS_futex, (uint32_t *)&s->bell[who], FUTEX_WAKE,
	    INT32_MAX, NULL, NULL, 0);
#endif
}

static int
shm_send(void *ctx, size_t peer, const void *buf, size_t len, size_t *sent)
{
	shm_transport *t = ctx;
	ring *r = &t->s->rings[t->self][peer];
	uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
	uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
	size_t room = RING - (size_t)(tail - head), k, n;

	n = len < room ? len : room;
	for (k = 0; k < n; k++)
		r->buf[(tail + k) % RING] = ((const unsigned char *)buf)[k];
	atomic_store_explicit(&r->tail, tail + n, memory_order_release);
	*sent = n;
	if (n > 0)
		ring_bell(t->s, peer);
	return 0;
}

static int
shm_recv(void *ctx, size_t peer, void *buf, size_t cap, size_t *got)
{
	shm_transport *t = ctx;
	ring *r = &t->s->rings[peer][t->self];
	bool closed = atomic_load_explicit(&r->closed, memory_order_acquire);
	uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
	uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	size_t avail = (size_t)(tail - head), k, n;

	n = cap < avail ? cap : avail;
	if (n == 0 && closed) {
		*got = 0;
		return DTFL_EOF;
	}
	for (k = 0; k < n; k++)
		((unsigned char *)buf)[k] = r->buf[(head + k) % RING];
	atomic_store_explicit(&r->head, head + n, memory_order_release);
	*got = n;
	if (n > 0)
		ring_bell(t->s, peer);	/* room for the writer */
	return 0;
}

static int
shm_shutdown(void *ctx, size_t peer)
{
	shm_transport *t = ctx;

	atomic_store_explicit(&t->s->rings[t->self][peer].closed, true,
	    memory_order_release);
	ring_bell(t->s, peer);
	return 0;
}

static void
shm_wait(void *ctx, uint64_t timeout_ns)
{
	shm_transport *t = ctx;
	_Atomic uint32_t *bell = &t->s->bell[t->self];
	uint32_t now = atomic_load_explicit(bell, memory_order_seq_cst);

	/* Rung since we last looked: return and let the caller poll. */
	if (now != t->seen) {
		t->seen = now;
		return;
	}
#if defined(__linux__)
	{
		struct timespec ts, *tsp = NULL;

		if (timeout_ns != UINT64_MAX) {
			ts.tv_sec = (time_t)(timeout_ns / 1000000000ull);
			ts.tv_nsec = (long)(timeout_ns % 1000000000ull);
			tsp = &ts;
		}
		(void)syscall(SYS_futex, (uint32_t *)bell, FUTEX_WAIT, now,
		    tsp, NULL, 0);
	}
#else
	{
		struct timespec ts = { 0, 200000 };

		(void)timeout_ns;
		nanosleep(&ts, NULL);
	}
#endif
	t->seen = atomic_load_explicit(bell, memory_order_seq_cst);
}

static void
shm_wake(void *ctx)
{
	shm_transport *t = ctx;

	ring_bell(t->s, t->self);
}

static void
run_shm(size_t n)
{
	shm *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE,
	    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	int rd[MAXN];
	pid_t pids[MAXN];
	size_t i;

	assert_true(s != MAP_FAILED);
	memset(s, 0, sizeof *s);
	for (i = 0; i < n; i++) {
		int p[2];

		assert_int(pipe(p), ==, 0);
		pids[i] = fork();
		assert_int(pids[i], >=, 0);
		if (pids[i] == 0) {
			shm_transport st = { s, i, n, 0 };
			dtfl_transport t = { shm_send, shm_recv, shm_wait,
			    shm_wake, shm_shutdown, NULL, &st };
			wctx c;
			int rc;

			close(p[0]);
			child_setup(p[1]);
			memset(&c, 0, sizeof c);
			rc = dtfl_execute_worker(NULL, i, n, &t, worker, &c);
			child_finish(p[1], rc, &c);
		}
		close(p[1]);
		rd[i] = p[0];
	}
	collect(pids, rd, n);
	munmap(s, sizeof *s);
}

DTFL_TEST(execute_worker_three_processes_shared_rings)
{
	(void)params; (void)data;
	run_shm(3);
	return MUNIT_OK;
}

DTFL_TEST(execute_worker_four_processes_shared_rings)
{
	(void)params; (void)data;
	run_shm(4);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Arguments and the one-worker case                                   */
/* ------------------------------------------------------------------ */

DTFL_TEST(execute_worker_single_needs_no_transport)
{
	wctx c;

	(void)params; (void)data;
	memset(&c, 0, sizeof c);
	assert_int(dtfl_execute_worker(NULL, 0, 1, NULL, worker, &c), ==, 0);
	assert_ullong(c.count, ==, M);
	assert_ullong(c.misrouted, ==, 0);
	return MUNIT_OK;
}

DTFL_TEST(execute_worker_rejects_bad_arguments)
{
	dtfl_transport half = { shm_send, shm_recv, NULL, NULL, NULL, NULL,
	    NULL };
	wctx c;

	(void)params; (void)data;
	assert_int(dtfl_execute_worker(NULL, 2, 2, NULL, worker, &c), ==,
	    DTFL_EINVAL);
	assert_int(dtfl_execute_worker(NULL, 0, 2, NULL, worker, &c), ==,
	    DTFL_EINVAL);
	assert_int(dtfl_execute_worker(NULL, 0, 2, &half, worker, &c), ==,
	    DTFL_EINVAL);
	assert_int(dtfl_execute_worker(NULL, 0, 1, NULL, NULL, &c), ==,
	    DTFL_EINVAL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(execute_worker_two_processes_socketpair),
	DTFL_T(execute_worker_four_processes_socketpair),
	DTFL_T(execute_worker_three_processes_shared_rings),
	DTFL_T(execute_worker_four_processes_shared_rings),
	DTFL_T(execute_worker_single_needs_no_transport),
	DTFL_T(execute_worker_rejects_bad_arguments),
	DTFL_T_END
};

DTFL_TEST_MAIN("execute_worker", tests)
