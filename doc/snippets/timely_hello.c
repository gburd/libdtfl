/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * doc/snippets/timely_hello.c: the manual's first timely example
 * (Timely Dataflow chapter).  Ports timely-dataflow
 * mdbook/src/chapter_0/chapter_0_0.md and chapter_2/chapter_2_5.md to C:
 * one worker streams 0..10 and inspects each record, printing "seen: N".
 */
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>

/* The source iterator: emit 0..10, one record per activation chunk. */
static bool
range_next(void *ctx, dtfl_emit *out)
{
	uint64_t *next = ctx;
	unsigned char buf[8];

	if (*next >= 10)
		return false;
	dtfl_put_u64(buf, *next);
	dtfl_emit_record(out, buf, sizeof buf);
	(*next)++;
	return true;
}

static void
inspect_seen(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)ctx;
	(void)time;
	(void)len;
	printf("seen: %llu\n", (unsigned long long)dtfl_get_u64(rec));
}

static void
build(dtfl_scope *scope, void *ctx)
{
	uint64_t *next = ctx;
	dtfl_stream *s = dtfl_to_stream(scope, dtfl_type_u64(), range_next,
	    DTFL_CTX(next, NULL));

	dtfl_stream_inspect(s, inspect_seen, DTFL_NOCTX);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t next = 0;

	(void)ctx;
	dtfl_worker_dataflow(w, "hello", tc, build, &next);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
}

int
main(void)
{
	return dtfl_execute_directly(worker, NULL);
}
