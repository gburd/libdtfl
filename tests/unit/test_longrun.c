/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_longrun.c: a maintained view driven one tiny commit at a
 * time keeps its traces compact.
 *
 * Differential's work is proportional to the change only if every trace's
 * compaction frontiers follow its readers.  Each test builds a pipeline on a
 * standalone worker, feeds one insert or one remove per epoch into a base of
 * a few rows for EPOCHS epochs, and asserts that every arrangement in the
 * pipeline holds a logarithmic number of batches.  The regression: the
 * arrange operators kept a second, never-advanced reader of their own trace
 * (upstream arrangement.rs moves its only reader into the Arranged), which
 * pinned the physical frontier at the minimum, so the spine never merged and
 * every cursor walked one batch per epoch: per-commit cost grew linearly and
 * total cost quadratically.  No trace handle is held: the dataflow alone
 * must let its traces compact.
 */
#include "dtfl_test.h"
#include "dd/arrange_int.h"
#include "dd/trace/trace_int.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define EPOCHS 2000
#define WARM 500
/* Generous: a fuelled spine holds about two batches per level. */
#define MAX_BATCHES 64
/*
 * Live allocations may move by a bounded amount between epoch WARM and
 * epoch EPOCHS; a trace that keeps every batch grows by thousands.
 */
#define MAX_LIVE_GROWTH 1000

/*
 * Live-allocation counter behind the library's allocation hooks.  Each
 * test runs in munit's forked child, so the hooks are installed before the
 * library first allocates; under --no-fork a later test reuses them.
 */
static atomic_long live;

static void *
count_malloc(size_t size, void *ctx)
{
	void *p = malloc(size);	/* DTFL_RAW_OK: allocation hook under test */

	(void)ctx;
	if (p != NULL)
		atomic_fetch_add_explicit(&live, 1, memory_order_relaxed);
	return p;
}

static void *
count_realloc(void *ptr, size_t size, void *ctx)
{
	void *p = realloc(ptr, size);	/* DTFL_RAW_OK: allocation hook */

	(void)ctx;
	if (ptr == NULL && p != NULL)
		atomic_fetch_add_explicit(&live, 1, memory_order_relaxed);
	return p;
}

static void
count_free(void *ptr, void *ctx)
{
	(void)ctx;
	if (ptr != NULL)
		atomic_fetch_sub_explicit(&live, 1, memory_order_relaxed);
	free(ptr);	/* DTFL_RAW_OK: allocation hook under test */
}

static bool hooks_installed;

static void
install_hooks(void)
{
	static const dtfl_alloc_hooks h = { count_malloc, count_realloc,
		count_free, NULL };

	if (!hooks_installed)
		assert_int(dtfl_set_alloc_hooks(&h), ==, DTFL_OK);
	hooks_installed = true;
}

/*
 * S_ARRANGED: arrange_by_key -> reduce_abelian, both traces inspected.
 * S_JOIN_CORE: two arrange_by_key -> join_core, both traces inspected.
 * S_REDUCE, S_COUNT, S_JOIN: the collection-level reduce, count_total and
 * join_map, whose arrangements are internal; only live allocations show.
 */
enum shape { S_ARRANGED, S_JOIN_CORE, S_REDUCE, S_COUNT, S_JOIN };

typedef struct lr {
	dtfl_input_session *in1, *in2;
	dtfl_probe *probe;
	enum shape shape;
	/* The arrangements' trace boxes, retained for inspection. */
	__dtfl_trace_box *box[2];
	size_t nbox;
} lr;

static void
keep_box(lr *c, dtfl_arranged *a)
{
	c->box[c->nbox++] = __dtfl_trace_box_retain(
	    __dtfl_arranged_agent(a)->box);
}

static void
ident(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx;
	dtfl_collection_emit_record(out, rec, len);
}

/* The input multiset's total count, as one output value. */
static void
emit_count(size_t n, const void *diffs, dtfl_reduce_emit *out)
{
	int64_t c = 0, one = 1, d;
	unsigned char b[8];
	size_t i;

	for (i = 0; i < n; i++) {
		memcpy(&d, (const unsigned char *)diffs + 8 * i, 8);
		c += d;
	}
	dtfl_put_i64(b, c);
	dtfl_reduce_emit_pair(out, b, 8, &one);
}

static void
cnt_core(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, dtfl_reduce_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)in_vals; (void)in_vlens;
	(void)m; (void)out_vals; (void)out_vlens; (void)out_diffs;
	emit_count(n, in_diffs, out);
}

static void
cnt(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)vals; (void)vlens;
	emit_count(n, diffs, out);
}

static void
join_res(void *ctx, const void *key, size_t klen, const void *v1, size_t l1,
    const void *v2, size_t l2, dtfl_join_emit *out)
{
	unsigned char b[24];

	(void)ctx; (void)klen; (void)l1; (void)l2;
	memcpy(b, key, 8);
	memcpy(b + 8, v1, 8);
	memcpy(b + 16, v2, 8);
	dtfl_join_emit_record(out, b, 24);
}

static void
build(dtfl_scope *scope, void *arg)
{
	lr *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *kv = dtfl_type_pair(u64, u64);
	const dtfl_type *ki = dtfl_type_pair(u64, dtfl_type_i64());
	const dtfl_type *f3[3] = { u64, u64, u64 };
	const dtfl_type *trip = dtfl_type_tuple(f3, 3);
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *in = dtfl_input_session_to_collection(c->in1, scope);
	dtfl_collection *in2 = dtfl_input_session_to_collection(c->in2, scope);
	dtfl_collection *m = dtfl_collection_map(in, kv, ident, DTFL_NOCTX);
	dtfl_collection *out = NULL, *p;

	switch (c->shape) {
	case S_ARRANGED: {
		dtfl_arranged *a = dtfl_collection_arrange_by_key(m, u64, u64,
		    "arr");
		dtfl_arranged *r = dtfl_arranged_reduce_abelian(a, "cnt",
		    dtfl_type_i64(), i64, cnt_core, DTFL_NOCTX);

		keep_box(c, a);
		keep_box(c, r);
		out = dtfl_arranged_as_collection(r, ki, NULL, DTFL_NOCTX);
		dtfl_arranged_free(r);
		dtfl_arranged_free(a);
		break;
	}
	case S_JOIN_CORE: {
		dtfl_arranged *a1 = dtfl_collection_arrange_by_key(m, u64, u64,
		    "a1");
		dtfl_arranged *a2 = dtfl_collection_arrange_by_key(in2, u64,
		    u64, "a2");

		keep_box(c, a1);
		keep_box(c, a2);
		out = dtfl_arranged_join_core(a1, a2, trip, join_res,
		    DTFL_NOCTX);
		dtfl_arranged_free(a2);
		dtfl_arranged_free(a1);
		break;
	}
	case S_REDUCE:
		out = dtfl_collection_reduce(m, u64, u64, dtfl_type_i64(), i64,
		    cnt, DTFL_NOCTX);
		break;
	case S_COUNT:
		out = dtfl_collection_count_total(m, i64);
		break;
	case S_JOIN:
		out = dtfl_collection_join_map(m, in2, u64, u64, u64, trip,
		    join_res, DTFL_NOCTX);
		break;
	}
	p = dtfl_collection_probe_with(out, c->probe);
	dtfl_collection_free(p);
	dtfl_collection_free(out);
	dtfl_collection_free(m);
	dtfl_collection_free(in2);
	dtfl_collection_free(in);
	dtfl_diff_free(i64);
	dtfl_type_free(trip);
	dtfl_type_free(ki);
	dtfl_type_free(kv);
}

static MunitResult
run(enum shape shape)
{
	const dtfl_type *u64, *kv;
	dtfl_diff *i64;
	dtfl_time_class *tc;
	dtfl_worker *w;
	size_t most[2] = { 0, 0 };
	long live_warm = 0, live_end;
	uint64_t e, t;
	size_t i;
	lr c;

	install_hooks();
	u64 = dtfl_type_u64();
	kv = dtfl_type_pair(u64, u64);
	i64 = dtfl_diff_i64();
	tc = dtfl_time_u64();
	w = dtfl_worker_new_standalone();
	memset(&c, 0, sizeof c);
	c.shape = shape;
	c.in1 = dtfl_input_session_new(kv, i64);
	c.in2 = dtfl_input_session_new(kv, i64);
	c.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "longrun", tc, build, &c), ==, 0);

	for (i = 0; i < 4; i++) {
		unsigned char b[16];

		dtfl_put_u64(b, i);
		dtfl_put_u64(b + 8, 100 + i);
		dtfl_input_session_insert(c.in1, b, 16);
		dtfl_input_session_insert(c.in2, b, 16);
	}
	for (e = 0; e < EPOCHS; e++) {
		unsigned char b[16];

		/* Odd epochs remove what the even epoch before inserted. */
		dtfl_put_u64(b, (e / 2) % 4);
		dtfl_put_u64(b + 8, 1000 + (e / 2) % 7);
		if (e % 2 == 0)
			dtfl_input_session_insert(c.in1, b, 16);
		else
			dtfl_input_session_remove(c.in1, b, 16);
		t = e + 1;
		dtfl_input_session_advance_to(c.in1, &t);
		dtfl_input_session_advance_to(c.in2, &t);
		dtfl_input_session_flush(c.in1);
		dtfl_input_session_flush(c.in2);
		while (dtfl_probe_less_than(c.probe, &t))
			dtfl_worker_step(w);
		for (i = 0; i < c.nbox; i++) {
			size_t n = __dtfl_trace_batch_count(c.box[i]->trace);

			if (n > most[i])
				most[i] = n;
		}
		if (e + 1 == WARM)
			live_warm = atomic_load(&live);
	}
	live_end = atomic_load(&live);
	munit_logf(MUNIT_LOG_INFO, "live allocations: %ld at epoch %d, %ld "
	    "at epoch %d", live_warm, WARM, live_end, EPOCHS);
	assert_long(live_end - live_warm, <=, MAX_LIVE_GROWTH);
	for (i = 0; i < c.nbox; i++) {
		munit_logf(MUNIT_LOG_INFO, "trace %zu: at most %zu batches",
		    i, most[i]);
		assert_size(most[i], <=, MAX_BATCHES);
	}

	dtfl_input_session_close(c.in1);
	dtfl_input_session_close(c.in2);
	while (dtfl_worker_step(w))
		;
	for (i = 0; i < c.nbox; i++)
		__dtfl_trace_box_release(c.box[i]);
	dtfl_probe_free(c.probe);
	dtfl_worker_free(w);
	dtfl_time_class_free(tc);
	dtfl_diff_free(i64);
	dtfl_type_free(kv);
	return MUNIT_OK;
}

DTFL_TEST(longrun__arranged_reduce_traces_stay_compact)
{
	(void)params; (void)data;
	return run(S_ARRANGED);
}

DTFL_TEST(longrun__reduce_stays_bounded)
{
	(void)params; (void)data;
	return run(S_REDUCE);
}

DTFL_TEST(longrun__count_total_stays_bounded)
{
	(void)params; (void)data;
	return run(S_COUNT);
}

static MunitTest tests[] = {
	DTFL_T(longrun__arranged_reduce_traces_stay_compact),
	DTFL_T(longrun__reduce_stays_bounded),
	DTFL_T(longrun__count_total_stays_bounded),
	DTFL_T_END
};

DTFL_TEST_MAIN("longrun", tests)
