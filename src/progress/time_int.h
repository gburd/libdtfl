/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/progress/time_int.h: library-internal time-class interface.
 *
 * The public header dtfl/time.h covers constructors and the per-time
 * operations users need.  This header adds what the progress tracker,
 * the scopes and the traces need: the class structure itself, scope
 * nesting (Rust's Refines), antichain-relative advance_by, and the
 * bincode codec.
 *
 * Invariants:
 *   - A class is immutable after construction and reference counted
 *     with an atomic count, so it may be shared across workers.
 *   - depth <= DTFL_TIME_MAX and sdepth <= DTFL_TIME_MAX.
 *   - Word-lexicographic order on stored times is Rust's derived Ord.
 */
#ifndef DTFL_TIME_INT_H
#define DTFL_TIME_INT_H

#include "base/base_int.h"
#include "dtfl/time.h"

typedef enum __dtfl_time_shape {
	DTFL_SHAPE_UNIT,
	DTFL_SHAPE_TOTAL,	/* one coordinate */
	DTFL_SHAPE_PRODUCT,	/* outer coordinates then inner */
	DTFL_SHAPE_LEX,		/* first coordinates then second */
	DTFL_SHAPE_ALTNEU,	/* one coordinate, 2t + neu over a TOTAL base */
	DTFL_SHAPE_POINTSTAMP	/* max_len coordinates, padded with minimum */
} __dtfl_time_shape;

struct dtfl_time_class {
	__dtfl_arc ref;
	__dtfl_time_shape shape;
	unsigned depth;		/* words per time */
	unsigned sdepth;	/* words per summary */
	bool total;		/* partial order is total */
	dtfl_coord coord;	/* TOTAL, ALTNEU (of base), POINTSTAMP */
	const dtfl_time_class *a;	/* PRODUCT outer / LEX first / ALTNEU base */
	const dtfl_time_class *b;	/* PRODUCT inner / LEX second */
};

/*
 * Summary layouts:
 *   UNIT: 0 words.  TOTAL: 1 word (the increment, unsigned or biased
 *   signed as the coordinate).  PRODUCT and LEX: a's words then b's.
 *   ALTNEU: 0 words (Rust's `()`).
 *   POINTSTAMP(k): 1 + k words, so k < DTFL_TIME_MAX.  Word 0 is
 *   `retain`: UINT64_MAX means None, otherwise the count.  Words 1..k
 *   are the per-coordinate actions (increments), 0-padded.
 */

/* Copy a time or summary. */
static inline void
__dtfl_time_copy(const dtfl_time_class *tc, uint64_t *dst, const uint64_t *src)
{
	if (tc->depth != 0)
		memmove(dst, src, tc->depth * sizeof(uint64_t));
}

static inline void
__dtfl_summary_copy(const dtfl_time_class *tc, uint64_t *dst, const uint64_t *src)
{
	if (tc->sdepth != 0)
		memmove(dst, src, tc->sdepth * sizeof(uint64_t));
}

/* Word-lexicographic compare of n words: Rust's Ord. */
static inline int
__dtfl_words_cmp(const uint64_t *a, const uint64_t *b, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i])
			return a[i] < b[i] ? -1 : 1;
	}
	return 0;
}

static inline bool
__dtfl_time_eq(const dtfl_time_class *tc, const uint64_t *a, const uint64_t *b)
{
	return __dtfl_words_cmp(a, b, tc->depth) == 0;
}

/*
 * Lattice::advance_by against an antichain of `n` times stored as a flat
 * run (differential lattice.rs:138-147).  Empty frontier: no change.
 * One element: join.  Otherwise the meet over all joins.
 */
void __dtfl_time_advance_by(const dtfl_time_class *tc, uint64_t *t,
    const uint64_t *frontier, size_t n);

/* Maximum element (Rust Maximum); only LEX's second component needs it. */
void __dtfl_time_maximum(const dtfl_time_class *tc, uint64_t *out);

/*
 * Refines: `inner` refines `outer`.  Supported pairs, first match wins
 * (classes match structurally, not by pointer):
 *   - identical classes (identity);
 *   - inner = PRODUCT(outer, X): to_inner appends X::minimum, to_outer
 *     drops the inner words, summarize keeps the outer words;
 *   - inner = ALTNEU(outer): to_inner 2t (panics if t >= 2^63), to_outer
 *     t/2, summarize default;
 *   - outer = UNIT, any inner: to_inner is Rust's Default for a TOTAL
 *     inner (0, which is not the minimum of a signed class;
 *     timestamp.rs implement_refines_empty) and the minimum otherwise;
 *     to_outer and summarize produce nothing.
 * __dtfl_refines returns false if the pair is not supported; the other
 * three panic on an unsupported pair.
 */
bool __dtfl_refines(const dtfl_time_class *inner, const dtfl_time_class *outer);
void __dtfl_time_to_inner(const dtfl_time_class *inner,
    const dtfl_time_class *outer, const uint64_t *t_outer, uint64_t *t_inner);
void __dtfl_time_to_outer(const dtfl_time_class *inner,
    const dtfl_time_class *outer, const uint64_t *t_inner, uint64_t *t_outer);
void __dtfl_summary_to_outer(const dtfl_time_class *inner,
    const dtfl_time_class *outer, const uint64_t *s_inner, uint64_t *s_outer);

/*
 * bincode-1 fixint little-endian encoding of a time as the pinned Rust
 * type would serialize it (PLAN D9).  Sizes:
 *   unsigned coord of b bits: b/8 bytes; signed: b/8 bytes two's
 *   complement; DURATION: u64 secs + u32 nanos (12 bytes); UNIT: 0;
 *   PRODUCT and LEX: a then b; ALTNEU: base then 1 byte bool;
 *   POINTSTAMP: u64 length then the trimmed coordinates.
 * __dtfl_time_encode appends to `out` (a vector of bytes, elem 1) and
 * returns the number of bytes written.  __dtfl_time_decode reads from
 * `p` (len bytes available); on success it stores the time, sets *used
 * to the bytes consumed (0 is valid, for UNIT) and returns 0.  It
 * returns DTFL_EPROTO for truncated or malformed bytes and DTFL_ERANGE
 * for a valid Rust value the class cannot hold (a Duration past 2^64
 * ns, an AltNeu time >= 2^63, a PointStamp longer than max_len); *t is
 * unchanged then.  A PointStamp with trailing minimum coordinates (not
 * canonical, so never written by Rust's PointStamp::new) is accepted
 * and canonicalized, including extra trailing minima past max_len.
 */
size_t __dtfl_time_encoded_len(const dtfl_time_class *tc, const uint64_t *t);
size_t __dtfl_time_encode(const dtfl_time_class *tc, const uint64_t *t,
    __dtfl_vec *out);
int __dtfl_time_decode(const dtfl_time_class *tc, const unsigned char *p,
    size_t len, uint64_t *t, size_t *used);

/*
 * The same for summaries (Rust's Summary types).  A PointStamp summary
 * is Option<usize> then Vec<action>; trailing identity actions are
 * trimmed on encode, and Some(usize::MAX) decodes as DTFL_ERANGE.
 */
size_t __dtfl_summary_encode(const dtfl_time_class *tc, const uint64_t *s,
    __dtfl_vec *out);
int __dtfl_summary_decode(const dtfl_time_class *tc, const unsigned char *p,
    size_t len, uint64_t *s, size_t *used);

#endif /* DTFL_TIME_INT_H */
