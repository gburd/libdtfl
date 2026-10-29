/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/embed/xtc_runtime.c: libdtfl workers as libxtc fibers.
 *
 * libxtc runs fibers (procs) on a few carrier threads, one event loop
 * each, and moves a migratable fiber between carriers when one runs
 * dry.  A fiber that blocks its carrier in the kernel stops every other
 * fiber on that carrier, so this program routes libdtfl's blocking into
 * libxtc:
 *
 *   sync hooks    the dtfl_set_sync_hooks(3) example: xtc_amutex_t for
 *                 mutexes, a condition built from xtc_notify_t, xtc_yield,
 *                 xtc_proc_sleep, and a `self` that names the fiber.
 *   thread hooks  each thread libdtfl starts is a migratable fiber, and
 *                 join parks the joining fiber on an xtc_notify_t.
 *   transport     dtfl_transport_socket, with its wait (a poll(2)) run
 *                 on libxtc's blocking pool while the fiber parks.
 *
 * A driver fiber then runs three executions of the same two dataflows,
 * a timely exchange (hello and forked_workers in miniature) and a
 * differential count_total with retractions:
 *
 *   process   dtfl_execute, DTFL_COMM_PROCESS, 4 worker fibers
 *   binary    dtfl_execute, DTFL_COMM_PROCESS_BINARY, 4 worker fibers
 *   inline    4 fibers, each dtfl_execute_worker over a socketpair mesh
 *
 * Each prints one line that does not depend on the carrier count, and
 * the exit status is non-zero if any total is wrong.  stderr gets how
 * often the hooks ran and which carriers each worker used.
 *
 * Usage: xtc_runtime [carriers]   (default 2; 1 puts every fiber on one
 * carrier, so any wait that blocks the carrier hangs the run)
 */
#define _POSIX_C_SOURCE 200809L

#include "dtfl/base.h"
#include "dtfl/collection.h"
#include "dtfl/config.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <xtc.h>
#include <xtc_async.h>
#include <xtc_blocking.h>
#include <xtc_exec.h>
#include <xtc_proc.h>
#include <xtc_sync.h>

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define WORKERS 4
#define RECORDS 10000
#define KEYS 7

/* Evidence that libdtfl's waits reached libxtc. */
static atomic_ulong n_lock, n_cond_wait, n_cond_timeout, n_offload;

/* ------------------------------------------------------------------ */
/* Sync hooks: the dtfl_set_sync_hooks(3) example, plus the counters    */
/* ------------------------------------------------------------------ */

typedef struct waiter {
	struct waiter *next;
	xtc_notify_t *n;
} waiter;
typedef struct xcond {
	xtc_amutex_t *lk;
	waiter *head;
} xcond;

static int
mi(void **m, void *c)
{
	xtc_amutex_t *a;

	(void)c;
	if (xtc_amutex_create(&a) != XTC_OK)
		return DTFL_ENOMEM;
	*m = a;
	return 0;
}

static void
md(void *m, void *c)
{
	(void)c;
	xtc_amutex_destroy(m);
}

static void
ml(void *m, void *c)
{
	(void)c;
	atomic_fetch_add_explicit(&n_lock, 1, memory_order_relaxed);
	(void)xtc_amutex_lock(m, -1);
}

static void
mu(void *m, void *c)
{
	(void)c;
	(void)xtc_amutex_unlock(m);
}

static int
ci(void **cv, void *c)
{
	xcond *k = calloc(1, sizeof *k);

	(void)c;
	if (k == NULL || xtc_amutex_create(&k->lk) != XTC_OK) {
		free(k);
		return DTFL_ENOMEM;
	}
	*cv = k;
	return 0;
}

static void
cd(void *cv, void *c)
{
	(void)c;
	xtc_amutex_destroy(((xcond *)cv)->lk);
	free(cv);
}

/*
 * The waiter joins the queue before it releases the caller's mutex, and
 * a signal stored in its notifier is not lost, so a signal between the
 * two still ends the wait.
 */
static void
cw(void *cv, void *m, uint64_t ns, void *c)
{
	xcond *k = cv;
	waiter w = { NULL, NULL }, **pp;

	(void)c;
	atomic_fetch_add_explicit(&n_cond_wait, 1, memory_order_relaxed);
	if (xtc_notify_create(&w.n) != XTC_OK) {
		/* A spurious return is allowed; the caller rechecks. */
		(void)xtc_amutex_unlock(m);
		xtc_yield();
		(void)xtc_amutex_lock(m, -1);
		return;
	}
	(void)xtc_amutex_lock(k->lk, -1);
	for (pp = &k->head; *pp != NULL; pp = &(*pp)->next)
		;
	*pp = &w;
	(void)xtc_amutex_unlock(k->lk);
	(void)xtc_amutex_unlock(m);
	(void)xtc_notify_wait(w.n, ns > INT64_MAX ? -1 : (int64_t)ns);
	(void)xtc_amutex_lock(k->lk, -1);
	for (pp = &k->head; *pp != NULL; pp = &(*pp)->next)
		if (*pp == &w) {	/* timed out: still queued */
			*pp = w.next;
			atomic_fetch_add_explicit(&n_cond_timeout, 1,
			    memory_order_relaxed);
			break;
		}
	(void)xtc_amutex_unlock(k->lk);
	xtc_notify_destroy(w.n);
	(void)xtc_amutex_lock(m, -1);
}

/* Signalling under k->lk keeps a timed-out waiter from freeing w->n. */
static void
wake_some(xcond *k, bool all)
{
	(void)xtc_amutex_lock(k->lk, -1);
	do {
		waiter *w = k->head;

		if (w == NULL)
			break;
		k->head = w->next;
		(void)xtc_notify_signal(w->n);
	} while (all);
	(void)xtc_amutex_unlock(k->lk);
}

static void
cs(void *cv, void *c)
{
	(void)c;
	wake_some(cv, false);
}

static void
cb(void *cv, void *c)
{
	(void)c;
	wake_some(cv, true);
}

/* Off a loop xtc_yield and xtc_proc_sleep return at once; fall back. */
static void
yl(void *c)
{
	(void)c;
	if (xtc_pid_is_none(xtc_self()))
		(void)sched_yield();
	else
		xtc_yield();
}

static void
sl(uint64_t ns, void *c)
{
	int64_t t = ns > INT64_MAX ? INT64_MAX : (int64_t)ns;

	(void)c;
	if (xtc_proc_sleep(t) != XTC_OK)
		(void)xtc_sleep_ns(t);
}

/*
 * libxtc 2.0's xtc_pid_t is 16 bytes: a 32-bit loop and slot, which name
 * a live fiber and survive its migration, and a 64-bit generation, which
 * only tells it from the slot's later occupants.  Off a loop it is
 * XTC_PID_NONE.
 */
static uintptr_t
me(void *c)
{
	xtc_pid_t p = xtc_self();
	uint64_t id;

	(void)c;
	if (xtc_pid_is_none(p))
		return (uintptr_t)pthread_self();
	id = (uint64_t)p.loop_id << 32 | p.local_id;
	return sizeof(uintptr_t) >= 8 ? (uintptr_t)id :
	    (uintptr_t)(id ^ id >> 32);
}

/* ------------------------------------------------------------------ */
/* Thread hooks: every libdtfl thread is a migratable fiber             */
/* ------------------------------------------------------------------ */

static xtc_exec_t *exec;
static int ncarriers;
static atomic_uint next_loop;

static int
spawn_fiber(xtc_proc_fn fn, void *arg)
{
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	unsigned k = atomic_fetch_add(&next_loop, 1);

	o.name = "dtfl";
	o.migratable = 1;
	return xtc_proc_spawn(xtc_exec_loop(exec, (int)(k % (unsigned)ncarriers)),
	    fn, arg, &o, NULL);
}

typedef struct xthread {
	void *(*start)(void *);
	void *arg;
	void *ret;
	xtc_notify_t *done;
} xthread;

static void
xthread_main(void *a)
{
	xthread *t = a;

	t->ret = t->start(t->arg);
	(void)xtc_notify_signal(t->done);
}

static int
th_spawn(void *(*start)(void *), void *arg, void **out, void *ctx)
{
	xthread *t = calloc(1, sizeof *t);

	(void)ctx;
	if (t == NULL || xtc_notify_create(&t->done) != XTC_OK) {
		free(t);
		return DTFL_ENOMEM;
	}
	t->start = start;
	t->arg = arg;
	if (spawn_fiber(xthread_main, t) != XTC_OK) {
		xtc_notify_destroy(t->done);
		free(t);
		return DTFL_EIO;
	}
	*out = t;
	return 0;
}

/* Called on the fiber that ran dtfl_execute: it parks, its carrier runs on. */
static int
th_join(void *h, void **ret, void *ctx)
{
	xthread *t = h;

	(void)ctx;
	(void)xtc_notify_wait(t->done, -1);
	*ret = t->ret;
	xtc_notify_destroy(t->done);
	free(t);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Transport: dtfl_transport_socket with its wait off the carrier       */
/* ------------------------------------------------------------------ */

/*
 * The socket transport's wait is poll(2), which would block the carrier.
 * Run it on libxtc's blocking pool and park the fiber meanwhile.  The
 * rest passes through: the sockets are non-blocking, and wake writes a
 * self-pipe from any thread.
 */
typedef struct fiber_transport {
	dtfl_transport sock;
} fiber_transport;

typedef struct wait_call {
	fiber_transport *f;
	uint64_t ns;
} wait_call;

static int
ft_send(void *c, size_t p, const void *b, size_t len, size_t *sent)
{
	fiber_transport *f = c;

	return f->sock.send(f->sock.ctx, p, b, len, sent);
}

static int
ft_recv(void *c, size_t p, void *b, size_t cap, size_t *got)
{
	fiber_transport *f = c;

	return f->sock.recv(f->sock.ctx, p, b, cap, got);
}

static int
wait_on_pool(void *a)
{
	wait_call *w = a;

	w->f->sock.wait(w->f->sock.ctx, w->ns);
	return 0;
}

/* shortcut: a parked worker holds a pool thread for its whole wait. */
static void
ft_wait(void *c, uint64_t ns)
{
	wait_call w = { c, ns };
	int r;

	atomic_fetch_add_explicit(&n_offload, 1, memory_order_relaxed);
	if (xtc_blocking_run(wait_on_pool, &w, &r) != XTC_OK)
		xtc_yield();	/* pool refused: a spurious return is allowed */
}

static void
ft_wake(void *c)
{
	fiber_transport *f = c;

	f->sock.wake(f->sock.ctx);
}

static int
ft_shutdown(void *c, size_t p)
{
	fiber_transport *f = c;

	return f->sock.shutdown(f->sock.ctx, p);
}

/* ------------------------------------------------------------------ */
/* The dataflows                                                        */
/* ------------------------------------------------------------------ */

typedef struct result {
	size_t peers;
	uint64_t count, sum, misrouted;	/* timely exchange */
	int64_t counted[KEYS];		/* final count_total per key */
	unsigned loops;			/* carriers seen, a bit each */
} result;

typedef struct state {
	result *r;
	size_t index;
	dtfl_input *input;
	dtfl_probe *tprobe;
	dtfl_input_session *keys;
	dtfl_probe *dprobe;
} state;

static uint64_t
by_value(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static void
tally(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	state *s = ctx;
	uint64_t v = dtfl_get_u64(rec);

	(void)time; (void)len;
	if (v % s->r->peers != s->index)
		s->r->misrouted++;
	s->r->count++;
	s->r->sum += v;
}

static void
build_hello(dtfl_scope *scope, void *arg)
{
	state *s = arg;
	dtfl_stream *st = dtfl_input_to_stream(s->input, scope);

	st = dtfl_stream_exchange(st, by_value, NULL);
	st = dtfl_stream_inspect(st, tally, DTFL_CTX(s, NULL));
	dtfl_stream_probe_with(st, s->tprobe);
}

/* Updates of (key, count): the sum of count * diff is the final count. */
static void
counted(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	state *s = ctx;
	uint64_t k = dtfl_get_u64(rec);
	int64_t d;

	(void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	if (k < KEYS)
		s->r->counted[k] += d *
		    dtfl_get_i64((const unsigned char *)rec + 8);
}

static void
build_count(dtfl_scope *scope, void *arg)
{
	state *s = arg;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *in = dtfl_input_session_to_collection(s->keys, scope);
	dtfl_collection *cnt = dtfl_collection_count_total(in, i64);
	dtfl_collection *ins = dtfl_collection_inspect(cnt, counted,
	    DTFL_CTX(s, NULL));
	dtfl_collection *pr = dtfl_collection_probe_with(ins, s->dprobe);

	dtfl_collection_free(pr);
	dtfl_collection_free(ins);
	dtfl_collection_free(cnt);
	dtfl_collection_free(in);
	dtfl_diff_free(i64);
}

/*
 * dtfl_worker_step never parks, so a fiber waiting in a loop of them
 * never gives up its carrier, and a peer on the same carrier never runs.
 * Wait with dtfl_worker_step_or_park, which parks through the hooks.
 */
static void
await(dtfl_worker *w, state *s, dtfl_probe *p, const uint64_t *t)
{
	while (dtfl_probe_less_than(p, t)) {
		(void)dtfl_worker_step_or_park(w, UINT64_MAX);
		s->r->loops |= 1u << (xtc_exec_loop_id() & 31);
	}
}

static void
worker(dtfl_worker *w, void *arg)
{
	result *all = arg;
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	uint64_t i, t1 = 1, t2 = 2;
	unsigned char buf[8];
	state s;

	/* Before any dataflow little can end this park: it usually times out. */
	(void)dtfl_worker_step_or_park(w, 1000000);
	s.index = dtfl_worker_index(w);
	s.r = &all[s.index];
	s.r->peers = dtfl_worker_peers(w);
	s.input = dtfl_input_new(dtfl_type_u64());
	s.tprobe = dtfl_probe_new();
	s.keys = dtfl_input_session_new(dtfl_type_u64(), i64);
	s.dprobe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "hello", tc, build_hello, &s);
	dtfl_worker_dataflow(w, "count", tc, build_count, &s);

	/* Round 1: every value, and its key, once per worker. */
	for (i = 0; i < RECORDS; i++) {
		dtfl_put_u64(buf, i);
		dtfl_input_send(s.input, buf, sizeof buf);
		dtfl_put_u64(buf, i % KEYS);
		dtfl_input_session_insert(s.keys, buf, sizeof buf);
	}
	dtfl_input_advance_to(s.input, &t1);
	dtfl_input_session_advance_to(s.keys, &t1);
	dtfl_input_session_flush(s.keys);
	await(w, &s, s.tprobe, dtfl_input_time(s.input));
	await(w, &s, s.dprobe, dtfl_input_session_time(s.keys));

	/* Round 2: retract the keys of the first half. */
	for (i = 0; i < RECORDS / 2; i++) {
		dtfl_put_u64(buf, i % KEYS);
		dtfl_input_session_remove(s.keys, buf, sizeof buf);
	}
	dtfl_input_session_advance_to(s.keys, &t2);
	dtfl_input_session_flush(s.keys);
	await(w, &s, s.dprobe, dtfl_input_session_time(s.keys));

	dtfl_input_close(s.input);
	dtfl_input_session_close(s.keys);
	dtfl_probe_free(s.tprobe);
	dtfl_probe_free(s.dprobe);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

/* ------------------------------------------------------------------ */
/* The three executions                                                 */
/* ------------------------------------------------------------------ */

static int
report(const char *name, int rc, const result *res, unsigned long waits)
{
	uint64_t count = 0, sum = 0, misrouted = 0, total = 0, k, v;
	int64_t want[KEYS] = { 0 }, got[KEYS] = { 0 };
	bool ok = rc == 0;
	size_t i;

	for (v = RECORDS / 2; v < RECORDS; v++)
		want[v % KEYS] += WORKERS;
	for (i = 0; i < WORKERS; i++) {
		count += res[i].count;
		sum += res[i].sum;
		misrouted += res[i].misrouted;
		for (k = 0; k < KEYS; k++)
			got[k] += res[i].counted[k];
	}
	for (k = 0; k < KEYS; k++) {
		ok = ok && got[k] == want[k];
		total += (uint64_t)got[k];
	}
	ok = ok && misrouted == 0 && count == (uint64_t)WORKERS * RECORDS &&
	    sum == (uint64_t)WORKERS * RECORDS * (RECORDS - 1) / 2;
	printf("%s: %d workers, records per worker %" PRIu64 ", sum per "
	    "worker %" PRIu64 ", misrouted %" PRIu64 ", counted per worker %"
	    PRIu64 " over %d keys: %s\n", name, WORKERS, count / WORKERS,
	    sum / WORKERS, misrouted, total / WORKERS, KEYS,
	    ok ? "ok" : "WRONG");
	fprintf(stderr, "%s: rc %d, %d carriers, %lu waits through the "
	    "hooks, carriers per worker:", name, rc, ncarriers, waits);
	for (i = 0; i < WORKERS; i++)
		fprintf(stderr, " %zu:%#x", i, res[i].loops);
	fprintf(stderr, "\n");
	return ok ? 0 : 1;
}

static int
execute_phase(const char *name, dtfl_comm_mode mode)
{
	dtfl_config *cfg = dtfl_config_new();
	result res[WORKERS];
	unsigned long w0 = atomic_load(&n_cond_wait);
	int rc;

	memset(res, 0, sizeof res);
	rc = dtfl_config_set_mode(cfg, mode);
	if (rc == 0)
		rc = dtfl_config_set_threads(cfg, WORKERS);
	if (rc == 0)
		rc = dtfl_execute(cfg, worker, res, NULL);
	dtfl_config_free(cfg);
	return report(name, rc, res, atomic_load(&n_cond_wait) - w0);
}

typedef struct inline_arg {
	int fds[WORKERS];
	size_t index;
	result *res;
	int rc;
	xtc_notify_t *done;
} inline_arg;

static void
inline_main(void *a)
{
	inline_arg *ia = a;
	fiber_transport f;
	dtfl_transport t;
	int rc;

	rc = dtfl_transport_socket(ia->fds, WORKERS, ia->index, &f.sock);
	if (rc == 0) {
		t.send = ft_send;
		t.recv = ft_recv;
		t.wait = ft_wait;
		t.wake = ft_wake;
		t.shutdown = ft_shutdown;
		t.destroy = NULL;
		t.ctx = &f;
		rc = dtfl_execute_worker(NULL, ia->index, WORKERS, &t, worker,
		    ia->res);
		f.sock.destroy(f.sock.ctx);
	}
	if (rc != 0)
		fprintf(stderr, "inline worker %zu: %s: %s\n", ia->index,
		    dtfl_strerror(rc), dtfl_last_error_detail());
	ia->rc = rc;
	(void)xtc_notify_signal(ia->done);
}

static int
inline_phase(void)
{
	inline_arg ia[WORKERS];
	result res[WORKERS];
	unsigned long o0 = atomic_load(&n_offload);
	size_t i, j;
	int rc = 0;

	memset(ia, 0, sizeof ia);
	memset(res, 0, sizeof res);
	for (i = 0; i < WORKERS; i++)
		ia[i].fds[i] = -1;
	for (i = 0; i < WORKERS; i++)
		for (j = i + 1; j < WORKERS; j++) {
			int sv[2];

			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
				perror("socketpair");
				return 1;
			}
			ia[i].fds[j] = sv[0];
			ia[j].fds[i] = sv[1];
		}
	for (i = 0; i < WORKERS; i++) {
		ia[i].index = i;
		ia[i].res = res;
		if (xtc_notify_create(&ia[i].done) != XTC_OK ||
		    spawn_fiber(inline_main, &ia[i]) != XTC_OK) {
			fprintf(stderr, "cannot start inline worker %zu\n", i);
			exit(1);	/* started peers would wait for ever */
		}
	}
	for (i = 0; i < WORKERS; i++) {
		(void)xtc_notify_wait(ia[i].done, -1);
		xtc_notify_destroy(ia[i].done);
		if (ia[i].rc != 0)
			rc = ia[i].rc;
	}
	return report("inline", rc, res, atomic_load(&n_offload) - o0);
}

static void
driver(void *arg)
{
	int *status = arg;

	*status |= execute_phase("process", DTFL_COMM_PROCESS);
	*status |= execute_phase("binary", DTFL_COMM_PROCESS_BINARY);
	*status |= inline_phase();
}

int
main(int argc, char **argv)
{
	dtfl_sync_hooks h = { mi, md, ml, mu, ci, cd, cw, cs, cb,
	    NULL, NULL, yl, sl, NULL, me, NULL };
	dtfl_thread_hooks th = { th_spawn, th_join, NULL };
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	int status = 0, rc;

	ncarriers = argc > 1 ? atoi(argv[1]) : 2;
	if (ncarriers < 1 || ncarriers > 32) {
		fprintf(stderr, "usage: %s [carriers 1..32]\n", argv[0]);
		return 2;
	}
	if ((rc = dtfl_set_sync_hooks(&h)) != 0 ||
	    (rc = dtfl_set_thread_hooks(&th)) != 0) {
		fprintf(stderr, "hooks: %s\n", dtfl_strerror(rc));
		return 1;
	}
	if (xtc_exec_init(&exec, ncarriers) != XTC_OK) {
		fprintf(stderr, "xtc_exec_init failed\n");
		return 1;
	}
	/* Idle carriers steal runnable workers, so workers migrate. */
	xtc_exec_set_eager_rebalance(exec, 1);
	o.name = "driver";
	if (xtc_proc_spawn(xtc_exec_loop(exec, 0), driver, &status, &o,
	    NULL) != XTC_OK || xtc_exec_run(exec) != XTC_OK) {
		fprintf(stderr, "xtc_exec_run failed\n");
		return 1;
	}
	(void)xtc_exec_fini(exec);
	xtc_blocking_shutdown();
	fprintf(stderr, "hooks: %lu mutex locks, %lu condition waits (%lu "
	    "timed out), %lu transport waits on the pool\n",
	    atomic_load(&n_lock), atomic_load(&n_cond_wait),
	    atomic_load(&n_cond_timeout), atomic_load(&n_offload));
	return status;
}
