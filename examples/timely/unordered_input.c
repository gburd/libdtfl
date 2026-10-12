/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/unordered_input.c: mirrors
 * timely/examples/unordered_input.rs.
 *
 * A single Thread worker creates an unordered input and sends one record
 * per round under a capability it downgrades each round.  inspect_batch
 * prints each (time, records) batch as "[t] -> [v]".
 */
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>

typedef struct build_ctx {
	dtfl_unordered_input *handle;
	dtfl_cap *cap;
} build_ctx;

static void
inspect_batch(void *ctx, const uint64_t *time, const dtfl_message *msg)
{
	size_t i, n = dtfl_message_len(msg);

	(void)ctx;
	printf("[%llu] -> [", (unsigned long long)time[0]);
	for (i = 0; i < n; i++) {
		size_t len;
		const void *rec = dtfl_message_record(msg, i, &len);

		printf("%s%llu", i ? ", " : "",
		    (unsigned long long)dtfl_get_u64(rec));
	}
	printf("]\n");
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_unordered_input_new(scope, dtfl_type_u64(),
	    &c->handle, &c->cap);

	dtfl_stream_inspect_batch(s, inspect_batch, DTFL_NOCTX);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c;
	uint64_t round;

	(void)ctx;
	dtfl_worker_dataflow(w, "unordered", tc, build, &c);

	for (round = 0; round < 10; round++) {
		unsigned char buf[8];
		uint64_t next = round + 1;
		dtfl_cap *later;

		dtfl_put_u64(buf, round);
		dtfl_unordered_input_send(c.handle, c.cap, buf, sizeof buf);
		later = dtfl_cap_delayed(c.cap, &next);
		dtfl_cap_drop(c.cap);
		c.cap = later;
		dtfl_worker_step(w);
	}
	dtfl_cap_drop(c.cap);	/* seals the input */
	dtfl_unordered_input_free(c.handle);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
}

int
main(void)
{
	return dtfl_execute_directly(worker, NULL);
}
