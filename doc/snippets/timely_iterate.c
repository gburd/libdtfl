/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * doc/snippets/timely_iterate.c: the manual's iteration example (Timely
 * Dataflow chapter, Advanced Timely).  Ports timely-dataflow
 * mdbook/src/chapter_4/chapter_4_2.md (the Collatz feedback loop) to C:
 * feed the number 9, concatenate the loop stream, apply the Collatz step
 * (x even -> x/2, else 3x+1), inspect, drop values at or below one, and
 * feed the survivors back.  Nine takes 19 steps to reach one.  One
 * worker; deterministic output.
 */
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>

static void
collatz(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	uint64_t x = dtfl_get_u64(rec);
	unsigned char buf[8];

	(void)ctx;
	(void)time;
	(void)len;
	dtfl_put_u64(buf, (x % 2 == 0) ? x / 2 : 3 * x + 1);
	dtfl_emit_record(out, buf, sizeof buf);
}

static void
show(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)ctx;
	(void)time;
	(void)len;
	printf("%llu\n", (unsigned long long)dtfl_get_u64(rec));
}

static bool
greater_than_one(void *ctx, const void *rec, size_t len)
{
	(void)ctx;
	(void)len;
	return dtfl_get_u64(rec) > 1;
}

static bool
start_next(void *ctx, dtfl_emit *out)
{
	bool *done = ctx;
	unsigned char buf[8];

	if (*done)
		return false;
	dtfl_put_u64(buf, 9);
	dtfl_emit_record(out, buf, sizeof buf);
	*done = true;
	return true;
}

static void
build(dtfl_scope *scope, void *ctx)
{
	bool *done = ctx;
	uint64_t summary[DTFL_TIME_MAX];
	dtfl_feedback *handle;
	dtfl_stream *in, *cycle, *step;

	in = dtfl_to_stream(scope, dtfl_type_u64(), start_next,
	    DTFL_CTX(done, NULL));
	dtfl_summary_default(dtfl_scope_time_class(scope), summary);
	summary[0] = 1;			/* advance the loop coordinate by one */
	cycle = dtfl_feedback_new(scope, dtfl_type_u64(), summary, &handle);
	step = dtfl_stream_concat(in, cycle);
	step = dtfl_stream_flat_map(step, dtfl_type_u64(), collatz, DTFL_NOCTX);
	step = dtfl_stream_inspect(step, show, DTFL_NOCTX);
	step = dtfl_stream_filter(step, greater_than_one, DTFL_NOCTX);
	dtfl_feedback_connect(handle, step);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	bool done = false;

	(void)ctx;
	dtfl_worker_dataflow(w, "collatz", tc, build, &done);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
}

int
main(void)
{
	return dtfl_execute_directly(worker, NULL);
}
