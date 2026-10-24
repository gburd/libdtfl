/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/diag/ws_server.c: the diagnostics WebSocket server loop.
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   diagnostics/src/server.rs
 * Copyright (c) 2019 Timely Dataflow Diagnostics contributors.
 *
 * server.rs runs a background thread with a non-blocking TcpListener:
 * it accepts WebSocket connections (tungstenite), announces each new
 * client to the diagnostics dataflow (so the cross-join replays the
 * full state), drains the capture channel of DiagnosticUpdates, buckets
 * them by inner timestamp, and flushes one Frame per closed timestamp
 * to each client.
 *
 * The C server keeps the same loop and the same Frame-per-timestamp
 * flushing, but the "full state on connect" and "incremental updates"
 * both come from the session's shared state rather than a second
 * dataflow: on accept it replays the live snapshot as one Frame, then
 * forwards the Frame payloads the session enqueues.  It binds 127.0.0.1
 * by default (PLAN D12, ADR) where upstream binds 0.0.0.0.
 *
 * The listener and client sockets are non-blocking for accept and
 * blocking with a short SO_SNDTIMEO/SO_RCVTIMEO for the handshake and
 * sends, so no call in the loop blocks forever: a wedged client cannot
 * stall the thread, and the loop polls the shared running flag so
 * dtfl_diag_server_stop joins promptly.
 */
#define _GNU_SOURCE	/* getaddrinfo, AI_PASSIVE */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "diag/diag_int.h"
#include "base/sock_int.h"

/* The flush cadence of the serve loop, matching server.rs (100 ms). */
#define FLUSH_INTERVAL_NS 100000000ULL
/* Per-send socket timeout so one slow client cannot wedge the thread. */
#define CLIENT_IO_TIMEOUT_SEC 5

/* A connected client. */
typedef struct ws_client {
	int fd;
} ws_client;

#define sleep_ns __dtfl_sleep_ns

/*
 * Binds a listening socket on iface:port.  port 0 lets the kernel pick;
 * the chosen port is returned in *port_out.  The socket is left
 * non-blocking.  Returns DTFL_OK or a DTFL_E* code.
 */
int
__dtfl_diag_listen(const char *iface, uint16_t port, int *fd_out,
    uint16_t *port_out)
{
	struct addrinfo hints, *res, *ai;
	char portbuf[16];
	int lfd = -1, one = 1, flags;
	struct sockaddr_storage ss;
	socklen_t slen = sizeof ss;

	snprintf(portbuf, sizeof portbuf, "%u", (unsigned)port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(iface, portbuf, &hints, &res) != 0)
		return __dtfl_err(DTFL_EIO, "diag: resolve %s:%u failed",
		    iface, (unsigned)port);
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		lfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (lfd < 0)
			continue;
		(void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one,
		    sizeof one);
		if (bind(lfd, ai->ai_addr, ai->ai_addrlen) == 0 &&
		    listen(lfd, 16) == 0)
			break;
		(void)close(lfd);
		lfd = -1;
	}
	freeaddrinfo(res);
	if (lfd < 0)
		return __dtfl_err(DTFL_EIO, "diag: bind/listen %s:%u failed",
		    iface, (unsigned)port);

	if (getsockname(lfd, (struct sockaddr *)&ss, &slen) == 0) {
		if (ss.ss_family == AF_INET)
			*port_out = ntohs(((struct sockaddr_in *)&ss)->sin_port);
		else
			*port_out =
			    ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
	} else {
		*port_out = port;
	}

	flags = fcntl(lfd, F_GETFL, 0);
	(void)fcntl(lfd, F_SETFL, flags | O_NONBLOCK);
	*fd_out = lfd;
	return DTFL_OK;
}

/* Builds the full-state snapshot as one Frame payload (owned, or NULL). */
static char *
snapshot_frame(__dtfl_diag_shared *sh)
{
	__dtfl_diag_frame frame;
	size_t i;

	__dtfl_diag_frame_begin(&frame);
	for (i = 0; i < sh->operators.len; i++) {
		__dtfl_diag_operator *op = __dtfl_vec_at(&sh->operators, i);

		__dtfl_diag_frame_add_operator(&frame, op->id, op->name,
		    op->addr, op->addr_len, 1);
	}
	for (i = 0; i < sh->channels.len; i++) {
		__dtfl_diag_channel *ch = __dtfl_vec_at(&sh->channels, i);

		__dtfl_diag_frame_add_channel(&frame, ch->id, ch->scope_addr,
		    ch->scope_len, ch->src_node, ch->src_port, ch->tgt_node,
		    ch->tgt_port, 1);
	}
	for (i = 0; i < sh->stats.len; i++) {
		__dtfl_diag_stat *st = __dtfl_vec_at(&sh->stats, i);

		__dtfl_diag_frame_add_stat(&frame, st->kind, st->id, st->value);
	}
	return __dtfl_diag_frame_finish(&frame, 0);
}

static void
set_io_timeout(int fd)
{
	struct timeval tv = { CLIENT_IO_TIMEOUT_SEC, 0 };
	int one = 1;
	int flags = fcntl(fd, F_GETFL, 0);

	/*
	 * Darwin and the BSDs copy the listener's O_NONBLOCK to the
	 * accepted socket, where Linux does not.  The handshake and sends
	 * must block, bounded by the timeouts below, or a request that has
	 * not arrived yet fails the handshake with EAGAIN.
	 */
	if (flags >= 0 && (flags & O_NONBLOCK) != 0)
		(void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	__dtfl_sock_nosigpipe(fd);
}

void *
__dtfl_diag_server_loop(void *arg)
{
	dtfl_diag_server *srv = arg;
	__dtfl_diag_shared *sh = srv->shared;
	__dtfl_vec clients;	/* ws_client */

	__dtfl_vec_init(&clients, sizeof(ws_client));

	while (atomic_load(&sh->running)) {
		int cfd;
		size_t i;
		__dtfl_vec batch;	/* char *: payloads drained this pass */

		/* Accept any pending connections (non-blocking). */
		for (;;) {
			cfd = accept(srv->listen_fd, NULL, NULL);
			if (cfd < 0)
				break;	/* EWOULDBLOCK or error: stop accepting */
			set_io_timeout(cfd);
			if (__dtfl_ws_handshake(cfd) != DTFL_OK) {
				(void)close(cfd);
				continue;
			}
			/* Replay the full current state as the first Frame. */
			{
				char *snap;

				__dtfl_mutex_lock(&sh->lock);
				snap = snapshot_frame(sh);
				__dtfl_mutex_unlock(&sh->lock);
				if (snap != NULL) {
					(void)__dtfl_ws_send_text(cfd, snap,
					    strlen(snap));
					__dtfl_free(snap);
				}
			}
			{
				ws_client c = { cfd };

				__dtfl_vec_push(&clients, &c);
			}
		}

		/* Drain the pending Frame queue under the lock. */
		__dtfl_vec_init(&batch, sizeof(char *));
		__dtfl_mutex_lock(&sh->lock);
		if (sh->queue.len > 0) {
			__dtfl_vec_push_n(&batch, sh->queue.data,
			    sh->queue.len);
			__dtfl_vec_clear(&sh->queue);
		}
		__dtfl_mutex_unlock(&sh->lock);

		/* Forward each payload to every client; drop dead ones. */
		for (i = 0; i < batch.len; i++) {
			char **pp = __dtfl_vec_at(&batch, i);
			char *payload = *pp;
			size_t j = 0;

			while (j < clients.len) {
				ws_client *c = __dtfl_vec_at(&clients, j);

				if (__dtfl_ws_send_text(c->fd, payload,
				    strlen(payload)) != DTFL_OK) {
					(void)close(c->fd);
					__dtfl_vec_swap_remove(&clients, j);
					continue;
				}
				j++;
			}
			__dtfl_free(payload);
		}
		__dtfl_vec_free(&batch);

		sleep_ns(FLUSH_INTERVAL_NS);
	}

	/* Shutdown: close every client gracefully. */
	{
		size_t i;

		for (i = 0; i < clients.len; i++) {
			ws_client *c = __dtfl_vec_at(&clients, i);

			__dtfl_ws_send_close(c->fd);
			(void)close(c->fd);
		}
	}
	__dtfl_vec_free(&clients);
	(void)close(srv->listen_fd);
	return NULL;
}
