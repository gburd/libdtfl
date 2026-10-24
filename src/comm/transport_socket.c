/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/stream.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/transport_socket.c: the built-in byte transport over
 * connected stream sockets (ADR-0013).
 *
 * dtfl_transport is the C form of upstream's Stream trait
 * (communication/src/allocator/zero_copy/stream.rs), which upstream
 * implements for TcpStream and UnixStream.  This file is those two
 * impls, in two flavors:
 *
 *   blocking (internal)  the cluster's send and receive threads, one of
 *                        each per peer, block in send(2) and recv(2) as
 *                        they always have.  wait is never needed.
 *   non-blocking         dtfl_transport_socket: one thread drives every
 *                        peer; wait polls the sockets and a self-pipe.
 *
 * Threads.  In the blocking flavor two threads use one descriptor, one
 * sending and one receiving, which sockets allow; the struct is
 * immutable after creation.  In the non-blocking flavor everything but
 * wake belongs to the thread that drives the transport; wake writes one
 * byte to the self-pipe from any thread.
 */
#define _GNU_SOURCE	/* MSG_NOSIGNAL, via base/sock_int.h */

#include "comm/zc_int.h"
#include "base/sock_int.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct sock_transport {
	int *fds;		/* [n]; fds[self] unused */
	size_t n;
	size_t self;
	bool blocking;
	bool owns_fds;		/* destroy closes fds */
	int pipe_rd, pipe_wr;	/* self-pipe; -1 in the blocking flavor */
	bool *short_send;	/* [n]; the last send to p was short */
	bool *eof;		/* [n]; recv from p has returned DTFL_EOF */
	struct pollfd *pfds;	/* [n + 1] scratch for wait */
} sock_transport;

static int
st_send(void *ctx, size_t peer, const void *buf, size_t len, size_t *sent)
{
	sock_transport *st = ctx;
	ssize_t w;

	*sent = 0;
	if (peer >= st->n || peer == st->self)
		return __dtfl_err(DTFL_EINVAL, "transport: no peer %zu", peer);
	if (len == 0)
		return 0;
	do {
		w = send(st->fds[peer], buf, len, DTFL_SEND_NOSIGNAL);
	} while (w < 0 && errno == EINTR);
	if (w < 0) {
		if (!st->blocking && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			st->short_send[peer] = true;
			return 0;
		}
		return __dtfl_err(DTFL_EIO, "%s", strerror(errno));
	}
	*sent = (size_t)w;
	st->short_send[peer] = !st->blocking && (size_t)w < len;
	return 0;
}

static int
st_recv(void *ctx, size_t peer, void *buf, size_t cap, size_t *got)
{
	sock_transport *st = ctx;
	ssize_t r;

	*got = 0;
	if (peer >= st->n || peer == st->self)
		return __dtfl_err(DTFL_EINVAL, "transport: no peer %zu", peer);
	do {
		r = recv(st->fds[peer], buf, cap, 0);
	} while (r < 0 && errno == EINTR);
	if (r < 0) {
		if (!st->blocking && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		return __dtfl_err(DTFL_EIO, "%s", strerror(errno));
	}
	if (r == 0 && cap > 0) {
		if (!st->blocking)
			st->eof[peer] = true;
		return DTFL_EOF;
	}
	*got = (size_t)r;
	return 0;
}

/* Drains the self-pipe; called by the driving thread only. */
static void
drain_pipe(sock_transport *st)
{
	char buf[64];

	while (read(st->pipe_rd, buf, sizeof buf) > 0)
		;
}

static void
st_wait(void *ctx, uint64_t timeout_ns)
{
	sock_transport *st = ctx;
	size_t p, k = 0;
	int ms;

	if (st->blocking)
		return;	/* send and recv block; there is nothing to wait for */
	for (p = 0; p < st->n; p++) {
		short ev = 0;

		if (p == st->self)
			continue;
		if (!st->eof[p])
			ev |= POLLIN;
		if (st->short_send[p])
			ev |= POLLOUT;
		if (ev == 0)
			continue;
		st->pfds[k].fd = st->fds[p];
		st->pfds[k].events = ev;
		st->pfds[k].revents = 0;
		k++;
	}
	st->pfds[k].fd = st->pipe_rd;
	st->pfds[k].events = POLLIN;
	st->pfds[k].revents = 0;
	k++;
	if (timeout_ns == UINT64_MAX)
		ms = -1;
	else if (timeout_ns / 1000000 >= (uint64_t)INT_MAX)
		ms = INT_MAX;
	else	/* round up, so a short timeout still sleeps */
		ms = (int)((timeout_ns + 999999) / 1000000);
	(void)poll(st->pfds, (nfds_t)k, ms);
	drain_pipe(st);
}

static void
st_wake(void *ctx)
{
	sock_transport *st = ctx;
	char one = 1;

	if (st->blocking)
		return;
	/* A full pipe already holds a pending wakeup. */
	(void)!write(st->pipe_wr, &one, 1);
}

static int
st_shutdown(void *ctx, size_t peer)
{
	sock_transport *st = ctx;

	if (peer >= st->n || peer == st->self)
		return __dtfl_err(DTFL_EINVAL, "transport: no peer %zu", peer);
	if (shutdown(st->fds[peer], SHUT_WR) != 0)
		return __dtfl_err(DTFL_EIO, "%s", strerror(errno));
	return 0;
}

static void
st_destroy(void *ctx)
{
	sock_transport *st = ctx;
	size_t p;

	if (st == NULL)
		return;
	if (st->owns_fds)
		for (p = 0; p < st->n; p++)
			if (p != st->self && st->fds[p] >= 0)
				(void)close(st->fds[p]);
	if (st->pipe_rd >= 0)
		(void)close(st->pipe_rd);
	if (st->pipe_wr >= 0)
		(void)close(st->pipe_wr);
	__dtfl_free(st->fds);
	__dtfl_free(st->short_send);
	__dtfl_free(st->eof);
	__dtfl_free(st->pfds);
	__dtfl_free(st);
}

static int
set_flags(int fd, bool nonblock)
{
	int fl = fcntl(fd, F_GETFL);

	if (fl < 0)
		return -1;
	if (nonblock)
		fl |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, fl) != 0)
		return -1;
	return fcntl(fd, F_SETFD, FD_CLOEXEC);
}

static sock_transport *
st_new(const int *fds, size_t n, size_t self, bool blocking)
{
	sock_transport *st = __dtfl_calloc(1, sizeof *st);

	st->n = n;
	st->self = self;
	st->blocking = blocking;
	st->pipe_rd = st->pipe_wr = -1;
	st->fds = __dtfl_memdup(fds, n * sizeof *fds);
	st->fds[self] = -1;
	st->short_send = __dtfl_calloc(n, sizeof *st->short_send);
	st->eof = __dtfl_calloc(n, sizeof *st->eof);
	st->pfds = __dtfl_calloc(n + 1, sizeof *st->pfds);
	return st;
}

static void
st_fill(sock_transport *st, dtfl_transport *out)
{
	out->send = st_send;
	out->recv = st_recv;
	out->wait = st_wait;
	out->wake = st_wake;
	out->shutdown = st_shutdown;
	out->destroy = st_destroy;
	out->ctx = st;
}

int
dtfl_transport_socket(const int *fds, size_t nfds, size_t self,
    dtfl_transport *out)
{
	sock_transport *st;
	int pfd[2];
	size_t p;

	if (fds == NULL || out == NULL || nfds == 0 || self >= nfds)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_transport_socket: need fds, out and self < nfds");
	for (p = 0; p < nfds; p++)
		if (p != self && fds[p] < 0)
			return __dtfl_err(DTFL_EINVAL,
			    "dtfl_transport_socket: fds[%zu] is not open", p);
	if (pipe(pfd) != 0)
		return __dtfl_err(DTFL_EIO, "pipe: %s", strerror(errno));
	if (set_flags(pfd[0], true) != 0 || set_flags(pfd[1], true) != 0) {
		(void)close(pfd[0]);
		(void)close(pfd[1]);
		return __dtfl_err(DTFL_EIO, "fcntl: %s", strerror(errno));
	}
	for (p = 0; p < nfds; p++) {
		if (p == self)
			continue;
		if (set_flags(fds[p], true) != 0) {
			(void)close(pfd[0]);
			(void)close(pfd[1]);
			return __dtfl_err(DTFL_EIO, "fcntl(fds[%zu]): %s", p,
			    strerror(errno));
		}
		__dtfl_sock_nosigpipe(fds[p]);
	}
	st = st_new(fds, nfds, self, false);
	st->owns_fds = true;
	st->pipe_rd = pfd[0];
	st->pipe_wr = pfd[1];
	st_fill(st, out);
	return DTFL_OK;
}

void
__dtfl_transport_socket_blocking(const int *fds, size_t nfds, size_t self,
    dtfl_transport *out)
{
	st_fill(st_new(fds, nfds, self, true), out);
}
