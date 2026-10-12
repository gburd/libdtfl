/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/pingpong.c: mirrors timely/examples/pingpong.rs.
 *
 * Arguments: iterations elements [timely flags].  Each worker seeds the
 * elements it owns (x % peers == index) into a feedback loop: concat with
 * the cycle, exchange by value, increment each record, and recirculate
 * records whose time is still below `iterations` via branch_when.  The
 * loop terminates when every record's time reaches `iterations`.  No
 * output; the example exercises a bounded exchange feedback loop.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>

typedef struct run_ctx {
	uint64_t iterations;
	uint64_t elements;
} run_ctx;

typedef struct seed_ctx {
	uint64_t next;
	uint64_t elements;
	size_t peers;
	size_t index;
} seed_ctx;

/* The owned elements of 0..elements, those with x % peers == index. */
static bool
seed_next(void *ctx, dtfl_emit *out)
{
	seed_ctx *s = ctx;

	while (s->next < s->elements) {
		uint64_t x = s->next++;

		if ((x % s->peers) == s->index) {
			unsigned char buf[8];

			dtfl_put_u64(buf, x);
			dtfl_emit_record(out, buf, sizeof buf);
			return true;
		}
	}
	return false;
}

static uint64_t
route_by_value(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static void
plus_one(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)time; (void)len;
	dtfl_put_u64(buf, dtfl_get_u64(rec) + 1);
	dtfl_emit_record(out, buf, sizeof buf);
}

/* Recirculate while the loop coordinate is still below iterations. */
static bool
time_below_iters(void *ctx, const uint64_t *time)
{
	uint64_t iterations = *(uint64_t *)ctx;

	return time[0] < iterations;
}

static void
build(dtfl_scope *scope, void *arg)
{
	run_ctx *r = arg;
	seed_ctx *s = calloc(1, sizeof *s);
	uint64_t summary[DTFL_TIME_MAX];
	dtfl_feedback *handle;
	dtfl_stream *cycle, *seed, *merged, *yes, *no;

	s->next = 0;
	s->elements = r->elements;
	s->peers = dtfl_scope_peers(scope);
	s->index = dtfl_scope_index(scope);

	dtfl_summary_default(dtfl_scope_time_class(scope), summary);
	summary[0] = 1;
	cycle = dtfl_feedback_new(scope, dtfl_type_u64(), summary, &handle);
	seed = dtfl_to_stream(scope, dtfl_type_u64(), seed_next,
	    DTFL_CTX(s, free));
	merged = dtfl_stream_concat(seed, cycle);
	merged = dtfl_stream_exchange(merged, route_by_value, NULL);
	merged = dtfl_stream_flat_map(merged, dtfl_type_u64(), plus_one,
	    DTFL_NOCTX);
	dtfl_stream_branch_when(merged, time_below_iters, DTFL_CTX(&r->iterations,
	    NULL), &yes, &no);
	dtfl_feedback_connect(handle, yes);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	run_ctx *r = ctx;
	dtfl_time_class *tc = dtfl_time_u64();

	dtfl_worker_dataflow(w, "pingpong", tc, build, r);
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

	r.iterations = ex_u64(argc, argv, 1, 10);
	r.elements = ex_u64(argc, argv, 2, 10);
	cfg = ex_config(argc, argv, 3);
	rc = dtfl_execute(cfg, worker, &r, NULL);
	dtfl_config_free(cfg);
	return rc;
}
