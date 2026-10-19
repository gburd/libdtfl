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

/* ------------------------------------------------------------------ */
/* Integer-proxy support (operators/mod.rs ValueHistory::load_iter and  */
/* the common.rs interesting-times helpers), for src/dd/int_proxy*.c.   */
/* ------------------------------------------------------------------ */

/* One time, padded to the widest class (a fixed-size vector element). */
typedef struct __dtfl_time_val {
	uint64_t t[DTFL_TIME_MAX];
} __dtfl_time_val;

/*
 * One row of a ProxyBridge (int_proxy/mod.rs): ((key_hash, value_id),
 * time, diff).  A bridge is a __dtfl_vec of these, sorted and consolidated
 * by ((hash, id), time).  The tactics reuse the row for (id, diff) lists,
 * leaving hash and time unused.
 */
typedef struct __dtfl_bridge_upd {
	uint64_t hash;
	uint64_t id;
	uint64_t time[DTFL_TIME_MAX];
	unsigned char diff[DTFL_COMMON_DIFF_MAX];
} __dtfl_bridge_upd;


/* Sort a vector of __dtfl_time_val ascending (Ord) and drop duplicates. */
void __dtfl_times_sort_dedup(__dtfl_vec *v, unsigned tw);

/*
 * consolidation::consolidate_updates over bridge rows: sort by ((hash, id),
 * time), sum equal entries with `dc`, drop zeros.  Rows used as (id, diff)
 * lists leave hash and time zero, which makes this consolidate by id.
 */
void __dtfl_bridge_consolidate(__dtfl_vec *rows, unsigned tw,
    const __dtfl_diff_class *dc);
/* Sort bridge rows by ((hash, id), time) without consolidating. */
void __dtfl_bridge_sort(__dtfl_vec *rows, unsigned tw);
/* Orders bridge rows by ((hash, id), time). */
int __dtfl_bridge_cmp(const __dtfl_bridge_upd *x, const __dtfl_bridge_upd *y,
    unsigned tw);

/*
 * ValueHistory accessors (operators/mod.rs): time() and meet() return NULL
 * when every edit is replayed; buffer_at returns the i-th buffered edit's
 * value bytes, time and diff.
 */
const uint64_t *__dtfl_value_history_time(const __dtfl_value_history *h);
const uint64_t *__dtfl_value_history_meet(const __dtfl_value_history *h);
void __dtfl_value_history_step(__dtfl_value_history *h);
bool __dtfl_value_history_step_while_time_is(__dtfl_value_history *h,
    const uint64_t *time);
void __dtfl_value_history_step_through(__dtfl_value_history *h,
    const uint64_t *time);
void __dtfl_value_history_advance_buffer_by(__dtfl_value_history *h,
    const uint64_t *meet);
size_t __dtfl_value_history_buffer_len(const __dtfl_value_history *h);
const void *__dtfl_value_history_buffer_at(const __dtfl_value_history *h,
    size_t i, size_t *vlen, const uint64_t **time, const void **diff);

/*
 * ValueHistory::load_iter over the bridge rows [0, n): the value is the
 * row's id (an 8-byte big-endian u64, so value order is id order), each time
 * is joined with advance_by if given, and the history is built.  This is
 * IdHistory (int_proxy/history.rs), a ValueHistory<u64, T, R>.
 */
void __dtfl_value_history_load_iter(__dtfl_value_history *h,
    const __dtfl_bridge_upd *rows, size_t n, const uint64_t *advance_by);

/* The bilinear wave over two loaded histories (common.rs bilinear_wave). */
void __dtfl_bilinear_wave(__dtfl_value_history *h0, __dtfl_value_history *h1,
    __dtfl_join_emit_fn emit, void *ctx);

/* TimeHistory (common.rs): a time-only ValueHistory. */
typedef struct __dtfl_time_history {
	__dtfl_vec history;	/* time_meet, sorted descending by time */
	__dtfl_vec buffer;	/* __dtfl_time_val, ascending, deduplicated */
	const dtfl_time_class *tc;
	unsigned tw;
} __dtfl_time_history;

void __dtfl_time_history_init(__dtfl_time_history *h,
    const dtfl_time_class *tc);
void __dtfl_time_history_free(__dtfl_time_history *h);
/* load the n times at `first`, `stride` bytes apart, joined with advance_by. */
void __dtfl_time_history_load(__dtfl_time_history *h, const uint64_t *first,
    size_t n, size_t stride, const uint64_t *advance_by);
const uint64_t *__dtfl_time_history_time(const __dtfl_time_history *h);
const uint64_t *__dtfl_time_history_meet(const __dtfl_time_history *h);
bool __dtfl_time_history_step_while_time_is(__dtfl_time_history *h,
    const uint64_t *time);
void __dtfl_time_history_advance_buffer_by(__dtfl_time_history *h,
    const uint64_t *meet);

/*
 * tile_descriptions (common.rs): cut [lower, upper) along `held` into at
 * most nheld descriptions.  Fills descs[0..ntiles) (the caller frees each),
 * tile_held[k] = the held index of tile k, and tile_of[i] = the tile of held
 * index i or SIZE_MAX (None).  Returns ntiles.
 */
size_t __dtfl_tile_descriptions(const __dtfl_antichain *lower,
    const __dtfl_antichain *upper, const __dtfl_antichain *held,
    __dtfl_description *descs, size_t *tile_held, size_t *tile_of);

/* KeyView (common.rs): one key's rows [i0, i1) of p_in and its pending. */
typedef struct __dtfl_key_view {
	const __dtfl_bridge_upd *p_in;
	size_t i0;
	size_t i1;
	const __dtfl_time_val *pending;
	size_t npending;
} __dtfl_key_view;

/* DiscoverScratch (common.rs): per-retire reusable discover_times state. */
typedef struct __dtfl_discover_scratch {
	__dtfl_time_history batch_replay;
	__dtfl_value_history input_replay;
	__dtfl_time_history output_replay;
	__dtfl_vec synth;		/* __dtfl_time_val */
	__dtfl_vec times_current;	/* __dtfl_time_val */
	__dtfl_vec temporary;		/* __dtfl_time_val */
	__dtfl_vec meets;		/* __dtfl_time_val */
} __dtfl_discover_scratch;

void __dtfl_discover_scratch_init(__dtfl_discover_scratch *s,
    const dtfl_time_class *tc, const __dtfl_diff_class *rin);
void __dtfl_discover_scratch_free(__dtfl_discover_scratch *s);

/*
 * discover_times (common.rs): append to `moments` the times in [lower,
 * upper) at which the key must be re-evaluated, and to `pended` the times
 * at or beyond upper to carry forward (both __dtfl_time_val vectors).  The
 * seed and output times are strided runs as in __dtfl_time_history_load.
 */
void __dtfl_discover_times(const __dtfl_key_view *key,
    const uint64_t *seed_times, size_t nseed, size_t seed_stride,
    const uint64_t *out_times, size_t nout, size_t out_stride,
    const uint64_t *upper, size_t nupper, __dtfl_discover_scratch *scratch,
    __dtfl_vec *moments, __dtfl_vec *pended);

#endif /* DTFL_DD_COMMON_INT_H */
