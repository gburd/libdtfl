/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/benches/chunk_bench.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * bench/chunk_bench.c: a microbenchmark of the one chunk layout at the Chunk
 * level -- build, merge (merge_chains), and full cursor scan -- across data
 * shapes, isolating the per-layout cost from the dataflow runtime.  Upstream
 * compares VecChunk (flat) against ColChunk (columnar trie); the columnar arm
 * is subsumed (PLAN D2), so this benchmarks the flat layout alone.
 *
 * A benchmark, not a test: not run by make check.  It links the internal
 * library because the chunk layer is internal (__dtfl_*).  It uses getenv-free
 * argv for the update count and prints timings; no fixed output is asserted.
 *
 * Run as: bench/chunk_bench [updates]
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include "dd/trace/chunk_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

typedef struct upd {
	uint64_t k, v, t;
	int64_t r;
} upd;

static double
ms_since(struct timespec t0)
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}

/* Build a sorted, consolidated chunk chain through the real arrange path. */
static void
build_chain(const __dtfl_chunk_ctx *x, const upd *u, size_t n,
    __dtfl_chunk_deque *out)
{
	__dtfl_consolidator con;
	__dtfl_chunk *c;
	size_t i;

	__dtfl_consolidator_init(&con, x->rt, x->tw, x->dc);
	for (i = 0; i < n; i++) {
		unsigned char rec[16];

		dtfl_put_u64(rec, u[i].k);
		dtfl_put_u64(rec + 8, u[i].v);
		__dtfl_consolidator_push(&con, rec, 16, &u[i].t, &u[i].r);
	}
	while ((c = __dtfl_consolidator_finish(&con)) != NULL)
		__dtfl_chunk_deque_push_back(out, c);
	__dtfl_consolidator_free(&con);
}

/* Count every (key, val, time, diff) via the straddle cursor over a batch. */
static void
count_times(const uint64_t *t, const void *d, void *vp)
{
	(void)t;
	(void)d;
	(*(size_t *)vp)++;
}

static size_t
scan_batch(const __dtfl_chunk_batch *b)
{
	__dtfl_chunk_cursor cur;
	size_t count = 0, len;

	__dtfl_chunk_cursor_init(&cur, b);
	while (__dtfl_chunk_cursor_key_valid(&cur, b)) {
		(void)__dtfl_chunk_cursor_key(&cur, b, &len);
		while (__dtfl_chunk_cursor_val_valid(&cur, b)) {
			(void)__dtfl_chunk_cursor_val(&cur, b, &len);
			__dtfl_chunk_cursor_map_times(&cur, b, count_times,
			    &count);
			__dtfl_chunk_cursor_step_val(&cur, b);
		}
		__dtfl_chunk_cursor_step_key(&cur, b);
	}
	return count;
}

/* A deque of chunk pointers sealed into a ChunkBatch for scanning. */
static __dtfl_chunk_batch *
seal(const __dtfl_chunk_ctx *x, dtfl_time_class *tc, __dtfl_chunk_deque *d)
{
	__dtfl_chunk **arr;
	__dtfl_antichain lo, hi, si;
	__dtfl_description desc;
	__dtfl_chunk_batch *b;
	uint64_t l = 0, h = 1, s = 0;
	size_t n = __dtfl_chunk_deque_len(d), i;

	/* Skip empty chunks (ChunkBatch requires non-empty); none here. */
	arr = n != 0 ? malloc(n * sizeof *arr) : NULL;
	for (i = 0; i < n; i++)
		arr[i] = __dtfl_chunk_deque_at(d, i);
	__dtfl_antichain_init_elem(&lo, tc, &l);
	__dtfl_antichain_init_elem(&hi, tc, &h);
	__dtfl_antichain_init_elem(&si, tc, &s);
	__dtfl_description_init(&desc, &lo, &hi, &si);
	b = __dtfl_chunk_batch_new(x, arr, n, &desc);
	free(arr);
	__dtfl_vec_free(&d->v);
	return b;
}

static void
bench_shape(const char *label, const __dtfl_chunk_ctx *x, dtfl_time_class *tc,
    const upd *u, size_t n)
{
	__dtfl_chunk_deque full, a, b, merged, settled;
	__dtfl_chunk_batch *batch;
	struct timespec t0;
	double build_ms, merge_ms, scan_ms;
	size_t i, records, scanned;
	upd *ua = malloc((n / 2 + 1) * sizeof *ua);
	upd *ub = malloc((n / 2 + 1) * sizeof *ub);
	size_t na = 0, nb = 0;

	/* Even/odd split for a heavily interleaving merge. */
	for (i = 0; i < n; i++) {
		if (i % 2 == 0)
			ua[na++] = u[i];
		else
			ub[nb++] = u[i];
	}

	__dtfl_chunk_deque_init(&full);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	build_chain(x, u, n, &full);
	build_ms = ms_since(t0);
	records = 0;
	for (i = 0; i < __dtfl_chunk_deque_len(&full); i++)
		records += __dtfl_batch_len(__dtfl_chunk_deque_at(&full, i));

	__dtfl_chunk_deque_init(&a);
	__dtfl_chunk_deque_init(&b);
	build_chain(x, ua, na, &a);
	build_chain(x, ub, nb, &b);
	__dtfl_chunk_deque_init(&merged);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	__dtfl_chunk_merge_chains(x, &a, &b, &merged);
	merge_ms = ms_since(t0);

	__dtfl_chunk_deque_init(&settled);
	__dtfl_chunk_settle_all(x, &merged, &settled);
	batch = seal(x, tc, &settled);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	scanned = scan_batch(batch);
	scan_ms = ms_since(t0);

	printf("\n== %s  (%zu records) ==\n", label, records);
	printf("  build ms  %10.1f\n", build_ms);
	printf("  merge ms  %10.1f\n", merge_ms);
	printf("  scan  ms  %10.1f  (%zu updates)\n", scan_ms, scanned);

	__dtfl_chunk_batch_release(batch);
	__dtfl_chunk_deque_free(&full);
	__dtfl_chunk_deque_free(&a);
	__dtfl_chunk_deque_free(&b);
	__dtfl_chunk_deque_free(&merged);
	free(ua);
	free(ub);
}

int
main(int argc, char **argv)
{
	size_t n = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 1000000;
	uint64_t seed = 0x2545F4914F6CDD1Dull;
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	upd *u = malloc(n * sizeof *u);
	size_t i;

	__dtfl_chunk_ctx_init(&x, false, rt, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64);

	/* Shape A: unique-ish u64 keys, zero val, single time (flat's turf). */
	for (i = 0; i < n; i++) {
		u[i].k = (uint64_t)i;
		u[i].v = 0;
		u[i].t = 0;
		u[i].r = 1;
	}
	for (i = 0; i < n; i++) {	/* shuffle */
		size_t j;

		seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
		j = (size_t)(seed % n);
		upd tmp = u[i]; u[i] = u[j]; u[j] = tmp;
	}
	bench_shape("A: unique u64 keys, 0 vals", &x, tc, u, n);

	/* Shape B: few keys, many random vals each (key dedup + val runs). */
	{
		uint64_t keys = (uint64_t)(n > 0 ? 1 : 0);

		while (keys * keys < n)
			keys++;
		for (i = 0; i < n; i++) {
			seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
			u[i].k = keys ? seed % keys : 0;
			seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
			u[i].v = keys ? seed % keys : 0;
			u[i].t = 0;
			u[i].r = 1;
		}
		bench_shape("B: few u64 keys, many random vals", &x, tc, u, n);
	}

	free(u);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return 0;
}
