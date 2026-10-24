/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/capture_recv.c: mirrors timely/examples/capture_recv.rs.
 *
 * The first positional argument is the number of source workers.  Each
 * receiving worker listens on 127.0.0.1:(8000 + i) for the source worker
 * ids assigned to it (i % peers == index), accepts one connection each,
 * and replays all of them into one dataflow, inspecting the records.
 * Run this first, then capture_send.
 *
 * Divergence from upstream: binds the loopback address only (D12).  Not
 * in the smoke harness: it needs capture_send connecting, which is not
 * deterministic in a single-process test; the binary transport itself is
 * covered by tests/unit/test_capture.c.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct recv_ctx {
	int *fds;
	size_t nfds;
} recv_ctx;

static void
inspect_each(void *ctx, const dtfl_message *msg, const uint64_t *frontier,
    size_t nfrontier)
{
	size_t i, n;

	(void)ctx; (void)frontier; (void)nfrontier;
	if (msg == NULL)		/* Err arm: a frontier-only report. */
		return;
	n = dtfl_message_len(msg);
	for (i = 0; i < n; i++) {
		size_t len;
		const void *rec = dtfl_message_record(msg, i, &len);

		printf("replayed: %llu\n", (unsigned long long)dtfl_get_u64(rec));
	}
}

static void
build(dtfl_scope *scope, void *arg)
{
	recv_ctx *c = arg;
	dtfl_stream *s = dtfl_event_reader_replay(c->fds, c->nfds, scope,
	    dtfl_type_u64());

	dtfl_stream_inspect_core(s, inspect_each, DTFL_NOCTX);
}

/* Listen on 127.0.0.1:port and accept one connection. */
static int
accept_one(uint16_t port)
{
	struct sockaddr_in sa;
	int lfd = socket(AF_INET, SOCK_STREAM, 0), fd, opt = 1;

	if (lfd < 0)
		return -1;
	setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
	    listen(lfd, 1) < 0) {
		perror("bind/listen");
		close(lfd);
		return -1;
	}
	fd = accept(lfd, NULL, NULL);
	close(lfd);
	return fd;
}

static void
worker(dtfl_worker *w, void *arg)
{
	size_t peers = dtfl_worker_peers(w), index = dtfl_worker_index(w);
	size_t source_peers = *(size_t *)arg, i, nfds = 0;
	int *fds = calloc(source_peers, sizeof *fds);
	recv_ctx c;
	dtfl_time_class *tc;

	for (i = 0; i < source_peers; i++)
		if (i % peers == index) {
			int fd = accept_one((uint16_t)(8000 + i));

			if (fd >= 0)
				fds[nfds++] = fd;
		}
	c.fds = fds;
	c.nfds = nfds;
	tc = dtfl_time_u64();
	dtfl_worker_dataflow(w, "capture_recv", tc, build, &c);
	while (dtfl_worker_step(w))
		;
	for (i = 0; i < nfds; i++)
		close(fds[i]);
	free(fds);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	size_t source_peers = ex_size(argc, argv, 1, 1);
	dtfl_config *cfg = ex_config(argc, argv, 1);
	int rc = dtfl_execute(cfg, worker, &source_peers, NULL);

	dtfl_config_free(cfg);
	return rc;
}
