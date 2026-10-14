/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * doc/snippets/dd_reduce.c: the manual's reduce example (Differential
 * Dataflow chapter, Differential Operators).  Ports differential-dataflow
 * mdbook/src/chapter_2/chapter_2_6.md to C: a collection "manages" of
 * (manager, person) pairs; reduce by manager to each manager's managee
 * with the lowest identifier.  Values arrive sorted by the value type's
 * order, so the minimum is the first.  One worker, inputs at time 0,
 * sorted output.
 */
#include "dtfl/collection.h"
#include "dtfl/reduce.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

#define SIZE 10

/* min reducer: the accumulated values are sorted; emit the first. */
static void
min_managee(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;
	unsigned char vbuf[8];

	(void)ctx;
	(void)key;
	(void)klen;
	(void)n;
	(void)vlens;
	(void)diffs;
	dtfl_put_u64(vbuf, dtfl_get_u64(vals[0]));
	dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &one);
}

static void
report(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	const unsigned char *p = rec;
	int64_t d;

	(void)ctx;
	(void)len;
	(void)time;
	memcpy(&d, diff, sizeof d);
	if (d <= 0)
		return;
	printf("manager %llu -> managee %llu\n",
	    (unsigned long long)dtfl_get_u64(p),
	    (unsigned long long)dtfl_get_u64(p + 8));
}

typedef struct build_ctx {
	dtfl_input_session *manages;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *manages = dtfl_input_session_to_collection(c->manages,
	    scope);
	dtfl_collection *red = dtfl_collection_reduce(manages, u64, u64, u64,
	    i64, min_managee, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(red, report, DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(red);
	dtfl_collection_free(manages);
	dtfl_diff_free(i64);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	build_ctx c;
	uint64_t one = 1, person;

	(void)ctx;
	c.manages = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "min-managee", tc, build, &c);

	if (dtfl_worker_index(w) == 0)
		for (person = 0; person < SIZE; person++) {
			unsigned char buf[16];

			dtfl_put_u64(buf, person / 2);
			dtfl_put_u64(buf + 8, person);
			dtfl_input_session_insert(c.manages, buf, sizeof buf);
		}
	dtfl_input_session_advance_to(c.manages, &one);
	dtfl_input_session_flush(c.manages);
	while (dtfl_probe_less_than(c.probe,
	    dtfl_input_session_time(c.manages)))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.manages);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(void)
{
	return dtfl_execute_directly(worker, NULL);
}
