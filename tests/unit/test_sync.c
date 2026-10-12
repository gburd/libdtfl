/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_sync.c: barrier and sequencer (P4).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/synchronization/barrier.rs
 *   timely/src/synchronization/sequence.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A single-worker barrier reaches each round at once; wait() must return
 * after advancing the round.  A single-worker sequencer must deliver
 * every pushed element, in push order, as the elapsed-time frontier
 * passes.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Barrier                                                             */
/* ------------------------------------------------------------------ */

DTFL_TEST(barrier__single_worker_rounds)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_barrier *b = dtfl_barrier_new(w);
	int round;

	(void)params; (void)data;
	/* Each wait advances one round and returns once the probe passes it. */
	for (round = 0; round < 20; round++)
		dtfl_barrier_wait(b);

	dtfl_barrier_free(b);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Sequencer                                                           */
/* ------------------------------------------------------------------ */

DTFL_TEST(sequence__single_worker_order)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_sequencer *seq = dtfl_sequencer_new(w, dtfl_type_u64());
	uint64_t got[8];
	size_t ngot = 0;
	int round;

	(void)params; (void)data;
	/* Push eight elements over successive rounds and drain each round. */
	for (round = 0; round < 8; round++) {
		unsigned char buf[8];
		const void *rec;
		size_t len;

		dtfl_put_u64(buf, (uint64_t)(100 + round));
		dtfl_sequencer_push(seq, buf, sizeof buf);
		dtfl_worker_step(w);
		while ((rec = dtfl_sequencer_next(seq, &len)) != NULL) {
			assert_size(len, ==, 8);
			if (ngot < 8)
				got[ngot++] = dtfl_get_u64(rec);
		}
	}
	/* Drain any that the frontier had not yet released. */
	for (round = 0; round < 64 && ngot < 8; round++) {
		const void *rec;
		size_t len;

		dtfl_worker_step(w);
		while ((rec = dtfl_sequencer_next(seq, &len)) != NULL) {
			if (ngot < 8)
				got[ngot++] = dtfl_get_u64(rec);
		}
	}

	assert_size(ngot, ==, 8);
	/* One worker: order is push order (per-worker counter breaks ties). */
	for (round = 0; round < 8; round++)
		assert_uint64(got[round], ==, (uint64_t)(100 + round));

	dtfl_sequencer_free(seq);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(barrier__single_worker_rounds),
	DTFL_T(sequence__single_worker_order),
	DTFL_T_END
};

DTFL_TEST_MAIN("sync", tests)
