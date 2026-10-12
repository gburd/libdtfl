/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/exchange.c: mirrors timely/examples/exchange.rs.
 *
 * Arguments: batch rounds [timely flags].  Each worker sends `batch`
 * records per round for `rounds` rounds, exchanging them across workers
 * by value, with a probe tracking the input frontier.  Upstream prints a
 * timing/throughput line; this prints a deterministic completion line so
 * the output is checkable.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>

typedef struct run_ctx {
	size_t batch;
	size_t rounds;
} run_ctx;

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
} build_ctx;

static uint64_t
route_by_value(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);

	s = dtfl_stream_exchange(s, route_by_value, NULL);
	dtfl_stream_probe_with(s, c->probe);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	run_ctx *r = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c;
	uint64_t round;

	c.input = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "exchange", tc, build, &c);

	for (round = 0; round < r->rounds; round++) {
		size_t i;

		for (i = 0; i < r->batch; i++) {
			unsigned char buf[8];

			dtfl_put_u64(buf, i);
			dtfl_input_send(c.input, buf, sizeof buf);
		}
		dtfl_input_advance_to(c.input, &round);
		while (dtfl_probe_less_than(c.probe, dtfl_input_time(c.input)))
			dtfl_worker_step(w);
	}
	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	printf("worker %zu complete\n", dtfl_worker_index(w));
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	run_ctx r;
	dtfl_config *cfg;
	int rc;

	r.batch = ex_size(argc, argv, 1, 1000);
	r.rounds = ex_size(argc, argv, 2, 100);
	cfg = ex_config(argc, argv, 2);
	rc = dtfl_execute(cfg, worker, &r, NULL);
	dtfl_config_free(cfg);
	return rc;
}
