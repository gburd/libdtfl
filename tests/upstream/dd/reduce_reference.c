/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/tests/reduce_reference.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dd/reduce_reference.c: the oracle_* family of the reduce
 * reference harness.
 *
 * The upstream file has four families.  Three of them (differential_*,
 * bfs_*, scc_*) cross-check the default cursor reduce against a second,
 * model-derived tactic, reduce_trace_reference, which this port does not
 * have; those rows stay todo (see dev/test-ledger.tsv).  The oracle_* family
 * does NOT need the second tactic: it runs ONE reduce (count / sum / min)
 * and checks its output against a from-scratch recomputation of
 * f(accumulated input) at a rich set of query times.  This is the executable
 * form of the stream-correctness statement.
 *
 * Time-type divergence: upstream runs the oracle over Pair<u64,u64> (a
 * product partial order) fed through new_unordered_input, which the C public
 * API does not expose; the input session emits at a single totally ordered
 * epoch.  So this port runs the identical count/sum/min oracle checks over
 * u64 time: feed random (key, val) updates at random u64 epochs with diffs
 * +/-1 (so updates cancel), capture the reduce output, and assert
 * acc(output, q) == f(acc(input, q)) at every input time, every output time,
 * and their pairwise joins (max over u64).  The property is time-class
 * independent, so the u64 instance exercises the same correctness statement.
 */
#include "dtfl_test.h"

#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Random input: ((key, val), time, diff), diffs +/-1 so updates cancel. */
/* ------------------------------------------------------------------ */

#define N_UPDATES 300
#define KEYS 10
#define VALS 8
#define SPAN 6

typedef struct in_upd {
	uint64_t key, val, time;
	int64_t diff;
} in_upd;

/* A small deterministic PRNG (splitmix64); the oracle holds for any input. */
static uint64_t
splitmix64(uint64_t *state)
{
	uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

static void
random_input(unsigned seed, in_upd *out)
{
	uint64_t st = 0x1234567ULL + (uint64_t)seed * 0x100000001ULL;
	size_t i;

	for (i = 0; i < N_UPDATES; i++) {
		out[i].key = splitmix64(&st) % KEYS;
		out[i].val = splitmix64(&st) % VALS;
		out[i].time = splitmix64(&st) % SPAN;
		out[i].diff = (splitmix64(&st) & 1) ? 1 : -1;
	}
}

/* ------------------------------------------------------------------ */
/* The reduce logics under test (count / sum / min).                   */
/* ------------------------------------------------------------------ */

typedef enum logic_kind { LOGIC_COUNT, LOGIC_SUM, LOGIC_MIN } logic_kind;

/*
 * count: output value = number of distinct accumulated values (input.len());
 * sum: output value = sum of val*diff; min: output value = the least value.
 * All emit the value as an i64 with output diff +1.  The input array is
 * already consolidated (one entry per value, non-zero net diff) and sorted
 * ascending by value, so vals[0] is the minimum.
 */
static void
reduce_logic(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	logic_kind kind = (logic_kind)(intptr_t)ctx;
	const int64_t *d = diffs;
	int64_t result = 0, one = 1;
	unsigned char vbuf[8];
	size_t i;

	(void)key; (void)klen; (void)vlens;
	if (n == 0)
		return;
	switch (kind) {
	case LOGIC_COUNT:
		result = (int64_t)n;
		break;
	case LOGIC_SUM:
		for (i = 0; i < n; i++)
			result += (int64_t)dtfl_get_u64(vals[i]) * d[i];
		break;
	case LOGIC_MIN:
		result = (int64_t)dtfl_get_u64(vals[0]);
		break;
	}
	dtfl_put_i64(vbuf, result);
	dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &one);
}

/* ------------------------------------------------------------------ */
/* Capture the reduce output as ((key, out_val), time, diff) updates.  */
/* ------------------------------------------------------------------ */

#define MAX_OUT 4096

typedef struct out_upd {
	uint64_t key;
	int64_t out_val;
	uint64_t time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAX_OUT];
	size_t n;
} collected;

/* The reduce output record is key(u64) || out_val(i64); the time trails it. */
static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	uint64_t t;
	const unsigned char *r = rec;

	(void)len;
	if (c->n >= MAX_OUT)
		return;
	c->u[c->n].key = dtfl_get_u64(r);
	c->u[c->n].out_val = dtfl_get_i64(r + 8);
	memcpy(&t, time, sizeof t);
	c->u[c->n].time = t;
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* as_collection would be implicit; the reduce output record is key || val. */

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
	logic_kind kind;
} build_ctx;

static void
build_reduce(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *i64 = dtfl_type_i64();
	dtfl_diff *odiff = dtfl_diff_i64();
	dtfl_collection *coll = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *red = dtfl_collection_reduce(coll, u64, u64, i64,
	    odiff, reduce_logic, DTFL_CTX((void *)(intptr_t)c->kind, NULL));
	dtfl_collection *ins = dtfl_collection_inspect(red, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_diff_free(odiff);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(red);
	dtfl_collection_free(coll);
}

/* ------------------------------------------------------------------ */
/* The from-scratch oracle: f(accumulated input) at a query time.      */
/* ------------------------------------------------------------------ */

/* An expected (key, out_val) -> net diff entry. */
typedef struct kv_diff {
	uint64_t key;
	int64_t out_val;
	int64_t diff;
} kv_diff;

typedef struct kv_map {
	kv_diff e[MAX_OUT];
	size_t n;
} kv_map;

static void
kv_map_add(kv_map *m, uint64_t key, int64_t out_val, int64_t diff)
{
	size_t i;

	for (i = 0; i < m->n; i++)
		if (m->e[i].key == key && m->e[i].out_val == out_val) {
			m->e[i].diff += diff;
			return;
		}
	if (m->n < MAX_OUT) {
		m->e[m->n].key = key;
		m->e[m->n].out_val = out_val;
		m->e[m->n].diff = diff;
		m->n++;
	}
}

/* Net diff of (key, out_val) in the map (0 if absent). */
static int64_t
kv_map_get(const kv_map *m, uint64_t key, int64_t out_val)
{
	size_t i;

	for (i = 0; i < m->n; i++)
		if (m->e[i].key == key && m->e[i].out_val == out_val)
			return m->e[i].diff;
	return 0;
}

/*
 * expected: for every key, accumulate the input values at times <= q, keep
 * values with non-zero net diff, apply the logic to that consolidated
 * multiset, and record (key, out_val) -> emitted diff.
 */
static void
compute_expected(const in_upd *updates, size_t nu, uint64_t q, logic_kind kind,
    kv_map *expected)
{
	uint64_t k;

	expected->n = 0;
	for (k = 0; k < KEYS; k++) {
		int64_t acc[VALS];
		int64_t result = 0;
		size_t distinct = 0, i;
		bool any = false;
		uint64_t minval = 0;
		bool have_min = false;

		memset(acc, 0, sizeof acc);
		for (i = 0; i < nu; i++)
			if (updates[i].key == k && updates[i].time <= q)
				acc[updates[i].val] += updates[i].diff;
		for (i = 0; i < VALS; i++) {
			if (acc[i] == 0)
				continue;
			any = true;
			distinct++;
			result += (int64_t)i * acc[i];
			if (!have_min) {
				minval = i;
				have_min = true;
			}
		}
		if (!any)
			continue;
		switch (kind) {
		case LOGIC_COUNT:
			kv_map_add(expected, k, (int64_t)distinct, 1);
			break;
		case LOGIC_SUM:
			kv_map_add(expected, k, result, 1);
			break;
		case LOGIC_MIN:
			kv_map_add(expected, k, (int64_t)minval, 1);
			break;
		}
	}
}

/* actual: accumulate the captured reduce output at times <= q. */
static void
compute_actual(const collected *out, uint64_t q, kv_map *actual)
{
	size_t i;

	actual->n = 0;
	for (i = 0; i < out->n; i++)
		if (out->u[i].time <= q)
			kv_map_add(actual, out->u[i].key, out->u[i].out_val,
			    out->u[i].diff);
}

/* Asserts the two maps agree on every non-zero (key, out_val). */
static void
assert_maps_equal(const kv_map *expected, const kv_map *actual, unsigned seed,
    uint64_t q)
{
	size_t i;

	for (i = 0; i < expected->n; i++)
		if (expected->e[i].diff != 0)
			munit_assert_int64(expected->e[i].diff, ==,
			    kv_map_get(actual, expected->e[i].key,
			    expected->e[i].out_val));
	for (i = 0; i < actual->n; i++)
		if (actual->e[i].diff != 0)
			munit_assert_int64(actual->e[i].diff, ==,
			    kv_map_get(expected, actual->e[i].key,
			    actual->e[i].out_val));
	(void)seed; (void)q;
}

/* ------------------------------------------------------------------ */
/* The oracle driver: feed one seed, reduce, check at every query time. */
/* ------------------------------------------------------------------ */

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

/* Feeds the updates grouped by epoch 0 .. SPAN, flushing between epochs. */
static void
feed(dtfl_worker *w, build_ctx *c, const in_upd *updates)
{
	uint64_t t;

	for (t = 0; t < SPAN; t++) {
		size_t i;
		uint64_t next = t + 1;
		probe_cond pc = { c->probe, next };

		for (i = 0; i < N_UPDATES; i++)
			if (updates[i].time == t) {
				unsigned char rec[16];
				int64_t d = updates[i].diff;

				dtfl_put_u64(rec, updates[i].key);
				dtfl_put_u64(rec + 8, updates[i].val);
				dtfl_input_session_update(c->in, rec, 16, &d);
			}
		dtfl_input_session_advance_to(c->in, &next);
		dtfl_input_session_flush(c->in);
		dtfl_worker_step_while(w, probe_behind, &pc);
	}
	dtfl_input_session_close(c->in);
	while (dtfl_worker_step(w))
		;
}

static void
run_oracle(unsigned seed, logic_kind kind)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *rec_t = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	build_ctx c;
	collected out;
	static in_upd updates[N_UPDATES];
	uint64_t q;

	memset(&out, 0, sizeof out);
	random_input(seed, updates);

	c.in = dtfl_input_session_new(rec_t, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.kind = kind;
	munit_assert_int(dtfl_worker_dataflow(w, "reduce", tc, build_reduce,
	    &c), ==, 0);
	feed(w, &c, updates);

	/* Check the accumulation identity at every u64 time 0 .. SPAN. */
	for (q = 0; q <= SPAN; q++) {
		kv_map expected, actual;

		compute_expected(updates, N_UPDATES, q, kind, &expected);
		compute_actual(&out, q, &actual);
		assert_maps_equal(&expected, &actual, seed, q);
	}

	dtfl_probe_free(c.probe);
	dtfl_type_free(rec_t);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
}

DTFL_TEST(reduce_reference__oracle_cursor_count)
{
	unsigned s;

	(void)params; (void)data;
	for (s = 0; s < 15; s++)
		run_oracle(s, LOGIC_COUNT);
	return MUNIT_OK;
}

DTFL_TEST(reduce_reference__oracle_reference_count)
{
	unsigned s;

	(void)params; (void)data;
	/*
	 * No reduce_trace_reference tactic exists in this port, so the
	 * reference oracle runs the same default reduce as oracle_cursor_count
	 * (the oracle checks the tactic against f(acc input), not against a
	 * second tactic).  The count oracle is exercised either way.
	 */
	for (s = 0; s < 15; s++)
		run_oracle(s, LOGIC_COUNT);
	return MUNIT_OK;
}

DTFL_TEST(reduce_reference__oracle_reference_sum)
{
	unsigned s;

	(void)params; (void)data;
	for (s = 0; s < 15; s++)
		run_oracle(s, LOGIC_SUM);
	return MUNIT_OK;
}

DTFL_TEST(reduce_reference__oracle_reference_min)
{
	unsigned s;

	(void)params; (void)data;
	for (s = 0; s < 15; s++)
		run_oracle(s, LOGIC_MIN);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(reduce_reference__oracle_cursor_count),
	DTFL_T(reduce_reference__oracle_reference_count),
	DTFL_T(reduce_reference__oracle_reference_sum),
	DTFL_T(reduce_reference__oracle_reference_min),
	DTFL_T_END
};

DTFL_TEST_MAIN("reduce_reference", tests)
