/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/ivm/sqlrow.h: encoding PostgreSQL column values as libdtfl
 * record bytes, for an incremental view maintenance extension.
 *
 * libdtfl compares records by their type descriptor and, for the
 * built-in descriptors, by memcmp of an order-preserving encoding (PLAN
 * D1).  A view's group keys, join keys and DISTINCT columns must compare
 * and hash the way SQL equality and ORDER BY do, so each column needs an
 * encoding whose byte order is the SQL order.  This header gives one for
 * the common fixed-width types and for NULL, built only on the public
 * dtfl_* API, and examples/ivm/sqlrow_check.c tests every rule below.
 *
 * Encodings (each column is a dtfl_type_option of its value type, so
 * NULL is representable):
 *   - NULL:            option tag 0.  It sorts before every non-NULL
 *                      value (ORDER BY ... NULLS FIRST).  For NULLS LAST
 *                      a caller flips the tag; nothing here does.
 *   - int2/int4/int8:  dtfl_type_i16/i32/i64 (big-endian, sign flipped).
 *   - bool:            dtfl_type_u8, 0 or 1.
 *   - date:            int4 days, timestamp/timestamptz: int8 usec,
 *                      exactly PostgreSQL's internal representation, so
 *                      they encode as int4/int8.
 *   - float4/float8:   sqlrow_float8_type below: IEEE 754 bits mapped so
 *                      memcmp order is numeric order, with PostgreSQL's
 *                      rules: -0.0 equals 0.0, and NaN equals NaN and is
 *                      greater than every other value.  (IEEE order and C
 *                      comparisons disagree with both rules.)
 *   - text/varchar/bytea: dtfl_type_bytes over the raw bytes.  That is
 *                      correct for bytea and for text under the C or
 *                      POSIX collation only.  Any other collation needs a
 *                      user descriptor whose cmp calls the collation and
 *                      whose hash is consistent with it (equal under the
 *                      collation implies equal hash), or the strxfrm key
 *                      stored as bytes.  Not provided here.
 *   - numeric:         not provided.  Its value order is not a byte
 *                      order of PostgreSQL's packed form; an extension
 *                      either normalizes it to an order-preserving key or
 *                      supplies a user descriptor that calls numeric_cmp.
 *
 * SQL semantics an extension must apply itself, outside the encoding:
 *   - Equality vs. NULL.  Encoded NULLs compare equal to each other, which
 *     is right for GROUP BY and DISTINCT (NULLs form one group) and wrong
 *     for joins (NULL = NULL is not true).  Filter rows whose join key is
 *     NULL before the join; sqlrow_is_null tells you which.
 *   - Aggregates ignore NULL inputs (count(col), sum, min, max) but
 *     count(*) does not; the reduce logic decides that per aggregate.
 */
#ifndef DTFL_EXAMPLE_SQLROW_H
#define DTFL_EXAMPLE_SQLROW_H

#include "dtfl/record.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * float8 as an order-preserving u64: flip all bits of negatives and only
 * the sign bit of non-negatives, the standard total-order mapping.  -0.0
 * is folded to +0.0 first and every NaN to one canonical quiet NaN, which
 * the mapping then places above +infinity.
 */
static inline uint64_t
sqlrow_float8_key(double d)
{
	uint64_t b;

	if (d != d)
		return UINT64_MAX;	/* every NaN: above +inf, all equal */
	if (d == 0.0)
		d = 0.0;		/* -0.0 == 0.0 */
	memcpy(&b, &d, sizeof b);
	return (b >> 63) ? ~b : b | UINT64_C(0x8000000000000000);
}

static inline double
sqlrow_float8_from_key(uint64_t k)
{
	uint64_t b;
	double d;

	if (k == UINT64_MAX) {
		d = 0.0;
		return d / d;	/* NaN */
	}
	/* Top bit set: came from a non-negative; clear: from a negative. */
	b = (k >> 63) ? k & ~UINT64_C(0x8000000000000000) : ~k;
	memcpy(&d, &b, sizeof d);
	return d;
}

/* The float8 key is a plain big-endian u64, so the built-in sorts it. */
static inline const dtfl_type *
sqlrow_float8_type(void)
{
	return dtfl_type_u64();
}

/* Writes a float8 value; returns the bytes written (8). */
static inline size_t
sqlrow_put_float8(void *buf, double d)
{
	return dtfl_put_u64(buf, sqlrow_float8_key(d));
}

static inline double
sqlrow_get_float8(const void *buf)
{
	return sqlrow_float8_from_key(dtfl_get_u64(buf));
}

/*
 * Nullable columns.  A nullable column of type T is dtfl_type_option(T):
 * one tag byte (0 NULL, 1 present) then the value.  The caller owns the
 * returned descriptor and frees it with dtfl_type_free.
 */
static inline const dtfl_type *
sqlrow_nullable(const dtfl_type *t)
{
	return dtfl_type_option(t);
}

/* Writes the NULL tag; returns 1. */
static inline size_t
sqlrow_put_null(void *buf)
{
	*(unsigned char *)buf = 0;
	return 1;
}

/* Writes the present tag; the value follows at buf + 1.  Returns 1. */
static inline size_t
sqlrow_put_present(void *buf)
{
	*(unsigned char *)buf = 1;
	return 1;
}

static inline bool
sqlrow_is_null(const void *col)
{
	return *(const unsigned char *)col == 0;
}

#endif /* DTFL_EXAMPLE_SQLROW_H */
