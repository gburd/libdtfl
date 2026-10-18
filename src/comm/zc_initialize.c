/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/initialize.rs
 *   communication/src/networking.rs (create_sockets, start_connections,
 *     await_connections)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/zc_initialize.c: cluster network setup.
 *
 * create_sockets() builds the full TCP mesh among `processes` processes.
 * Each process connects to every lower-indexed process and accepts from
 * every higher-indexed one, so every ordered pair gets exactly one
 * socket; the handshake exchanges process indices (each connector writes
 * its own index as a big-endian u64, each acceptor reads it) so both
 * ends agree on who is who before any data flows.  The result is a
 * vector of `processes` fds with my_index set to -1 (no socket to self).
 *
 * __dtfl_tcp_initialize() then builds one TcpBuilder per local worker,
 * creates the mpsc handoff grids, and starts one send and one receive
 * thread per remote process.  It returns the builders and a comms guard
 * that joins the network threads and closes the sockets on teardown.
 *
 * Threads.  create_sockets runs the connect and accept phases on two
 * helper threads (as upstream) and joins them.  The send and recv
 * threads live until teardown; the guard joins them.
 */
#define _GNU_SOURCE	/* getaddrinfo, AI_PASSIVE */

#include "comm/zc_int.h"
#include "comm/init_int.h"
#include "base/os_thread.h"
#include "base/sock_int.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ByteOrder helpers for the handshake (big-endian u64, as networking.c). */
static void
put_be64(unsigned char *p, uint64_t v)
{
	int i;

	for (i = 7; i >= 0; i--) {
		p[i] = (unsigned char)(v & 0xff);
		v >>= 8;
	}
}

static uint64_t
get_be64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

/* Writes exactly len bytes to fd; false on error or peer close. */
static bool
write_all(int fd, const unsigned char *buf, size_t len)
{
	size_t off = 0;
	ssize_t w;

	while (off < len) {
		w = send(fd, buf + off, len - off, DTFL_SEND_NOSIGNAL);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return false;
		off += (size_t)w;
	}
	return true;
}

/* Reads exactly len bytes from fd; false on error or peer close. */
static bool
read_all(int fd, unsigned char *buf, size_t len)
{
	size_t off = 0;
	ssize_t r;

	while (off < len) {
		r = recv(fd, buf + off, len - off, 0);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		off += (size_t)r;
	}
	return true;
}

/* Splits "host:port" at the last colon.  host and port point into a copy. */
static int
split_hostport(const char *addr, char *host, size_t hostcap, char *port,
    size_t portcap)
{
	const char *colon = strrchr(addr, ':');
	size_t hlen;

	if (colon == NULL)
		return __dtfl_err(DTFL_EINVAL, "address '%s' has no port", addr);
	hlen = (size_t)(colon - addr);
	if (hlen == 0 || hlen >= hostcap || strlen(colon + 1) >= portcap)
		return __dtfl_err(DTFL_EINVAL, "address '%s' malformed", addr);
	memcpy(host, addr, hlen);
	host[hlen] = '\0';
	strcpy(port, colon + 1);
	return 0;
}

/* ------------------------------------------------------------------ */
/* start_connections / await_connections                              */
/* ------------------------------------------------------------------ */

/* How long to retry a refused connect before giving up (test backstop). */
#define CONNECT_DEADLINE_NS (UINT64_C(30) * 1000000000)
#define RETRY_SLEEP_NS (UINT64_C(20) * 1000000)

static void
sleep_ns(uint64_t ns)
{
	struct timespec ts;

	ts.tv_sec = (time_t)(ns / UINT64_C(1000000000));
	ts.tv_nsec = (long)(ns % UINT64_C(1000000000));
	(void)nanosleep(&ts, NULL);
}

typedef struct connect_arg {
	char *const *addresses;
	size_t my_index;
	int *fds;		/* fds[0 .. my_index) filled on success */
	int rc;
} connect_arg;

typedef struct accept_arg {
	char *const *addresses;
	size_t my_index;
	size_t processes;
	int *fds;		/* fds[my_index+1 .. processes) filled */
	int rc;
} accept_arg;

/* Connect to each lower-indexed process; write our index as the handshake. */
static void *
start_connections(void *v)
{
	connect_arg *a = v;
	char host[256], port[32];
	unsigned char hs[8];
	struct addrinfo hints, *res, *ai;
	size_t idx;
	int fd, one = 1;

	for (idx = 0; idx < a->my_index; idx++) {
		uint64_t deadline = __dtfl_clock_ns() + CONNECT_DEADLINE_NS;

		if ((a->rc = split_hostport(a->addresses[idx], host,
		    sizeof host, port, sizeof port)) != 0)
			return NULL;
		for (;;) {
			memset(&hints, 0, sizeof hints);
			hints.ai_family = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			if (getaddrinfo(host, port, &hints, &res) != 0) {
				if (__dtfl_clock_ns() >= deadline) {
					a->rc = __dtfl_err(DTFL_EIO,
					    "resolve %s:%s failed", host, port);
					return NULL;
				}
				sleep_ns(RETRY_SLEEP_NS);
				continue;
			}
			fd = -1;
			for (ai = res; ai != NULL; ai = ai->ai_next) {
				fd = socket(ai->ai_family, ai->ai_socktype,
				    ai->ai_protocol);
				if (fd < 0)
					continue;
				if (connect(fd, ai->ai_addr, ai->ai_addrlen)
				    == 0)
					break;
				(void)close(fd);
				fd = -1;
			}
			freeaddrinfo(res);
			if (fd >= 0)
				break;
			if (__dtfl_clock_ns() >= deadline) {
				a->rc = __dtfl_err(DTFL_EIO,
				    "connect to %s:%s failed", host, port);
				return NULL;
			}
			sleep_ns(RETRY_SLEEP_NS);
		}
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
		    sizeof one);
		__dtfl_sock_nosigpipe(fd);
		put_be64(hs, (uint64_t)a->my_index);
		if (!write_all(fd, hs, sizeof hs)) {
			(void)close(fd);
			a->rc = __dtfl_err(DTFL_EIO,
			    "handshake send to process %zu failed", idx);
			return NULL;
		}
		a->fds[idx] = fd;
	}
	a->rc = 0;
	return NULL;
}

/* Accept from each higher-indexed process; read its index for placement. */
static void *
await_connections(void *v)
{
	accept_arg *a = v;
	char host[256], port[32];
	unsigned char hs[8];
	struct addrinfo hints, *res, *ai;
	size_t want = a->processes - a->my_index - 1, got = 0;
	int lfd = -1, one = 1;
	uint64_t deadline = __dtfl_clock_ns() + CONNECT_DEADLINE_NS;

	if (want == 0) {
		a->rc = 0;
		return NULL;
	}
	if ((a->rc = split_hostport(a->addresses[a->my_index], host,
	    sizeof host, port, sizeof port)) != 0)
		return NULL;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(host, port, &hints, &res) != 0) {
		a->rc = __dtfl_err(DTFL_EIO, "resolve %s:%s failed", host, port);
		return NULL;
	}
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		lfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (lfd < 0)
			continue;
		(void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one,
		    sizeof one);
		if (bind(lfd, ai->ai_addr, ai->ai_addrlen) == 0 &&
		    listen(lfd, 128) == 0)
			break;
		(void)close(lfd);
		lfd = -1;
	}
	freeaddrinfo(res);
	if (lfd < 0) {
		a->rc = __dtfl_err(DTFL_EIO, "bind/listen %s:%s failed", host,
		    port);
		return NULL;
	}

	while (got < want) {
		int fd = accept(lfd, NULL, NULL);
		uint64_t idx;

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			if (__dtfl_clock_ns() >= deadline) {
				(void)close(lfd);
				a->rc = __dtfl_err(DTFL_EIO,
				    "accept timed out after %zu/%zu", got, want);
				return NULL;
			}
			sleep_ns(RETRY_SLEEP_NS);
			continue;
		}
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one,
		    sizeof one);
		__dtfl_sock_nosigpipe(fd);
		if (!read_all(fd, hs, sizeof hs)) {
			(void)close(fd);
			continue;
		}
		idx = get_be64(hs);
		if (idx <= a->my_index || idx >= a->processes) {
			(void)close(fd);
			(void)close(lfd);
			a->rc = __dtfl_err(DTFL_EIO,
			    "handshake: peer index %llu out of range",
			    (unsigned long long)idx);
			return NULL;
		}
		a->fds[idx] = fd;
		got++;
	}
	(void)close(lfd);
	a->rc = 0;
	return NULL;
}

/*
 * create_sockets: the full mesh.  On success fills fds[processes] (-1 at
 * my_index, a connected socket elsewhere) and returns 0.  On failure
 * returns an error with detail and closes any fds already opened.
 */
int
__dtfl_create_sockets(char *const *addresses, size_t processes,
    size_t my_index, int **fds_out)
{
	int *fds = __dtfl_calloc(DTFL_MAX(processes, 1), sizeof *fds);
	connect_arg carg;
	accept_arg aarg;
	__dtfl_thread ct, at;
	size_t i;
	int rc;

	for (i = 0; i < processes; i++)
		fds[i] = -1;

	carg.addresses = addresses;
	carg.my_index = my_index;
	carg.fds = fds;
	carg.rc = 0;
	aarg.addresses = addresses;
	aarg.my_index = my_index;
	aarg.processes = processes;
	aarg.fds = fds;
	aarg.rc = 0;

	rc = __dtfl_thread_spawn("dtfl:connect", start_connections, &carg, &ct);
	if (rc != 0) {
		__dtfl_free(fds);
		return rc;
	}
	rc = __dtfl_thread_spawn("dtfl:accept", await_connections, &aarg, &at);
	if (rc != 0) {
		(void)__dtfl_thread_join(ct);
		__dtfl_free(fds);
		return rc;
	}
	(void)__dtfl_thread_join(ct);
	(void)__dtfl_thread_join(at);

	if (carg.rc != 0 || aarg.rc != 0) {
		for (i = 0; i < processes; i++)
			if (fds[i] >= 0)
				(void)close(fds[i]);
		__dtfl_free(fds);
		return carg.rc != 0 ? carg.rc : aarg.rc;
	}
	*fds_out = fds;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Network threads and the comms guard                                 */
/* ------------------------------------------------------------------ */

typedef struct net_thread_arg {
	int fd;
	bool is_send;
	size_t worker_offset;
	__dtfl_mpsc **queues;	/* owned by the loop: targets or sources */
	size_t nqueues;
} net_thread_arg;

struct __dtfl_comms_guard {
	__dtfl_thread *sends;
	__dtfl_thread *recvs;
	size_t n;		/* send/recv thread count (processes - 1) */
	int *fds;		/* [processes]; closed at join */
	size_t processes;
	net_thread_arg *args;	/* 2 * n */
};

static void *
net_thread_main(void *v)
{
	net_thread_arg *a = v;

	if (a->is_send)
		__dtfl_tcp_send_loop(a->fd, a->queues, a->nqueues);
	else
		__dtfl_tcp_recv_loop(a->fd, a->queues, a->nqueues,
		    a->worker_offset);
	return NULL;
}

/*
 * initialize_networking_from_sockets: wrap the typed Process builders in
 * TcpBuilders and start the network threads.  `fds` has `processes`
 * entries with my_index == -1.  Takes ownership of `inner` (freed) and
 * `fds` (closed by the guard).  Fills *builders (threads entries; caller
 * frees the array) and *guard.
 */
int
__dtfl_tcp_initialize(__dtfl_allocator_builder **inner, size_t threads,
    size_t my_index, size_t processes, int *fds,
    __dtfl_allocator_builder ***builders_out, __dtfl_comms_guard **guard_out)
{
	size_t nremote = processes - 1;
	__dtfl_allocator_builder **builders =
	    __dtfl_calloc(DTFL_MAX(threads, 1), sizeof *builders);
	__dtfl_mpsc ***net_promises =
	    __dtfl_calloc(DTFL_MAX(nremote, 1), sizeof *net_promises);
	__dtfl_mpsc ***net_futures =
	    __dtfl_calloc(DTFL_MAX(nremote, 1), sizeof *net_futures);
	__dtfl_comms_guard *g = __dtfl_calloc(1, sizeof *g);
	size_t pr, i, started = 0;
	int rc = 0;

	for (pr = 0; pr < nremote; pr++) {
		net_promises[pr] = __dtfl_calloc(threads, sizeof **net_promises);
		net_futures[pr] = __dtfl_calloc(threads, sizeof **net_futures);
	}

	__dtfl_tcp_builders(inner, threads, my_index, processes, builders,
	    net_promises, net_futures);
	__dtfl_free(inner);

	g->n = nremote;
	g->processes = processes;
	g->fds = fds;
	g->sends = __dtfl_calloc(DTFL_MAX(nremote, 1), sizeof *g->sends);
	g->recvs = __dtfl_calloc(DTFL_MAX(nremote, 1), sizeof *g->recvs);
	g->args = __dtfl_calloc(DTFL_MAX(2 * nremote, 1), sizeof *g->args);

	/*
	 * One send and one recv thread per remote process, in process-index
	 * order (skipping self), matching the net_promises/net_futures grid
	 * order and the recv worker_offset = threads * my_index.
	 */
	for (i = 0; i < processes; i++) {
		net_thread_arg *sa, *ra;
		char name[48];

		if (i == my_index)
			continue;
		DTFL_ASSERT(fds[i] >= 0);
		sa = &g->args[2 * started];
		ra = &g->args[2 * started + 1];

		sa->fd = fds[i];
		sa->is_send = true;
		sa->queues = net_futures[started];	/* send drains these */
		sa->nqueues = threads;
		ra->fd = fds[i];
		ra->is_send = false;
		ra->queues = net_promises[started];	/* recv fills these */
		ra->nqueues = threads;
		ra->worker_offset = threads * my_index;

		snprintf(name, sizeof name, "dtfl:send-%zu", i);
		rc = __dtfl_thread_spawn(name, net_thread_main, sa,
		    &g->sends[started]);
		if (rc != 0)
			__dtfl_panic("cannot start send thread %zu: %s", i,
			    dtfl_last_error_detail());
		snprintf(name, sizeof name, "dtfl:recv-%zu", i);
		rc = __dtfl_thread_spawn(name, net_thread_main, ra,
		    &g->recvs[started]);
		if (rc != 0)
			__dtfl_panic("cannot start recv thread %zu: %s", i,
			    dtfl_last_error_detail());
		started++;
	}

	__dtfl_free(net_promises);
	__dtfl_free(net_futures);
	*builders_out = builders;
	*guard_out = g;
	return 0;
}

void
__dtfl_comms_guard_join(__dtfl_comms_guard *g)
{
	size_t i;

	if (g == NULL)
		return;
	for (i = 0; i < g->n; i++)
		(void)__dtfl_thread_join(g->sends[i]);
	for (i = 0; i < g->n; i++)
		(void)__dtfl_thread_join(g->recvs[i]);
	for (i = 0; i < g->processes; i++)
		if (g->fds[i] >= 0)
			(void)close(g->fds[i]);
	__dtfl_free(g->sends);
	__dtfl_free(g->recvs);
	__dtfl_free(g->args);
	__dtfl_free(g->fds);
	__dtfl_free(g);
}
