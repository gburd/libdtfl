/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_input.c: the input / probe frontier property.
 *
 * For a random monotonic sequence of epochs, each carrying a random
 * number of records, every record sent at epoch e comes out of the
 * dataflow at time e exactly once, and after advancing the input to the
 * next epoch and stepping, the probe's frontier reports that epoch:
 * probe.less_than(next) is false and probe.less_equal(prev) holds, so
 * the probe tracks exactly the frontier the handle advanced to
 * (input.rs advance_to feeds progress; probe.rs mirrors the frontier).
 */
#include "pbt.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"
#include "progress/time_int.h"

#define MAX_OUT 8192

typedef struct collected {
	uint64_t times[MAX_OUT];
	size_t n;
} collected;

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	collected *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t i, k = dtfl_message_len(m);
		const uint64_t *t = dtfl_message_time(m);

		for (i = 0; i < k && c->n < MAX_OUT; i++)
			c->times[c->n++] = t[0];
	}
	return false;
}

typedef struct scenario {
	dtfl_worker *w;
	dtfl_time_class *tc;
	dtfl_input *input;
	dtfl_probe *probe;
	collected *out;
} scenario;

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
	collected *out;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);
	dtfl_op_builder *b;

	s = dtfl_stream_probe_with(s, c->probe);
	b = dtfl_op_builder_new(scope, "Sink");
	dtfl_op_builder_input(b, s, dtfl_pact_pipeline());
	dtfl_op_builder_build(b, sink_logic, c->out, NULL);
}

static void
scenario_free(void *vp)
{
	scenario *s = vp;

	/* The input is closed by the property; free the rest. */
	dtfl_probe_free(s->probe);
	dtfl_time_class_free(s->tc);
	dtfl_worker_free(s->w);
	__dtfl_free(s->out);
	__dtfl_free(s);
}

static void
prop_times_and_frontier(pbt_tc *tc)
{
	scenario *s = __dtfl_calloc(1, sizeof *s);
	build_ctx bc;
	size_t rounds = (size_t)pbt_int(tc, 1, 16);
	uint64_t epoch = 0;
	size_t total = 0;
	size_t counts[32];
	uint64_t epochs[32];
	size_t r, i;

	pbt_defer(tc, scenario_free, s);
	s->out = __dtfl_calloc(1, sizeof(collected));
	s->tc = dtfl_time_u64();
	s->input = dtfl_input_new(dtfl_type_u64());
	s->probe = dtfl_probe_new();
	s->w = dtfl_worker_new_standalone();

	/* Draw a strictly increasing epoch and a record count per round. */
	for (r = 0; r < rounds; r++) {
		epoch += (uint64_t)pbt_int(tc, 1, 8);
		epochs[r] = epoch;
		counts[r] = (size_t)pbt_int(tc, 0, 16);
		total += counts[r];
	}

	bc.input = s->input;
	bc.probe = s->probe;
	bc.out = s->out;
	PBT_ASSERT(dtfl_worker_dataflow(s->w, "pbt", s->tc, build, &bc) == 0);

	for (r = 0; r < rounds; r++) {
		uint64_t cur = epochs[r];
		uint64_t next = (r + 1 < rounds) ? epochs[r + 1] : cur + 1;

		/* Open this epoch, then send its records at that time. */
		dtfl_input_advance_to(s->input, &cur);
		for (i = 0; i < counts[r]; i++) {
			unsigned char buf[8];

			dtfl_put_u64(buf, (uint64_t)i);
			dtfl_input_send(s->input, buf, sizeof buf);
		}
		/* The probe is at or before the epoch we are sending at. */
		PBT_ASSERT(dtfl_probe_less_equal(s->probe, &cur));
		dtfl_input_advance_to(s->input, &next);
		while (dtfl_probe_less_than(s->probe, &next))
			(void)dtfl_worker_step(s->w);
		/* The probe now reports exactly `next`: not strictly before it. */
		PBT_ASSERT(!dtfl_probe_less_than(s->probe, &next));
	}

	dtfl_input_close(s->input);
	while (dtfl_worker_step(s->w))
		;
	PBT_ASSERT(dtfl_probe_done(s->probe));

	/* Every record came out exactly once, at its epoch's time. */
	PBT_ASSERT(s->out->n == total);
	{
		size_t seen = 0;

		for (r = 0; r < rounds; r++) {
			size_t at_epoch = 0;

			for (i = 0; i < s->out->n; i++)
				if (s->out->times[i] == epochs[r])
					at_epoch++;
			PBT_ASSERT(at_epoch == counts[r]);
			seen += counts[r];
		}
		PBT_ASSERT(seen == total);
	}
}

static const pbt_prop props[] = {
	{ "times_and_frontier", prop_times_and_frontier, 0 },
};

PBT_MAIN("input", props)
