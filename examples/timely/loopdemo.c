/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/loopdemo.c: mirrors timely/examples/loopdemo.rs.
 *
 * Arguments: rate duration_s [timely flags].  Like openloop, an
 * open-loop latency-throughput probe, but the dataflow is a Collatz
 * feedback loop: input concatenated with the loop stream, mapped by the
 * Collatz step (x even -> x/2, else 3x+1) and filtered to drop values at
 * or below one, with the surviving stream probed and fed back.  The loop
 * ends when rate * duration_s requests have retired; worker 0 prints the
 * latency distribution.  Keep rate and duration small.
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct run_ctx {
	size_t rate;
	size_t duration_s;
} run_ctx;

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
} build_ctx;

/* Collatz step: x even -> x / 2, else 3x + 1. */
static void
collatz(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	uint64_t x = dtfl_get_u64(rec);
	unsigned char buf[8];

	(void)ctx; (void)time; (void)len;
	dtfl_put_u64(buf, (x % 2 == 0) ? x / 2 : 3 * x + 1);
	dtfl_emit_record(out, buf, sizeof buf);
}

static bool
greater_than_one(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)len;
	return dtfl_get_u64(rec) > 1;
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	uint64_t summary[DTFL_TIME_MAX];
	dtfl_feedback *handle;
	dtfl_stream *in, *cycle, *step;

	in = dtfl_input_to_stream(c->input, scope);
	dtfl_summary_default(dtfl_scope_time_class(scope), summary);
	summary[0] = 1;
	cycle = dtfl_feedback_new(scope, dtfl_type_u64(), summary, &handle);
	step = dtfl_stream_concat(in, cycle);
	step = dtfl_stream_flat_map(step, dtfl_type_u64(), collatz, DTFL_NOCTX);
	step = dtfl_stream_filter(step, greater_than_one, DTFL_NOCTX);
	step = dtfl_stream_probe_with(step, c->probe);
	dtfl_feedback_connect(handle, step);
}

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t
acknowledged_ns(dtfl_probe *p)
{
	size_t n;
	const uint64_t *f = dtfl_probe_frontier(p, &n);

	return n > 0 ? f[0] : (uint64_t)-1;
}

static unsigned
trailing_zeros(uint64_t v)
{
	unsigned z = 0;

	if (v == 0)
		return 64;
	while ((v & 1) == 0) {
		z++;
		v >>= 1;
	}
	return z;
}

static uint64_t
next_pow2(uint64_t v)
{
	uint64_t p = 1;

	while (p < v)
		p <<= 1;
	return p;
}

static void
worker(dtfl_worker *w, void *ctx)
{
	run_ctx *r = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c;
	size_t index = dtfl_worker_index(w);
	size_t peers = dtfl_worker_peers(w);
	uint64_t ns_per_request = 1000000000ull / r->rate;
	size_t insert_counter = index, retire_counter = index;
	uint64_t inserted_ns = 0, start;
	size_t counter_limit = r->rate * r->duration_s;
	uint64_t counts[64][16];

	memset(counts, 0, sizeof counts);
	c.input = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "loopdemo", tc, build, &c);
	start = now_ns();

	while (retire_counter < counter_limit) {
		uint64_t elapsed_ns = now_ns() - start;
		uint64_t ack = acknowledged_ns(c.probe);

		while ((uint64_t)(retire_counter * ns_per_request) < ack &&
		    retire_counter < counter_limit) {
			uint64_t requested_at = retire_counter * ns_per_request;
			uint64_t latency_ns = elapsed_ns - requested_at;
			unsigned ci = trailing_zeros(next_pow2(latency_ns));
			unsigned low = (unsigned)((latency_ns >> (ci - 5)) & 0xF);

			counts[ci][low]++;
			retire_counter += peers;
		}

		{
			uint64_t delta = inserted_ns - ack;
			uint64_t scale = next_pow2(delta ? delta : 1);
			uint64_t target_ns = elapsed_ns & ~(scale - 1);

			if (inserted_ns < target_ns) {
				uint64_t t = target_ns;

				while ((uint64_t)(insert_counter * ns_per_request) <
				    target_ns) {
					unsigned char buf[8];

					dtfl_put_u64(buf, insert_counter);
					dtfl_input_send(c.input, buf, sizeof buf);
					insert_counter += peers;
				}
				dtfl_input_advance_to(c.input, &t);
				inserted_ns = target_ns;
			}
		}
		dtfl_worker_step(w);
	}

	if (index == 0) {
		uint64_t total = 0, sum = 0;
		size_t i, sub;

		for (i = 0; i < 64; i++)
			for (sub = 0; sub < 16; sub++)
				total += counts[i][sub];
		for (i = 64; i-- > 10;) {
			for (sub = 16; sub-- > 0;) {
				if (sum > 0 && sum < total) {
					uint64_t latency = (1ull << (i - 1)) +
					    ((uint64_t)sub << (i - 5));
					double fraction = (double)sum /
					    (double)total;

					printf("%llu\t%g\n",
					    (unsigned long long)latency, fraction);
				}
				sum += counts[i][sub];
			}
		}
	}

	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	run_ctx r;
	dtfl_config *cfg;
	int rc;

	r.rate = ex_size(argc, argv, 1, 10000);
	r.duration_s = ex_size(argc, argv, 2, 1);
	if (r.rate == 0)
		r.rate = 1;
	cfg = ex_config(argc, argv, 2);
	rc = dtfl_execute(cfg, worker, &r, NULL);
	dtfl_config_free(cfg);
	return rc;
}
