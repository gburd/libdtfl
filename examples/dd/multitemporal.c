/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/multitemporal.c: mirrors
 * differential-dataflow/examples/multitemporal.rs.
 *
 * A multi-temporal histogram over a Pair<u64, u64> timestamp, whose
 * order is the product partial order (two incomparable coordinates), not
 * a total order.  Upstream drives it from an interactive command stream;
 * this port feeds a fixed, deterministic set of (value, Pair-time, diff)
 * updates and then answers a fixed set of queries, so its output can be
 * checked against a hand-computed result.
 *
 * The dataflow is upstream's exactly:
 *     data.as_collection()
 *         .count()                       // (value, count)
 *         .map(|(_value, count)| count)  // keep the count
 *         .arrange_by_self();            // histogram of counts
 * The arrangement's trace is kept and queried at distinct Pair times.
 *
 * Because the Pair time is a partial order, the ordered dtfl_input_session
 * cannot feed it (dtfl_input_to_stream needs a totally ordered scope).
 * The input is instead an unordered input over the differential update
 * record (data bytes, then the scope's time words, then the diff), which
 * dtfl_stream_as_collection views as a collection; this is the C form of
 * timely's new_unordered_input followed by differential's AsCollection.
 * The update record here is fixed size: an i64 value, two u64 time
 * coordinates and an i64 diff, 32 bytes, laid out with the public put/get
 * helpers.  A single capability at the minimum time (0, 0) sends every
 * update, since (0, 0) precedes every chosen time in the product order.
 *
 * A query at Pair time q reports, for each distinct count, how many
 * values have that count at q, by summing the diff of every (count, time,
 * diff) entry of the trace whose time is <= q in the product order
 * (upstream's cursor.map_times filter).  The trace is enumerated with
 * dtfl_trace_to_vec_through at the empty frontier, which selects the whole
 * settled trace (TraceReader::cursor), matching upstream's trace.cursor().
 *
 * The upstream Pair's PartialOrder is the product order
 * (first.less_equal && second.less_equal); dtfl_time_product gives exactly
 * that.  dtfl_time_lex, the other public Pair time, is lexicographic and
 * total, which is a different order and would answer these queries wrong.
 * Upstream's Pair components are isize; this port uses u64 coordinates,
 * which agree with isize on the non-negative times the example uses.
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/reduce.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The fixed update set: (value, (time1, time2), diff).  Value 111 appears
 * at the incomparable Pair coordinates (1, 1) and (2, 0) as well as the
 * origin, the case that needs the partial-order input bridge.
 */
typedef struct update {
	int64_t value;
	uint64_t time1;
	uint64_t time2;
	int64_t diff;
} update;

static const update UPDATES[] = {
	{ 0, 0, 0, 1 },
	{ 111, 0, 0, 1 },
	{ 111, 1, 1, 2 },
	{ 111, 2, 0, -1 },
	{ 222, 0, 0, 1 },
};
#define NUPDATES (sizeof UPDATES / sizeof UPDATES[0])

/* The fixed query times, each a Pair (time1, time2). */
static const uint64_t QUERIES[][2] = {
	{ 0, 0 }, { 1, 1 }, { 2, 0 }, { 2, 1 }, { 2, 2 },
};
#define NQUERIES (sizeof QUERIES / sizeof QUERIES[0])

/*
 * map (value, count) -> count.  The count operator's output record is the
 * tuple (i64 value, i64 count); keep just the count, at byte offset 8.
 */
static void
keep_count(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 8, 8);
}

/* Per-query accumulator: a count value and its summed multiplicity. */
typedef struct bucket {
	int64_t count;
	int64_t sum;
} bucket;

#define MAXBUCKETS 64

typedef struct query_acc {
	const dtfl_time_class *tc;
	const uint64_t *q;		/* the query Pair time */
	bucket buckets[MAXBUCKETS];
	size_t nbuckets;
} query_acc;

/*
 * One (count, time, diff) trace entry.  Fold it into the per-count sum,
 * but only where time <= q in the product order (upstream map_times).
 */
static void
query_fold(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, const uint64_t *time, const void *diff)
{
	query_acc *a = ctx;
	int64_t count, d;
	size_t i;

	(void)klen; (void)val; (void)vlen;
	if (!dtfl_time_less_equal(a->tc, time, a->q))
		return;
	count = dtfl_get_i64(key);
	memcpy(&d, diff, sizeof d);	/* diff bytes are native, not encoded */
	for (i = 0; i < a->nbuckets; i++)
		if (a->buckets[i].count == count) {
			a->buckets[i].sum += d;
			return;
		}
	/* The histogram has one bucket per distinct count; a few suffice. */
	a->buckets[a->nbuckets].count = count;
	a->buckets[a->nbuckets].sum = d;
	a->nbuckets++;
}

static int
cmp_bucket(const void *x, const void *y)
{
	const bucket *p = x, *q = y;

	if (p->count != q->count)
		return p->count < q->count ? -1 : 1;
	return 0;
}

/* The trace survives the build scope so the worker can query it. */
typedef struct build_ctx {
	dtfl_unordered_input *input;
	dtfl_cap *cap;
	dtfl_diff *diff;
	dtfl_probe *probe;
	dtfl_trace *trace;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *i64 = dtfl_type_i64();
	const dtfl_type *u64 = dtfl_type_u64();
	/*
	 * The update record type: (i64 value, u64 time1, u64 time2, i64 diff),
	 * the data record followed by the scope's two time words and the diff
	 * (what dtfl_stream_as_collection expects).  All fields are fixed, so
	 * the tuple is a fixed 32-byte record.
	 */
	const dtfl_type *upd_fields[4] = { i64, u64, u64, i64 };
	const dtfl_type *upd = dtfl_type_tuple(upd_fields, 4);
	dtfl_stream *st = dtfl_unordered_input_new(scope, upd, &c->input,
	    &c->cap);
	dtfl_collection *data = dtfl_stream_as_collection(st, i64,
	    dtfl_diff_retain(c->diff));
	dtfl_collection *counted = dtfl_collection_count(data, c->diff);
	dtfl_collection *counts = dtfl_collection_map(counted, i64, keep_count,
	    DTFL_NOCTX);
	dtfl_arranged *arr = dtfl_collection_arrange(counts, "arrange_by_self");
	/* Read the arrangement back so a probe says the input has drained. */
	dtfl_collection *as_coll = dtfl_arranged_as_collection(arr, i64, NULL,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(as_coll, c->probe);

	c->trace = dtfl_arranged_trace(arr);

	dtfl_collection_free(probed);
	dtfl_collection_free(as_coll);
	dtfl_arranged_free(arr);
	dtfl_collection_free(counts);
	dtfl_collection_free(counted);
	dtfl_collection_free(data);
	dtfl_type_free(upd);
}

/* Lay out and send one update record under the input's capability. */
static void
send_update(build_ctx *c, const update *u)
{
	unsigned char buf[32];

	dtfl_put_i64(buf, u->value);
	memcpy(buf + 8, &u->time1, sizeof u->time1);
	memcpy(buf + 16, &u->time2, sizeof u->time2);
	memcpy(buf + 24, &u->diff, sizeof u->diff);
	dtfl_unordered_input_send(c->input, c->cap, buf, sizeof buf);
}

/* Report the histogram at query time (q[0], q[1]); returns the bucket set. */
static size_t
run_query(build_ctx *c, const dtfl_time_class *tc, const uint64_t *q,
    bucket *out)
{
	query_acc a;
	size_t i, k;

	memset(&a, 0, sizeof a);
	a.tc = tc;
	a.q = q;
	/* The empty frontier selects the whole settled trace (cursor). */
	dtfl_trace_to_vec_through(c->trace, NULL, 0, query_fold,
	    DTFL_CTX(&a, NULL));

	/* Drop zero sums, as upstream prints only non-zero occurrence counts. */
	k = 0;
	for (i = 0; i < a.nbuckets; i++)
		if (a.buckets[i].sum != 0)
			out[k++] = a.buckets[i];
	if (k > 1)
		qsort(out, k, sizeof *out, cmp_bucket);

	printf("Report at (%llu, %llu)\n", (unsigned long long)q[0],
	    (unsigned long long)q[1]);
	for (i = 0; i < k; i++)
		printf("    values with occurrence count %lld: %lld\n",
		    (long long)out[i].count, (long long)out[i].sum);
	return k;
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *inner = dtfl_time_u64();
	/* Pair<u64, u64> under the product partial order (upstream Pair). */
	dtfl_time_class *tc = dtfl_time_product(inner, inner);
	dtfl_diff *i64 = dtfl_diff_i64();
	build_ctx c;
	size_t i;
	int ok = 1;
	/* A time well past every chosen coordinate, to seal the input. */
	uint64_t seal[2] = { 1000000, 1000000 };

	(void)ctx;
	c.diff = i64;
	c.probe = dtfl_probe_new();
	c.trace = NULL;
	dtfl_worker_dataflow(w, "multitemporal", tc, build, &c);

	for (i = 0; i < NUPDATES; i++)
		send_update(&c, &UPDATES[i]);
	dtfl_unordered_input_flush(c.input);

	/*
	 * Downgrade the capability past every update time, then drop it: this
	 * closes the input so the arrangement finishes.  (0, 0) <= seal in the
	 * product order, so the downgrade is legal.
	 */
	{
		dtfl_cap *later = dtfl_cap_delayed(c.cap, seal);

		dtfl_cap_drop(c.cap);
		c.cap = later;
	}
	dtfl_cap_drop(c.cap);
	dtfl_unordered_input_free(c.input);
	while (dtfl_worker_step(w))
		;

	/*
	 * Hand-computed answers.  Each value's multiplicity at q is the sum of
	 * its diffs whose Pair time is <= q (product order), and count emits a
	 * (value, count) record only for a non-zero multiplicity:
	 *   000: +1 @ (0,0)                     -> 1 for every q here
	 *   222: +1 @ (0,0)                     -> 1 for every q here
	 *   111: +1 @ (0,0), +2 @ (1,1), -1 @ (2,0)
	 *        q=(0,0): 1; q=(1,1): 1+2=3; q=(2,0): 1-1=0 (no record);
	 *        q=(2,1): 1+2-1=2; q=(2,2): 1+2-1=2
	 * The histogram counts values per count value:
	 *   (0,0): count 1 -> 3
	 *   (1,1): count 1 -> 2, count 3 -> 1
	 *   (2,0): count 1 -> 2
	 *   (2,1): count 1 -> 2, count 2 -> 1
	 *   (2,2): count 1 -> 2, count 2 -> 1
	 */
	{
		static const bucket EXPECT[NQUERIES][2] = {
			{ { 1, 3 } },
			{ { 1, 2 }, { 3, 1 } },
			{ { 1, 2 } },
			{ { 1, 2 }, { 2, 1 } },
			{ { 1, 2 }, { 2, 1 } },
		};
		static const size_t NEXPECT[NQUERIES] = { 1, 2, 1, 2, 2 };

		for (i = 0; i < NQUERIES; i++) {
			bucket got[MAXBUCKETS];
			size_t n = run_query(&c, tc, QUERIES[i], got);
			size_t j;

			if (n != NEXPECT[i])
				ok = 0;
			for (j = 0; ok && j < n; j++)
				if (got[j].count != EXPECT[i][j].count ||
				    got[j].sum != EXPECT[i][j].sum)
					ok = 0;
		}
	}
	printf("validation: %s\n", ok ? "ok" : "FAILED");

	dtfl_trace_free(c.trace);
	dtfl_probe_free(c.probe);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
	dtfl_time_class_free(inner);
	if (!ok)
		exit(1);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc;

	/* A single worker so the trace is the whole, unsplit histogram. */
	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
