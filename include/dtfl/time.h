/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/time.h: timestamps and path summaries (PLAN D3).
 *
 * A time is a vector of `depth` uint64_t coordinates.  The depth and the
 * meaning of each coordinate come from a dtfl_time_class, which a scope
 * carries.  Times are passed as `const uint64_t *` and stored as flat
 * runs of `depth` words.  Rust's derived Ord on every supported
 * timestamp type equals the lexicographic order of these words (PLAN
 * D3, "Ord is word order"); the partial order and lattice operations
 * depend on the class.
 *
 * Classes are immutable after creation and may be shared between
 * threads.  See dtfl_time_class(3).
 */
#ifndef DTFL_TIME_H
#define DTFL_TIME_H

#include "dtfl/base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum coordinates in one time. */
#define DTFL_TIME_MAX 16

/* Coordinate kinds. */
typedef enum dtfl_coord_kind {
	DTFL_COORD_UNSIGNED = 0,	/* unsigned integer of a declared width */
	DTFL_COORD_SIGNED = 1,		/* signed integer, stored biased (D3) */
	DTFL_COORD_DURATION = 2		/* nanoseconds; bincode as (secs, nanos) */
} dtfl_coord_kind;

typedef struct dtfl_coord {
	dtfl_coord_kind kind;
	unsigned bits;		/* 8, 16, 32 or 64; DURATION is 64 */
} dtfl_coord;

typedef struct dtfl_time_class dtfl_time_class;

/*
 * Constructors.  Each returns a new class owned by the caller and
 * released with dtfl_time_class_free(); classes are reference counted,
 * and a composite class retains its children.
 */

/* (): no coordinates.  Every time compares equal. */
DTFL_API dtfl_time_class *dtfl_time_unit(void);

/* A totally ordered integer or Duration timestamp: one coordinate. */
DTFL_API dtfl_time_class *dtfl_time_total(dtfl_coord coord);

/* Shorthands for dtfl_time_total of the common coordinates. */
DTFL_API dtfl_time_class *dtfl_time_u64(void);
DTFL_API dtfl_time_class *dtfl_time_u32(void);
DTFL_API dtfl_time_class *dtfl_time_duration(void);

/*
 * Product<outer, inner>: product partial order.  Depth is the sum of the
 * children's depths; outer coordinates come first.
 */
DTFL_API dtfl_time_class *dtfl_time_product(const dtfl_time_class *outer,
    const dtfl_time_class *inner);

/*
 * (first, second) with lexicographic order, as Rust tuples.  The meet of
 * two elements with incomparable first components uses the maximum of
 * the second (differential lattice.rs:204-230).
 */
DTFL_API dtfl_time_class *dtfl_time_lex(const dtfl_time_class *first,
    const dtfl_time_class *second);

/*
 * AltNeu<t> over a totally ordered one-coordinate class, encoded as
 * 2t + neu.  Returns NULL (and sets the error detail) if `base` is not a
 * one-coordinate total order or is wider than 63 bits of range.
 */
DTFL_API dtfl_time_class *dtfl_time_altneu(const dtfl_time_class *base);

/*
 * PointStamp<coord> padded to `max_len` coordinates (PLAN D3).  Missing
 * coordinates are the minimum.  1 <= max_len <= DTFL_TIME_MAX.
 */
DTFL_API dtfl_time_class *dtfl_time_pointstamp(dtfl_coord coord,
    unsigned max_len);

DTFL_API dtfl_time_class *dtfl_time_class_retain(const dtfl_time_class *tc);
DTFL_API void dtfl_time_class_free(const dtfl_time_class *tc);

/* Number of coordinates in a time of this class. */
DTFL_API unsigned dtfl_time_depth(const dtfl_time_class *tc);

/* Number of words in a path summary of this class. */
DTFL_API unsigned dtfl_summary_depth(const dtfl_time_class *tc);

/* True if the partial order is total (TotalOrder in Rust). */
DTFL_API bool dtfl_time_is_total(const dtfl_time_class *tc);

/*
 * Order.  dtfl_time_cmp is Rust's Ord (word-lexicographic);
 * dtfl_time_less_equal is the class's partial order.
 */
DTFL_API int dtfl_time_cmp(const dtfl_time_class *tc, const uint64_t *a,
    const uint64_t *b);
DTFL_API bool dtfl_time_less_equal(const dtfl_time_class *tc,
    const uint64_t *a, const uint64_t *b);
DTFL_API bool dtfl_time_less_than(const dtfl_time_class *tc,
    const uint64_t *a, const uint64_t *b);

/* Lattice.  `out` may alias either input. */
DTFL_API void dtfl_time_minimum(const dtfl_time_class *tc, uint64_t *out);
DTFL_API void dtfl_time_join(const dtfl_time_class *tc, const uint64_t *a,
    const uint64_t *b, uint64_t *out);
DTFL_API void dtfl_time_meet(const dtfl_time_class *tc, const uint64_t *a,
    const uint64_t *b, uint64_t *out);

/*
 * Path summaries.  A summary is dtfl_summary_depth() words.
 * dtfl_summary_default is the identity summary (Rust Default).
 * results_in and followed_by return false where Rust returns None
 * (overflow at the coordinate's declared width).
 */
DTFL_API void dtfl_summary_default(const dtfl_time_class *tc, uint64_t *out);
DTFL_API bool dtfl_summary_results_in(const dtfl_time_class *tc,
    const uint64_t *summary, const uint64_t *time, uint64_t *out);
DTFL_API bool dtfl_summary_followed_by(const dtfl_time_class *tc,
    const uint64_t *a, const uint64_t *b, uint64_t *out);
DTFL_API bool dtfl_summary_less_equal(const dtfl_time_class *tc,
    const uint64_t *a, const uint64_t *b);

/*
 * Coordinate encoding.  Integer coordinates are stored as described in
 * PLAN D3; these convert between native values and stored words.
 */
DTFL_API uint64_t dtfl_coord_from_i64(int64_t v);
DTFL_API int64_t dtfl_coord_to_i64(uint64_t w);

/* Formats a time for debugging, e.g. "(3, 7)".  Returns snprintf's value. */
DTFL_API int dtfl_time_format(const dtfl_time_class *tc, const uint64_t *t,
    char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_TIME_H */
