/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_arrange_lifetime.c: the batch-pointer stream owns its
 * batch reference for the lifetime of the stream message.
 *
 * dtfl_collection_reduce builds reduce -> as_collection: the reduce
 * operator seals an output batch, commits it to its shared output trace,
 * and sends the same batch pointer down a Pipeline edge to the
 * as_collection operator.  The stream record must own a reference to that
 * batch (the arena's elem drop/retain hooks, set per batch-pointer
 * session), so the batch survives even when the shared spine merges and
 * frees the trace's own reference, and so an emitted batch that no
 * consumer reads is still released once (no leak).  This drives many
 * distinct keys across many epochs and checks, under ASan + LSan, that
 * the run completes with no use-after-free and no leaked batch.  The exact
 * merge-while-cursoring timing of the original bug is reproduced by the
 * explain scc sufficiency tests (tests/upstream/ddir/explain.c); this is
 * the fast in-component guard for the retain/release pairing itself,
 * including the leak direction (a retain with no matching drop).
 */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define KEYS 64
#define EPOCHS 24

typedef struct seen {
	uint64_t sum;		/* XOR of (key, count, time, diff) seen */
	size_t n;		/* records inspected */
} seen;

static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	seen *s = ctx;
	int64_t d;

	(void)len;
	memcpy(&d, diff, sizeof d);
	s->sum ^= dtfl_get_u64(rec) + time[0] * 1000003u + (uint64_t)d;
	s->n++;
}

/* count reducer: emit the number of distinct values with diff 1. */
static void
count_logic(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;
	unsigned char vbuf[8];

	(void)ctx; (void)key; (void)klen; (void)vals; (void)vlens; (void)diffs;
	dtfl_put_u64(vbuf, (uint64_t)n);
	dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &one);
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	seen *out;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_diff *od = dtfl_diff_i64();
	dtfl_collection *red = dtfl_collection_reduce(base, dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_u64(), od, count_logic, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(red, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(red);
	dtfl_diff_free(od);
	dtfl_collection_free(base);
}

typedef struct probe_cond {
	dtfl_probe *probe;
	uint64_t target;
} probe_cond;

static bool
probe_behind(void *vctx)
{
	probe_cond *pc = vctx;

	return dtfl_probe_less_than(pc->probe, &pc->target);
}

/*
 * Each epoch adds one distinct value to every key, so each key's count
 * grows and reduce retracts the old count and asserts the new one at a new
 * time.  That produces a steady stream of output batches at distinct times
 * into one shared output trace, which is exactly the fuelled-merge shape
 * that frees an earlier batch an as_collection cursor still reads.
 */
DTFL_TEST(arrange_lifetime__reduce_as_collection_survives_merge)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *f[2] = { dtfl_type_u64(), dtfl_type_u64() };
	const dtfl_type *rt = dtfl_type_tuple(f, 2);
	build_ctx c;
	seen out;
	unsigned char kv[16];
	uint64_t epoch;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(rt, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "dd", tc, build, &c), ==, 0);

	for (epoch = 0; epoch < EPOCHS; epoch++) {
		uint64_t key;
		uint64_t next = epoch + 1;
		probe_cond pc;

		for (key = 0; key < KEYS; key++) {
			dtfl_put_u64(kv, key);
			dtfl_put_u64(kv + 8, epoch);	/* a new value each epoch */
			dtfl_input_session_insert(c.in, kv, sizeof kv);
		}
		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		pc.probe = c.probe;
		pc.target = next;
		dtfl_worker_step_while(w, probe_behind, &pc);
	}

	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	/* The run completed and the inspector saw output (no UAF, no leak). */
	assert_size(out.n, >, 0);

	dtfl_probe_free(c.probe);
	dtfl_type_free(rt);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(arrange_lifetime__reduce_as_collection_survives_merge),
	{ 0 },
};

DTFL_TEST_MAIN("arrange_lifetime", tests)
