/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/barrier.c: mirrors timely/examples/barrier.rs.
 *
 * Argument: iterations [timely flags].  A feedback loop drives a
 * unary_notify "Barrier" operator seeded at time 0; on each notification
 * it requests the next time until the time reaches `iterations`, so the
 * loop runs exactly `iterations` rounds and terminates.  No output.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

typedef struct run_ctx { uint64_t iterations; } run_ctx;

/* On each notification, request the next time while below iterations. */
static void
barrier_logic(void *ctx, dtfl_op *op, dtfl_notificator *note)
{
	uint64_t iterations = *(uint64_t *)ctx;
	dtfl_cap *cap;
	uint64_t count;

	(void)op;
	while (dtfl_notificator_next(note, &cap, &count)) {
		uint64_t next = dtfl_cap_time(cap)[0] + 1;

		if (next < iterations)
			dtfl_notificator_notify_at(note,
			    dtfl_cap_delayed(cap, &next));
		dtfl_cap_drop(cap);
	}
}

static void
build(dtfl_scope *scope, void *arg)
{
	run_ctx *r = arg;
	uint64_t summary[DTFL_TIME_MAX];
	uint64_t zero = 0;
	dtfl_feedback *handle;
	dtfl_stream *cycle, *out;

	dtfl_summary_default(dtfl_scope_time_class(scope), summary);
	summary[0] = 1;
	cycle = dtfl_feedback_new(scope, dtfl_type_unit(), summary, &handle);
	out = dtfl_stream_unary_notify(cycle, dtfl_pact_pipeline(), "Barrier",
	    &zero, 1, dtfl_type_unit(), barrier_logic,
	    DTFL_CTX(&r->iterations, NULL));
	dtfl_feedback_connect(handle, out);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	run_ctx *r = ctx;
	dtfl_time_class *tc = dtfl_time_u64();

	dtfl_worker_dataflow(w, "barrier", tc, build, r);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	run_ctx r;
	dtfl_config *cfg;
	int rc;

	r.iterations = ex_u64(argc, argv, 1, 1000000);
	cfg = ex_config(argc, argv, 1);
	rc = dtfl_execute(cfg, worker, &r, NULL);
	dtfl_config_free(cfg);
	return rc;
}
