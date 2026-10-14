/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * doc/snippets/timely_ops.c: the manual's stream-operator example
 * (Timely Dataflow chapter, Building Timely Dataflows).  Ports
 * timely-dataflow mdbook/src/chapter_2/chapter_2_3.md (map, flat_map,
 * filter) to C: stream 0..10, map x -> x+1, keep the even results, and
 * inspect.  One worker; deterministic output.
 */
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>

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

/* map x -> x + 1 (flat_map emitting exactly one record). */
static void
plus_one(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	unsigned char buf[8];

	(void)ctx;
	(void)time;
	(void)len;
	dtfl_put_u64(buf, dtfl_get_u64(rec) + 1);
	dtfl_emit_record(out, buf, sizeof buf);
}

static bool
is_even(void *ctx, const void *rec, size_t len)
{
	(void)ctx;
	(void)len;
	return dtfl_get_u64(rec) % 2 == 0;
}

static void
say(void *ctx, const uint64_t *time, const void *rec, size_t len)
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

	s = dtfl_stream_flat_map(s, dtfl_type_u64(), plus_one, DTFL_NOCTX);
	s = dtfl_stream_filter(s, is_even, DTFL_NOCTX);
	dtfl_stream_inspect(s, say, DTFL_NOCTX);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t next = 0;

	(void)ctx;
	dtfl_worker_dataflow(w, "ops", tc, build, &next);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
}

int
main(void)
{
	return dtfl_execute_directly(worker, NULL);
}
