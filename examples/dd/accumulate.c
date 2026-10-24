/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/accumulate.c: mirrors
 * differential-dataflow/examples/accumulate.rs.
 *
 * Demonstrates in-place accumulation whose space is bounded by the number of
 * distinct keys: the input collection is entered into an iterative subscope
 * at inner time 0 (enter_at with |_| 0), consolidated, and left again, so
 * repeated key insertions collapse to one update per key.  Upstream inserts
 * batches of random keys forever and reports throughput; this port inserts a
 * fixed multiset once and inspects the consolidated (key, count) result, which
 * is exact and checkable.  The enter_at / consolidate / leave pipeline is
 * upstream's.
 */
#include "example_args.h"

#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

/*
 * A fixed multiset of u32 keys (with repeats).  Accumulated counts:
 *   0 -> 3, 1 -> 1, 2 -> 2, 3 -> 4.
 */
static const uint32_t fixed_keys[] = {
	0, 0, 0, 1, 2, 2, 3, 3, 3, 3,
};
#define N_KEYS (sizeof fixed_keys / sizeof fixed_keys[0])

/* enter_at logic: every record enters at inner time 0 (upstream |_| 0). */
static void
enter_at_zero(void *ctx, const void *rec, size_t len, uint64_t *inner_time_out)
{
	(void)ctx; (void)rec; (void)len;
	inner_time_out[0] = 0;
}

static void
inspect_acc(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	printf("(%llu, %lld)\n", (unsigned long long)dtfl_get_u32(rec),
	    (long long)d);
}

typedef struct iter_ctx {
	dtfl_collection *data;		/* the entered source, in the subscope */
	dtfl_collection *result;	/* set by the inner builder */
} iter_ctx;

static void
build_inner(dtfl_scope *inner, void *arg)
{
	iter_ctx *c = arg;
	dtfl_collection *entered = dtfl_collection_enter_at(c->data, inner,
	    enter_at_zero, DTFL_NOCTX);
	dtfl_collection *con = dtfl_collection_consolidate(entered);

	c->result = dtfl_collection_leave(con);
	dtfl_collection_free(con);
	dtfl_collection_free(entered);
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	iter_ctx *ic;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *data = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *ins, *probed;

	c->ic->data = data;
	c->ic->result = NULL;
	dtfl_scope_iterative(scope, "accumulate", build_inner, c->ic);

	ins = dtfl_collection_inspect(c->ic->result, inspect_acc, DTFL_NOCTX);
	probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(c->ic->result);
	dtfl_collection_free(data);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *u32 = dtfl_type_u32();
	build_ctx c;
	iter_ctx ic;
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	memset(&ic, 0, sizeof ic);
	c.in = dtfl_input_session_new(u32, i64);
	c.probe = dtfl_probe_new();
	c.ic = &ic;
	dtfl_worker_dataflow(w, "accumulate", tc, build, &c);

	for (i = 0; i < N_KEYS; i++) {
		unsigned char buf[4];

		dtfl_put_u32(buf, fixed_keys[i]);
		dtfl_input_session_insert(c.in, buf, sizeof buf);
	}
	dtfl_input_session_advance_to(c.in, &one);
	dtfl_input_session_flush(c.in);
	while (dtfl_probe_less_than(c.probe, &one))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	/* Upstream takes a key-count positional; keep that shape. */
	int skip = argc > 1 ? 1 : 0;
	dtfl_config *cfg = ex_config(argc, argv, skip);
	int rc;

	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
