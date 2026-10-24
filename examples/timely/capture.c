/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/capture.c: an in-process capture-and-replay round trip,
 * covering the ground of timely/examples/capture_send.rs and
 * capture_recv.rs without their TCP transport.
 *
 * Upstream splits capture over a socket: capture_send captures a stream
 * into an EventWriter over TCP, and capture_recv replays it from an
 * EventReader.  The binary EventWriter/EventReader transport is not yet
 * built in this library (only the in-memory event link is), so this
 * single example captures 0..10 into an event link and replays it in a
 * second dataflow, printing "replayed: N" for each record, which is the
 * observable behaviour of capture_recv.
 */
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>

typedef struct src_ctx { uint64_t next; } src_ctx;

static bool
range_next(void *ctx, dtfl_emit *out)
{
	src_ctx *s = ctx;
	unsigned char buf[8];

	if (s->next >= 10)
		return false;
	dtfl_put_u64(buf, s->next++);
	dtfl_emit_record(out, buf, sizeof buf);
	return true;
}

typedef struct capture_build {
	src_ctx src;
	dtfl_event_pusher pusher;
} capture_build;

static void
build_capture(dtfl_scope *scope, void *arg)
{
	capture_build *c = arg;
	dtfl_stream *s = dtfl_to_stream(scope, dtfl_type_u64(), range_next,
	    DTFL_CTX(&c->src, NULL));

	dtfl_stream_capture_into(s, c->pusher, DTFL_NOCTX);
}

static void
inspect_replayed(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)ctx; (void)time; (void)len;
	printf("replayed: %llu\n", (unsigned long long)dtfl_get_u64(rec));
}

static void
build_replay(dtfl_scope *scope, void *arg)
{
	dtfl_event_link *link = arg;
	dtfl_stream *s = dtfl_event_link_replay(link, scope);

	dtfl_stream_inspect(s, inspect_replayed, DTFL_NOCTX);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_event_link *link = dtfl_event_link_new(tc, dtfl_type_u64());
	capture_build cb;

	(void)ctx;
	cb.src.next = 0;
	cb.pusher = dtfl_event_link_pusher(link);
	dtfl_worker_dataflow(w, "capture", tc, build_capture, &cb);
	dtfl_worker_dataflow(w, "replay", tc, build_replay, link);
	while (dtfl_worker_step(w))
		;
	dtfl_event_link_free(link);
	dtfl_time_class_free(tc);
}

int
main(void)
{
	return dtfl_execute_directly(worker, NULL);
}
