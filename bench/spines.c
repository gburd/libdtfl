/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/examples/spines.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * bench/spines.c: compares the trace batch backings over the same updates --
 * the ord-neu layered trie (key / val spines) against the one Chunk layout
 * (the vec spine) -- at build and scan.  Upstream arranges String keys end to
 * end and compares key / val / vec / col spines; the col mode is subsumed (D2)
 * and the public arrange path uses ord-neu, so this benchmarks the two batch
 * backings directly, which is the apples-to-apples the port note calls for.
 *
 * A benchmark, not a test: not run by make check.  Links the internal library
 * (the batch backings are internal).  Reads the update count and round count
 * from argv; prints timings, asserts no fixed output.
 *
 * Run as: bench/spines [keys] [rounds]
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include "dd/trace/chunk_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

static double
ms_since(struct timespec t0)
{
	struct timespec t1;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
}

static void
count_times(const uint64_t *t, const void *d, void *vp)
{
	(void)t;
	(void)d;
	(*(size_t *)vp)++;
}

/* ---- ord-neu (key spine) backing: build one batch, scan it. --------- */
static double
bench_ord(dtfl_time_class *tc, const uint64_t *keys, size_t n, size_t *scanned)
{
	__dtfl_batch *chunk = __dtfl_batch_new(1, 8);
	__dtfl_antichain lo, hi, si;
	__dtfl_description desc;
	__dtfl_batch_ord *b;
	__dtfl_cursor cur;
	struct timespec t0;
	double build_ms;
	uint64_t l = 0, h = 1, s = 0, t = 0;
	int64_t one = 1;
	size_t i, len, cnt = 0;

	for (i = 0; i < n; i++) {
		unsigned char rec[8];

		dtfl_put_u64(rec, keys[i]);
		__dtfl_batch_push(chunk, rec, 8, &t, &one);
	}
	__dtfl_consolidate_batch(chunk, dtfl_type_u64(), I64);
	__dtfl_antichain_init_elem(&lo, tc, &l);
	__dtfl_antichain_init_elem(&hi, tc, &h);
	__dtfl_antichain_init_elem(&si, tc, &s);
	__dtfl_description_init(&desc, &lo, &hi, &si);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	b = __dtfl_builder_ord_seal(true, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64, &chunk, 1, &desc);
	build_ms = ms_since(t0);
	__dtfl_batch_release(chunk);

	__dtfl_cursor_init(&cur, b);
	while (__dtfl_cursor_key_valid(&cur, b)) {
		(void)__dtfl_cursor_key(&cur, b, &len);
		while (__dtfl_cursor_val_valid(&cur, b)) {
			__dtfl_cursor_map_times(&cur, b, count_times, &cnt);
			__dtfl_cursor_step_val(&cur, b);
		}
		__dtfl_cursor_step_key(&cur, b);
	}
	*scanned = cnt;
	__dtfl_batch_ord_release(b);
	return build_ms;
}

/* ---- chunk (vec spine) backing: build one batch, scan it. ----------- */
static double
bench_chunk(dtfl_time_class *tc, const uint64_t *keys, size_t n,
    size_t *scanned)
{
	const dtfl_type *rt = dtfl_type_u64();	/* key batch: record is key */
	__dtfl_chunk_ctx x;
	__dtfl_chunk *chunk = __dtfl_batch_new(1, 8);
	__dtfl_chunk_builder bld;
	__dtfl_chunk_cursor cur;
	__dtfl_antichain lo, hi, si;
	__dtfl_description desc;
	__dtfl_chunk_batch *b;
	struct timespec t0;
	double build_ms;
	uint64_t l = 0, h = 1, s = 0, t = 0;
	int64_t one = 1;
	size_t i, len, cnt = 0;

	__dtfl_chunk_ctx_init(&x, true, rt, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64);
	/*
	 * Consolidate the whole stream into one globally-sorted chunk (the
	 * real arrange path reaches the builder globally consolidated); settle
	 * then grades it into TARGET-sized pieces, each a disjoint sub-range,
	 * so no key straddles a chunk boundary with a duplicate.
	 */
	for (i = 0; i < n; i++) {
		unsigned char rec[8];

		dtfl_put_u64(rec, keys[i]);
		__dtfl_batch_push(chunk, rec, 8, &t, &one);
	}
	__dtfl_consolidate_batch(chunk, rt, I64);
	__dtfl_antichain_init_elem(&lo, tc, &l);
	__dtfl_antichain_init_elem(&hi, tc, &h);
	__dtfl_antichain_init_elem(&si, tc, &s);
	__dtfl_description_init(&desc, &lo, &hi, &si);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	__dtfl_chunk_builder_init(&bld, &x);
	__dtfl_chunk_builder_push(&bld, chunk);
	b = __dtfl_chunk_builder_done(&bld, &desc);
	build_ms = ms_since(t0);

	__dtfl_chunk_cursor_init(&cur, b);
	while (__dtfl_chunk_cursor_key_valid(&cur, b)) {
		(void)__dtfl_chunk_cursor_key(&cur, b, &len);
		while (__dtfl_chunk_cursor_val_valid(&cur, b)) {
			__dtfl_chunk_cursor_map_times(&cur, b, count_times,
			    &cnt);
			__dtfl_chunk_cursor_step_val(&cur, b);
		}
		__dtfl_chunk_cursor_step_key(&cur, b);
	}
	*scanned = cnt;
	__dtfl_chunk_batch_release(b);
	return build_ms;
}

int
main(int argc, char **argv)
{
	size_t keys = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 500000;
	size_t rounds = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10) : 1;
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t seed = 0x9E3779B97F4A7C15ull;
	uint64_t *data = malloc(keys * sizeof *data);
	size_t i, round, so = 0, sc = 0;
	double bo = 0, bc = 0;

	for (i = 0; i < keys; i++) {
		seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
		data[i] = seed % (keys ? keys : 1);
	}
	for (round = 0; round < rounds; round++) {
		bo += bench_ord(tc, data, keys, &so);
		bc += bench_chunk(tc, data, keys, &sc);
	}
	printf("spines: %zu keys x %zu rounds\n", keys, rounds);
	printf("              %12s %12s\n", "ord(trie)", "chunk(vec)");
	printf("  build ms    %12.1f %12.1f\n", bo, bc);
	printf("  scanned     %12zu %12zu\n", so, sc);

	free(data);
	dtfl_time_class_free(tc);
	return 0;
}
