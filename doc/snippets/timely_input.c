/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * doc/snippets/timely_input.c: the manual's input/exchange/inspect/probe
 * example (Timely Dataflow chapter, Core Concepts).  Ports
 * timely-dataflow mdbook/src/chapter_0/chapter_0_1.md and
 * chapter_1/chapter_1_3.md (examples/hello.rs) to C: feed rounds 0..10
 * into an input, exchange by value, inspect each, and use a probe to
 * keep the input and the dataflow in step.  Run on one worker so the
 * output is deterministic ("hello N").
 */
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
} build_ctx;

static uint64_t
route_by_value(const void *rec, size_t len, void *ctx)
{
	(void)len;
	(void)ctx;
	return dtfl_get_u64(rec);
}

static void
say_hello(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)ctx;
	(void)time;
	(void)len;
	printf("hello %llu\n", (unsigned long long)dtfl_get_u64(rec));
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);

	s = dtfl_stream_exchange(s, route_by_value, NULL);
	s = dtfl_stream_inspect(s, say_hello, DTFL_NOCTX);
	dtfl_stream_probe_with(s, c->probe);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c;
	uint64_t round;

	(void)ctx;
	c.input = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "hello", tc, build, &c);

	for (round = 0; round < 10; round++) {
		uint64_t next = round + 1;
		unsigned char buf[8];

		dtfl_put_u64(buf, round);
		dtfl_input_send(c.input, buf, sizeof buf);
		dtfl_input_advance_to(c.input, &next);
		while (dtfl_probe_less_than(c.probe, dtfl_input_time(c.input)))
			dtfl_worker_step(w);
	}
	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
}

int
main(void)
{
	return dtfl_execute_directly(worker, NULL);
}
