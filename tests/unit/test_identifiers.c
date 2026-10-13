/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/identifiers.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/unit/test_identifiers.c: unique identifier assignment.
 *
 * algorithms_identifiers__are_unique feeds a small collection and runs
 * dtfl_algo_identifiers, then asserts that no two input records receive
 * the same identifier.  Upstream tests this with a crippled hash to force
 * collisions and confirm every element still gets a distinct id; this
 * port exercises the real hash over a small input and checks the same
 * property: the multiset of assigned ids has no duplicate.
 */
#include "dtfl_test.h"

#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAXOUT 256

typedef struct outrec {
	uint64_t rec;
	uint64_t id;
	int64_t diff;
} outrec;

typedef struct collected {
	outrec u[MAXOUT];
	size_t n;
} collected;

/* identifiers emits (record, u64 id); the record here is a u64. */
static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	const unsigned char *p = rec;

	(void)time; (void)len;
	if (c->n >= MAXOUT)
		return;
	c->u[c->n].rec = dtfl_get_u64(p);
	c->u[c->n].id = dtfl_get_u64(p + 8);
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
} build_ctx;

static void
build_ids(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *ids = dtfl_algo_identifiers(base, dtfl_type_u64());
	dtfl_collection *con = dtfl_collection_consolidate(ids);
	dtfl_collection *ins = dtfl_collection_inspect(con, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(ids);
	dtfl_collection_free(base);
}

DTFL_TEST(algorithms_identifiers__are_unique)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *rdiff = dtfl_diff_i64();
	build_ctx c;
	collected out;
	uint64_t one = 1, v;
	size_t i, j, live = 0;
	int steps;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), rdiff);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "ids", tc, build_ids, &c), ==, 0);

	for (v = 1; v < 10; v++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, v);
		dtfl_input_session_insert(c.in, buf, 8);
	}
	dtfl_input_session_advance_to(c.in, &one);
	dtfl_input_session_flush(c.in);
	dtfl_input_session_close(c.in);

	for (steps = 0; steps < 1000000 && dtfl_worker_step(w); steps++)
		;
	assert_true(dtfl_probe_done(c.probe));
	assert_int(steps, <, 1000000);

	/* Every input record 1..9 receives exactly one surviving id, and no
	 * two records share an id (ids are unique). */
	for (i = 0; i < out.n; i++)
		if (out.u[i].diff > 0)
			live++;
	assert_size(live, ==, 9);
	for (i = 0; i < out.n; i++) {
		if (out.u[i].diff <= 0)
			continue;
		for (j = i + 1; j < out.n; j++) {
			if (out.u[j].diff <= 0)
				continue;
			assert_true(out.u[i].id != out.u[j].id);
		}
	}

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(rdiff);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(algorithms_identifiers__are_unique),
	DTFL_T_END
};

DTFL_TEST_MAIN("identifiers", tests)
