/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/compact.c: mirrors differential-dataflow/examples/compact.rs.
 *
 * Maintains distinct() over an ever-changing integer collection: it inserts
 * value % large and value % small for value in 0..total in batches, then
 * removes them all again, which exercises trace compaction as the collection
 * grows and shrinks.  Upstream prints only timings; this port inspects the
 * distinct collection so the output is deterministic: the net count of
 * distinct records after loading (the number of distinct residues) and after
 * unloading (zero).  Arguments mirror upstream (large small batch total);
 * with none, small fixed sizes are used so the counts are exact.
 */
#include "example_args.h"

#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

/* Net count of live distinct records, maintained by the inspect sink. */
typedef struct counter { int64_t live; } counter;

static void
inspect_live(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	counter *c = ctx;
	int64_t d;

	(void)rec; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	c->live += d;
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	counter *out;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *dist = dtfl_collection_distinct(base, i64);
	dtfl_collection *ins = dtfl_collection_inspect(dist, inspect_live,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_diff_free(i64);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(dist);
	dtfl_collection_free(base);
}

static void
run_to(dtfl_worker *w, dtfl_input_session *in, dtfl_probe *p, uint64_t t)
{
	dtfl_input_session_advance_to(in, &t);
	dtfl_input_session_flush(in);
	while (dtfl_probe_less_than(p, &t))
		dtfl_worker_step(w);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	const int *argc_argv = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	build_ctx c;
	counter out = { 0 };
	uint64_t large, small, total, v, batch, next, distinct_residues;

	(void)argc_argv;
	/* Small fixed sizes keep the counts exact and the run brief. */
	large = 100;
	small = 10;
	batch = 50;
	total = 300;
	c.in = dtfl_input_session_new(dtfl_type_u64(), i64);
	c.probe = dtfl_probe_new();
	c.out = &out;
	dtfl_worker_dataflow(w, "compact", tc, build, &c);

	/* Load: insert value % large and value % small for value in 0..total. */
	next = batch;
	for (v = 0; v < total; v++) {
		unsigned char buf[8];

		if (v >= next) {
			run_to(w, c.in, c.probe, next);
			next += batch;
		}
		dtfl_put_u64(buf, v % large);
		dtfl_input_session_insert(c.in, buf, sizeof buf);
		dtfl_put_u64(buf, v % small);
		dtfl_input_session_insert(c.in, buf, sizeof buf);
	}
	run_to(w, c.in, c.probe, total);

	/* The distinct residues are 0..large (since total >= large here). */
	distinct_residues = total < large ? total : large;
	printf("loaded distinct: %lld (expected %llu)\n", (long long)out.live,
	    (unsigned long long)distinct_residues);

	/* Unload: remove the same records again. */
	next = batch;
	for (v = 0; v < total; v++) {
		unsigned char buf[8];

		if (v >= next) {
			run_to(w, c.in, c.probe, total + next);
			next += batch;
		}
		dtfl_put_u64(buf, v % large);
		dtfl_input_session_remove(c.in, buf, sizeof buf);
		dtfl_put_u64(buf, v % small);
		dtfl_input_session_remove(c.in, buf, sizeof buf);
	}
	run_to(w, c.in, c.probe, total + total);

	printf("unloaded distinct: %lld (expected 0)\n", (long long)out.live);

	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc;

	/* The residue counting assumes a single worker over the fixed input. */
	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
