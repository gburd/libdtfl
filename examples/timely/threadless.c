/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/threadless.c: mirrors timely/examples/threadless.rs.
 *
 * A single standalone worker (Thread allocator) built and stepped by
 * hand.  It feeds 0..10 through an inspect that prints each value, with a
 * probe tracking the input frontier.
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

static void
inspect_val(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)ctx; (void)time; (void)len;
	printf("%llu\n", (unsigned long long)dtfl_get_u64(rec));
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);

	s = dtfl_stream_inspect(s, inspect_val, DTFL_NOCTX);
	dtfl_stream_probe_with(s, c->probe);
}

int
main(void)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c;
	uint64_t i;

	c.input = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "threadless", tc, build, &c);

	for (i = 0; i < 10; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, i);
		dtfl_input_send(c.input, buf, sizeof buf);
		dtfl_input_advance_to(c.input, &i);
		while (dtfl_probe_less_than(c.probe, dtfl_input_time(c.input)))
			dtfl_worker_step(w);
	}
	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
	return 0;
}
