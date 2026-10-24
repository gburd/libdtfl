/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/flow_controlled.c: mirrors
 * timely/examples/flow_controlled.rs.
 *
 * iterator_source emits (timestamp, value) records from 0..count in
 * chunks of ten, choosing each chunk's time as x / 100 * 100 and holding
 * back while a probe is behind.  inspect_time prints each record with its
 * time; the probe closes the flow-control loop.  Upstream uses 100000
 * values; the count is an optional argument so the run stays bounded.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct src_ctx {
	uint64_t next;
	uint64_t count;
} src_ctx;

/* Emit the next chunk of up to ten values; set *time to x / 100 * 100. */
static bool
source_next(void *ctx, uint64_t *time, dtfl_emit *out)
{
	src_ctx *s = ctx;
	uint64_t first, k;

	if (s->next >= s->count)
		return false;
	first = s->next;
	*time = first / 100 * 100;
	for (k = 0; k < 10 && s->next < s->count; k++) {
		unsigned char buf[16];

		/* record is (timestamp, value) = (x, x). */
		dtfl_put_u64(buf, s->next);
		dtfl_put_u64(buf + 8, s->next);
		dtfl_emit_record(out, buf, sizeof buf);
		s->next++;
	}
	return true;
}

static void
inspect_time(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)ctx; (void)len;
	fprintf(stderr, "@ %llu: (%llu, %llu)\n", (unsigned long long)time[0],
	    (unsigned long long)dtfl_get_u64(rec),
	    (unsigned long long)dtfl_get_u64((const unsigned char *)rec + 8));
}

typedef struct build_ctx {
	src_ctx src;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	dtfl_stream *s = dtfl_iterator_source(scope, pair, source_next, c->probe,
	    DTFL_CTX(&c->src, NULL));

	s = dtfl_stream_inspect(s, inspect_time, DTFL_NOCTX);
	dtfl_stream_probe_with(s, c->probe);
	dtfl_type_free(pair);
}

typedef struct worker_ctx { uint64_t count; } worker_ctx;

static void
worker(dtfl_worker *w, void *ctx)
{
	worker_ctx *wc = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c;

	c.src.next = 0;
	c.src.count = wc->count;
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "flow_controlled", tc, build, &c);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	worker_ctx wc;
	dtfl_config *cfg;
	int rc;

	wc.count = ex_u64(argc, argv, 1, 1000);
	cfg = ex_config(argc, argv, 1);
	rc = dtfl_execute(cfg, worker, &wc, NULL);
	dtfl_config_free(cfg);
	return rc;
}
