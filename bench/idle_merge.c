/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * bench/idle_merge.c: the longest single step of an idle arrangement,
 * with and without idle merge effort (PLAN P16.6).
 *
 * Loads `keys` distinct u64 keys into one arrangement over `epochs`
 * epochs, so the trace is a spine of several batches, then stops feeding
 * and steps the idle worker `steps` times, timing each step.  With
 * effort 0 (differential's default, Config::idle_merge_effort None) an
 * idle trace does no merge work; with effort E each step applies about E
 * updates of merge work until the trace is one batch.  The interesting
 * number is the longest step: idle merging must not stall the worker.
 *
 *   bench/idle_merge keys epochs effort steps
 *
 * Prints one line: keys epochs effort steps load_s batches_before
 * batches_after max_step_us p99_step_us total_idle_ms.
 *
 * Not a port: upstream has no such benchmark.  Numbers come only from a
 * dedicated metal host.
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/config.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct run {
	uint64_t keys, epochs, steps;
	int64_t effort;
	dtfl_input_session *in;
	dtfl_probe *probe;
	dtfl_trace *watch;
} run;

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static size_t
batches(const dtfl_trace *t)
{
	dtfl_trace_stats st;

	memset(&st, 0, sizeof st);
	st.size = sizeof st;
	if (dtfl_trace_stats_get(t, &st) != DTFL_OK)
		return 0;
	return st.batches;
}

static void
build(dtfl_scope *scope, void *arg)
{
	run *r = arg;
	dtfl_collection *c = dtfl_input_session_to_collection(r->in, scope);
	dtfl_arranged *a = dtfl_collection_arrange(c, "Idle");

	(void)dtfl_stream_probe_with(dtfl_arranged_stream(a), r->probe);
	r->watch = dtfl_arranged_trace(a);
	/* A monitoring handle must not hold the trace back. */
	dtfl_trace_set_logical_compaction(r->watch, NULL, 0);
	dtfl_trace_set_physical_compaction(r->watch, NULL, 0);
	dtfl_arranged_free(a);
	dtfl_collection_free(c);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static void
worker(dtfl_worker *w, void *ctx)
{
	run *r = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t *step_ns = malloc(r->steps * sizeof *step_ns);
	uint64_t k, e, t, t0, total = 0;
	size_t before, i;
	double load_s;

	dtfl_worker_dataflow(w, "idle_merge", tc, build, r);
	t0 = now_ns();
	for (e = 0, k = 0; e < r->epochs; e++) {
		uint64_t end = (e + 1) * r->keys / r->epochs;

		for (; k < end; k++) {
			unsigned char b[8];

			dtfl_put_u64(b, k * 0x9e3779b97f4a7c15ULL);
			dtfl_input_session_insert(r->in, b, sizeof b);
		}
		t = e + 1;
		dtfl_input_session_advance_to(r->in, &t);
		dtfl_input_session_flush(r->in);
		while (dtfl_probe_less_than(r->probe, &t))
			dtfl_worker_step(w);
	}
	load_s = (double)(now_ns() - t0) / 1e9;
	before = batches(r->watch);
	for (i = 0; i < r->steps; i++) {
		uint64_t s = now_ns();

		dtfl_worker_step(w);
		step_ns[i] = now_ns() - s;
		total += step_ns[i];
	}
	printf("%llu %llu %lld %llu %.2f %zu %zu", (unsigned long long)r->keys,
	    (unsigned long long)r->epochs, (long long)r->effort,
	    (unsigned long long)r->steps, load_s, before, batches(r->watch));
	qsort(step_ns, r->steps, sizeof *step_ns, cmp_u64);
	printf(" %.1f %.1f %.1f\n", step_ns[r->steps - 1] / 1e3,
	    step_ns[r->steps * 99 / 100] / 1e3, total / 1e6);
	free(step_ns);
	dtfl_trace_free(r->watch);
	dtfl_input_session_close(r->in);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	run r;
	dtfl_config *cfg;
	dtfl_diff *i64 = dtfl_diff_i64();
	int rc;

	if (argc != 5) {
		fprintf(stderr, "usage: %s keys epochs effort steps\n", argv[0]);
		return 2;
	}
	memset(&r, 0, sizeof r);
	r.keys = strtoull(argv[1], NULL, 10);
	r.epochs = strtoull(argv[2], NULL, 10);
	r.effort = strtoll(argv[3], NULL, 10);
	r.steps = strtoull(argv[4], NULL, 10);
	if (r.keys == 0 || r.epochs == 0 || r.steps == 0 || r.effort < 0) {
		fprintf(stderr, "%s: keys, epochs and steps must be positive\n",
		    argv[0]);
		return 2;
	}
	r.in = dtfl_input_session_new(dtfl_type_u64(), i64);
	r.probe = dtfl_probe_new();
	cfg = dtfl_config_new();
	if (dtfl_config_set_idle_merge_effort(cfg, r.effort) != DTFL_OK) {
		fprintf(stderr, "%s: bad effort\n", argv[0]);
		return 2;
	}
	rc = dtfl_execute(cfg, worker, &r, NULL);
	dtfl_config_free(cfg);
	dtfl_probe_free(r.probe);
	dtfl_diff_free(i64);
	return rc;
}
