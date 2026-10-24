/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/tests/import.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tests/upstream/dd/import.c: the upstream trace-import test suite.
 *
 * Dataflow 1 arranges a collection of (src, dst) edges by key (src) and
 * shares its trace; dataflow 2 imports that trace, reduce_abelian sums the
 * weights of the matched values per key emitting (sum, 1), as_collection
 * forms the record (src, sum), and the per-epoch output multiset is checked
 * against the expected vectors from import.rs.
 *
 * The upstream suite partitions the 4-worker process config by src % peers;
 * the per-key totals are peer-independent, so this port runs a single
 * standalone worker (peers == 1), which sees every edge and produces the
 * same reduce output.  The three variants differ only in WHEN dataflow 2 is
 * built relative to draining dataflow 1: vanilla builds both up front;
 * completed drains dataflow 1 to its probe before building the importer;
 * stalled feeds dataflow 1 incrementally and checks the imported frontier
 * advances to the input time.  import_skewed is #[ignore] upstream, so it
 * is excluded from this port (see the ledger note).
 */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAX_OUT 256

/* One captured reduce output: record (src, sum), its time and diff. */
typedef struct out_upd {
	uint64_t src;
	uint64_t sum;
	uint64_t time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAX_OUT];
	size_t n;
} collected;

/* The reduce output record is (src u64, sum u64); the time trails it. */
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
	c->u[c->n].src = dtfl_get_u64(r);
	c->u[c->n].sum = dtfl_get_u64(r + 8);
	/* The embedded time may be unaligned after the record. */
	memcpy(&t, time, sizeof t);
	c->u[c->n].time = t;
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* Net diff of record (src, sum) at `time`, summed over the output. */
static int64_t
out_diff(const collected *c, uint64_t src, uint64_t sum, uint64_t time)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].src == src && c->u[i].sum == sum &&
		    c->u[i].time == time)
			s += c->u[i].diff;
	return s;
}

/* The number of distinct (src, sum, time) with a non-zero net diff. */
static size_t
out_distinct(const collected *c)
{
	size_t i, j, count = 0;

	for (i = 0; i < c->n; i++) {
		bool seen = false;

		for (j = 0; j < i; j++)
			if (c->u[j].src == c->u[i].src &&
			    c->u[j].sum == c->u[i].sum &&
			    c->u[j].time == c->u[i].time) {
				seen = true;
				break;
			}
		if (!seen &&
		    out_diff(c, c->u[i].src, c->u[i].sum, c->u[i].time) != 0)
			count++;
	}
	return count;
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

/* ------------------------------------------------------------------ */
/* The 6-epoch retraction sequence from import.rs (input_epochs).      */
/* ------------------------------------------------------------------ */

typedef struct edge {
	uint64_t src, dst;
	int64_t w;
} edge;

static const edge epoch0[] = {
	{ 2, 0, 1 }, { 1, 0, 1 }, { 1, 3, 1 }, { 4, 2, 1 },
};
static const edge epoch1[] = { { 2, 0, -1 }, { 1, 0, -1 } };
static const edge epoch2[] = { { 2, 0, 1 }, { 1, 3, -1 } };
static const edge epoch3[] = {
	{ 1, 0, 1 }, { 1, 3, 1 }, { 2, 0, -1 }, { 4, 2, -1 },
};
static const edge epoch4[] = { { 2, 0, 1 }, { 4, 2, 1 }, { 1, 3, -1 } };
static const edge epoch5[] = { { 1, 3, 1 }, { 4, 2, -1 } };

static const edge *const input_epochs[6] = {
	epoch0, epoch1, epoch2, epoch3, epoch4, epoch5,
};
static const size_t epoch_len[6] = {
	4, 2, 2, 4, 3, 2,
};

/* One expected output update: record (src, sum) at `time` with `diff`. */
typedef struct exp_upd {
	uint64_t src, sum, time;
	int64_t diff;
} exp_upd;

static const exp_upd expected[] = {
	{ 1, 2, 0, 1 }, { 2, 1, 0, 1 }, { 4, 1, 0, 1 },
	{ 1, 1, 1, 1 }, { 1, 2, 1, -1 }, { 2, 1, 1, -1 },
	{ 1, 1, 2, -1 }, { 2, 1, 2, 1 },
	{ 1, 2, 3, 1 }, { 2, 1, 3, -1 }, { 4, 1, 3, -1 },
	{ 1, 1, 4, 1 }, { 1, 2, 4, -1 }, { 2, 1, 4, 1 }, { 4, 1, 4, 1 },
	{ 1, 1, 5, -1 }, { 1, 2, 5, 1 }, { 4, 1, 5, -1 },
};
#define NEXP (sizeof expected / sizeof expected[0])

/*
 * Feeds the whole 6-epoch sequence into the input: advance to epoch t, then
 * add every edge's weight at the current epoch, flush between epochs so the
 * frontier tracks the input time, and close.
 */
static void
feed_input(dtfl_input_session *in)
{
	size_t t, i;

	for (t = 0; t < 6; t++) {
		uint64_t at = t;
		const edge *es = input_epochs[t];

		dtfl_input_session_advance_to(in, &at);
		for (i = 0; i < epoch_len[t]; i++) {
			unsigned char rec[16];
			int64_t w = es[i].w;

			dtfl_put_u64(rec, es[i].src);
			dtfl_put_u64(rec + 8, es[i].dst);
			dtfl_input_session_update(in, rec, 16, &w);
		}
		dtfl_input_session_flush(in);
	}
	dtfl_input_session_close(in);
}

/* Asserts the captured output equals the expected per-epoch multiset. */
static void
check_output(const collected *out)
{
	size_t i;

	for (i = 0; i < NEXP; i++)
		assert_int64(out_diff(out, expected[i].src, expected[i].sum,
		    expected[i].time), ==, expected[i].diff);
	assert_size(out_distinct(out), ==, NEXP);
}

/*
 * reduce_abelian logic: emit the summed weights of the matched values as the
 * output value (u64) with diff +1, mirroring the upstream closure
 * `|_k, s, t| t.push((s.iter().map(|&(_, w)| w).sum(), 1i64))`.  The
 * reduce_abelian wrapper retracts the prior output, so the logic produces
 * only the new output.
 */
static void
sum_weights(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, dtfl_reduce_emit *out)
{
	const int64_t *idiffs = in_diffs;
	uint64_t sum = 0;
	size_t i;
	unsigned char vbuf[8];
	int64_t one = 1;

	(void)ctx; (void)key; (void)klen; (void)in_vals; (void)in_vlens;
	(void)m; (void)out_vals; (void)out_vlens; (void)out_diffs;
	for (i = 0; i < n; i++)
		sum += (uint64_t)idiffs[i];
	dtfl_put_u64(vbuf, sum);
	dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &one);
}

/* as_collection kv fn: the record is key(src) || value(sum). */
static void
kv_concat(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	unsigned char rec[16];

	(void)ctx;
	memcpy(rec, key, klen);
	memcpy(rec + klen, val, vlen);
	dtfl_arranged_emit_record(out, rec, klen + vlen);
}

/*
 * The importer dataflow (import.rs dataflow 2): import the shared trace,
 * reduce_abelian its values into a per-key weight sum, read it back as a
 * collection, inspect the output, and probe it.
 */
typedef struct importer_ctx {
	dtfl_trace *trace;
	dtfl_probe *probe;
	collected *out;
} importer_ctx;

static void
build_importer(dtfl_scope *scope, void *arg)
{
	importer_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *out_rec = dtfl_type_pair(u64, u64);
	dtfl_diff *odiff = dtfl_diff_i64();
	dtfl_arranged *imported = dtfl_trace_import(c->trace, scope, "Import");
	dtfl_arranged *red = dtfl_arranged_reduce_abelian(imported, "Reduce",
	    u64, odiff, sum_weights, DTFL_NOCTX);
	dtfl_collection *coll = dtfl_arranged_as_collection(red, out_rec,
	    kv_concat, DTFL_NOCTX);
	dtfl_collection *ins = dtfl_collection_inspect(coll, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_diff_free(odiff);
	dtfl_type_free(out_rec);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(coll);
	dtfl_arranged_free(red);
	dtfl_arranged_free(imported);
}

/*
 * The stalled importer: import the shared trace and probe its arranged
 * stream directly (upstream `trace.import(scope).stream.probe()`), with no
 * reduce.  Reading the arrangement back as a collection carries the same
 * frontier the batch stream does.
 */
static void
build_stalled_importer(dtfl_scope *scope, void *arg)
{
	importer_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	dtfl_arranged *imported = dtfl_trace_import(c->trace, scope, "Import");
	dtfl_collection *coll = dtfl_arranged_as_collection(imported, u64, NULL,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(coll, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(coll);
	dtfl_arranged_free(imported);
}

/* ------------------------------------------------------------------ */
/* Dataflow 1: arrange the edges by key (src), share the trace.        */
/* ------------------------------------------------------------------ */

typedef struct arranger_ctx {
	dtfl_input_session *in;
	dtfl_trace *trace;	/* out: the shared trace handle */
	dtfl_probe *probe;	/* optional: a probe on the arranged stream */
} arranger_ctx;

static void
build_arranger(dtfl_scope *scope, void *arg)
{
	arranger_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	dtfl_collection *edges = dtfl_input_session_to_collection(c->in, scope);
	dtfl_arranged *arr = dtfl_collection_arrange_by_key(edges, u64, u64,
	    "ArrangeByKey");

	c->trace = dtfl_arranged_trace(arr);
	if (c->probe != NULL) {
		/*
		 * Probe the arranged stream (upstream probes arranged.stream);
		 * reading it back as a collection carries the same frontier.
		 */
		dtfl_collection *coll = dtfl_arranged_as_collection(arr, u64,
		    NULL, DTFL_NOCTX);
		dtfl_collection *probed = dtfl_collection_probe_with(coll,
		    c->probe);

		dtfl_collection_free(probed);
		dtfl_collection_free(coll);
	}
	dtfl_arranged_free(arr);
	dtfl_collection_free(edges);
}

/* ------------------------------------------------------------------ */
/* test_import_vanilla: both dataflows built up front.                 */
/* ------------------------------------------------------------------ */

DTFL_TEST(import__test_import_vanilla)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *rec_t = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	arranger_ctx a;
	importer_ctx im;
	collected out;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	a.in = dtfl_input_session_new(rec_t, r);
	a.trace = NULL;
	a.probe = NULL;
	assert_int(dtfl_worker_dataflow(w, "arrange", tc, build_arranger, &a),
	    ==, 0);
	assert_ptr_not_null(a.trace);

	im.trace = a.trace;
	im.probe = dtfl_probe_new();
	im.out = &out;
	assert_int(dtfl_worker_dataflow(w, "import", tc, build_importer, &im),
	    ==, 0);

	feed_input(a.in);
	while (dtfl_worker_step(w))
		;

	check_output(&out);

	dtfl_probe_free(im.probe);
	dtfl_trace_free(a.trace);
	dtfl_type_free(rec_t);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* test_import_completed_dataflow: drain dataflow 1 before importing.  */
/* ------------------------------------------------------------------ */

DTFL_TEST(import__test_import_completed_dataflow)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *rec_t = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	arranger_ctx a;
	importer_ctx im;
	collected out;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	a.in = dtfl_input_session_new(rec_t, r);
	a.trace = NULL;
	a.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "arrange", tc, build_arranger, &a),
	    ==, 0);
	assert_ptr_not_null(a.trace);

	/* Feed and drain dataflow 1 to its probe before building the import. */
	feed_input(a.in);
	while (!dtfl_probe_done(a.probe) && dtfl_worker_step(w))
		;
	assert_true(dtfl_probe_done(a.probe));

	im.trace = a.trace;
	im.probe = dtfl_probe_new();
	im.out = &out;
	assert_int(dtfl_worker_dataflow(w, "import", tc, build_importer, &im),
	    ==, 0);
	while (dtfl_worker_step(w))
		;

	check_output(&out);

	dtfl_probe_free(a.probe);
	dtfl_probe_free(im.probe);
	dtfl_trace_free(a.trace);
	dtfl_type_free(rec_t);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* test_import_stalled_dataflow: the imported frontier catches up to   */
/* the input time even though no element after import pushes it.       */
/* ------------------------------------------------------------------ */

DTFL_TEST(import__test_import_stalled_dataflow)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	arranger_ctx a;
	importer_ctx im;
	collected out;
	unsigned char rec[8];
	uint64_t one = 1, two = 2;
	probe_cond pc;
	int k;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	/* arrange_by_self over a single-field record (upstream String). */
	a.in = dtfl_input_session_new(dtfl_type_u64(), r);
	a.trace = NULL;
	a.probe = dtfl_probe_new();
	assert_int(dtfl_worker_dataflow(w, "arrange", tc, build_arranger, &a),
	    ==, 0);
	assert_ptr_not_null(a.trace);

	/* insert one record, advance to 1, step until probe1 catches up. */
	dtfl_put_u64(rec, 42);
	dtfl_input_session_insert(a.in, rec, sizeof rec);
	dtfl_input_session_advance_to(a.in, &one);
	dtfl_input_session_flush(a.in);
	pc.probe = a.probe; pc.target = one;
	dtfl_worker_step_while(w, probe_behind, &pc);

	/* advance to 2, step until probe1 catches up. */
	dtfl_input_session_advance_to(a.in, &two);
	dtfl_input_session_flush(a.in);
	pc.target = two;
	dtfl_worker_step_while(w, probe_behind, &pc);

	/* Build the importer and probe its arranged stream. */
	im.trace = a.trace;
	im.probe = dtfl_probe_new();
	im.out = NULL;
	assert_int(dtfl_worker_dataflow(w, "import", tc,
	    build_stalled_importer, &im), ==, 0);

	/* A few bare steps, as upstream does (worker.step() x4). */
	for (k = 0; k < 8; k++)
		dtfl_worker_step(w);

	/*
	 * The imported arrangement must have advanced to the input time (2),
	 * so the probe is NOT strictly before it: !probe2.less_than(2).
	 */
	assert_false(dtfl_probe_less_than(im.probe, &two));

	dtfl_input_session_close(a.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(a.probe);
	dtfl_probe_free(im.probe);
	dtfl_trace_free(a.trace);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(import__test_import_vanilla),
	DTFL_T(import__test_import_completed_dataflow),
	DTFL_T(import__test_import_stalled_dataflow),
	DTFL_T_END
};

DTFL_TEST_MAIN("import", tests)
