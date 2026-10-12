/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/hello.c: mirrors timely/examples/hello.rs.
 *
 * Each worker feeds rounds 0..10 (only worker 0 sends), exchanges records
 * across workers by value, and inspects the output, printing
 * "worker I:\thello N".  A probe tracks the input frontier so the worker
 * steps until each round is complete.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
	size_t index;
} build_ctx;

static uint64_t
route_by_value(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static void
inspect_hello(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	size_t index = *(size_t *)ctx;

	(void)time; (void)len;
	printf("worker %zu:\thello %llu\n", index,
	    (unsigned long long)dtfl_get_u64(rec));
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);

	s = dtfl_stream_exchange(s, route_by_value, NULL);
	s = dtfl_stream_inspect(s, inspect_hello, DTFL_CTX(&c->index, NULL));
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
	c.index = dtfl_worker_index(w);
	dtfl_worker_dataflow(w, "hello", tc, build, &c);

	for (round = 0; round < 10; round++) {
		uint64_t next = round + 1;

		if (c.index == 0) {
			unsigned char buf[8];

			dtfl_put_u64(buf, round);
			dtfl_input_send(c.input, buf, sizeof buf);
		}
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
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
