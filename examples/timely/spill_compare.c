/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/examples/spill_compare.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * examples/timely/spill_compare.c: pushes a configurable volume of byte
 * chunks through an Exchange, optionally with the spilling MergeQueue
 * policy installed.  Compare peak RSS and wall-clock time:
 *
 *   # spill enabled (file-backed): bounded RSS, disk traffic to --spill-dir
 *   spill_compare --with-spill --total-mb 2048 -w 2
 *
 *   # spill disabled: the queues hold everything until it is drained
 *   spill_compare --total-mb 2048 -w 2
 *
 * Spill engages on the zero-copy MergeQueue path, so this example always
 * uses DTFL_COMM_PROCESS_BINARY.  Each worker injects total / workers of
 * data and routes every record to peer (index + 1) % workers, forcing
 * all bytes across a MergeQueue.
 *
 * Options (upstream's, plus --total-mb; the defaults are small so that
 * the smoke test can run it, upstream's are 50 GB and 256 MB):
 *   --total-gb N         Total cluster-wide data in GB
 *   --total-mb N         Total cluster-wide data in MB (default: 64)
 *   --chunk-kb N         Size of each record payload (default: 256)
 *   --workers N, -w N    Number of worker threads (default: 2)
 *   --threshold-mb N     Spill threshold (default: 4)
 *   --head-reserve-mb N  Head reserve / prefetch budget (default: 2)
 *   --spill-dir PATH     Directory for temporary files (default: $TMPDIR,
 *                        else /tmp; std::env::temp_dir upstream)
 *   --with-spill         Install the file-backed spill policy
 *   --rss-every-secs N   RSS sampling cadence (default: 2)
 *
 * Upstream prints progress every 5 s and the production and drain times;
 * this port prints those to stderr, with the RSS samples, so stdout
 * carries only lines that are the same on every run (the configuration
 * and each worker's received count and checksum), and an "rss_peak" and
 * "elapsed" line the smoke test filters out.  RSS is the resident set
 * from /proc/self/statm where it exists, else getrusage's peak, else
 * "unavailable"; the sampler is a plain thread (upstream shells out to
 * ps).
 */
#define _DEFAULT_SOURCE			/* getrusage */
#define _POSIX_C_SOURCE 200809L		/* clock_gettime, nanosleep */

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

typedef struct run_ctx {
	size_t chunk_bytes;
	size_t chunks_per_worker;
	atomic_bool stop;
	atomic_uint_fast64_t rss_peak_kb;
	unsigned rss_every_secs;
	struct timespec start;
} run_ctx;

static double
secs_since(const struct timespec *t0)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)(t.tv_sec - t0->tv_sec) +
	    (double)(t.tv_nsec - t0->tv_nsec) / 1e9;
}

/* parse_arg: the value after `flag`, or def when absent or unparsable. */
static size_t
parse_arg(int argc, char **argv, const char *flag, size_t def)
{
	char *end;
	unsigned long long v;
	int i;

	for (i = 1; i + 1 < argc; i++) {
		if (strcmp(argv[i], flag) != 0)
			continue;
		errno = 0;
		v = strtoull(argv[i + 1], &end, 10);
		if (errno != 0 || end == argv[i + 1] || *end != '\0')
			return def;
		return (size_t)v;
	}
	return def;
}

static const char *
parse_str(int argc, char **argv, const char *flag, const char *def)
{
	int i;

	for (i = 1; i + 1 < argc; i++)
		if (strcmp(argv[i], flag) == 0)
			return argv[i + 1];
	return def;
}

static int
has_flag(int argc, char **argv, const char *flag)
{
	int i;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], flag) == 0)
			return 1;
	return 0;
}

/* getrusage's peak resident set in KB, or 0 if unknown. */
static uint64_t
peak_rss_kb(void)
{
	struct rusage ru;

	if (getrusage(RUSAGE_SELF, &ru) != 0 || ru.ru_maxrss <= 0)
		return 0;
#ifdef __APPLE__
	return (uint64_t)ru.ru_maxrss / 1024;	/* bytes on macOS */
#else
	return (uint64_t)ru.ru_maxrss;
#endif
}

/* Resident set in KB, or 0 if unknown. */
static uint64_t
rss_kb(void)
{
	FILE *f = fopen("/proc/self/statm", "r");
	unsigned long long size, resident;

	if (f != NULL) {
		int n = fscanf(f, "%llu %llu", &size, &resident);

		fclose(f);
		if (n == 2)
			return (uint64_t)resident *
			    (uint64_t)sysconf(_SC_PAGESIZE) / 1024;
	}
	/* The peak, not the current size, but bounded vs unbounded shows. */
	return peak_rss_kb();
}

static void
print_rss(run_ctx *r, const char *label)
{
	uint64_t kb = rss_kb();
	uint64_t peak = atomic_load(&r->rss_peak_kb);

	while (kb > peak && !atomic_compare_exchange_weak(&r->rss_peak_kb,
	    &peak, kb))
		;
	if (kb > 0)
		fprintf(stderr, "[t=%6.1fs  RSS %8llu KB / %6llu MB]  %s\n",
		    secs_since(&r->start), (unsigned long long)kb,
		    (unsigned long long)(kb / 1024), label);
	else
		fprintf(stderr, "[t=%6.1fs  RSS unavailable]  %s\n",
		    secs_since(&r->start), label);
}

/* The RSS sampler, stopped when the workers finish. */
static void *
sampler(void *arg)
{
	run_ctx *r = arg;
	unsigned ms = 0;

	print_rss(r, "start");
	while (!atomic_load(&r->stop)) {
		/* Poll the stop flag every 50 ms between samples. */
		struct timespec nap = { 0, 50 * 1000000L };

		nanosleep(&nap, NULL);
		ms += 50;
		if (ms >= r->rss_every_secs * 1000u) {
			ms = 0;
			print_rss(r, "running");
		}
	}
	print_rss(r, "done");
	return NULL;
}

/* ------------------------------------------------------------------ */
/* The dataflow                                                        */
/* ------------------------------------------------------------------ */

typedef struct sink_state {
	size_t index;
	uint64_t target;
	size_t received_bytes;
	size_t received_chunks;
	uint64_t checksum;	/* sum of each chunk's FNV-1a */
	struct timespec last_print;
} sink_state;

static uint64_t
route_target(const void *rec, size_t len, void *ctx)
{
	(void)rec; (void)len;
	return ((sink_state *)ctx)->target;
}

static uint64_t
fnv1a(const unsigned char *p, size_t n)
{
	uint64_t h = UINT64_C(0xcbf29ce484222325);
	size_t i;

	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * UINT64_C(0x100000001b3);
	return h;
}

/* The Sink: counts what arrives, prints progress every 5 s. */
static void
sink(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	sink_state *s = ctx;
	size_t n;
	const unsigned char *p = dtfl_get_bytes(rec, &n);

	(void)time; (void)len;
	s->received_bytes += n;
	s->received_chunks++;
	s->checksum += fnv1a(p, n);
	if (secs_since(&s->last_print) >= 5.0) {
		fprintf(stderr, "worker %zu: received %zu chunks (%zu MB)\n",
		    s->index, s->received_chunks, s->received_bytes >> 20);
		clock_gettime(CLOCK_MONOTONIC, &s->last_print);
	}
}

typedef struct build_ctx {
	dtfl_input *input;
	sink_state *sink;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);

	s = dtfl_stream_exchange(s, route_target, c->sink);
	(void)dtfl_stream_inspect(s, sink, DTFL_CTX(c->sink, NULL));
}

static void
worker(dtfl_worker *w, void *ctx)
{
	run_ctx *r = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	size_t index = dtfl_worker_index(w);
	size_t words = r->chunk_bytes / 8, i, k;
	unsigned char *rec = malloc(8 + r->chunk_bytes);
	sink_state *s = calloc(1, sizeof *s);
	struct timespec prod_start, drain_start;
	uint64_t rng;
	build_ctx c;

	if (rec == NULL || s == NULL) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	s->index = index;
	s->target = (index + 1) % dtfl_worker_peers(w);
	clock_gettime(CLOCK_MONOTONIC, &s->last_print);
	c.input = dtfl_input_new(dtfl_type_bytes());
	c.sink = s;
	dtfl_worker_dataflow(w, "spill_compare", tc, build, &c);

	/*
	 * Production: each worker pushes its share into the input in one
	 * shot, with no step calls, then closes the input and lets the
	 * framework drain.  xorshift64 keeps the bytes incompressible.
	 */
	clock_gettime(CLOCK_MONOTONIC, &prod_start);
	rng = UINT64_C(0x9E3779B97F4A7C15) ^
	    ((uint64_t)index * UINT64_C(0xBF58476D1CE4E5B9));
	dtfl_put_u64(rec, r->chunk_bytes);
	for (k = 0; k < r->chunks_per_worker; k++) {
		for (i = 0; i < words; i++) {
			rng ^= rng << 13;
			rng ^= rng >> 7;
			rng ^= rng << 17;
			memcpy(rec + 8 + 8 * i, &rng, 8);
		}
		dtfl_input_send(c.input, rec, 8 + r->chunk_bytes);
	}
	dtfl_input_close(c.input);
	fprintf(stderr, "worker %zu: production %.2fs\n", index,
	    secs_since(&prod_start));

	clock_gettime(CLOCK_MONOTONIC, &drain_start);
	while (dtfl_worker_step_or_park(w, UINT64_MAX))
		;
	fprintf(stderr, "worker %zu: drain %.2fs\n", index,
	    secs_since(&drain_start));
	printf("worker %zu: received %zu chunks (%zu MB) checksum %016llx\n",
	    index, s->received_chunks, s->received_bytes >> 20,
	    (unsigned long long)s->checksum);
	free(rec);
	free(s);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	size_t total_mb = parse_arg(argc, argv, "--total-mb", 64);
	size_t total_gb = parse_arg(argc, argv, "--total-gb", 0);
	size_t chunk_kb = parse_arg(argc, argv, "--chunk-kb", 256);
	size_t workers = parse_arg(argc, argv, "--workers",
	    parse_arg(argc, argv, "-w", 2));
	size_t threshold_mb = parse_arg(argc, argv, "--threshold-mb", 4);
	size_t head_reserve_mb = parse_arg(argc, argv, "--head-reserve-mb", 2);
	size_t rss_every = parse_arg(argc, argv, "--rss-every-secs", 2);
	int with_spill = has_flag(argc, argv, "--with-spill");
	const char *tmp = getenv("TMPDIR");
	const char *spill_dir = parse_str(argc, argv, "--spill-dir",
	    tmp != NULL && *tmp != '\0' ? tmp : "/tmp");
	size_t total_bytes, chunk_bytes, total_chunks;
	dtfl_config *cfg = dtfl_config_new();
	pthread_t th;
	uint64_t peak;
	run_ctx r;
	int rc;

	if (total_gb > 0)
		total_mb = total_gb << 10;
	if (workers == 0 || chunk_kb == 0) {
		fprintf(stderr, "spill_compare: --workers and --chunk-kb must "
		    "be positive\n");
		return 2;
	}
	chunk_bytes = chunk_kb << 10;
	total_bytes = total_mb << 20;
	total_chunks = total_bytes / chunk_bytes;

	memset(&r, 0, sizeof r);
	r.chunk_bytes = chunk_bytes;
	r.chunks_per_worker = total_chunks / workers;
	r.rss_every_secs = rss_every > 0 ? (unsigned)rss_every : 1;
	atomic_init(&r.stop, false);
	atomic_init(&r.rss_peak_kb, 0);

	printf("spill_compare configuration:\n");
	printf("  workers:         %zu\n", workers);
	printf("  total:           %zu MB (%zu chunks of %zu KB)\n", total_mb,
	    total_chunks, chunk_kb);
	printf("  per worker:      %zu chunks (%zu MB)\n", r.chunks_per_worker,
	    (r.chunks_per_worker * chunk_bytes) >> 20);
	printf("  with_spill:      %s\n", with_spill ? "true" : "false");
	if (with_spill) {
		printf("  threshold:       %zu MB\n", threshold_mb);
		printf("  head_reserve:    %zu MB\n", head_reserve_mb);
	}
	printf("\n");
	fflush(stdout);
	/* The directory varies per host; keep it off the compared stdout. */
	if (with_spill)
		fprintf(stderr, "  spill_dir:       %s\n", spill_dir);

	/* Spill engages on the zero-copy MergeQueue path. */
	if (dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS_BINARY) != 0 ||
	    dtfl_config_set_threads(cfg, workers) != 0 ||
	    (with_spill && dtfl_config_set_spill_file(cfg, spill_dir,
	    threshold_mb << 20, head_reserve_mb << 20) != 0)) {
		fprintf(stderr, "spill_compare: %s\n", dtfl_last_error_detail());
		dtfl_config_free(cfg);
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &r.start);
	if (pthread_create(&th, NULL, sampler, &r) != 0) {
		fprintf(stderr, "spill_compare: cannot start the RSS sampler\n");
		dtfl_config_free(cfg);
		return 1;
	}
	rc = dtfl_execute(cfg, worker, &r, NULL);
	atomic_store(&r.stop, true);
	pthread_join(th, NULL);
	dtfl_config_free(cfg);
	if (rc != 0) {
		fprintf(stderr, "spill_compare: %s\n", dtfl_last_error_detail());
		return 1;
	}

	printf("\n");
	peak = atomic_load(&r.rss_peak_kb);
	if (peak_rss_kb() > peak)
		peak = peak_rss_kb();
	if (peak > 0)
		printf("rss_peak: %llu MB\n", (unsigned long long)(peak / 1024));
	else
		printf("rss_peak: n/a\n");
	printf("elapsed: %.2fs\n", secs_since(&r.start));
	printf("OK\n");
	return 0;
}
