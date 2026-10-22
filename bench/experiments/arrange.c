/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   experiments/src/bin/arrange.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * bench/experiments/arrange.c: the arrange latency-throughput experiment.
 * Each worker loads its share of `recs` random (key, ()) records at time
 * 0 (LOADING rate), then, if `rate` > 0, applies insert/remove pairs in
 * one of two modes and prints a latency histogram:
 *
 *   closed loop  (default) waves of `rate` pairs per worker, each wave
 *                awaited before the next, for 25 s; THROUGHPUT is the
 *                pairs per second, and each wave's latency is recorded
 *                once more than 5 whole seconds have passed
 *   open loop    (an `open-loop` argument anywhere) requests on a fixed
 *                schedule of rate/2 pairs per second across all workers,
 *                independent of acknowledgement, until each worker has
 *                seen 10 * keys acknowledgements; latency is the
 *                acknowledgement time minus the scheduled time, recorded
 *                for the second half of the acknowledgements, so a stall
 *                is charged to every request it delays
 *
 * The computation under test is selected by `comp`:
 *
 *   exchange  exchange the update records by key, then probe
 *   arrange   arrange_by_self, then probe its batch stream
 *   count     arrange_by_self, count_total, then probe
 *   distinct  arrange_by_self, distinct_total, then probe
 *   (other)   just probe the raw collection
 *
 * Run as, with upstream's argument order:
 *
 *   bench/experiments/arrange keys recs rate work comp [timely args]
 *       [open-loop] [--seconds=N]
 *
 * `work` is parsed as upstream does (a non-number becomes SIZE_MAX) and
 * only echoed in the LATENCY lines; upstream does nothing else with it.
 * The timely arguments (-w, -n, -p, -h, ...) follow the positionals and
 * are parsed by dtfl_config_from_args and run by dtfl_execute, as
 * upstream's execute_from_args(args().skip(4)).  Output
 * lines match upstream's, Rust {:?} and {} float formatting included:
 *
 *   ARRANGE LOADING    peers recs_per_s
 *   ARRANGE THROUGHPUT peers updates_per_s            (closed loop)
 *   ARRANGE LATENCY    peers keys recs rate work Comp Mode ns fraction
 *
 * Each LATENCY line is the lower edge of a histogram bucket (64 powers
 * of two, 16 sub-buckets each) and the fraction of worker 0's samples
 * above it, in increasing order of latency.
 *
 * As upstream, each worker pins itself to the index-th CPU of the set the
 * process may run on (core_affinity::set_for_current(core_ids[index]));
 * this is Linux only and a no-op elsewhere.
 *
 * Differences from upstream, both deliberate:
 *
 *   - The generators are xorshift64* rather than rand 0.3's StdRng
 *     seeded with [1, 2, 3, index], and a key is drawn modulo `keys`
 *     rather than by gen_range.  As upstream, rng1 (additions) and rng2
 *     (deletions) of a worker are seeded alike, so each deletion removes
 *     the oldest live record and the collection stays a window of `recs`
 *     records, and different workers draw different keys.
 *   - --seconds=N is a port addition for the smoke test: it replaces
 *     the 25 s closed-loop window and scales the 5 s warm-up with it
 *     (samples count from 6/25 of the window, which is upstream's
 *     as_secs() > 5 at 25 s).  Upstream has no such knob; leave it out
 *     to run upstream's experiment.
 *
 * A benchmark, not a test: make check runs it only at tiny sizes for
 * about a second per mode (tests/dist/test_bench_arrange.sh), and
 * performance numbers come only from dedicated metal hosts.
 */
#if defined(__linux__)
#define _GNU_SOURCE	/* sched_getaffinity, CPU_SET */
#else
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */
#endif

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux__)
#include <sched.h>
#endif

enum comp { C_NOTHING, C_EXCHANGE, C_ARRANGE, C_COUNT, C_DISTINCT };

/* Comp's #[derive(Debug)] names, as the LATENCY lines print them. */
static const char *const comp_debug[] = {
	"Nothing", "Exchange", "Arrange", "Count", "Distinct"
};

/* Upstream's closed-loop window and warm-up, in seconds. */
#define CLOSED_LOOP_SECS 25
#define WARMUP_SECS 5

typedef struct run_cfg {
	uint64_t keys, recs, rate, work;
	enum comp comp;
	bool open_loop;
	uint64_t window_ns;	/* closed loop runs while elapsed < this */
	uint64_t warmup_ns;	/* and records once elapsed >= this */
} run_cfg;

typedef struct bench_ctx {
	dtfl_input_session *input;
	dtfl_probe *probe;
	enum comp comp;
} bench_ctx;

/* Comp::Exchange routes |&(x,_,_)| x.0 as u64: the key, first 8 bytes. */
static uint64_t
route_by_key(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static void
build(dtfl_scope *scope, void *arg)
{
	bench_ctx *c = arg;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *data = dtfl_input_session_to_collection(c->input,
	    scope);
	dtfl_collection *probed = NULL;

	switch (c->comp) {
	case C_EXCHANGE:
		/* data.inner.exchange(..).probe() */
		(void)dtfl_stream_probe_with(dtfl_stream_exchange(
		    dtfl_collection_inner(data), route_by_key, NULL), c->probe);
		break;
	case C_ARRANGE: {
		/* data.arrange_by_self().stream.probe() */
		dtfl_arranged *a = dtfl_collection_arrange(data, "ArrangeBySelf");

		(void)dtfl_stream_probe_with(dtfl_arranged_stream(a), c->probe);
		dtfl_arranged_free(a);
		break;
	}
	case C_COUNT: {
		/* data.arrange_by_self().count_total().probe() */
		dtfl_collection *ct = dtfl_collection_count_total(data, i64);

		probed = dtfl_collection_probe_with(ct, c->probe);
		dtfl_collection_free(ct);
		break;
	}
	case C_DISTINCT: {
		/* data.arrange_by_self().distinct_total().probe() */
		dtfl_collection *d = dtfl_collection_distinct_total(data, i64);

		probed = dtfl_collection_probe_with(d, c->probe);
		dtfl_collection_free(d);
		break;
	}
	case C_NOTHING:
	default:
		/* data.probe() */
		probed = dtfl_collection_probe_with(data, c->probe);
		break;
	}
	dtfl_collection_free(probed);
	dtfl_collection_free(data);
	dtfl_diff_free(i64);
}

/*
 * A deterministic xorshift64* generator in place of StdRng.  Worker
 * index i seeds RNG_SEED + 0x1000193 * i (worker 0 keeps the seed of the
 * earlier single-worker port), the counterpart of upstream's seed
 * [1, 2, 3, index]: alike for a worker's two generators, different
 * across workers.
 */
#define RNG_SEED 0x9e3779b97f4a7c15ULL

static uint64_t
next_key(uint64_t *state, uint64_t keys)
{
	*state ^= *state >> 12;
	*state ^= *state << 25;
	*state ^= *state >> 27;
	return (*state * 0x2545F4914F6CDD1DULL) % keys;
}

/* Instant::now() and elapsed(), in nanoseconds of CLOCK_MONOTONIC. */
static uint64_t
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

/* usize::next_power_of_two().trailing_zeros(): ceil(log2(x)), x >= 1. */
static unsigned
log2_ceil(uint64_t x)
{
	unsigned k = 0;

	while (k < 64 && (1ULL << k) < x)
		k++;
	return k;
}

/*
 * counts[count_index][low_bits] += 1 for a latency of `ns`.  Upstream
 * shifts by count_index - 5, which underflows below 32 ns, and indexes
 * past 63 above 2^63 ns; neither latency occurs, so they are dropped.
 */
static void
record_latency(uint64_t counts[64][16], uint64_t ns)
{
	unsigned ci = log2_ceil(ns);

	if (ci >= 5 && ci < 64)
		counts[ci][(ns >> (ci - 5)) & 0xF]++;
}

/*
 * Rust's float formatting: {:?} (debug) prints the shortest decimal that
 * round-trips, with at least one decimal place; {} (display) prints the
 * shortest round-trip decimal with none forced.  This takes the fewest
 * places whose correctly rounded value round-trips, which agrees with
 * Rust's shortest digits except, rarely, next to a power of two.
 * shortcut: {:?} switches to exponent form outside [1e-4, 1e16), which
 * no rate or fraction here reaches; add it if a printed value can.
 */
static void
fmt_f64(char *buf, size_t len, double v, bool debug)
{
	int p;

	for (p = debug ? 1 : 0; p < 400; p++) {
		snprintf(buf, len, "%.*f", p, v);
		if (strtod(buf, NULL) == v)
			break;
	}
}

/*
 * core_affinity::get_core_ids() then set_for_current(core_ids[index]):
 * the CPUs this thread may run on, in increasing order; pin to the
 * index-th.  Upstream panics when index is past the end; so does this.
 */
static void
pin_to_core(size_t index)
{
#if defined(__linux__)
	cpu_set_t full, one;
	size_t i, seen = 0;

	CPU_ZERO(&full);
	if (sched_getaffinity(0, sizeof full, &full) != 0) {
		fprintf(stderr, "arrange: sched_getaffinity: %s\n",
		    strerror(errno));
		abort();
	}
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (!CPU_ISSET(i, &full))
			continue;
		if (seen++ == index) {
			CPU_ZERO(&one);
			CPU_SET(i, &one);
			/* core_affinity ignores the result; so does this. */
			(void)sched_setaffinity(0, sizeof one, &one);
			return;
		}
	}
	fprintf(stderr, "arrange: worker %zu has no core: only %zu CPUs "
	    "are available\n", index, seen);
	abort();
#else
	(void)index;
#endif
}

/* Inserts one random addition and removes one random deletion. */
static void
update_pair(dtfl_input_session *in, uint64_t *rng1, uint64_t *rng2,
    uint64_t keys)
{
	unsigned char buf[8];

	dtfl_put_u64(buf, next_key(rng1, keys));
	dtfl_input_session_insert(in, buf, sizeof buf);
	dtfl_put_u64(buf, next_key(rng2, keys));
	dtfl_input_session_remove(in, buf, sizeof buf);
}

static void
closed_loop(const run_cfg *cfg, dtfl_worker *w, bench_ctx *c, uint64_t *rng1,
    uint64_t *rng2, uint64_t counts[64][16])
{
	uint64_t peers = dtfl_worker_peers(w), index = dtfl_worker_index(w);
	uint64_t rate = cfg->rate, timer = now_ns(), wave = 1, t;
	uint64_t elapsed = now_ns() - timer;
	double seconds;

	while (elapsed < cfg->window_ns) {
		uint64_t round, elapsed1;

		for (round = 0; round < rate; round++) {
			t = ((wave * rate) + round) * peers + index;
			dtfl_input_session_advance_to(c->input, &t);
			update_pair(c->input, rng1, rng2, cfg->keys);
		}
		wave += 1;
		t = wave * rate * peers;
		dtfl_input_session_advance_to(c->input, &t);
		dtfl_input_session_flush(c->input);

		elapsed1 = elapsed;
		while (dtfl_probe_less_than(c->probe,
		    dtfl_input_session_time(c->input)))
			dtfl_worker_step(w);
		elapsed = now_ns() - timer;
		if (elapsed >= cfg->warmup_ns)
			record_latency(counts, elapsed - elapsed1);
	}

	/* as_secs() as f64 + subsec_nanos() as f64 / 1e9 */
	elapsed = now_ns() - timer;
	seconds = (double)(elapsed / 1000000000ULL) +
	    (double)(elapsed % 1000000000ULL) / 1000000000.0;
	if (index == 0) {
		char f[64];

		fmt_f64(f, sizeof f,
		    2.0 * (double)((wave - 1) * rate * peers) / seconds, true);
		printf("ARRANGE\tTHROUGHPUT\t%llu\t%s\n",
		    (unsigned long long)peers, f);
	}
}

static void
open_loop(const run_cfg *cfg, dtfl_worker *w, bench_ctx *c, uint64_t *rng1,
    uint64_t *rng2, uint64_t counts[64][16])
{
	uint64_t peers = dtfl_worker_peers(w), index = dtfl_worker_index(w);
	uint64_t requests_per_sec = cfg->rate / 2;
	uint64_t ns_per_request = 1000000000ULL / requests_per_sec;
	uint64_t request_counter = peers + index;	/* skip first request */
	uint64_t ack_counter = peers + index;
	uint64_t inserted_ns = 1, ack_target = 10 * cfg->keys;
	uint64_t timer = now_ns();

	while (ack_counter < ack_target) {
		uint64_t elapsed_ns = now_ns() - timer;
		uint64_t acknowledged_ns, target_ns;
		size_t nf;
		const uint64_t *f = dtfl_probe_frontier(c->probe, &nf);

		/*
		 * probe.with_frontier(|f| f[0]): the input is open, so the
		 * frontier is not empty.
		 */
		acknowledged_ns = nf > 0 ? f[0] : UINT64_MAX;

		/* Record any completed, unrecorded measurements. */
		while (ack_counter * ns_per_request < acknowledged_ns &&
		    ack_counter < ack_target) {
			uint64_t requested_at = ack_counter * ns_per_request;

			if (ack_counter > ack_target / 2)
				record_latency(counts, elapsed_ns - requested_at);
			ack_counter += peers;
		}

		/*
		 * Introduce records up to the present only once the system
		 * has acknowledged everything inserted so far.
		 */
		target_ns = acknowledged_ns >= inserted_ns ? elapsed_ns :
		    inserted_ns;
		if (inserted_ns < target_ns) {
			while (request_counter * ns_per_request < target_ns) {
				uint64_t t = request_counter * ns_per_request;

				dtfl_input_session_advance_to(c->input, &t);
				update_pair(c->input, rng1, rng2, cfg->keys);
				request_counter += peers;
			}
			dtfl_input_session_advance_to(c->input, &target_ns);
			dtfl_input_session_flush(c->input);
			inserted_ns = target_ns;
		}

		dtfl_worker_step(w);
	}
}

/*
 * Prints (latency, fraction) for each bucket edge, walking down from the
 * largest bucket to index 10 and printing in increasing latency, as
 * upstream's results.drain(..).rev().
 */
static void
print_latencies(const run_cfg *cfg, uint64_t peers, uint64_t counts[64][16])
{
	struct {
		uint64_t latency;
		double fraction;
	} results[64 * 16];
	size_t n = 0;
	uint64_t total = 0, sum = 0;
	int index, sub;

	for (index = 0; index < 64; index++)
		for (sub = 0; sub < 16; sub++)
			total += counts[index][sub];
	for (index = 63; index >= 10; index--) {
		for (sub = 15; sub >= 0; sub--) {
			if (sum > 0 && sum < total) {
				results[n].latency = (1ULL << (index - 1)) +
				    ((uint64_t)sub << (index - 5));
				results[n].fraction = (double)sum / (double)total;
				n++;
			}
			sum += counts[index][sub];
		}
	}
	while (n-- > 0) {
		char f[512];

		fmt_f64(f, sizeof f, results[n].fraction, false);
		printf("ARRANGE\tLATENCY\t%llu\t%llu\t%llu\t%llu\t%llu\t%s\t%s"
		    "\t%llu\t%s\n", (unsigned long long)peers,
		    (unsigned long long)cfg->keys,
		    (unsigned long long)cfg->recs,
		    (unsigned long long)cfg->rate,
		    (unsigned long long)cfg->work, comp_debug[cfg->comp],
		    cfg->open_loop ? "OpenLoop" : "ClosedLoop",
		    (unsigned long long)results[n].latency, f);
	}
}

static void
worker(dtfl_worker *w, void *arg)
{
	const run_cfg *cfg = arg;
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *rec = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_unit());
	uint64_t index = dtfl_worker_index(w), peers = dtfl_worker_peers(w);
	uint64_t i, n, t, timer, rng1, rng2;
	bench_ctx c;

	pin_to_core((size_t)index);

	c.input = dtfl_input_session_new(rec, i64);
	c.probe = dtfl_probe_new();
	c.comp = cfg->comp;
	dtfl_worker_dataflow(w, "arrange", tc, build, &c);

	rng1 = rng2 = RNG_SEED + 0x1000193ULL * index;

	/* This worker's share of the recs initial records. */
	timer = now_ns();
	n = cfg->recs / peers + (index < cfg->recs % peers ? 1 : 0);
	for (i = 0; i < n; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, next_key(&rng1, cfg->keys));
		dtfl_input_session_insert(c.input, buf, sizeof buf);
	}
	t = 1;
	dtfl_input_session_advance_to(c.input, &t);
	dtfl_input_session_flush(c.input);
	while (dtfl_probe_less_than(c.probe, dtfl_input_session_time(c.input)))
		dtfl_worker_step(w);

	if (index == 0) {
		uint64_t elapsed1_ns = now_ns() - timer;
		char f[64];

		fmt_f64(f, sizeof f,
		    1000000000.0 * (double)cfg->recs / (double)elapsed1_ns, true);
		printf("ARRANGE\tLOADING\t%llu\t%s\n", (unsigned long long)peers,
		    f);
	}

	if (cfg->rate > 0) {
		/* Worker-local; only worker 0's are printed. */
		uint64_t counts[64][16];

		memset(counts, 0, sizeof counts);
		if (cfg->open_loop)
			open_loop(cfg, w, &c, &rng1, &rng2, counts);
		else
			closed_loop(cfg, w, &c, &rng1, &rng2, counts);
		if (index == 0)
			print_latencies(cfg, peers, counts);
	}

	/* Dropping the session flushes it; the dataflow then drains. */
	dtfl_input_session_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_type_free(rec);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

/* usize::from_str: optional '+', then decimal digits, no overflow. */
static bool
parse_usize(const char *s, uint64_t *out)
{
	uint64_t v = 0;

	if (*s == '+')
		s++;
	if (*s == '\0')
		return false;
	for (; *s != '\0'; s++) {
		uint64_t d = (uint64_t)(*s - '0');

		if (*s < '0' || *s > '9' || v > (UINT64_MAX - d) / 10)
			return false;
		v = v * 10 + d;
	}
	*out = v;
	return true;
}

static enum comp
parse_comp(const char *s)
{
	if (strcmp(s, "exchange") == 0)
		return C_EXCHANGE;
	if (strcmp(s, "arrange") == 0)
		return C_ARRANGE;
	if (strcmp(s, "count") == 0)
		return C_COUNT;
	if (strcmp(s, "distinct") == 0)
		return C_DISTINCT;
	return C_NOTHING;
}

static int
usage(void)
{
	fprintf(stderr, "usage: arrange keys recs rate work comp "
	    "[timely args] [open-loop] [--seconds=N]\n");
	return 2;
}

int
main(int argc, char **argv)
{
	static const char sec_opt[] = "--seconds=";
	run_cfg cfg;
	dtfl_config *tcfg;
	dtfl_guards *g = NULL;
	double seconds = CLOSED_LOOP_SECS;
	int a, after, j, nrest = 0, rc;

	/* Upstream unwraps args 1 to 5: a missing or bad one panics. */
	if (argc < 6 || !parse_usize(argv[1], &cfg.keys) ||
	    !parse_usize(argv[2], &cfg.recs) ||
	    !parse_usize(argv[3], &cfg.rate))
		return usage();
	if (!parse_usize(argv[4], &cfg.work))
		cfg.work = SIZE_MAX;	/* parse().unwrap_or(usize::MAX) */
	cfg.comp = parse_comp(argv[5]);

	/* args().any(|x| x == "open-loop") */
	cfg.open_loop = false;
	for (a = 0; a < argc; a++)
		if (strcmp(argv[a], "open-loop") == 0)
			cfg.open_loop = true;

	/* gen_range(0, 0) panics upstream, and so does 1e9 / (rate / 2). */
	if ((cfg.keys == 0 && (cfg.recs > 0 || cfg.rate > 0)) ||
	    (cfg.open_loop && cfg.rate == 1)) {
		fprintf(stderr, "arrange: keys must be > 0, and an open-loop "
		    "rate 0 or >= 2\n");
		return 2;
	}

	/* The port's --seconds=N, removed before timely sees the rest. */
	for (a = j = 6; a < argc; a++) {
		if (strncmp(argv[a], sec_opt, sizeof sec_opt - 1) == 0) {
			char *end;

			seconds = strtod(argv[a] + sizeof sec_opt - 1, &end);
			if (*end != '\0' || !(seconds > 0) || seconds > 1e9)
				return usage();
			continue;
		}
		argv[j++] = argv[a];
	}
	argc = j;
	cfg.window_ns = (uint64_t)(seconds * 1e9);
	cfg.warmup_ns = cfg.window_ns / CLOSED_LOOP_SECS * (WARMUP_SECS + 1);

	/*
	 * execute_from_args(args().skip(4)) hands getopts work, comp and the
	 * rest; dtfl_config_from_args skips its first argument as the program
	 * name, so it starts at rate.  The free arguments (work, comp,
	 * open-loop) pass through.  An option timely does not know fails, as
	 * in getopts, except after "--", whose arguments come last in rest.
	 */
	for (after = 0, a = 4; a < argc; a++)
		if (strcmp(argv[a], "--") == 0) {
			after = argc - a - 1;
			break;
		}
	tcfg = dtfl_config_new();
	if (dtfl_config_from_args(tcfg, argc - 3, argv + 3, argv + 3,
	    &nrest) != 0) {
		fprintf(stderr, "arrange: %s\n", dtfl_last_error_detail());
		dtfl_config_free(tcfg);
		return 2;
	}
	for (a = 1; a < nrest - after; a++)
		if (argv[3 + a][0] == '-' && argv[3 + a][1] != '\0') {
			fprintf(stderr, "arrange: Unrecognized option: '%s'\n",
			    argv[3 + a]);
			dtfl_config_free(tcfg);
			return 2;
		}

	rc = dtfl_execute(tcfg, worker, &cfg, &g);
	if (rc == 0)
		rc = dtfl_guards_join(g);
	else
		fprintf(stderr, "arrange: %s\n", dtfl_last_error_detail());
	dtfl_config_free(tcfg);
	return rc == 0 ? 0 : 1;
}
