/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/interop/test_operator_oracle.c: libdtfl operators against real
 * differential-dataflow output (PLAN section 6 tier 7).  The strongest
 * fidelity check: Rust ran join, count and reduce(min) over fixed inputs
 * across epochs with insertions and retractions, and emitted the
 * consolidated output updates (record, time, diff) into
 * tests/interop/fixtures/operator_oracle.bin (dev/gen-fixtures.d/
 * operator_oracle.rs).  This test builds the SAME computations in libdtfl
 * with the same inputs and epochs, collects the consolidated output, and
 * asserts the multiset of (record, time, diff) equals the Rust fixture
 * exactly.
 *
 * WHAT IS PROVEN: the C operators compute the same consolidated multiset
 * as Rust, byte for byte on the output records.  Records are in libdtfl's
 * order-preserving D1 encoding (not Rust bincode); the Rust generator
 * emits records in that same encoding, so the comparison is on the actual
 * output bytes.  A single wrong count, missing retraction or misordered
 * record aborts the test with the offending (record, time, diff).
 */
#include "dtfl_test.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE "tests/interop/fixtures/operator_oracle.bin"
#define MAX_OUT 256
#define MAX_REC 64

/* ------------------------------------------------------------------ */
/* Collected output updates (record bytes, time, diff)                 */
/* ------------------------------------------------------------------ */
typedef struct upd {
	unsigned char rec[MAX_REC];
	size_t rlen;
	uint64_t time;
	int64_t diff;
} upd;

typedef struct collected {
	upd u[MAX_OUT];
	size_t n;
} collected;

static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	uint64_t t;

	munit_assert_size(c->n, <, MAX_OUT);
	munit_assert_size(len, <=, MAX_REC);
	memcpy(c->u[c->n].rec, rec, len);
	c->u[c->n].rlen = len;
	memcpy(&t, time, sizeof t);		/* may be unaligned */
	c->u[c->n].time = t;
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* ------------------------------------------------------------------ */
/* Fixture reader                                                       */
/* ------------------------------------------------------------------ */
static uint32_t
rd_u32_le(const unsigned char *p)
{
	uint32_t v = 0;
	size_t i;

	for (i = 0; i < 4; i++)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}
static uint64_t
rd_u64_le(const unsigned char *p)
{
	uint64_t v = 0;
	size_t i;

	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}
static int64_t
rd_i64_le(const unsigned char *p)
{
	return (int64_t)rd_u64_le(p);
}

static unsigned char *
slurp(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	long n;
	unsigned char *buf;

	if (f == NULL) {
		const char *base[2] = { getenv("top_srcdir"), getenv("srcdir") };
		size_t k;
		char alt[1024];

		for (k = 0; k < 2 && f == NULL; k++) {
			if (base[k] == NULL)
				continue;
			snprintf(alt, sizeof alt, "%s/%s", base[k], path);
			f = fopen(alt, "rb");
		}
	}
	munit_assert_not_null(f);
	munit_assert_int(fseek(f, 0, SEEK_END), ==, 0);
	n = ftell(f);
	munit_assert_long(n, >, 0);
	munit_assert_int(fseek(f, 0, SEEK_SET), ==, 0);
	buf = malloc((size_t)n);
	munit_assert_not_null(buf);
	munit_assert_size(fread(buf, 1, (size_t)n, f), ==, (size_t)n);
	fclose(f);
	*out_len = (size_t)n;
	return buf;
}

/*
 * One expected section: a list of (record, time, diff) updates.  The
 * comparison is a multiset equality: every fixture update must match the
 * net diff libdtfl produced for that record at that time, and the total
 * number of distinct (record, time) cells must agree so libdtfl did not
 * produce an extra update.
 */
typedef struct expected {
	char name[16];
	const unsigned char *recs[MAX_OUT];
	size_t rlens[MAX_OUT];
	uint64_t times[MAX_OUT];
	int64_t diffs[MAX_OUT];
	size_t n;
} expected;

/* Net diff libdtfl produced for a record at a time (summed over output). */
static int64_t
got_diff(const collected *c, const unsigned char *rec, size_t rlen,
    uint64_t time)
{
	size_t i;
	int64_t sum = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].rlen == rlen && c->u[i].time == time &&
		    memcmp(c->u[i].rec, rec, rlen) == 0)
			sum += c->u[i].diff;
	return sum;
}

/* Assert the libdtfl output multiset equals the expected section. */
static void
assert_section(const expected *e, const collected *got)
{
	size_t i, j;
	int64_t net_got = 0, net_exp = 0;

	/* Every expected (record, time) has the expected net diff. */
	for (i = 0; i < e->n; i++) {
		int64_t g = got_diff(got, e->recs[i], e->rlens[i], e->times[i]);

		if (g != e->diffs[i]) {
			char hex[2 * MAX_REC + 1];
			size_t k;

			for (k = 0; k < e->rlens[i]; k++)
				snprintf(hex + 2 * k, 3, "%02x", e->recs[i][k]);
			munit_errorf("%s: record %s at time %llu: expected net "
			    "diff %lld, libdtfl produced %lld", e->name, hex,
			    (unsigned long long)e->times[i], (long long)e->diffs[i],
			    (long long)g);
		}
	}
	/* No libdtfl output cell is missing from the expected set: every
	 * (record, time) libdtfl emitted appears in the fixture. */
	for (j = 0; j < got->n; j++) {
		bool found = false;

		for (i = 0; i < e->n && !found; i++)
			found = e->rlens[i] == got->u[j].rlen &&
			    e->times[i] == got->u[j].time &&
			    memcmp(e->recs[i], got->u[j].rec, got->u[j].rlen) == 0;
		if (!found) {
			char hex[2 * MAX_REC + 1];
			size_t k;

			for (k = 0; k < got->u[j].rlen; k++)
				snprintf(hex + 2 * k, 3, "%02x", got->u[j].rec[k]);
			munit_errorf("%s: libdtfl produced record %s at time "
			    "%llu with diff %lld, absent from Rust fixture",
			    e->name, hex, (unsigned long long)got->u[j].time,
			    (long long)got->u[j].diff);
		}
	}
	/* Totals agree (a cheap global cross-check). */
	for (i = 0; i < e->n; i++)
		net_exp += e->diffs[i];
	for (j = 0; j < got->n; j++)
		net_got += got->u[j].diff;
	munit_assert_int64(net_got, ==, net_exp);
}

/* Parse the section named `name` from the fixture into `e`. */
static void
load_section(const unsigned char *buf, size_t flen, const char *name,
    expected *e)
{
	size_t off = 12, s, nsec = rd_u32_le(buf + 8);

	memset(e, 0, sizeof *e);
	for (s = 0; s < nsec; s++) {
		size_t nl = rd_u32_le(buf + off);
		const char *sn = (const char *)(buf + off + 4);
		size_t nu, u;
		bool want;

		off += 4 + nl;
		nu = rd_u32_le(buf + off);
		off += 4;
		want = strlen(name) == nl && memcmp(name, sn, nl) == 0;
		if (want) {
			munit_assert_size(nl, <, sizeof e->name);
			memcpy(e->name, name, nl);
			e->n = nu;
			munit_assert_size(nu, <=, MAX_OUT);
		}
		for (u = 0; u < nu; u++) {
			size_t rl = rd_u32_le(buf + off);

			off += 4;
			if (want) {
				e->recs[u] = buf + off;
				e->rlens[u] = rl;
			}
			off += rl;
			if (want) {
				e->times[u] = rd_u64_le(buf + off);
				e->diffs[u] = rd_i64_le(buf + off + 8);
			}
			off += 16;
			munit_assert_size(off, <=, flen);
		}
	}
	munit_assert_size(e->name[0], !=, 0);	/* section found */
}

/* ------------------------------------------------------------------ */
/* Shared epoch driver                                                 */
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

static void
put_u64(unsigned char *buf, size_t *off, uint64_t v)
{
	*off += dtfl_put_u64(buf + *off, v);
}

/* ------------------------------------------------------------------ */
/* join: (key,v1) join (key,v2) -> key||v1||v2                         */
/* ------------------------------------------------------------------ */
typedef struct join_build {
	dtfl_input_session *in1, *in2;
	dtfl_probe *probe;
	collected *out;
} join_build;

static void
build_join(dtfl_scope *scope, void *arg)
{
	join_build *c = arg;
	dtfl_collection *c1 = dtfl_input_session_to_collection(c->in1, scope);
	dtfl_collection *c2 = dtfl_input_session_to_collection(c->in2, scope);
	const dtfl_type *fields[3] = { dtfl_type_u64(), dtfl_type_u64(),
	    dtfl_type_u64() };
	const dtfl_type *out_type = dtfl_type_tuple(fields, 3);
	dtfl_collection *j = dtfl_collection_join(c1, c2, dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_u64(), out_type);
	dtfl_collection *con = dtfl_collection_consolidate(j);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_type_free(out_type);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(j);
	dtfl_collection_free(c1);
	dtfl_collection_free(c2);
}

static void
pair_insert(dtfl_input_session *s, uint64_t a, uint64_t b)
{
	unsigned char rec[16];
	size_t off = 0;

	put_u64(rec, &off, a);
	put_u64(rec, &off, b);
	dtfl_input_session_insert(s, rec, off);
}
static void
pair_remove(dtfl_input_session *s, uint64_t a, uint64_t b)
{
	unsigned char rec[16];
	size_t off = 0;

	put_u64(rec, &off, a);
	put_u64(rec, &off, b);
	dtfl_input_session_remove(s, rec, off);
}

static void
advance_both(dtfl_worker *w, dtfl_input_session *i1, dtfl_input_session *i2,
    dtfl_probe *probe, uint64_t next)
{
	probe_cond pc = { probe, next };

	dtfl_input_session_advance_to(i1, &next);
	if (i2 != NULL)
		dtfl_input_session_advance_to(i2, &next);
	dtfl_input_session_flush(i1);
	if (i2 != NULL)
		dtfl_input_session_flush(i2);
	dtfl_worker_step_while(w, probe_behind, &pc);
}

static void
run_join(const unsigned char *buf, size_t flen)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	join_build c;
	collected out;
	expected e;

	memset(&out, 0, sizeof out);
	c.in1 = dtfl_input_session_new(pt, r);
	c.in2 = dtfl_input_session_new(pt, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	munit_assert_int(dtfl_worker_dataflow(w, "join", tc, build_join, &c),
	    ==, 0);

	/* epoch 0: c1 = {(0,10),(1,20)}; c2 = {(0,100),(1,200)}. */
	pair_insert(c.in1, 0, 10);
	pair_insert(c.in1, 1, 20);
	pair_insert(c.in2, 0, 100);
	pair_insert(c.in2, 1, 200);
	advance_both(w, c.in1, c.in2, c.probe, 1);
	/* epoch 1: retract (1,20), insert (1,21); add (2,30) and c2 (2,300). */
	pair_remove(c.in1, 1, 20);
	pair_insert(c.in1, 1, 21);
	pair_insert(c.in1, 2, 30);
	pair_insert(c.in2, 2, 300);
	advance_both(w, c.in1, c.in2, c.probe, 2);
	dtfl_input_session_close(c.in1);
	dtfl_input_session_close(c.in2);
	while (dtfl_worker_step(w))
		;

	load_section(buf, flen, "join", &e);
	assert_section(&e, &out);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pt);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
}

/* ------------------------------------------------------------------ */
/* count: key -> key || count(i64)                                     */
/* ------------------------------------------------------------------ */
typedef struct one_build {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
	bool reduce;
} one_build;

static void
smallest_logic(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t count = (int64_t)n;
	unsigned char vbuf[8];

	(void)ctx; (void)key; (void)klen; (void)vlens; (void)diffs;
	dtfl_put_u64(vbuf, dtfl_get_u64(vals[0]));
	dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &count);
}

static void
build_one(dtfl_scope *scope, void *arg)
{
	one_build *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_diff *od = dtfl_diff_i64();
	dtfl_collection *op, *con, *ins, *probed;

	if (c->reduce)
		op = dtfl_collection_reduce(base, dtfl_type_u64(),
		    dtfl_type_u64(), dtfl_type_u64(), od, smallest_logic,
		    DTFL_NOCTX);
	else
		op = dtfl_collection_count(base, od);
	con = dtfl_collection_consolidate(op);
	ins = dtfl_collection_inspect(con, inspect_sink, DTFL_CTX(c->out, NULL));
	probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_diff_free(od);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(op);
	dtfl_collection_free(base);
}

static void
run_count(const unsigned char *buf, size_t flen)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	one_build c;
	collected out;
	expected e;
	unsigned char k[8];

	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.reduce = false;
	munit_assert_int(dtfl_worker_dataflow(w, "count", tc, build_one, &c),
	    ==, 0);

	/* epoch 0: keys 0,1,1,1,2,2. */
	dtfl_put_u64(k, 0); dtfl_input_session_insert(c.in, k, 8);
	dtfl_put_u64(k, 1); dtfl_input_session_insert(c.in, k, 8);
	dtfl_put_u64(k, 1); dtfl_input_session_insert(c.in, k, 8);
	dtfl_put_u64(k, 1); dtfl_input_session_insert(c.in, k, 8);
	dtfl_put_u64(k, 2); dtfl_input_session_insert(c.in, k, 8);
	dtfl_put_u64(k, 2); dtfl_input_session_insert(c.in, k, 8);
	advance_both(w, c.in, NULL, c.probe, 1);
	/* epoch 1: remove two 1s and both 2s. */
	dtfl_put_u64(k, 1); dtfl_input_session_remove(c.in, k, 8);
	dtfl_put_u64(k, 1); dtfl_input_session_remove(c.in, k, 8);
	dtfl_put_u64(k, 2); dtfl_input_session_remove(c.in, k, 8);
	dtfl_put_u64(k, 2); dtfl_input_session_remove(c.in, k, 8);
	advance_both(w, c.in, NULL, c.probe, 2);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	load_section(buf, flen, "count", &e);
	assert_section(&e, &out);

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
}

/* ------------------------------------------------------------------ */
/* reduce(min): (key,val) -> key || minval, diff = #distinct values    */
/* ------------------------------------------------------------------ */
static void
run_reduce(const unsigned char *buf, size_t flen)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *pt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	one_build c;
	collected out;
	expected e;

	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(pt, r);
	c.probe = dtfl_probe_new();
	c.out = &out;
	c.reduce = true;
	munit_assert_int(dtfl_worker_dataflow(w, "reduce", tc, build_one, &c),
	    ==, 0);

	/* epoch 0: key 0 -> {5,3,9}; key 1 -> {2}. */
	pair_insert(c.in, 0, 5);
	pair_insert(c.in, 0, 3);
	pair_insert(c.in, 0, 9);
	pair_insert(c.in, 1, 2);
	advance_both(w, c.in, NULL, c.probe, 1);
	/* epoch 1: retract (0,3) so key 0's min becomes 5. */
	pair_remove(c.in, 0, 3);
	advance_both(w, c.in, NULL, c.probe, 2);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	load_section(buf, flen, "reduce", &e);
	assert_section(&e, &out);

	dtfl_probe_free(c.probe);
	dtfl_type_free(pt);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
}

DTFL_TEST(operator_oracle_join)
{
	size_t flen;
	unsigned char *buf;

	(void)params; (void)data;
	buf = slurp(FIXTURE, &flen);
	munit_assert_uint32(rd_u32_le(buf), ==, 0x4454464eu);	/* "DTFN" */
	run_join(buf, flen);
	free(buf);
	return MUNIT_OK;
}

DTFL_TEST(operator_oracle_count)
{
	size_t flen;
	unsigned char *buf;

	(void)params; (void)data;
	buf = slurp(FIXTURE, &flen);
	run_count(buf, flen);
	free(buf);
	return MUNIT_OK;
}

DTFL_TEST(operator_oracle_reduce)
{
	size_t flen;
	unsigned char *buf;

	(void)params; (void)data;
	buf = slurp(FIXTURE, &flen);
	run_reduce(buf, flen);
	free(buf);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(operator_oracle_join),
	DTFL_T(operator_oracle_count),
	DTFL_T(operator_oracle_reduce),
	DTFL_T_END
};

DTFL_TEST_MAIN("interop", tests)
