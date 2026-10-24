/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/interop/test_ord_order.c: dtfl_type_cmp reproduces Rust Ord.
 *
 * Reads tests/interop/fixtures/ord_order.bin, which real Rust produced
 * (dev/gen-fixtures.d/ord_order.rs): a set of (i64, String, Option<u32>)
 * values encoded in libdtfl's order-preserving form, emitted in Rust's
 * derived Ord order.  This test is the fidelity oracle for D1: it builds
 * the matching dtfl_type and checks that
 *   1. every record decodes to exactly its stored length, and
 *   2. dtfl_type_cmp on adjacent fixture records is strictly increasing
 *      (so cmp agrees with Rust Ord on consecutive pairs), and
 *   3. after a shuffle, sorting by dtfl_type_cmp restores the fixture
 *      order exactly (so cmp agrees with Rust Ord on all pairs, not just
 *      neighbors).
 * A mismatch means the C encoding or comparator diverges from Rust, which
 * would corrupt every arrangement, consolidation and reduce.
 */
#include "dtfl_test.h"
#include "dtfl/record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE "tests/interop/fixtures/ord_order.bin"

typedef struct rec {
	unsigned char *bytes;
	size_t len;
} rec;

static uint32_t
rd_be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Reads the whole fixture into a malloc'd buffer; aborts the test on I/O
 * failure (a missing fixture is a build-tree error, not a soft skip). */
static unsigned char *
slurp(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	long n;
	unsigned char *buf;

	if (f == NULL) {
		/* VPATH/distcheck: the fixture lives in the source tree. */
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

/* The modeled type: (i64, (string, option<u32>)), left-nested pairs so a
 * record is i64 || string || option, matching the Rust encoder.  The
 * composite constructors retain their arguments, so this drops the local
 * references to the intermediates and the caller frees only the outer
 * pair (which releases the chain). */
static const dtfl_type *
modeled_type(void)
{
	const dtfl_type *opt = dtfl_type_option(dtfl_type_u32());
	const dtfl_type *inner = dtfl_type_pair(dtfl_type_string(), opt);
	const dtfl_type *outer = dtfl_type_pair(dtfl_type_i64(), inner);

	dtfl_type_free(opt);
	dtfl_type_free(inner);
	return outer;
}

static const dtfl_type *g_type;

static int
rec_cmp(const void *x, const void *y)
{
	const rec *a = x, *b = y;

	return dtfl_type_cmp(g_type, a->bytes, a->len, b->bytes, b->len);
}

DTFL_TEST(ord_order_matches_rust)
{
	size_t flen, i, off, n;
	unsigned char *buf;
	const dtfl_type *t = modeled_type();
	rec *recs, *shuf;

	(void)params; (void)data;
	buf = slurp(FIXTURE, &flen);
	munit_assert_size(flen, >=, 12);
	munit_assert_uint32(rd_be32(buf), ==, 0x4454464fu);	/* "DTFO" */
	munit_assert_uint32(rd_be32(buf + 4), ==, 1);		/* version */
	n = rd_be32(buf + 8);
	munit_assert_size(n, >, 0);

	recs = malloc(n * sizeof *recs);
	munit_assert_not_null(recs);
	off = 12;
	for (i = 0; i < n; i++) {
		size_t elen, vlen;

		munit_assert_size(off + 4, <=, flen);
		elen = rd_be32(buf + off);
		off += 4;
		munit_assert_size(off + elen, <=, flen);
		recs[i].bytes = buf + off;
		recs[i].len = elen;
		/* The record must decode to exactly its stored length: the
		 * C descriptor and the Rust encoder agree on field widths. */
		vlen = dtfl_type_value_len(t, buf + off, elen);
		munit_assert_size(vlen, ==, elen);
		off += elen;
	}
	munit_assert_size(off, ==, flen);

	/* Adjacent fixture records are strictly increasing by dtfl_type_cmp:
	 * cmp agrees with Rust Ord on every consecutive pair, and no two
	 * distinct values compare equal. */
	for (i = 1; i < n; i++)
		munit_assert_int(dtfl_type_cmp(t, recs[i - 1].bytes,
		    recs[i - 1].len, recs[i].bytes, recs[i].len), <, 0);

	/* Shuffle, then sort by dtfl_type_cmp; the result must equal the
	 * fixture order byte for byte.  This checks all pairs, not just
	 * neighbors: a cmp that got a far-apart pair wrong would not
	 * reconstruct the Rust order. */
	shuf = malloc(n * sizeof *shuf);
	munit_assert_not_null(shuf);
	memcpy(shuf, recs, n * sizeof *shuf);
	for (i = n; i-- > 1;) {
		size_t j = (size_t)munit_rand_int_range(0, (int)i);
		rec tmp = shuf[i];

		shuf[i] = shuf[j];
		shuf[j] = tmp;
	}
	g_type = t;
	qsort(shuf, n, sizeof *shuf, rec_cmp);
	for (i = 0; i < n; i++) {
		munit_assert_size(shuf[i].len, ==, recs[i].len);
		munit_assert_memory_equal(shuf[i].len, shuf[i].bytes, recs[i].bytes);
	}

	free(shuf);
	free(recs);
	free(buf);
	dtfl_type_free(t);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(ord_order_matches_rust),
	DTFL_T_END
};

DTFL_TEST_MAIN("interop", tests)
