/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * doc/snippets/dd_manages.c: the manual's differential example
 * (Differential Dataflow chapter, Motivation).  Ports
 * differential-dataflow mdbook/src/chapter_0/chapter_0_1.md (skip-level
 * reports) to C: a collection "manages" of (manager, person) pairs, a
 * binary tree of size 10 (person reports to person/2).  Map each
 * (m2, m1) to (m1, m2), join with manages on m1, and report
 * (m1, (m2, p)): each person's manager's manager.  One worker, inputs at
 * time 0, consolidated and sorted output.
 */
#include "dtfl/collection.h"
#include "dtfl/arrange.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

#define SIZE 10

/* map (m2, m1) -> (m1, m2): swap the two u64 fields. */
static void
swap_pair(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[16];

	(void)ctx;
	(void)len;
	memcpy(buf, (const unsigned char *)rec + 8, 8);
	memcpy(buf + 8, rec, 8);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/*
 * join_map result for key m1 with values m2 and p: emit the whole
 * output record m1 || m2 || p, so the result reads (m1, (m2, p)).
 * join_map emits exactly the bytes the closure writes (the key is not
 * prepended), so the closure writes the key too.
 */
static void
skip_level(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[24];

	(void)ctx;
	(void)klen;
	(void)v1len;
	(void)v2len;
	memcpy(buf, key, 8);
	memcpy(buf + 8, v1, 8);
	memcpy(buf + 16, v2, 8);
	dtfl_join_emit_record(out, buf, sizeof buf);
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
	printf("(%llu, (%llu, %llu))\n",
	    (unsigned long long)dtfl_get_u64(p),
	    (unsigned long long)dtfl_get_u64(p + 8),
	    (unsigned long long)dtfl_get_u64(p + 16));
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
	const dtfl_type *pair = dtfl_type_pair(u64, u64);
	const dtfl_type *triple = dtfl_type_tuple(
	    (const dtfl_type *[]){ u64, u64, u64 }, 3);
	dtfl_collection *manages = dtfl_input_session_to_collection(c->manages,
	    scope);
	dtfl_collection *swapped = dtfl_collection_map(manages, pair, swap_pair,
	    DTFL_NOCTX);
	dtfl_collection *joined = dtfl_collection_join_map(swapped, manages,
	    u64, u64, u64, triple, skip_level, DTFL_NOCTX);
	dtfl_collection *con = dtfl_collection_consolidate(joined);
	dtfl_collection *ins = dtfl_collection_inspect(con, report, DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(joined);
	dtfl_collection_free(swapped);
	dtfl_collection_free(manages);
	dtfl_type_free(triple);
	dtfl_type_free(pair);
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
	dtfl_worker_dataflow(w, "manages", tc, build, &c);

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
