/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/tests/barrier.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * tests/upstream/timely/barrier.c: the ported barrier.rs integration test.
 *
 * A feedback loop drives a unary_notify "Barrier" operator seeded at
 * time 0.  On each notification the operator requests the next time
 * (time + 1) while that time is below 100, so the loop runs 100 rounds
 * and then terminates.  The test asserts the property barrier.rs checks:
 * each round of execution is notified of at most one time, i.e. the
 * notificator yields at most one capability per logic activation.  It
 * runs on one, two and three workers (Thread and Process(2)/Process(3)
 * upstream; DTFL_COMM_THREAD and DTFL_COMM_PROCESS here).
 */
#include "dtfl_test.h"

#include "dtfl/config.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdatomic.h>

/* Shared across workers: the error flag is set if any round saw > 1. */
typedef struct run_ctx {
	atomic_int too_many;
} run_ctx;

/*
 * On each activation, drain the notificator.  Upstream asserts count <= 1
 * per activation; mirror that by counting the delivered capabilities and
 * flagging the shared context if more than one arrive at once.
 */
static void
barrier_logic(void *ctx, dtfl_op *op, dtfl_notificator *note)
{
	run_ctx *r = ctx;
	dtfl_cap *cap;
	uint64_t folded;
	int count = 0;

	(void)op;
	while (dtfl_notificator_next(note, &cap, &folded)) {
		uint64_t next = dtfl_cap_time(cap)[0] + 1;

		count++;
		if (next < 100)
			dtfl_notificator_notify_at(note,
			    dtfl_cap_delayed(cap, &next));
		dtfl_cap_drop(cap);
	}
	if (count > 1)
		atomic_store_explicit(&r->too_many, 1, memory_order_relaxed);
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
	    &zero, 1, dtfl_type_unit(), barrier_logic, DTFL_CTX(r, NULL));
	dtfl_feedback_connect(handle, out);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();

	dtfl_worker_dataflow(w, "barrier", tc, build, ctx);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
}

static MunitResult
run_workers(size_t workers)
{
	run_ctx r;
	dtfl_config *cfg = dtfl_config_new();
	int rc;

	atomic_init(&r.too_many, 0);
	if (workers <= 1) {
		dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	} else {
		dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS);
		dtfl_config_set_threads(cfg, workers);
	}
	rc = dtfl_execute(cfg, worker, &r, NULL);
	dtfl_config_free(cfg);
	assert_int(rc, ==, 0);
	assert_int(atomic_load(&r.too_many), ==, 0);
	return MUNIT_OK;
}

DTFL_TEST(barrier__barrier_sync_1w)
{
	(void)params; (void)data;
	return run_workers(1);
}

DTFL_TEST(barrier__barrier_sync_2w)
{
	(void)params; (void)data;
	return run_workers(2);
}

DTFL_TEST(barrier__barrier_sync_3w)
{
	(void)params; (void)data;
	return run_workers(3);
}

static MunitTest tests[] = {
	DTFL_T(barrier__barrier_sync_1w),
	DTFL_T(barrier__barrier_sync_2w),
	DTFL_T(barrier__barrier_sync_3w),
	DTFL_T_END
};

DTFL_TEST_MAIN("timely/barrier", tests)
