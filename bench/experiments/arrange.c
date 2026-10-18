/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   experiments/src/bin/arrange.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * bench/experiments/arrange.c: the arrange throughput experiment.  It
 * builds a degree-counting-style dataflow over (key, ()) records and
 * drives it in a closed loop: load `recs` records at random keys (timing
 * the load rate), then apply `rate` insert/remove pairs per round for a
 * bounded wall-clock window, timing the sustained update throughput.
 * The computation under test is selected by `comp`:
 *
 *   nothing   just probe the raw collection
 *   arrange   arrange the collection, then probe
 *   count     count_total, then probe
 *   distinct  distinct_total, then probe
 *
 * (Upstream also has an exchange arm; the port exchanges inside arrange,
 * so arrange is the lowest arm that exercises the shuffle.)
 *
 * A benchmark, not a test: not run by make check.  It uses the public
 * API and a deterministic xorshift RNG (upstream uses a seeded StdRng),
 * so a run is reproducible given the arguments.  Upstream also has an
 * open-loop mode and runs for 25 seconds; this port runs a bounded
 * window (default 5 s, override with the 5th positional) so a run on a
 * dedicated host finishes predictably.
 *
 * Run as: bench/experiments/arrange [keys [recs [rate [comp [seconds]]]]]
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum comp { C_NOTHING, C_ARRANGE, C_COUNT, C_DISTINCT };

typedef struct bench_ctx {
	dtfl_input_session *input;
	dtfl_probe *probe;
	enum comp comp;
} bench_ctx;

/* arranged -> collection: re-emit each (key, ()) pair as the key record. */
static void
arr_kv(void *ctx, const void *key, size_t klen, const void *val, size_t vlen,
    dtfl_arranged_emit *out)
{
	(void)ctx; (void)val; (void)vlen;
	dtfl_arranged_emit_record(out, key, klen);
}

static void
build(dtfl_scope *scope, void *arg)
{
	bench_ctx *c = arg;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *data = dtfl_input_session_to_collection(c->input,
	    scope);
	dtfl_collection *probed;

	switch (c->comp) {
	case C_ARRANGE: {
		dtfl_arranged *a = dtfl_collection_arrange(data, "arrange");
		dtfl_collection *ac = dtfl_arranged_as_collection(a,
		    dtfl_type_u64(), arr_kv, DTFL_NOCTX);

		probed = dtfl_collection_probe_with(ac, c->probe);
		dtfl_collection_free(ac);
		dtfl_arranged_free(a);
		break;
	}
	case C_COUNT: {
		dtfl_collection *ct = dtfl_collection_count_total(data, i64);

		probed = dtfl_collection_probe_with(ct, c->probe);
		dtfl_collection_free(ct);
		break;
	}
	case C_DISTINCT: {
		dtfl_collection *d = dtfl_collection_distinct_total(data, i64);

		probed = dtfl_collection_probe_with(d, c->probe);
		dtfl_collection_free(d);
		break;
	}
	case C_NOTHING:
	default:
		probed = dtfl_collection_probe_with(data, c->probe);
		break;
	}
	dtfl_collection_free(probed);
	dtfl_collection_free(data);
	dtfl_diff_free(i64);
}

/* A deterministic xorshift64* RNG (upstream seeds a StdRng per worker). */
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t
next_key(uint64_t keys)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (rng_state * 0x2545F4914F6CDD1DULL) % keys;
}

static double
secs_since(struct timespec t0)
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
}

typedef struct run_cfg {
	uint64_t keys, recs, rate;
	enum comp comp;
	double seconds;
} run_cfg;

static void
worker(dtfl_worker *w, void *arg)
{
	run_cfg *cfg = arg;
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *rec = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_unit());
	bench_ctx c;
	struct timespec t0;
	uint64_t i, t, wave;
	double load_s, run_s;

	c.input = dtfl_input_session_new(rec, i64);
	c.probe = dtfl_probe_new();
	c.comp = cfg->comp;
	dtfl_worker_dataflow(w, "arrange", tc, build, &c);

	/* Load phase: insert recs records at random keys, time the load. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < cfg->recs; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, next_key(cfg->keys));
		dtfl_input_session_insert(c.input, buf, sizeof buf);
	}
	t = 1;
	dtfl_input_session_advance_to(c.input, &t);
	dtfl_input_session_flush(c.input);
	while (dtfl_probe_less_than(c.probe, dtfl_input_session_time(c.input)))
		dtfl_worker_step(w);
	load_s = secs_since(t0);
	printf("ARRANGE\tLOADING\t1\t%.1f\n", (double)cfg->recs / load_s);

	/* Closed-loop phase: rate insert/remove pairs per round, bounded. */
	if (cfg->rate > 0) {
		clock_gettime(CLOCK_MONOTONIC, &t0);
		wave = 1;
		while (secs_since(t0) < cfg->seconds) {
			uint64_t round;

			for (round = 0; round < cfg->rate; round++) {
				unsigned char buf[8];

				t = wave * cfg->rate + round;
				dtfl_input_session_advance_to(c.input, &t);
				dtfl_put_u64(buf, next_key(cfg->keys));
				dtfl_input_session_insert(c.input, buf,
				    sizeof buf);
				dtfl_put_u64(buf, next_key(cfg->keys));
				dtfl_input_session_remove(c.input, buf,
				    sizeof buf);
			}
			wave++;
			t = wave * cfg->rate;
			dtfl_input_session_advance_to(c.input, &t);
			dtfl_input_session_flush(c.input);
			while (dtfl_probe_less_than(c.probe,
			    dtfl_input_session_time(c.input)))
				dtfl_worker_step(w);
		}
		run_s = secs_since(t0);
		printf("ARRANGE\tTHROUGHPUT\t1\t%.1f\n",
		    2.0 * (double)((wave - 1) * cfg->rate) / run_s);
	}

	dtfl_input_session_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_type_free(rec);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

static enum comp
parse_comp(const char *s)
{
	if (strcmp(s, "arrange") == 0)
		return C_ARRANGE;
	if (strcmp(s, "count") == 0)
		return C_COUNT;
	if (strcmp(s, "distinct") == 0)
		return C_DISTINCT;
	return C_NOTHING;
}

int
main(int argc, char **argv)
{
	run_cfg cfg;

	cfg.keys = argc > 1 ? strtoull(argv[1], NULL, 10) : 1000000;
	cfg.recs = argc > 2 ? strtoull(argv[2], NULL, 10) : 1000000;
	cfg.rate = argc > 3 ? strtoull(argv[3], NULL, 10) : 1000;
	cfg.comp = argc > 4 ? parse_comp(argv[4]) : C_ARRANGE;
	cfg.seconds = argc > 5 ? strtod(argv[5], NULL) : 5.0;
	return dtfl_execute_directly(worker, &cfg);
}
