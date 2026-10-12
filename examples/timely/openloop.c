/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/openloop.c: mirrors timely/examples/openloop.rs.
 *
 * Arguments: rate duration_s [timely flags].  An open-loop
 * latency-throughput probe: each worker introduces records at a target
 * rate into a dataflow that discards them (filter false) and observes the
 * output frontier to retire requests, recording a latency histogram.  The
 * loop ends when rate * duration_s requests have retired.  Worker 0
 * prints the latency distribution.  Keep rate and duration small; the run
 * is wall-clock bounded.
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <time.h>
#include <string.h>

typedef struct run_ctx {
	size_t rate;
	size_t duration_s;
} run_ctx;

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
} build_ctx;

static bool
drop_all(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)rec; (void)len;
	return false;
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);

	s = dtfl_stream_filter(s, drop_all, DTFL_NOCTX);
	dtfl_stream_probe_with(s, c->probe);
}

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The output frontier's first coordinate, or ~0 when empty. */
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
	dtfl_barrier *barrier;
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
	dtfl_worker_dataflow(w, "openloop", tc, build, &c);

	/* Re-synchronize all workers before timing. */
	barrier = dtfl_barrier_new(w);
	dtfl_barrier_wait(barrier);
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

		/* Geometrically increasing outstanding batches (technique 3). */
		{
			uint64_t delta = inserted_ns - ack;
			uint64_t scale = next_pow2(delta ? delta : 1);
			uint64_t target_ns;

			if (scale < 1024)
				scale = 1024;
			target_ns = elapsed_ns & ~(scale - 1);
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
	dtfl_barrier_free(barrier);	/* closes the barrier input before draining */
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
