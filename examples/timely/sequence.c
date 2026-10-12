/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/sequence.c: mirrors timely/examples/sequence.rs.
 *
 * Argument: rounds [timely flags].  Four process workers each push a
 * "worker I, round R" string into a Sequencer every round; every worker
 * drains the sequencer and prints what it received, in the globally
 * agreed order.  Upstream loops forever with per-worker sleeps; this caps
 * the loop at `rounds` so it terminates, with a short per-round sleep so
 * the elapsed-time (Duration) frontier spreads the pushes and the
 * sequencer releases them.
 */
#define _POSIX_C_SOURCE 200809L	/* nanosleep */

#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <time.h>

typedef struct run_ctx { uint64_t rounds; } run_ctx;

static void
nap_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void
drain(dtfl_sequencer *seq, size_t index)
{
	const void *rec;
	size_t len;

	while ((rec = dtfl_sequencer_next(seq, &len)) != NULL) {
		size_t plen;
		const char *s = dtfl_get_bytes(rec, &plen);

		printf("Worker %zu:\t recv'd: %.*s\n", index, (int)plen, s);
	}
}

static void
worker(dtfl_worker *w, void *ctx)
{
	run_ctx *r = ctx;
	size_t df = dtfl_worker_next_dataflow_index(w);
	dtfl_sequencer *seq = dtfl_sequencer_new(w, dtfl_type_string());
	size_t index = dtfl_worker_index(w);
	uint64_t round;

	for (round = 0; round < r->rounds; round++) {
		char msg[64];
		unsigned char buf[80];
		int n = snprintf(msg, sizeof msg, "worker %zu, round %llu",
		    index, (unsigned long long)round);
		size_t len = dtfl_put_bytes(buf, msg, (size_t)n);

		nap_ms(10);
		dtfl_sequencer_push(seq, buf, len);
		dtfl_worker_step(w);
		drain(seq, index);
	}
	/*
	 * Let elapsed time pass the last push so the frontier releases every
	 * worker's messages, stepping and draining as it does.
	 */
	for (round = 0; round < 50; round++) {
		nap_ms(10);
		dtfl_worker_step(w);
		drain(seq, index);
	}
	/*
	 * The sequencer's internal dataflow has a catchup source that
	 * reschedules forever, so dtfl_execute's join would spin on it.
	 * Drop it explicitly before returning (upstream loops forever and
	 * never shuts down cleanly).
	 */
	dtfl_sequencer_free(seq);
	dtfl_worker_drop_dataflow(w, df);
}

int
main(int argc, char **argv)
{
	run_ctx r;
	dtfl_config *cfg;
	int rc;

	r.rounds = ex_u64(argc, argv, 1, 3);
	cfg = ex_config(argc, argv, 1);
	/* Default to 4 process workers, as upstream Config::process(4). */
	if (dtfl_config_mode(cfg) == DTFL_COMM_THREAD) {
		dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS);
		dtfl_config_set_threads(cfg, 4);
	}
	rc = dtfl_execute(cfg, worker, &r, NULL);
	dtfl_config_free(cfg);
	return rc;
}
