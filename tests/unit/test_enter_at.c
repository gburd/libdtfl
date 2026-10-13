/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_enter_at.c: dtfl_collection_enter_at places each
 * record into a nested iterative subscope at a per-record initial inner
 * time (Collection::enter_at, collection.rs).  The test enters a few
 * records with distinct computed inner times, inspects them inside the
 * subscope, and checks each appears at its computed inner coordinate.
 */
#include "dtfl_test.h"

#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 64

typedef struct seen {
	uint64_t rec;
	uint64_t outer;
	uint64_t inner;
} seen;

typedef struct collected {
	seen s[MAX_OUT];
	size_t n;
} collected;

/*
 * Inside the subscope the embedded time is Product<u64, u64>: time[0]
 * is the refined outer coordinate, time[1] the inner coordinate set by
 * enter_at.
 */
static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;

	(void)diff;
	if (c->n >= MAX_OUT || len < 8)
		return;
	c->s[c->n].rec = dtfl_get_u64(rec);
	c->s[c->n].outer = time[0];
	c->s[c->n].inner = time[1];
	c->n++;
}

/* The per-record initial inner time: 256 * node, so each record lands
 * at a distinct, non-minimum inner coordinate. */
static void
initial_time(void *ctx, const void *rec, size_t len, uint64_t *inner_time_out)
{
	(void)ctx; (void)len;
	inner_time_out[0] = 256 * dtfl_get_u64(rec);
}

typedef struct enter_at_build {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
} enter_at_build;

typedef struct inner_ctx {
	dtfl_collection *source;	/* outer-scope source, not owned */
	enter_at_build *b;
} inner_ctx;

static void
build_inner(dtfl_scope *inner, void *arg)
{
	inner_ctx *ic = arg;
	dtfl_collection *entered = dtfl_collection_enter_at(ic->source, inner,
	    initial_time, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(entered, sink,
	    DTFL_CTX(ic->b->out, NULL));
	dtfl_collection *leaving = dtfl_collection_leave(ins);
	dtfl_collection *probed = dtfl_collection_probe_with(leaving,
	    ic->b->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(leaving);
	dtfl_collection_free(ins);
	dtfl_collection_free(entered);
}

static void
build_outer(dtfl_scope *scope, void *arg)
{
	enter_at_build *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	inner_ctx ic = { base, c };

	dtfl_scope_iterative(scope, "EnterAtScope", build_inner, &ic);
	dtfl_collection_free(base);
}

DTFL_TEST(enter_at__per_record_initial_time)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	enter_at_build c;
	collected out;
	uint64_t i, one = 1;
	int steps;
	static const uint64_t nodes[] = { 1, 2, 5, 7 };
	size_t ni;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "enter_at", tc, build_outer, &c),
	    ==, 0);

	for (ni = 0; ni < sizeof nodes / sizeof nodes[0]; ni++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, nodes[ni]);
		dtfl_input_session_insert(c.in, buf, 8);
	}
	dtfl_input_session_advance_to(c.in, &one);
	dtfl_input_session_flush(c.in);
	dtfl_input_session_close(c.in);

	/* Bound the run so a non-terminating loop fails rather than hangs. */
	for (steps = 0; steps < 100000 && dtfl_worker_step(w); steps++)
		;
	assert_true(dtfl_probe_done(c.probe));
	assert_int(steps, <, 100000);

	/* Each record must appear once, at inner time 256 * node and outer 0. */
	assert_size(out.n, ==, sizeof nodes / sizeof nodes[0]);
	for (ni = 0; ni < sizeof nodes / sizeof nodes[0]; ni++) {
		bool found = false;

		for (i = 0; i < out.n; i++) {
			if (out.s[i].rec == nodes[ni]) {
				assert_uint64(out.s[i].outer, ==, 0);
				assert_uint64(out.s[i].inner, ==,
				    256 * nodes[ni]);
				found = true;
			}
		}
		assert_true(found);
	}

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(enter_at__per_record_initial_time),
	DTFL_T_END,
};

DTFL_TEST_MAIN("enter_at", tests)
