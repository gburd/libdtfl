/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/logging-send.c: mirrors timely/examples/logging-send.rs.
 *
 * Registers a subscriber on the worker's "timely" structured-event log,
 * builds a dataflow (input -> exchange -> probe), then re-registers a
 * second subscriber (replacing the first) and builds a second dataflow, so
 * the first dataflow's events reach LOG1 and the second's reach LOG2, as
 * upstream switches its "timely" logger between the two dataflows.  Each
 * subscriber prints the operator-creation (Operates) and channel-creation
 * (Channels) events, which describe the dataflow structure and are
 * deterministic for a fixed dataflow.
 *
 * Divergence: upstream also registers the timely/progress, timely/
 * reachability, timely/summary and a user-level "input" logging stream and
 * prints a non-deterministic throughput line; the public subscriber API
 * (dtfl/log.h) exposes the "timely", "differential/arrange" and
 * "communication" streams, so this port shows the "timely" stream only and
 * drops the timing line for a checkable output.  Single worker.
 */
#include "example_args.h"

#include "dtfl/collection.h"
#include "dtfl/log.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <string.h>

/* The two subscriber callbacks print with their own tag. */
static void
log_batch(const dtfl_log_batch *b, const char *tag)
{
	size_t n = dtfl_log_batch_len(b), i;

	for (i = 0; i < n; i++) {
		switch (dtfl_log_event_kind_at(b, i)) {
		case DTFL_LOG_OPERATES:
			printf("%s: Operates id=%zu name=%s\n", tag,
			    dtfl_log_operates_id(b, i),
			    dtfl_log_operates_name(b, i));
			break;
		case DTFL_LOG_CHANNELS:
			printf("%s: Channels id=%zu\n", tag,
			    dtfl_log_channels_id(b, i));
			break;
		default:
			break;
		}
	}
}

static void
on_log1(const dtfl_log_batch *b, void *ctx)
{
	(void)ctx;
	log_batch(b, "LOG1");
}

static void
on_log2(const dtfl_log_batch *b, void *ctx)
{
	(void)ctx;
	log_batch(b, "LOG2");
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
} build_ctx;

/* input_from(input).exchange(|x| x).probe_with(probe). */
static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *col = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *con = dtfl_collection_consolidate(col);
	dtfl_collection *probed = dtfl_collection_probe_with(con, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(con);
	dtfl_collection_free(col);
}

static void
drive(dtfl_worker *w, dtfl_input_session *in, dtfl_probe *probe,
    uint64_t epoch, size_t batch)
{
	size_t i;

	for (i = 0; i < batch; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, i);
		dtfl_input_session_insert(in, buf, sizeof buf);
	}
	dtfl_input_session_advance_to(in, &epoch);
	dtfl_input_session_flush(in);
	while (dtfl_probe_less_than(probe, dtfl_input_session_time(in)))
		dtfl_worker_step(w);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	build_ctx c1, c2;
	size_t batch = 5;

	(void)ctx;
	c1.in = dtfl_input_session_new(dtfl_type_u64(), i64);
	c1.probe = dtfl_probe_new();
	c2.in = dtfl_input_session_new(dtfl_type_u64(), i64);
	c2.probe = dtfl_probe_new();

	/* First "timely" subscriber, then the first dataflow. */
	dtfl_worker_log_subscribe(w, "timely", on_log1, NULL, NULL);
	dtfl_worker_dataflow(w, "logsend1", tc, build, &c1);

	/* Replace the subscriber, then the second dataflow (events -> LOG2). */
	dtfl_worker_log_subscribe(w, "timely", on_log2, NULL, NULL);
	dtfl_worker_dataflow(w, "logsend2", tc, build, &c2);

	drive(w, c1.in, c1.probe, 1, batch);
	drive(w, c2.in, c2.probe, 1, batch);
	dtfl_input_session_close(c1.in);
	dtfl_input_session_close(c2.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c1.probe);
	dtfl_probe_free(c2.probe);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	/* Positional args (batch rounds) are kept for upstream parity. */
	int skip = argc > 2 ? 2 : 0;
	dtfl_config *cfg = ex_config(argc, argv, skip);
	int rc;

	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
