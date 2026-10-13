/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_tcp.c: the Cluster (TCP) allocator over a loopback
 * mesh.
 *
 * A two-process cluster is run inside one test binary: two threads, each
 * a "process" with one worker, connect to 127.0.0.1 on a pair of
 * ephemeral ports and form the full TCP mesh via __dtfl_comm_initialize
 * with a Cluster config.  Each worker then runs a tiny exchange: it
 * sends records to the global worker chosen by hash(record) % peers and
 * asserts that every record it receives was addressed to it and that
 * each (sender) stream arrives in order.  Worker-to-self traffic travels
 * the inner Process allocator; cross-process traffic crosses the socket,
 * so a correct run proves the send/recv loops and the handshake route
 * data to the right worker.
 *
 * No worker thread calls munit asserts (their longjmp target is on the
 * main thread): each records the first failure in its arg and returns,
 * and the main thread asserts after joining.  Every socket phase is
 * bounded by the create_sockets deadline, so a lost handshake fails
 * rather than hanging.
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
	size_t my_index;
	size_t peers;		/* total workers across the cluster */
	const char *addr0;
	const char *addr1;
	size_t got;		/* records received */
	char err[256];
} proc_arg;

/*
 * One cluster process (one local worker).  Builds its allocator, sends
 * NMSG records per peer-slot to the hash-chosen worker, receives until
 * it has every record addressed to it, and checks ordering.
 */
static void *
proc_main(void *v)
{
	proc_arg *x = v;
	const char *addrs[2] = { x->addr0, x->addr1 };
	dtfl_config *cfg = dtfl_config_new();
	__dtfl_allocator_builder **b = NULL;
	__dtfl_comms_guard *g = NULL;
	__dtfl_allocator *a = NULL;
	size_t n = 0, peers, me, s, k, want = 0;
	__dtfl_push **push = NULL;
	__dtfl_pull *pull = NULL;
	size_t *next = NULL;		/* per-source expected seqno */
	uint64_t t0;
	int rc;

	if (dtfl_config_set_mode(cfg, DTFL_COMM_CLUSTER) != 0 ||
	    dtfl_config_set_cluster(cfg, x->my_index, addrs, 2) != 0) {
		snprintf(x->err, sizeof x->err, "config");
		goto out_cfg;
	}
	rc = __dtfl_comm_initialize(cfg, &b, &n, &g);
	if (rc != 0) {
		snprintf(x->err, sizeof x->err, "init rc=%d: %s", rc,
		    dtfl_last_error_detail());
		goto out_cfg;
	}
	if (n != 1) {
		snprintf(x->err, sizeof x->err, "local workers %zu", n);
		goto out_builders;
	}
	a = __dtfl_allocator_build(b[0]);
	__dtfl_free(b);
	b = NULL;
	peers = a->peers;
	me = a->index;
	if (peers != x->peers || me != x->my_index) {
		snprintf(x->err, sizeof x->err, "index %zu peers %zu", me,
		    peers);
		goto out_alloc;
	}

	push = calloc(peers, sizeof *push);
	next = calloc(peers, sizeof *next);
	if (push == NULL || next == NULL)
		abort();
	__dtfl_allocate(a, 1, &test_msg_class, push, &pull);

	/*
	 * Record layout: (sender << 48) | (seq << 16) | target.  route()
	 * reads the low bits, so a record addressed to `target` lands on
	 * worker `target`; the receiver recovers sender and seq to check
	 * per-sender ordering.
	 */
	for (k = 0; k < NMSG; k++) {
		size_t target;

		for (target = 0; target < peers; target++) {
			uint64_t rec = ((uint64_t)me << 48) |
			    ((uint64_t)k << 16) | (uint64_t)target;

			if (route(rec, peers) != target) {
				snprintf(x->err, sizeof x->err,
				    "route(%llu)=%zu != %zu",
				    (unsigned long long)rec,
				    route(rec, peers), target);
				goto out_endpoints;
			}
			__dtfl_push_send(push[target], &test_msg_class,
			    test_msg_new(rec));
		}
	}
	for (s = 0; s < peers; s++)
		__dtfl_push_done(push[s]);

	/* We receive NMSG records from each of the `peers` senders. */
	want = peers * NMSG;
	t0 = __dtfl_clock_ns();
	while (x->got < want && x->err[0] == '\0') {
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
			/* Every record we get must be addressed to us. */
			if (tgt != me || route(val, peers) != me) {
				snprintf(x->err, sizeof x->err,
				    "worker %zu got misrouted %llu", me,
				    (unsigned long long)val);
			} else if (src >= peers) {
				snprintf(x->err, sizeof x->err,
				    "bad src %zu", src);
			} else if (seq != next[src]) {
				snprintf(x->err, sizeof x->err,
				    "worker %zu src %zu: seq %zu want %zu",
				    me, src, seq, next[src]);
			} else {
				next[src]++;
			}
			x->got++;
			test_msg_free(&test_msg_class, m);
		}
		__dtfl_allocator_release(a);
		if (!any) {
			if (__dtfl_clock_ns() - t0 > DEADLINE_NS) {
				snprintf(x->err, sizeof x->err, "worker %zu "
				    "timeout: %zu/%zu", me, x->got, want);
				break;
			}
			__dtfl_allocator_await_events(a, 50 * MS);
		}
	}

out_endpoints:
	for (s = 0; s < peers; s++)
		if (push[s] != NULL)
			__dtfl_push_destroy(push[s]);
	if (pull != NULL)
		__dtfl_pull_destroy(pull);
	free(push);
	free(next);
out_alloc:
	if (a != NULL)
		__dtfl_allocator_destroy(a);
out_builders:
	if (b != NULL)
		__dtfl_free(b);
	/* Join the network threads; a clean shutdown must not hang. */
	__dtfl_comms_guard_join(g);
out_cfg:
	dtfl_config_free(cfg);
	return NULL;
}

DTFL_TEST(cluster_two_process_loopback_exchange)
{
	int p0, p1;
	char a0[32], a1[32];
	proc_arg x[2];
	__dtfl_thread t[2];
	size_t i;

	(void)params; (void)data;
	p0 = pick_port();
	p1 = pick_port();
	if (p0 < 0 || p1 < 0 || p0 == p1)
		return MUNIT_SKIP;
	snprintf(a0, sizeof a0, "127.0.0.1:%d", p0);
	snprintf(a1, sizeof a1, "127.0.0.1:%d", p1);

	atomic_store(&test_msg_live, 0);
	for (i = 0; i < 2; i++) {
		memset(&x[i], 0, sizeof x[i]);
		x[i].my_index = i;
		x[i].peers = 2;
		x[i].addr0 = a0;
		x[i].addr1 = a1;
	}
	for (i = 0; i < 2; i++)
		assert_int(__dtfl_thread_spawn("proc", proc_main, &x[i],
		    &t[i]), ==, 0);
	for (i = 0; i < 2; i++)
		(void)__dtfl_thread_join(t[i]);

	for (i = 0; i < 2; i++) {
		if (x[i].err[0] != '\0')
			munit_errorf("process %zu: %s", i, x[i].err);
		assert_size(x[i].got, ==, 2 * NMSG);
	}
	/* Every message freed exactly once (no leak, no double free). */
	assert_long(atomic_load(&test_msg_live), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(cluster_two_process_loopback_exchange),
	DTFL_T_END
};

DTFL_TEST_MAIN("tcp", tests)
