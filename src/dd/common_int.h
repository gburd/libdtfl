/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/dd/common_int.h: time-ordered replay machinery shared by the
 * differential operators (join and reduce).
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1:
 *   differential-dataflow/src/operators/mod.rs (ValueHistory, EditList)
 *   differential-dataflow/src/operators/common.rs (JoinThinker/bilinear_wave)
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * __dtfl_value_history loads one key's (value, time, diff) edits, orders
 * them by time, and replays them with a meet-advanced, consolidated buffer
 * so a key with many distinct times stays linear.  A value is an index into
 * the history's own slices arena; a time is tw uint64_t words; a diff is dw
 * bytes with a __dtfl_diff_class algebra.  __dtfl_join_think joins two
 * histories, visiting every product of edits (times joined, diffs to be
 * multiplied) in time order.
 *
 * Reduce's single-trace case: load one history from its input cursor with
 * __dtfl_value_history_load_cursor, then walk its edits in (value, time)
 * order with __dtfl_value_history_map; it does not use __dtfl_join_think.
 *
 * The diff byte width is capped at the built-in and tuple classes' maximum
 * (DTFL_COMMON_DIFF_MAX); a wider class would need the inline buffers to
 * grow.  Nothing here is installed or exported.
 */
#ifndef DTFL_DD_COMMON_INT_H
#define DTFL_DD_COMMON_INT_H

#include "dd/dd_int.h"
#include "dd/trace/trace_int.h"

/* The widest inline diff value: a tuple of four u64 is 32 bytes. */
#define DTFL_COMMON_DIFF_MAX 64

/*
 * A key's edits, grouped by value.  `vals` holds the distinct value bytes
 * in insertion order; `vend[i]` is one past the last edit of value i, so
 * value i owns edits [vend[i-1], vend[i]) (vend[-1] == 0).  `times` and
 * `diffs` are parallel columns of the edits.  Each value's edits are
 * consolidated as the value is sealed, so a (value, time) pair appears once
 * with a non-zero diff.
 */
typedef struct __dtfl_edit_list {
	__dtfl_slices vals;	/* distinct value bytes */
	__dtfl_vec vend;	/* size_t, one per value: edit upper bound */
	__dtfl_vec times;	/* tw words per edit */
	__dtfl_vec diffs;	/* dw bytes per edit */
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
	unsigned tw;
	unsigned dw;
} __dtfl_edit_list;

/* A loaded, time-ordered replay of one key's (value, time, diff) edits. */
typedef struct __dtfl_value_history {
	__dtfl_edit_list edits;
	__dtfl_vec history;	/* hist_rec, sorted descending by time */
	__dtfl_vec buffer;	/* buf_rec, consolidated */
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
	unsigned tw;
	unsigned dw;
} __dtfl_value_history;

void __dtfl_value_history_init(__dtfl_value_history *h,
    const dtfl_time_class *tc, const __dtfl_diff_class *dc);
void __dtfl_value_history_free(__dtfl_value_history *h);
void __dtfl_value_history_clear(__dtfl_value_history *h);

/*
 * Load the edits of the cursor list's current key (its values and their
 * (time, diff) updates), advancing each time by `meet` if given, then
 * build the time-ordered replay.  The cursor is left positioned past the
 * key's values; the caller steps the key.
 */
void __dtfl_value_history_load_cursor(__dtfl_value_history *h,
    __dtfl_cursor_list *cl, const uint64_t *meet);

/* The number of edits (post-consolidation) loaded; for the think gate. */
size_t __dtfl_value_history_nedits(const __dtfl_value_history *h);

/* Visit every (value, time, diff) edit in (value, time) order. */
typedef void (*__dtfl_edit_fn)(void *ctx, const void *val, size_t vlen,
    const uint64_t *time, const void *diff);
void __dtfl_value_history_map(const __dtfl_value_history *h,
    __dtfl_edit_fn fn, void *ctx);

/*
 * The callback the join driver fires for each matched product: the two
 * value byte slices, the joined time, and the two diffs (so the caller
 * multiplies them, difference.rs Multiply).
 */
typedef void (*__dtfl_join_emit_fn)(void *ctx, const void *v1, size_t v1len,
    const void *v2, size_t v2len, const uint64_t *time, const void *d1,
    const void *d2);

/*
 * Join two loaded histories: for every pair of edits, join the times and
 * multiply the diffs (through `emit`), visiting the products in time order
 * (JoinThinker::think).  For small histories a plain cross product; for
 * large ones the bilinear wave.
 */
void __dtfl_join_think(__dtfl_value_history *h1, __dtfl_value_history *h2,
    __dtfl_join_emit_fn emit, void *ctx);

#endif /* DTFL_DD_COMMON_INT_H */
