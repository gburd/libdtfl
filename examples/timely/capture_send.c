/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/capture_send.c: mirrors timely/examples/capture_send.rs.
 *
 * Each worker connects a TCP stream to 127.0.0.1:(8000 + worker index),
 * builds a dataflow that streams 0..10 and captures it into an
 * EventWriter wrapping that stream.  Run capture_recv first (it listens),
 * then capture_send.
 *
 * Divergence from upstream: binds the loopback address only (D12).  Not
 * in the smoke harness: it needs capture_recv listening on a matching
 * port, which is not deterministic in a single-process test; the binary
 * transport itself is covered by tests/unit/test_capture.c.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct send_ctx {
	int fd;
	dtfl_event_writer *w;		/* created in build, freed in worker */
} send_ctx;

/* 0..10 as a source, captured into the writer over the socket. */
static bool
source_logic(dtfl_op *op, void *vctx)
{
	dtfl_cap *cap = dtfl_op_initial_cap(op, 0);
	uint64_t t = 0;
	size_t i;

	(void)vctx;
	if (cap == NULL)
		return false;
	for (i = 0; i < 10; i++) {
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		unsigned char buf[8];

		dtfl_put_u64(buf, (uint64_t)i);
		dtfl_session_give(s, buf, sizeof buf);
		dtfl_session_close(s);
	}
	(void)t;
	dtfl_cap_drop(cap);
	return false;
}

static void
build(dtfl_scope *scope, void *arg)
{
	send_ctx *c = arg;
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Source");
	dtfl_stream *out = dtfl_op_builder_output(b, dtfl_type_u64());

	c->w = dtfl_event_writer_new(c->fd, dtfl_scope_time_class(scope),
	    dtfl_type_u64());
	dtfl_op_builder_build(b, source_logic, NULL, NULL);
	dtfl_stream_capture_into(out, dtfl_event_writer_pusher(c->w),
	    DTFL_NOCTX);
}

static void
worker(dtfl_worker *w, void *arg)
{
	int idx = (int)dtfl_worker_index(w);
	struct sockaddr_in sa;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	send_ctx c;
	dtfl_time_class *tc;

	(void)arg;
	if (fd < 0) {
		perror("socket");
		return;
	}
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)(8000 + idx));
	inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
		perror("connect");
		close(fd);
		return;
	}
	c.fd = fd;
	c.w = NULL;
	tc = dtfl_time_u64();
	dtfl_worker_dataflow(w, "capture_send", tc, build, &c);
	while (dtfl_worker_step(w))
		;
	dtfl_event_writer_free(c.w);
	dtfl_time_class_free(tc);
	close(fd);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
