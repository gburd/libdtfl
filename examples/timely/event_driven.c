/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/event_driven.c: mirrors timely/examples/event_driven.rs.
 *
 * Arguments: dataflows length [record] [rounds].  Builds `dataflows`
 * dataflows, each a region of `length` identity maps over a unit stream
 * entered and left again, with a probe.  Each round advances one
 * dataflow's input (sending a unit when "record" is given) and steps
 * until its probe passes the round.  Upstream loops forever; this caps
 * the loop at `rounds` so it terminates.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>
#include <string.h>

typedef struct region_ctx {
	dtfl_stream *outer;	/* the entered stream, set by the parent */
	size_t length;
	dtfl_stream *result;	/* the left stream, read back by the parent */
} region_ctx;

static void
identity(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	(void)ctx; (void)time;
	dtfl_emit_record(out, rec, len);
}

static void
build_region(dtfl_scope *inner, void *arg)
{
	region_ctx *c = arg;
	dtfl_stream *s = dtfl_stream_enter(c->outer, inner);
	size_t step;

	for (step = 0; step < c->length; step++)
		s = dtfl_stream_flat_map(s, dtfl_type_unit(), identity,
		    DTFL_NOCTX);
	c->result = dtfl_stream_leave(s);
}

typedef struct df_build {
	dtfl_input *input;
	dtfl_probe *probe;
	size_t length;
} df_build;

static void
build_dataflow(dtfl_scope *scope, void *arg)
{
	df_build *c = arg;
	region_ctx rc;

	rc.outer = dtfl_input_to_stream(c->input, scope);
	rc.length = c->length;
	rc.result = NULL;
	dtfl_scope_region(scope, "region", build_region, &rc);
	dtfl_stream_probe_with(rc.result, c->probe);
}

typedef struct worker_ctx {
	size_t dataflows;
	size_t length;
	bool record;
	uint64_t rounds;
} worker_ctx;

static void
worker(dtfl_worker *w, void *ctx)
{
	worker_ctx *wc = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_input **inputs = calloc(wc->dataflows, sizeof *inputs);
	dtfl_probe **probes = calloc(wc->dataflows, sizeof *probes);
	size_t d;
	uint64_t round;

	for (d = 0; d < wc->dataflows; d++) {
		df_build c;

		inputs[d] = dtfl_input_new(dtfl_type_unit());
		probes[d] = dtfl_probe_new();
		c.input = inputs[d];
		c.probe = probes[d];
		c.length = wc->length;
		dtfl_worker_dataflow(w, "event_driven", tc, build_dataflow, &c);
	}

	for (round = 0; round < wc->rounds; round++) {
		size_t df = (size_t)(round % wc->dataflows);
		uint64_t at = round;

		if (wc->record)
			dtfl_input_send(inputs[df], NULL, 0);
		dtfl_input_advance_to(inputs[df], &at);
		while (dtfl_probe_less_than(probes[df], &at))
			dtfl_worker_step(w);
	}

	for (d = 0; d < wc->dataflows; d++) {
		dtfl_input_close(inputs[d]);
		dtfl_probe_free(probes[d]);
	}
	while (dtfl_worker_step(w))
		;
	free(inputs);
	free(probes);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	worker_ctx wc;
	dtfl_config *cfg;
	int rc;

	wc.dataflows = ex_size(argc, argv, 1, 4);
	wc.length = ex_size(argc, argv, 2, 4);
	wc.record = argc > 3 && strcmp(argv[3], "record") == 0;
	wc.rounds = ex_u64(argc, argv, 4, 16);
	if (wc.dataflows == 0)
		wc.dataflows = 1;
	cfg = ex_config(argc, argv, 4);
	rc = dtfl_execute(cfg, worker, &wc, NULL);
	dtfl_config_free(cfg);
	return rc;
}
