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
 * spawn hook must stay at zero and, on Linux, /proc/self/task must hold
 * as many entries when the execution returns as when the child started
 * (one, outside sanitizer runtimes).
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
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
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

/* This worker returns from its worker function 2 s late. */
static size_t late_index = SIZE_MAX;

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
	if (c->index == late_index) {
		struct timespec d = { 2, 0 };

		nanosleep(&d, NULL);
	}
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
static int tasks_at_start = -1;	/* this child's threads before libdtfl */

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
	/*
	 * Threads libdtfl added.  Compared with the count at the child's
	 * start, not with 1: a sanitizer runtime may own a thread of its
	 * own in every process.
	 */
	r.tasks = tasks_at_start < 0 ? -1 : task_count() - tasks_at_start;
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
			assert_int(r.tasks, ==, 0);
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
	tasks_at_start = task_count();
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

static uint64_t
cpu_us(const struct rusage *r)
{
	return (uint64_t)(r->ru_utime.tv_sec + r->ru_stime.tv_sec) * 1000000u +
	    (uint64_t)(r->ru_utime.tv_usec + r->ru_stime.tv_usec);
}

/*
 * Workers 0 and 1 finish at once and wait for worker 2's end of stream,
 * which comes 2 s later.  Their streams to each other have ended by
 * then, and poll(2) reports a socket at end of file readable at once,
 * so a driver that stopped reading a peer at its end-of-stream header,
 * without reading on to the end of file, spun for the whole 2 s.  The
 * three children together should use a few milliseconds of CPU.
 */
DTFL_TEST(execute_worker_waits_idle_for_a_late_peer)
{
	struct rusage r0, r1;
	uint64_t used;

	(void)params; (void)data;
	assert_int(getrusage(RUSAGE_CHILDREN, &r0), ==, 0);
	late_index = 2;
	run_sockets(3);
	assert_int(getrusage(RUSAGE_CHILDREN, &r1), ==, 0);
	used = cpu_us(&r1) - cpu_us(&r0);
	munit_logf(MUNIT_LOG_INFO, "workers used %llu us of CPU",
	    (unsigned long long)used);
	assert_ullong(used, <, 500000);
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

/* ------------------------------------------------------------------ */
/* DTFL_WORKERS_PROCESSES: dtfl_execute forks the workers itself       */
/* ------------------------------------------------------------------ */

/*
 * Each forked worker checks its own routing and totals and exits
 * non-zero on a mismatch, so a passing dtfl_execute means every child
 * got its share.  The panic hook makes a broken child exit non-zero
 * rather than abort with a core.
 *
 * Each child arms its own alarm: fork(2) does not carry the test's
 * alarm into the children, and a hung child keeps the write end of
 * munit's report pipe, which it inherited, open.  The test's alarm then
 * kills only the test process, and munit waits on the pipe for as long
 * as the children live.
 */
#define CHILD_ALARM 20

static void
forked_worker(dtfl_worker *w, void *arg)
{
	wctx c;

	(void)arg;
	alarm(CHILD_ALARM);
	memset(&c, 0, sizeof c);
	worker(w, &c);
	if (c.misrouted != 0 || c.count != M)
		_exit(5);
}

static void
quiet_panic(const char *msg, void *ctx)
{
	(void)ctx;
	fprintf(stderr, "forked worker panicked: %s\n", msg);
	_exit(6);
}

static int
run_forked(dtfl_comm_mode mode, size_t n)
{
	dtfl_config *cfg = dtfl_config_new();
	int rc;

	assert_int(dtfl_config_set_mode(cfg, mode), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, n), ==, 0);
	assert_int(dtfl_config_set_workers(cfg, DTFL_WORKERS_PROCESSES), ==, 0);
	assert_int(dtfl_config_workers(cfg), ==, DTFL_WORKERS_PROCESSES);
	dtfl_set_panic_hook(quiet_panic, NULL);
	rc = dtfl_execute(cfg, forked_worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}

DTFL_TEST(execute_forked_process_mode)
{
	(void)params; (void)data;
	assert_int(run_forked(DTFL_COMM_PROCESS, 4), ==, 0);
	assert_int(run_forked(DTFL_COMM_PROCESS_BINARY, 3), ==, 0);
	return MUNIT_OK;
}

/* A child that fails makes dtfl_execute fail, rather than hang. */
static void
failing_worker(dtfl_worker *w, void *arg)
{
	(void)arg;
	if (dtfl_worker_index(w) == 1)
		_exit(7);
	forked_worker(w, NULL);
}

DTFL_TEST(execute_forked_reports_a_failed_child)
{
	dtfl_config *cfg = dtfl_config_new();

	(void)params; (void)data;
	alarm(3 * CHILD_ALARM);
	dtfl_set_panic_hook(quiet_panic, NULL);
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, 3), ==, 0);
	assert_int(dtfl_config_set_workers(cfg, DTFL_WORKERS_PROCESSES), ==, 0);
	/*
	 * Worker 1 exits with 7; its peers then see their streams end early
	 * and panic (6).  The first failure waited for is reported.  A peer
	 * that never sees the end (a descriptor of worker 1's leaked into
	 * another child) dies of its alarm instead, and that detail says
	 * "killed by signal 14".
	 */
	assert_int(dtfl_execute(cfg, failing_worker, NULL, NULL), ==, DTFL_EIO);
	if (strstr(dtfl_last_error_detail(), "exited with status") == NULL)
		munit_errorf("expected a child to exit non-zero, got: %s",
		    dtfl_last_error_detail());
	dtfl_config_free(cfg);
	alarm(0);
	return MUNIT_OK;
}

/*
 * With SIGCHLD ignored the kernel reaps the children itself, and
 * waitpid(2) fails with ECHILD instead of returning their status.  A
 * good run must still return 0, and a failed one DTFL_EIO.
 */
DTFL_TEST(execute_forked_with_sigchld_ignored)
{
	dtfl_config *cfg = dtfl_config_new();

	(void)params; (void)data;
	alarm(3 * CHILD_ALARM);
	(void)signal(SIGCHLD, SIG_IGN);
	if (run_forked(DTFL_COMM_PROCESS, 3) != 0)
		munit_errorf("a good run failed: %s", dtfl_last_error_detail());
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, 3), ==, 0);
	assert_int(dtfl_config_set_workers(cfg, DTFL_WORKERS_PROCESSES), ==, 0);
	assert_int(dtfl_execute(cfg, failing_worker, NULL, NULL), ==, DTFL_EIO);
	assert_not_null(strstr(dtfl_last_error_detail(), "SIGCHLD"));
	dtfl_config_free(cfg);
	(void)signal(SIGCHLD, SIG_DFL);
	alarm(0);
	return MUNIT_OK;
}

DTFL_TEST(execute_forked_rejects_cluster_and_sim)
{
	dtfl_config *cfg = dtfl_config_new();

	(void)params; (void)data;
	assert_int(dtfl_config_set_workers(cfg, (dtfl_workers)9), ==,
	    DTFL_EINVAL);
	assert_int(dtfl_config_set_workers(cfg, DTFL_WORKERS_PROCESSES), ==, 0);
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_SIM), ==, 0);
	assert_int(dtfl_execute(cfg, forked_worker, NULL, NULL), ==,
	    DTFL_EINVAL);
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_CLUSTER), ==, 0);
	assert_int(dtfl_execute(cfg, forked_worker, NULL, NULL), ==,
	    DTFL_EINVAL);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* A dtfl_sync_activator from another thread wakes a parked worker     */
/* ------------------------------------------------------------------ */

/*
 * The operator holds its capability until a thread of its own fires a
 * sync activator, 200 ms in.  Until then the worker has nothing to do
 * and parks in the transport's wait, so only the activator's wake,
 * through the parker to the transport's wake, can end that park.
 */
typedef struct act_state {
	dtfl_cap *cap;
	dtfl_sync_activator *act;
	atomic_bool fired;
	pthread_t th;
	bool started;
} act_state;

static void *
act_fire(void *arg)
{
	act_state *s = arg;
	struct timespec d = { 0, 200000000 };

	nanosleep(&d, NULL);
	atomic_store(&s->fired, true);
	(void)dtfl_sync_activator_activate(s->act);
	return NULL;
}

static bool
act_logic(dtfl_op *op, void *ctx)
{
	act_state *s = ctx;

	if (!s->started) {
		s->started = true;
		s->cap = dtfl_op_initial_cap(op, 0);
		s->act = dtfl_op_sync_activator(op);
		if (pthread_create(&s->th, NULL, act_fire, s) != 0)
			_exit(8);
		return false;
	}
	if (s->cap != NULL && atomic_load(&s->fired)) {
		dtfl_cap_drop(s->cap);
		s->cap = NULL;
		pthread_join(s->th, NULL);
		dtfl_sync_activator_free(s->act);
	}
	return false;
}

static void
act_build(dtfl_scope *scope, void *arg)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "act");

	(void)dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, act_logic, arg, NULL);
}

static void
act_worker(dtfl_worker *w, void *arg)
{
	dtfl_time_class *tc = dtfl_time_u64();
	act_state s;

	(void)arg;
	alarm(CHILD_ALARM);	/* a lost wake hangs this child */
	memset(&s, 0, sizeof s);
	dtfl_worker_dataflow(w, "act", tc, act_build, &s);
	while (dtfl_worker_step_or_park(w, UINT64_MAX))
		;
	dtfl_time_class_free(tc);
	if (s.cap != NULL || !atomic_load(&s.fired))
		_exit(9);
}

DTFL_TEST(execute_worker_sync_activator_wakes_transport_wait)
{
	dtfl_config *cfg = dtfl_config_new();

	(void)params; (void)data;
	alarm(60);
	dtfl_set_panic_hook(quiet_panic, NULL);
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, 2), ==, 0);
	assert_int(dtfl_config_set_workers(cfg, DTFL_WORKERS_PROCESSES), ==, 0);
	if (dtfl_execute(cfg, act_worker, NULL, NULL) != 0)
		munit_errorf("dtfl_execute: %s", dtfl_last_error_detail());
	dtfl_config_free(cfg);
	alarm(0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(execute_worker_two_processes_socketpair),
	DTFL_T(execute_worker_four_processes_socketpair),
	DTFL_T(execute_worker_waits_idle_for_a_late_peer),
	DTFL_T(execute_worker_three_processes_shared_rings),
	DTFL_T(execute_worker_four_processes_shared_rings),
	DTFL_T(execute_worker_single_needs_no_transport),
	DTFL_T(execute_worker_rejects_bad_arguments),
	DTFL_T(execute_forked_process_mode),
	DTFL_T(execute_forked_reports_a_failed_child),
	DTFL_T(execute_forked_with_sigchld_ignored),
	DTFL_T(execute_forked_rejects_cluster_and_sim),
	DTFL_T(execute_worker_sync_activator_wakes_transport_wait),
	DTFL_T_END
};

DTFL_TEST_MAIN("execute_worker", tests)
