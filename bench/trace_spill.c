/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/examples/columnar_spill.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * bench/trace_spill.c: the file-backed chunk-spill demonstration.  Fills a
 * ChunkBatch past a per-worker record budget so settle pages committed chunks
 * out to a temp-file store, reads the batch back through the straddle cursor,
 * confirms every update survived, and prints the spill counters (chunks and
 * records paged out, bytes written, chunks fetched back) and the compression
 * ratio stand-in (serialized bytes per record).
 *
 * Upstream is an example (columnar_spill.rs) over the public arrange path with
 * a per-worker spiller installed through timely::execute.  libdtfl's chunk and
 * spill layer is internal (__dtfl_*) and the public arrange path uses the
 * ord-neu backing, so a public example cannot reach the spill; this lives in
 * bench/ and links the internal library, exercising the real spill end to end.
 * A public arrangement-spill knob is deferred (see dev notes).
 *
 * Not run by make check (a demonstration/benchmark).  getenv-free argv.
 *
 * Run as: bench/trace_spill [records] [budget_records]
 */
#include "dd/trace/chunk_int.h"

#include <stdio.h>
#include <stdlib.h>

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

static void
count_times(const uint64_t *t, const void *d, void *vp)
{
	(void)t;
	(void)d;
	(*(size_t *)vp)++;
}

int
main(int argc, char **argv)
{
	size_t records = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10)
	    : 200000;
	size_t budget = argc > 2 ? (size_t)strtoull(argv[2], NULL, 10)
	    : DTFL_CHUNK_TARGET;
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_chunk_ctx x;
	__dtfl_spill_state st;
	__dtfl_spill_stats stats = { 0, 0, 0, 0 };
	__dtfl_spill_store *store = __dtfl_spill_file_store_new();
	__dtfl_chunk_builder bld;
	__dtfl_chunk_batch *b;
	__dtfl_chunk_cursor cur;
	__dtfl_antichain lo, hi, si;
	__dtfl_description desc;
	uint64_t l = 0, h = 1, s = 0, t = 0;
	int64_t one = 1;
	size_t per = DTFL_CHUNK_TARGET, done = 0, seen = 0, len;

	__dtfl_chunk_ctx_init(&x, false, rt, dtfl_type_u64(), dtfl_type_u64(),
	    tc, I64);
	__dtfl_spill_install(&x, &st, budget, store, &stats);

	/* Push one TARGET-sized chunk of distinct keys at a time. */
	__dtfl_chunk_builder_init(&bld, &x);
	while (done < records) {
		__dtfl_chunk *c = __dtfl_batch_new(1, 8);
		size_t m = records - done < per ? records - done : per, j;

		for (j = 0; j < m; j++) {
			unsigned char rec[16];

			dtfl_put_u64(rec, (uint64_t)(done + j));
			dtfl_put_u64(rec + 8, 0);
			__dtfl_batch_push(c, rec, 16, &t, &one);
		}
		__dtfl_chunk_builder_push(&bld, c);
		done += m;
	}
	__dtfl_antichain_init_elem(&lo, tc, &l);
	__dtfl_antichain_init_elem(&hi, tc, &h);
	__dtfl_antichain_init_elem(&si, tc, &s);
	__dtfl_description_init(&desc, &lo, &hi, &si);
	b = __dtfl_chunk_builder_done(&bld, &desc);

	/* Read it back; count every surviving update. */
	__dtfl_chunk_cursor_init(&cur, b);
	while (__dtfl_chunk_cursor_key_valid(&cur, b)) {
		(void)__dtfl_chunk_cursor_key(&cur, b, &len);
		while (__dtfl_chunk_cursor_val_valid(&cur, b)) {
			__dtfl_chunk_cursor_map_times(&cur, b, count_times,
			    &seen);
			__dtfl_chunk_cursor_step_val(&cur, b);
		}
		__dtfl_chunk_cursor_step_key(&cur, b);
	}

	printf("trace_spill: %zu records, budget %zu\n", records, budget);
	printf("  spilled chunks:  %zu\n", stats.spilled_chunks);
	printf("  spilled records: %zu\n", stats.spilled_records);
	printf("  fetched chunks:  %zu\n", stats.fetched_chunks);
	printf("  bytes written:   %zu\n", stats.bytes_written);
	if (stats.spilled_records != 0)
		printf("  bytes/record:    %.1f\n",
		    (double)stats.bytes_written / stats.spilled_records);
	printf("  read back:       %zu records %s\n", seen,
	    seen == records ? "(intact)" : "(DATA LOST)");

	__dtfl_chunk_batch_release(b);
	__dtfl_spill_uninstall(&x);
	__dtfl_spill_file_store_free(store);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	return seen == records ? 0 : 1;
}
