/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/interop/test_fuzz_oracle.c: libdtfl count and distinct against
 * real differential-dataflow on a fixed seed set (PLAN section 6 tier 7).
 *
 * Reads tests/interop/fixtures/fuzz_oracle.bin, which real differential at
 * the pin produced (dev/gen-fixtures.d/fuzz_oracle.rs): for each of a
 * fixed set of seeds, the consolidated output of count and distinct over
 * the seed's generated update stream.  This test regenerates the IDENTICAL
 * stream from each seed (the same splitmix64 PRNG and generation rule as
 * the Rust side), runs it through libdtfl's count and distinct, and
 * asserts the output multiset of (record, time, diff) equals the Rust
 * answer exactly.
 *
 * This anchors the broad random search in tests/pbt/pbt_oracle.c (which
 * checks libdtfl against a from-scratch C model) to actual Rust output on
 * a sample, so the fuzz oracle is a Rust cross-check and not only a
 * self-check.  Records are in libdtfl's D1 encoding, which the Rust
 * generator reproduces, so the comparison is on the real output bytes.
 */
#include "dtfl_test.h"

#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE "tests/interop/fixtures/fuzz_oracle.bin"
#define NKEYS 8
#define MAX_UPDATES 16
#define MAX_EPOCHS 3
#define MAX_OUT 512
#define MAX_REC 16

/* ------------------------------------------------------------------ */
/* Shared PRNG and stream generation (mirror of fuzz_oracle.rs)         */
/* ------------------------------------------------------------------ */
static uint64_t
splitmix64(uint64_t *state)
{
	uint64_t z;

	*state += 0x9e3779b97f4a7c15ull;
	z = *state;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

typedef struct gen_upd {
	uint64_t key;
	uint64_t epoch;
	int64_t diff;
} gen_upd;

/* Must reproduce fuzz_oracle.rs::gen_stream bit for bit. */
static size_t
gen_stream(uint64_t seed, gen_upd *out)
{
	uint64_t st = seed;
	size_t n = (size_t)(splitmix64(&st) % (MAX_UPDATES + 1));
	size_t i;
	uint64_t epoch = 0;

	for (i = 0; i < n; i++) {
		int64_t mag;

		out[i].key = splitmix64(&st) % NKEYS;
		if (splitmix64(&st) % 2 == 0 && epoch + 1 < MAX_EPOCHS)
			epoch += 1;
		out[i].epoch = epoch;
		mag = (int64_t)(splitmix64(&st) % 3 + 1);	/* 1..=3 */
		out[i].diff = (splitmix64(&st) % 2 == 0) ? mag : -mag;
	}
	return n;
}

/* ------------------------------------------------------------------ */
/* Output collection                                                    */
/* ------------------------------------------------------------------ */
typedef struct out_upd {
	unsigned char rec[MAX_REC];
	size_t rlen;
	uint64_t time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAX_OUT];
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
	memcpy(&t, time, sizeof t);
	c->u[c->n].time = t;
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

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

/* ------------------------------------------------------------------ */
/* Driver                                                               */
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

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
	bool distinct;
} build_ctx;

static void
build_op(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_diff *od = dtfl_diff_i64();
	dtfl_collection *op = c->distinct ? dtfl_collection_distinct(base, od)
	    : dtfl_collection_count(base, od);
	dtfl_collection *con = dtfl_collection_consolidate(op);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_diff_free(od);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(op);
	dtfl_collection_free(base);
}

static void
run_operator(const gen_upd *stream, size_t n, uint64_t max_epoch,
    bool distinct, collected *out)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	uint64_t cur = 0, e;
	size_t i;

	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = out;
	c.distinct = distinct;
	munit_assert_int(dtfl_worker_dataflow(w, "op", tc, build_op, &c), ==, 0);

	for (i = 0; i < n; i++) {
		unsigned char k[8];

		while (cur < stream[i].epoch) {
			uint64_t next = cur + 1;
			probe_cond pc = { c.probe, next };

			dtfl_input_session_advance_to(c.in, &next);
			dtfl_input_session_flush(c.in);
			dtfl_worker_step_while(w, probe_behind, &pc);
			cur = next;
		}
		dtfl_put_u64(k, stream[i].key);
		dtfl_input_session_update(c.in, k, 8, &stream[i].diff);
	}
	for (e = cur; e <= max_epoch; e++) {
		uint64_t next = e + 1;
		probe_cond pc = { c.probe, next };

		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		dtfl_worker_step_while(w, probe_behind, &pc);
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(r);
	dtfl_worker_free(w);
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
 * Compare one Rust section (the committed answer) against the libdtfl
 * output: every Rust (record, time) has the stated net diff in libdtfl's
 * output, and libdtfl emits no (record, time) with a non-zero net diff that
 * is absent from the Rust answer.  `*off` advances past the section.
 */
static void
compare_section(const unsigned char *buf, size_t flen, size_t *off,
    const collected *got, uint64_t seed, const char *which)
{
	size_t nu = rd_u32_le(buf + *off), u, j;
	/* The (record, time) pairs the Rust answer names, for the reverse check. */
	struct {
		unsigned char rec[MAX_REC];
		size_t rlen;
		uint64_t time;
	} rust[MAX_OUT];
	size_t nrust = 0;

	*off += 4;
	munit_assert_size(nu, <=, MAX_OUT);

	for (u = 0; u < nu; u++) {
		size_t rl = rd_u32_le(buf + *off);
		const unsigned char *rec;
		uint64_t time;
		int64_t diff, g;

		*off += 4;
		munit_assert_size(rl, <=, MAX_REC);
		munit_assert_size(*off + rl + 16, <=, flen);
		rec = buf + *off;
		*off += rl;
		time = rd_u64_le(buf + *off);
		diff = (int64_t)rd_u64_le(buf + *off + 8);
		*off += 16;

		g = got_diff(got, rec, rl, time);
		if (g != diff) {
			char hex[2 * MAX_REC + 1];
			size_t k;

			for (k = 0; k < rl; k++)
				snprintf(hex + 2 * k, 3, "%02x", rec[k]);
			munit_errorf("seed %llu %s: record %s at time %llu: Rust "
			    "diff %lld, libdtfl %lld",
			    (unsigned long long)seed, which, hex,
			    (unsigned long long)time, (long long)diff,
			    (long long)g);
		}
		memcpy(rust[nrust].rec, rec, rl);
		rust[nrust].rlen = rl;
		rust[nrust].time = time;
		nrust++;
	}

	/* Reverse direction: every libdtfl output cell (record, time) must
	 * appear in the Rust answer, so libdtfl produced no extra update. */
	for (j = 0; j < got->n; j++) {
		bool found = false;
		size_t r;

		for (r = 0; r < nrust && !found; r++)
			found = rust[r].rlen == got->u[j].rlen &&
			    rust[r].time == got->u[j].time &&
			    memcmp(rust[r].rec, got->u[j].rec,
				got->u[j].rlen) == 0;
		if (!found) {
			char hex[2 * MAX_REC + 1];
			size_t k;

			for (k = 0; k < got->u[j].rlen; k++)
				snprintf(hex + 2 * k, 3, "%02x",
				    got->u[j].rec[k]);
			munit_errorf("seed %llu %s: libdtfl produced record %s "
			    "at time %llu (diff %lld), absent from Rust answer",
			    (unsigned long long)seed, which, hex,
			    (unsigned long long)got->u[j].time,
			    (long long)got->u[j].diff);
		}
	}
}

DTFL_TEST(fuzz_oracle_matches_rust)
{
	size_t flen, off, s, nseed;
	unsigned char *buf;

	(void)params; (void)data;
	buf = slurp(FIXTURE, &flen);
	munit_assert_size(flen, >=, 12);
	munit_assert_uint32(rd_u32_le(buf), ==, 0x4454465au);	/* "DTFZ" */
	munit_assert_uint32(rd_u32_le(buf + 4), ==, 1);		/* version */
	nseed = rd_u32_le(buf + 8);
	munit_assert_size(nseed, >, 0);

	off = 12;
	for (s = 0; s < nseed; s++) {
		gen_upd stream[MAX_UPDATES];
		size_t n, i;
		uint64_t seed, max_epoch = 0;
		static collected count_out, dist_out;

		munit_assert_size(off + 8, <=, flen);
		seed = rd_u64_le(buf + off);
		off += 8;

		n = gen_stream(seed, stream);
		for (i = 0; i < n; i++)
			if (stream[i].epoch > max_epoch)
				max_epoch = stream[i].epoch;

		memset(&count_out, 0, sizeof count_out);
		memset(&dist_out, 0, sizeof dist_out);
		run_operator(stream, n, max_epoch, false, &count_out);
		run_operator(stream, n, max_epoch, true, &dist_out);

		compare_section(buf, flen, &off, &count_out, seed, "count");
		compare_section(buf, flen, &off, &dist_out, seed, "distinct");
	}
	munit_assert_size(off, ==, flen);

	free(buf);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(fuzz_oracle_matches_rust),
	DTFL_T_END
};

DTFL_TEST_MAIN("interop", tests)
