/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/prefix_sum.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dd/prefix_sum.c: end-to-end test for dtfl_algo_prefix_sum.
 *
 * Upstream prefix_sum.rs has no integration test of its own; its
 * correctness is exercised through its users.  This test builds a fixed
 * collection of ((index, key), data) records, runs the differential
 * prefix sum with addition as the associative combiner and zero 0, and
 * compares the resulting ((index, key), prefix) against a hand-written
 * sequential scan over the same input: for each element the prefix is
 * the sum of data at the same key with index strictly less than its own
 * (upstream's prefix_sum is an exclusive scan).  Two independent keys
 * share the dataflow to exercise the per-key independence the method
 * promises.
 */
#include "dtfl_test.h"

#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

/* The input elements: ((index, key), data). */
typedef struct elem {
	uint64_t index, key, data;
} elem;

/* addition, the associative combiner over u64 data. */
static void
add_combine(void *ctx, const void *key, size_t klen, const void *a,
    const void *b, void *out)
{
	uint64_t x, y;

	(void)ctx; (void)key; (void)klen;
	memcpy(&x, a, 8);
	memcpy(&y, b, 8);
	x += y;
	memcpy(out, &x, 8);
}

/*
 * The sequential oracle: for element i the prefix is the sum of data at
 * the same key with index strictly less than elems[i].index (upstream's
 * prefix_sum is an exclusive scan).  Fills out[i].
 */
static void
scan_oracle(const elem *elems, size_t n, uint64_t *out)
{
	size_t i, j;

	for (i = 0; i < n; i++) {
		uint64_t sum = 0;

		for (j = 0; j < n; j++)
			if (elems[j].key == elems[i].key &&
			    elems[j].index < elems[i].index)
				sum += elems[j].data;
		out[i] = sum;
	}
}

/* The differential result: ((index, key), prefix) records with diffs. */
typedef struct result {
	uint64_t index, key, prefix;
	int64_t diff;
} result;

typedef struct resultlog {
	result r[256];
	size_t n;
} resultlog;

static void
sink_log(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	resultlog *c = ctx;
	const unsigned char *p = rec;

	(void)time; (void)len;
	if (c->n >= 256)
		return;
	c->r[c->n].index = dtfl_get_u64(p);
	c->r[c->n].key = dtfl_get_u64(p + 8);
	c->r[c->n].prefix = dtfl_get_u64(p + 16);
	memcpy(&c->r[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* The net diff of ((index, key), prefix) across the result log. */
static int64_t
net_at(const resultlog *c, uint64_t index, uint64_t key, uint64_t prefix)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->r[i].index == index && c->r[i].key == key &&
		    c->r[i].prefix == prefix)
			s += c->r[i].diff;
	return s;
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	resultlog *out;
} build_ctx;

static void
build_prefix(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *in = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *ps = dtfl_algo_prefix_sum(in, 0, add_combine,
	    DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(ps, sink_log,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(ps);
	dtfl_collection_free(in);
}

static MunitResult
run_case(const elem *elems, size_t n)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *rdiff = dtfl_diff_i64();
	const dtfl_type *ik = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	const dtfl_type *rec = dtfl_type_pair(ik, dtfl_type_u64());
	build_ctx c;
	resultlog out;
	uint64_t oracle[256];
	uint64_t one = 1;
	size_t i;
	int steps;

	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(rec, rdiff);
	c.probe = dtfl_probe_new();
	c.out = &out;
	assert_int(dtfl_worker_dataflow(w, "prefix_sum", tc, build_prefix, &c),
	    ==, 0);

	for (i = 0; i < n; i++) {
		unsigned char buf[24];

		dtfl_put_u64(buf, elems[i].index);
		dtfl_put_u64(buf + 8, elems[i].key);
		dtfl_put_u64(buf + 16, elems[i].data);
		dtfl_input_session_insert(c.in, buf, 24);
	}
	dtfl_input_session_advance_to(c.in, &one);
	dtfl_input_session_flush(c.in);
	dtfl_input_session_close(c.in);

	for (steps = 0; steps < 2000000 && dtfl_worker_step(w); steps++)
		;
	assert_true(dtfl_probe_done(c.probe));
	assert_int(steps, <, 2000000);

	scan_oracle(elems, n, oracle);
	for (i = 0; i < n; i++)
		assert_int64(net_at(&out, elems[i].index, elems[i].key,
		    oracle[i]), ==, 1);
	/* No spurious prefix: the result multiset is exactly n elements. */
	{
		int64_t total = 0;

		for (i = 0; i < out.n; i++)
			total += out.r[i].diff;
		assert_int64(total, ==, (int64_t)n);
	}

	dtfl_probe_free(c.probe);
	dtfl_type_free(rec);
	dtfl_type_free(ik);
	dtfl_time_class_free(tc);
	dtfl_diff_free(rdiff);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/*
 * A single-key sequence 1,2,3,4,5 at indices 0..4: exclusive prefixes
 * 0,1,3,6,10.
 */
DTFL_TEST(prefix_sum__single_key)
{
	static const elem e[] = {
	    { 0, 7, 1 }, { 1, 7, 2 }, { 2, 7, 3 }, { 3, 7, 4 }, { 4, 7, 5 },
	};

	(void)params; (void)data;
	return run_case(e, sizeof e / sizeof e[0]);
}

/*
 * Two independent keys interleaved; each scans within its own key.
 */
DTFL_TEST(prefix_sum__two_keys)
{
	static const elem e[] = {
	    { 0, 1, 10 }, { 1, 1, 20 }, { 2, 1, 30 },
	    { 0, 2, 5 }, { 1, 2, 7 }, { 2, 2, 11 }, { 3, 2, 13 },
	};

	(void)params; (void)data;
	return run_case(e, sizeof e / sizeof e[0]);
}

/*
 * A sparse index set exercises the binary-interval requests at indices
 * that are not contiguous powers of two.
 */
DTFL_TEST(prefix_sum__sparse_index)
{
	static const elem e[] = {
	    { 0, 3, 2 }, { 1, 3, 4 }, { 5, 3, 8 }, { 6, 3, 16 }, { 9, 3, 32 },
	};

	(void)params; (void)data;
	return run_case(e, sizeof e / sizeof e[0]);
}

static MunitTest tests[] = {
	DTFL_T(prefix_sum__single_key),
	DTFL_T(prefix_sum__two_keys),
	DTFL_T(prefix_sum__sparse_index),
	DTFL_T_END
};

DTFL_TEST_MAIN("dd/prefix_sum", tests)
