/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tools/dtfl-diag/connect.c: open the source computation's log streams.
 *
 * Ported from dataflow-diag (HEAD 0fa322c9)
 *   connect/src/lib.rs
 *   connect/src/receive/connect.rs
 *   connect/src/receive/mod.rs
 * Copyright (c) 2017 the timely-dataflow diagnostics contributors.
 *
 * connect.rs's open_sockets binds a listening socket and accepts one
 * connection per source worker; make_readers wraps each as an
 * EventReader.  This file provides the same for dtfl-diag using only
 * public interfaces: it either opens the recorded-stream files passed
 * with -f/--diff-file, or binds iface:port and accepts source_peers
 * connections.  The descriptors are handed to dtfl_diag_replay_fds.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "diag_cmd.h"

static int
open_files(const char *const *files, size_t nfiles, int *fds, size_t maxfds)
{
	size_t i;
	int n = 0;

	for (i = 0; i < nfiles && (size_t)n < maxfds; i++) {
		int fd = open(files[i], O_RDONLY);

		if (fd < 0) {
			fprintf(stderr, "dtfl-diag: cannot open %s: %s\n",
			    files[i], strerror(errno));
			while (n > 0)
				(void)close(fds[--n]);
			return -1;
		}
		fds[n++] = fd;
	}
	return n;
}

static int
accept_peers(const char *iface, uint16_t port, size_t peers, int *fds,
    size_t maxfds)
{
	struct addrinfo hints, *res, *ai;
	char portbuf[16];
	int lfd = -1, one = 1, n = 0;
	size_t want = peers < maxfds ? peers : maxfds;

	snprintf(portbuf, sizeof portbuf, "%u", (unsigned)port);
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(iface, portbuf, &hints, &res) != 0) {
		fprintf(stderr, "dtfl-diag: resolve %s:%u failed\n", iface,
		    (unsigned)port);
		return -1;
	}
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
	if (lfd < 0) {
		fprintf(stderr, "dtfl-diag: bind/listen %s:%u failed\n", iface,
		    (unsigned)port);
		return -1;
	}
	fprintf(stderr, "Listening for %zu connections on %s:%u\n", want,
	    iface, (unsigned)port);
	while ((size_t)n < want) {
		int fd = accept(lfd, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "dtfl-diag: accept failed: %s\n",
			    strerror(errno));
			break;
		}
		fds[n++] = fd;
	}
	(void)close(lfd);
	if ((size_t)n < want) {
		while (n > 0)
			(void)close(fds[--n]);
		return -1;
	}
	fprintf(stderr, "Trace sources connected\n");
	return n;
}

int
diag_connect(const struct diag_opts *opts, int which, int *fds, size_t maxfds)
{
	const char *const *files;
	size_t nfiles;
	uint16_t port;

	if (which == DIAG_STREAM_DIFFERENTIAL) {
		files = opts->diff_files;
		nfiles = opts->ndiff_files;
		port = opts->diff_port;
	} else {
		files = opts->files;
		nfiles = opts->nfiles;
		port = opts->port;
	}
	if (nfiles > 0)
		return open_files(files, nfiles, fds, maxfds);
	return accept_peers(opts->iface, port, opts->source_peers, fds, maxfds);
}
