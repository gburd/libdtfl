/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_tcp.c: the Cluster (TCP) allocator over a loopback
 * mesh.
 *
 * A cluster is run inside one test binary: one thread per process, each
 * a "process" with `threads` workers, connects to 127.0.0.1 on a set of
 * ephemeral ports and forms the full TCP mesh via __dtfl_comm_initialize
 * with a Cluster config.  Each worker then runs a tiny exchange: it
 * sends records to the global worker chosen by hash(record) % peers and
 * asserts that every record it receives was addressed to it and that
 * each (sender) stream arrives in order.  Worker-to-local traffic
 * travels the inner Process allocator; cross-process traffic crosses the
 * socket, so a correct run proves the send/recv loops and the handshake
 * route data to the right worker.
 *
 * No worker thread calls munit asserts (their longjmp target is on the
 * main thread): each process records the first failure in its arg and
 * returns, and the main thread asserts after joining.  Every socket
 * phase is bounded by the create_sockets deadline, so a lost handshake
 * fails rather than hanging.
 */
#include "dtfl_test.h"
#include "test_comm_msg.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/os_thread.h"
#include "comm/init_int.h"

#define DEADLINE_NS (UINT64_C(40) * 1000000000)	/* 40 s backstop */
#define MS (UINT64_C(1000000))
#define NMSG 200

/* record -> global worker.  The low 16 bits carry the routing target. */
static size_t
route(uint64_t v, size_t peers)
{
	return (size_t)((v & 0xffff) % peers);
}

/* Binds a loopback socket to an ephemeral port and returns the port. */
static int
pick_port(void)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0), port;

	if (fd < 0)
		return -1;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &len) != 0) {
		(void)close(fd);
		return -1;
	}
	port = ntohs(sa.sin_port);
	(void)close(fd);
	return port;
}

typedef struct proc_arg {
	size_t my_index;	/* process index */
	size_t processes;
	size_t threads;		/* local workers */
	size_t peers;		/* threads * processes */
	const char *const *addrs;
	size_t got;		/* records received across local workers */
	char err[256];
} proc_arg;

typedef struct worker_arg {
	__dtfl_allocator_builder *builder;  /* built on this worker's thread */
	__dtfl_allocator *a;	/* set by the worker after build */
	size_t peers;
	size_t me;		/* global worker index */
	size_t got;
	char err[256];
} worker_arg;

/*
 * One worker thread: build its allocator (the builder rendezvous during
 * build, so each worker must build on its own thread), send NMSG records
 * to each target (routed by value), then receive every record addressed
 * to it, checking per-sender order.  Record layout:
 * (sender << 48) | (seq << 16) | target.
 */
static void *
worker_main(void *v)
{
	worker_arg *w = v;
	__dtfl_allocator *a = __dtfl_allocator_build(w->builder);
	size_t peers = w->peers, me = w->me, target, k, s, want;
	__dtfl_push **push = calloc(peers, sizeof *push);
	__dtfl_pull *pull = NULL;
	size_t *next = calloc(peers, sizeof *next);
	uint64_t t0;

	w->a = a;
	if (a->index != me || a->peers != peers) {
		snprintf(w->err, sizeof w->err, "index %zu peers %zu",
		    a->index, a->peers);
	}
	if (push == NULL || next == NULL)
		abort();
	__dtfl_allocate(a, 1, &test_msg_class, push, &pull);

	for (k = 0; k < NMSG; k++)
		for (target = 0; target < peers; target++) {
			uint64_t rec = ((uint64_t)me << 48) |
			    ((uint64_t)k << 16) | (uint64_t)target;

			if (route(rec, peers) != target) {
				snprintf(w->err, sizeof w->err,
				    "route(%llu)=%zu != %zu",
				    (unsigned long long)rec,
				    route(rec, peers), target);
				goto done;
			}
			__dtfl_push_send(push[target], &test_msg_class,
			    test_msg_new(rec));
		}
	for (s = 0; s < peers; s++)
		__dtfl_push_done(push[s]);

	want = peers * NMSG;
	t0 = __dtfl_clock_ns();
	while (w->got < want && w->err[0] == '\0') {
		void *m;
		bool any = false;

		__dtfl_allocator_receive(a);
		__dtfl_vec_clear(&a->events.ids);
		while ((m = __dtfl_pull_recv(pull)) != NULL) {
			uint64_t val = test_msg_val(m);
			size_t src = (size_t)(val >> 48);
			size_t seq = (size_t)((val >> 16) & 0xffffffff);
			size_t tgt = (size_t)(val & 0xffff);

			any = true;
			if (tgt != me || route(val, peers) != me) {
				snprintf(w->err, sizeof w->err,
				    "worker %zu got misrouted %llu", me,
				    (unsigned long long)val);
			} else if (src >= peers) {
				snprintf(w->err, sizeof w->err, "bad src %zu",
				    src);
			} else if (seq != next[src]) {
				snprintf(w->err, sizeof w->err,
				    "worker %zu src %zu: seq %zu want %zu",
				    me, src, seq, next[src]);
			} else {
				next[src]++;
			}
			w->got++;
			test_msg_free(&test_msg_class, m);
		}
		__dtfl_allocator_release(a);
		if (!any) {
			if (__dtfl_clock_ns() - t0 > DEADLINE_NS) {
				snprintf(w->err, sizeof w->err, "worker %zu "
				    "timeout: %zu/%zu", me, w->got, want);
				break;
			}
			__dtfl_allocator_await_events(a, 50 * MS);
		}
	}

done:
	for (s = 0; s < peers; s++)
		if (push[s] != NULL)
			__dtfl_push_destroy(push[s]);
	if (pull != NULL)
		__dtfl_pull_destroy(pull);
	free(push);
	free(next);
	return NULL;
}

/*
 * One cluster process: build the mesh, spawn one thread per local
 * worker, run the exchange, join, then join the network threads.
 */
static void *
proc_main(void *v)
{
	proc_arg *x = v;
	dtfl_config *cfg = dtfl_config_new();
	__dtfl_allocator_builder **b = NULL;
	__dtfl_comms_guard *g = NULL;
	worker_arg *wa = NULL;
	__dtfl_thread *wt = NULL;
	size_t n = 0, i;
	int rc;

	if (dtfl_config_set_threads(cfg, x->threads) != 0 ||
	    dtfl_config_set_mode(cfg, DTFL_COMM_CLUSTER) != 0 ||
	    dtfl_config_set_cluster(cfg, x->my_index, x->addrs,
	    x->processes) != 0) {
		snprintf(x->err, sizeof x->err, "config");
		goto out_cfg;
	}
	rc = __dtfl_comm_initialize(cfg, &b, &n, &g);
	if (rc != 0) {
		snprintf(x->err, sizeof x->err, "init rc=%d: %s", rc,
		    dtfl_last_error_detail());
		goto out_cfg;
	}
	if (n != x->threads) {
		snprintf(x->err, sizeof x->err, "local workers %zu", n);
		goto out_builders;
	}

	wa = calloc(n, sizeof *wa);
	wt = calloc(n, sizeof *wt);
	if (wa == NULL || wt == NULL)
		abort();

	/*
	 * Each worker thread builds its own allocator and runs: the
	 * TcpBuilder (and its inner Process builder) rendezvous during
	 * build, so a serial build on one thread would deadlock.
	 */
	for (i = 0; i < n; i++) {
		wa[i].builder = b[i];
		wa[i].a = NULL;
		wa[i].peers = x->peers;
		wa[i].me = x->my_index * x->threads + i;
		if (__dtfl_thread_spawn("work", worker_main, &wa[i], &wt[i])
		    != 0)
			abort();
	}
	__dtfl_free(b);
	b = NULL;
	for (i = 0; i < n; i++)
		(void)__dtfl_thread_join(wt[i]);
	for (i = 0; i < n; i++) {
		if (x->err[0] == '\0' && wa[i].err[0] != '\0')
			snprintf(x->err, sizeof x->err, "%s", wa[i].err);
		x->got += wa[i].got;
		if (wa[i].a != NULL)
			__dtfl_allocator_destroy(wa[i].a);
	}
	free(wa);
	free(wt);
out_builders:
	if (b != NULL)
		__dtfl_free(b);
	__dtfl_comms_guard_join(g);
out_cfg:
	dtfl_config_free(cfg);
	return NULL;
}

/* Runs a `processes` x `threads` cluster over loopback and checks it. */
static MunitResult
run_cluster(size_t processes, size_t threads)
{
	int ports[8];
	char addrbuf[8][32];
	const char *addrs[8];
	proc_arg *x;
	__dtfl_thread *t;
	size_t peers = processes * threads, i;
	MunitResult res = MUNIT_OK;

	if (processes > 8)
		return MUNIT_SKIP;
	for (i = 0; i < processes; i++) {
		ports[i] = pick_port();
		if (ports[i] < 0)
			return MUNIT_SKIP;
		snprintf(addrbuf[i], sizeof addrbuf[i], "127.0.0.1:%d",
		    ports[i]);
		addrs[i] = addrbuf[i];
	}
	/* Distinct ports (ephemeral picks can collide across iterations). */
	for (i = 1; i < processes; i++) {
		size_t j;

		for (j = 0; j < i; j++)
			if (ports[i] == ports[j])
				return MUNIT_SKIP;
	}

	x = calloc(processes, sizeof *x);
	t = calloc(processes, sizeof *t);
	if (x == NULL || t == NULL)
		abort();
	atomic_store(&test_msg_live, 0);
	for (i = 0; i < processes; i++) {
		memset(&x[i], 0, sizeof x[i]);
		x[i].my_index = i;
		x[i].processes = processes;
		x[i].threads = threads;
		x[i].peers = peers;
		x[i].addrs = addrs;
	}
	for (i = 0; i < processes; i++)
		assert_int(__dtfl_thread_spawn("proc", proc_main, &x[i],
		    &t[i]), ==, 0);
	for (i = 0; i < processes; i++)
		(void)__dtfl_thread_join(t[i]);

	for (i = 0; i < processes; i++) {
		if (x[i].err[0] != '\0') {
			munit_logf(MUNIT_LOG_ERROR, "process %zu: %s", i,
			    x[i].err);
			res = MUNIT_FAIL;
		}
		if (x[i].got != threads * peers * NMSG) {
			munit_logf(MUNIT_LOG_ERROR, "process %zu got %zu, "
			    "want %zu", i, x[i].got, threads * peers * NMSG);
			res = MUNIT_FAIL;
		}
	}
	if (atomic_load(&test_msg_live) != 0) {
		munit_logf(MUNIT_LOG_ERROR, "leak: %ld messages live",
		    atomic_load(&test_msg_live));
		res = MUNIT_FAIL;
	}
	free(x);
	free(t);
	return res;
}

DTFL_TEST(cluster_two_process_loopback_exchange)
{
	(void)params; (void)data;
	return run_cluster(2, 1);
}

DTFL_TEST(cluster_three_process_mesh_exchange)
{
	(void)params; (void)data;
	return run_cluster(3, 1);
}

DTFL_TEST(cluster_two_process_two_workers)
{
	(void)params; (void)data;
	return run_cluster(2, 2);
}

static MunitTest tests[] = {
	DTFL_T(cluster_two_process_loopback_exchange),
	DTFL_T(cluster_three_process_mesh_exchange),
	DTFL_T(cluster_two_process_two_workers),
	DTFL_T_END
};

DTFL_TEST_MAIN("tcp", tests)
