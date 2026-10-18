/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/sock_int.h: SIGPIPE suppression for library sockets.
 *
 * A write to a reset peer must fail with EPIPE rather than raise SIGPIPE,
 * and the library may not install signal handlers (AGENTS.md section 5).
 * Linux and the BSDs take MSG_NOSIGNAL on each send; Darwin has no such
 * flag and offers the per-socket SO_NOSIGPIPE option instead.  Use
 * DTFL_SEND_NOSIGNAL as the send() flags and call __dtfl_sock_nosigpipe
 * on every socket the library creates or accepts; each is a no-op where
 * the other mechanism applies.
 */
#ifndef DTFL_SOCK_INT_H
#define DTFL_SOCK_INT_H

#include <sys/types.h>
#include <sys/socket.h>

#ifdef MSG_NOSIGNAL
#define DTFL_SEND_NOSIGNAL MSG_NOSIGNAL
#else
#define DTFL_SEND_NOSIGNAL 0
#endif

static inline void
__dtfl_sock_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
	int one = 1;

	(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
	(void)fd;
#endif
}

#endif /* DTFL_SOCK_INT_H */
