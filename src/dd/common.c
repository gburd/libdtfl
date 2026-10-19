/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/mod.rs
 *   differential-dataflow/src/operators/common.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/common.c: the time-ordered replay machinery shared by the
 * differential operators (join here, reduce next door).
 *
 * __dtfl_value_history (ValueHistory in operators/mod.rs) loads one key's
 * (value, time, diff) edits, orders them by time, and replays them with a
 * meet-advanced, consolidated buffer so a key with many distinct times
 * stays linear rather than quadratic.  __dtfl_join_think (the JoinThinker
 * and its bilinear_wave in common.rs) joins two such histories: for every
 * pair of edits it joins the times and multiplies the diffs, visiting the
 * products in time order.  Both are value-and-container agnostic: a value
 * is an index into the history's own slices arena, a time is tw uint64_t
 * words, a diff is dw bytes with a __dtfl_diff_class algebra.
 *
 * The reduce operator loads a single input history the same way and walks
 * one side's edits (map); it shares __dtfl_value_history and ignores the
 * two-history join driver.  Nothing here is installed or exported.
 */
#include "dd/common_int.h"
#include "dd/dd_int.h"
#include "dd/trace/trace_int.h"
#include "dtfl/record.h"
#include "progress/time_int.h"

/*
 * A stable merge sort of row indices by `cmp(ctx, i, j)`, matching the
 * pattern of consolidate.c: the records themselves may be large, so we
 * sort an index permutation and the caller gathers.  `idx` is a scratch
 * array of `n` indices (already filled 0..n), `tmp` an n-element scratch.
 */
static void
idx_msort(size_t *idx, size_t *tmp, size_t n,
    int (*cmp)(void *ctx, size_t a, size_t b), void *ctx)
{
	size_t h = n / 2, i = 0, j, k = 0;

	if (n < 2)
		return;
	idx_msort(idx, tmp, h, cmp, ctx);
	idx_msort(idx + h, tmp, n - h, cmp, ctx);
	if (cmp(ctx, idx[h - 1], idx[h]) <= 0)
		return;
	for (j = h; i < h && j < n;)
		tmp[k++] = cmp(ctx, idx[j], idx[i]) < 0 ? idx[j++] : idx[i++];
	while (i < h)
		tmp[k++] = idx[i++];
	memcpy(idx, tmp, k * sizeof *idx);
}

/* ------------------------------------------------------------------ */
/* EditList (operators/mod.rs EditList): one key's (value, time, diff).  */
/* ------------------------------------------------------------------ */

static void
edit_list_init(__dtfl_edit_list *e, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc)
{
	__dtfl_slices_init(&e->vals);
	__dtfl_vec_init(&e->vend, sizeof(size_t));
	e->tc = tc;
	e->dc = dc;
	e->tw = dtfl_time_depth(tc);
	e->dw = (unsigned)dc->width;
	__dtfl_vec_init(&e->times, (size_t)e->tw * sizeof(uint64_t));
	__dtfl_vec_init(&e->diffs, DTFL_MAX(e->dw, 1u));
}

static void
edit_list_free(__dtfl_edit_list *e)
{
	__dtfl_slices_free(&e->vals);
	__dtfl_vec_free(&e->vend);
	__dtfl_vec_free(&e->times);
	__dtfl_vec_free(&e->diffs);
}

static void
edit_list_clear(__dtfl_edit_list *e)
{
	__dtfl_slices_clear(&e->vals);
	__dtfl_vec_clear(&e->vend);
	__dtfl_vec_clear(&e->times);
	__dtfl_vec_clear(&e->diffs);
}

static size_t
edit_list_nvals(const __dtfl_edit_list *e)
{
	return e->vend.len;
}

static size_t
edit_list_nedits(const __dtfl_edit_list *e)
{
	return e->times.len;
}

/* Lower and upper edit bounds of value i. */
static void
edit_list_bounds(const __dtfl_edit_list *e, size_t i, size_t *lo, size_t *hi)
{
	*lo = (i == 0) ? 0 : *DTFL_VEC_AT(&e->vend, size_t, i - 1);
	*hi = *DTFL_VEC_AT(&e->vend, size_t, i);
}

static const uint64_t *
edit_list_time(const __dtfl_edit_list *e, size_t i)
{
	return (const uint64_t *)(void *)e->times.data + (size_t)e->tw * i;
}

static const void *
edit_list_diff(const __dtfl_edit_list *e, size_t i)
{
	return e->diffs.data + (size_t)e->dw * i;
}

/* Pushes one edit for the value not yet sealed (advance time by meet). */
static void
edit_list_push(__dtfl_edit_list *e, const uint64_t *time, const void *diff,
    const uint64_t *meet)
{
	uint64_t t[DTFL_TIME_MAX] = { 0 };

	if (e->tw != 0) {
		memcpy(t, time, (size_t)e->tw * sizeof(uint64_t));
		if (meet != NULL)
			dtfl_time_join(e->tc, t, meet, t);
	}
	__dtfl_vec_push(&e->times, t);
	if (e->dw != 0)
		__dtfl_vec_push(&e->diffs, diff);
	else {
		unsigned char z = 0;

		__dtfl_vec_push(&e->diffs, &z);
	}
}

/*
 * Consolidate the edits pushed since the last seal (from index `prev`) by
 * (time only), summing equal times and dropping zero sums.  Insertion
 * sort of the small per-value edit run, as upstream's consolidate_from.
 */
static size_t
edit_list_consolidate_from(__dtfl_edit_list *e, size_t prev)
{
	size_t n = edit_list_nedits(e), i, j, w;
	unsigned tw = e->tw, dw = e->dw;
	unsigned char *tb = (unsigned char *)e->times.data;
	unsigned char *db = (unsigned char *)e->diffs.data;
	size_t tsz = (size_t)tw * sizeof(uint64_t);
	size_t dsz = DTFL_MAX(dw, 1u);

	/* Stable insertion sort of [prev, n) by time words. */
	for (i = prev + 1; i < n; i++) {
		uint64_t kt[DTFL_TIME_MAX];
		unsigned char kd[DTFL_COMMON_DIFF_MAX];

		memcpy(kt, tb + i * tsz, tsz);
		memcpy(kd, db + i * dsz, dsz);
		j = i;
		while (j > prev &&
		    __dtfl_words_cmp((const uint64_t *)(void *)(tb + (j - 1) * tsz),
		    kt, tw) > 0) {
			memcpy(tb + j * tsz, tb + (j - 1) * tsz, tsz);
			memcpy(db + j * dsz, db + (j - 1) * dsz, dsz);
			j--;
		}
		memcpy(tb + j * tsz, kt, tsz);
		memcpy(db + j * dsz, kd, dsz);
	}

	/* Fold equal-time runs, keeping non-zero sums, compacting to w. */
	w = prev;
	for (i = prev; i < n;) {
		unsigned char acc[DTFL_COMMON_DIFF_MAX];

		memcpy(acc, db + i * dsz, dsz);
		for (j = i + 1; j < n &&
		    __dtfl_words_cmp((const uint64_t *)(void *)(tb + i * tsz),
		    (const uint64_t *)(void *)(tb + j * tsz), tw) == 0; j++)
			e->dc->plus_equals(e->dc, acc, db + j * dsz);
		if (!e->dc->is_zero(e->dc, acc)) {
			memmove(tb + w * tsz, tb + i * tsz, tsz);
			memcpy(db + w * dsz, acc, dsz);
			w++;
		}
		i = j;
	}
	e->times.len = w;
	e->diffs.len = w;
	return w;
}

/* Seals the current value's edits; records it only if any survive. */
static void
edit_list_seal(__dtfl_edit_list *e, const void *val, size_t vlen)
{
	size_t prev = (e->vend.len == 0) ? 0 :
	    *DTFL_VEC_AT(&e->vend, size_t, e->vend.len - 1);
	size_t after = edit_list_consolidate_from(e, prev);

	if (after > prev) {
		__dtfl_slices_push(&e->vals, val, vlen);
		__dtfl_vec_push(&e->vend, &after);
	}
}

/* ------------------------------------------------------------------ */
/* ValueHistory (operators/mod.rs ValueHistory): time-ordered replay.    */
/* ------------------------------------------------------------------ */

/* One history record: (time, meet, value index, edit index). */
typedef struct hist_rec {
	uint64_t time[DTFL_TIME_MAX];
	uint64_t meet[DTFL_TIME_MAX];
	size_t value_index;
	size_t edit_index;
} hist_rec;

/* One buffered edit: ((value index, time), diff) accumulated/collapsed. */
typedef struct buf_rec {
	size_t value_index;
	uint64_t time[DTFL_TIME_MAX];
	unsigned char diff[DTFL_COMMON_DIFF_MAX];
} buf_rec;

void
__dtfl_value_history_init(__dtfl_value_history *h, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc)
{
	edit_list_init(&h->edits, tc, dc);
	__dtfl_vec_init(&h->history, sizeof(hist_rec));
	__dtfl_vec_init(&h->buffer, sizeof(buf_rec));
	h->tc = tc;
	h->dc = dc;
	h->tw = dtfl_time_depth(tc);
	h->dw = (unsigned)dc->width;
}

void
__dtfl_value_history_free(__dtfl_value_history *h)
{
	edit_list_free(&h->edits);
	__dtfl_vec_free(&h->history);
	__dtfl_vec_free(&h->buffer);
}

void
__dtfl_value_history_clear(__dtfl_value_history *h)
{
	edit_list_clear(&h->edits);
	__dtfl_vec_clear(&h->history);
	__dtfl_vec_clear(&h->buffer);
}

/* Compares two history records descending by time (so the least pops last). */
static int
hist_desc_idx(void *vctx, size_t a, size_t b)
{
	struct { const hist_rec *recs; unsigned tw; } *c = vctx;

	return __dtfl_words_cmp(c->recs[b].time, c->recs[a].time, c->tw);
}

/*
 * build (ValueHistory::build): expand the edit list into one history
 * record per edit, sort descending by time, and set each record's meet to
 * the meet of its time with all times later in the replay (the suffix
 * meet).  After this `history.last()` is the least un-replayed time.
 */
static void
value_history_build(__dtfl_value_history *h)
{
	size_t nv = edit_list_nvals(&h->edits), vi, ei;
	size_t i, n;
	hist_rec *recs;

	__dtfl_vec_clear(&h->buffer);
	__dtfl_vec_clear(&h->history);
	for (vi = 0; vi < nv; vi++) {
		size_t lo, hi;

		edit_list_bounds(&h->edits, vi, &lo, &hi);
		for (ei = lo; ei < hi; ei++) {
			hist_rec r;

			memset(&r, 0, sizeof r);
			if (h->tw != 0) {
				memcpy(r.time, edit_list_time(&h->edits, ei),
				    (size_t)h->tw * sizeof(uint64_t));
				memcpy(r.meet, r.time,
				    (size_t)h->tw * sizeof(uint64_t));
			}
			r.value_index = vi;
			r.edit_index = ei;
			__dtfl_vec_push(&h->history, &r);
		}
	}
	n = h->history.len;
	if (n == 0)
		return;
	recs = (hist_rec *)(void *)h->history.data;
	if (n > 1) {
		struct { const hist_rec *recs; unsigned tw; } cx =
		    { recs, h->tw };
		size_t *idx = __dtfl_alloc(__dtfl_mul_size(n, 2 * sizeof *idx));
		size_t *tmp = idx + n;
		hist_rec *sorted = __dtfl_alloc(__dtfl_mul_size(n,
		    sizeof(hist_rec)));

		for (i = 0; i < n; i++)
			idx[i] = i;
		idx_msort(idx, tmp, n, hist_desc_idx, &cx);
		for (i = 0; i < n; i++)
			sorted[i] = recs[idx[i]];
		memcpy(recs, sorted, n * sizeof(hist_rec));
		__dtfl_free(sorted);
		__dtfl_free(idx);
	}
	/*
	 * Suffix meet: records are descending by time, so index n-1 is the
	 * least.  reduce from front to back carrying the running meet, which
	 * matches upstream's iter_mut().reduce(prev -> cur): cur.meet =
	 * meet(cur.meet, prev.meet).
	 */
	for (i = 1; i < n; i++)
		if (h->tw != 0)
			dtfl_time_meet(h->tc, recs[i].meet, recs[i - 1].meet,
			    recs[i].meet);
}

/* The next (least) un-replayed time, or NULL. */
static const uint64_t *
value_history_time(const __dtfl_value_history *h)
{
	if (h->history.len == 0)
		return NULL;
	return ((const hist_rec *)(void *)h->history.data)[h->history.len - 1].time;
}

/* The meet of all un-replayed times, or NULL. */
static const uint64_t *
value_history_meet(const __dtfl_value_history *h)
{
	if (h->history.len == 0)
		return NULL;
	return ((const hist_rec *)(void *)h->history.data)[h->history.len - 1].meet;
}

/* The next un-replayed edit's value bytes, time and diff. */
static void
value_history_edit(const __dtfl_value_history *h, const void **val,
    size_t *vlen, const uint64_t **time, const void **diff)
{
	const hist_rec *r = &((const hist_rec *)(void *)h->history.data)
	    [h->history.len - 1];

	*val = __dtfl_slices_get(&h->edits.vals, r->value_index, vlen);
	*time = r->time;
	*diff = edit_list_diff(&h->edits, r->edit_index);
}

/* Move the next edit into the buffer. */
static void
value_history_step(__dtfl_value_history *h)
{
	hist_rec r = ((hist_rec *)(void *)h->history.data)[h->history.len - 1];
	buf_rec b;

	h->history.len--;
	memset(&b, 0, sizeof b);
	b.value_index = r.value_index;
	if (h->tw != 0)
		memcpy(b.time, r.time, (size_t)h->tw * sizeof(uint64_t));
	if (h->dw != 0)
		memcpy(b.diff, edit_list_diff(&h->edits, r.edit_index), h->dw);
	__dtfl_vec_push(&h->buffer, &b);
}

/* Compares buffered edits by (value index, time) for consolidation. */
static int
buf_cmp_idx(void *vctx, size_t a, size_t b)
{
	struct { const buf_rec *recs; unsigned tw; } *c = vctx;
	const buf_rec *x = &c->recs[a], *y = &c->recs[b];

	if (x->value_index < y->value_index)
		return -1;
	if (x->value_index > y->value_index)
		return 1;
	return __dtfl_words_cmp(x->time, y->time, c->tw);
}

/*
 * advance_buffer_by (ValueHistory::advance_buffer_by): join every buffered
 * time by `meet`, then consolidate the buffer by (value, time), dropping
 * zero diffs.  This is the collapse that keeps the replay linear.
 */
static void
value_history_advance_buffer_by(__dtfl_value_history *h, const uint64_t *meet)
{
	buf_rec *b = (buf_rec *)(void *)h->buffer.data;
	size_t n = h->buffer.len, i, w;

	if (n == 0)
		return;
	if (h->tw != 0)
		for (i = 0; i < n; i++)
			dtfl_time_join(h->tc, b[i].time, meet, b[i].time);
	if (n > 1) {
		struct { const buf_rec *recs; unsigned tw; } cx = { b, h->tw };
		size_t *idx = __dtfl_alloc(__dtfl_mul_size(n, 2 * sizeof *idx));
		size_t *tmp = idx + n;
		buf_rec *sorted = __dtfl_alloc(__dtfl_mul_size(n,
		    sizeof(buf_rec)));

		for (i = 0; i < n; i++)
			idx[i] = i;
		idx_msort(idx, tmp, n, buf_cmp_idx, &cx);
		for (i = 0; i < n; i++)
			sorted[i] = b[idx[i]];
		memcpy(b, sorted, n * sizeof(buf_rec));
		__dtfl_free(sorted);
		__dtfl_free(idx);
	}
	w = 0;
	for (i = 0; i < n;) {
		buf_rec acc = b[i];
		size_t j;

		for (j = i + 1; j < n && b[j].value_index == acc.value_index &&
		    __dtfl_words_cmp(b[j].time, acc.time, h->tw) == 0; j++)
			if (h->dw != 0)
				h->dc->plus_equals(h->dc, acc.diff, b[j].diff);
		if (h->dw == 0 || !h->dc->is_zero(h->dc, acc.diff))
			b[w++] = acc;
		i = j;
	}
	h->buffer.len = w;
}

/* ------------------------------------------------------------------ */
/* Loading a history from a cursor (EditList::load / ValueHistory::build).*/
/* ------------------------------------------------------------------ */

/* map_times callback: pushes each (time, diff) of the current value. */
typedef struct load_ctx {
	__dtfl_edit_list *edits;
	const uint64_t *meet;
} load_ctx;

static void
load_on_time(const uint64_t *time, const void *diff, void *vctx)
{
	load_ctx *c = vctx;

	edit_list_push(c->edits, time, diff, c->meet);
}

/*
 * __dtfl_value_history_load_cursor: load the edits of the cursor list's
 * current key (its values and their (time, diff) updates), advancing each
 * time by `meet` if given, then build the time-ordered replay.  The cursor
 * is left positioned past the key's values; the caller steps the key.
 */
void
__dtfl_value_history_load_cursor(__dtfl_value_history *h,
    __dtfl_cursor_list *cl, const uint64_t *meet)
{
	__dtfl_value_history_clear(h);
	while (__dtfl_cursor_list_val_valid(cl)) {
		size_t vlen;
		const void *val = __dtfl_cursor_list_val(cl, &vlen);
		load_ctx lc = { &h->edits, meet };

		__dtfl_cursor_list_map_times(cl, load_on_time, &lc);
		edit_list_seal(&h->edits, val, vlen);
		__dtfl_cursor_list_step_val(cl);
	}
	value_history_build(h);
}

/* The number of edits (post-consolidation) loaded; for the think gate. */
size_t
__dtfl_value_history_nedits(const __dtfl_value_history *h)
{
	return edit_list_nedits(&h->edits);
}

/*
 * __dtfl_value_history_map: visit every (value, time, diff) edit of a
 * loaded history, in (value, time) order.  Reduce's single-history walk.
 */
void
__dtfl_value_history_map(const __dtfl_value_history *h, __dtfl_edit_fn fn,
    void *ctx)
{
	size_t nv = edit_list_nvals(&h->edits), vi, ei;

	for (vi = 0; vi < nv; vi++) {
		size_t lo, hi, vlen;
		const void *val = __dtfl_slices_get(&h->edits.vals, vi, &vlen);

		edit_list_bounds(&h->edits, vi, &lo, &hi);
		for (ei = lo; ei < hi; ei++)
			fn(ctx, val, vlen, edit_list_time(&h->edits, ei),
			    edit_list_diff(&h->edits, ei));
	}
}

/* ------------------------------------------------------------------ */
/* JoinThinker (join.rs cursors::JoinThinker::think + bilinear_wave).    */
/* ------------------------------------------------------------------ */

/* The dead-simple cross product used when either history is small. */
static void
join_cross(const __dtfl_value_history *h1, const __dtfl_value_history *h2,
    __dtfl_join_emit_fn emit, void *ctx)
{
	size_t nv1 = edit_list_nvals(&h1->edits), vi1, ei1;
	const dtfl_time_class *tc = h1->tc;
	unsigned tw = h1->tw;

	for (vi1 = 0; vi1 < nv1; vi1++) {
		size_t lo1, hi1, v1len;
		const void *v1 = __dtfl_slices_get(&h1->edits.vals, vi1, &v1len);

		edit_list_bounds(&h1->edits, vi1, &lo1, &hi1);
		for (ei1 = lo1; ei1 < hi1; ei1++) {
			const uint64_t *t1 = edit_list_time(&h1->edits, ei1);
			const void *d1 = edit_list_diff(&h1->edits, ei1);
			size_t nv2 = edit_list_nvals(&h2->edits), vi2, ei2;

			for (vi2 = 0; vi2 < nv2; vi2++) {
				size_t lo2, hi2, v2len;
				const void *v2 = __dtfl_slices_get(
				    &h2->edits.vals, vi2, &v2len);

				edit_list_bounds(&h2->edits, vi2, &lo2, &hi2);
				for (ei2 = lo2; ei2 < hi2; ei2++) {
					const uint64_t *t2 =
					    edit_list_time(&h2->edits, ei2);
					const void *d2 =
					    edit_list_diff(&h2->edits, ei2);
					uint64_t jt[DTFL_TIME_MAX];

					if (tw != 0)
						dtfl_time_join(tc, t1, t2, jt);
					emit(ctx, v1, v1len, v2, v2len, jt, d1,
					    d2);
				}
			}
		}
	}
}

/*
 * The buffer wave used when both histories are large (bilinear_wave):
 * step the history with the earlier un-replayed edit, consolidate the
 * other's buffer under the meet of its remaining times, and emit the
 * product of the stepped edit with every buffered edit of the other side.
 */
static void
join_wave(__dtfl_value_history *h1, __dtfl_value_history *h2,
    __dtfl_join_emit_fn emit, void *ctx)
{
	const dtfl_time_class *tc = h1->tc;
	unsigned tw = h1->tw;

	for (;;) {
		const uint64_t *t1 = value_history_time(h1);
		const uint64_t *t2 = value_history_time(h2);
		bool step1;

		if (t1 != NULL && t2 != NULL)
			step1 = __dtfl_words_cmp(t1, t2, tw) < 0;
		else if (t1 != NULL)
			step1 = true;
		else if (t2 != NULL)
			step1 = false;
		else
			break;

		if (step1) {
			const void *ev, *ed;
			size_t evlen;
			const uint64_t *et;
			const buf_rec *b;
			size_t i, n;

			value_history_advance_buffer_by(h2,
			    value_history_meet(h1));
			value_history_edit(h1, &ev, &evlen, &et, &ed);
			b = (const buf_rec *)(void *)h2->buffer.data;
			n = h2->buffer.len;
			for (i = 0; i < n; i++) {
				size_t bvlen;
				const void *bv = __dtfl_slices_get(
				    &h2->edits.vals, b[i].value_index, &bvlen);
				uint64_t jt[DTFL_TIME_MAX];

				if (tw != 0)
					dtfl_time_join(tc, et, b[i].time, jt);
				emit(ctx, ev, evlen, bv, bvlen, jt, ed,
				    b[i].diff);
			}
			value_history_step(h1);
		} else {
			const void *ev, *ed;
			size_t evlen;
			const uint64_t *et;
			const buf_rec *b;
			size_t i, n;

			value_history_advance_buffer_by(h1,
			    value_history_meet(h2));
			value_history_edit(h2, &ev, &evlen, &et, &ed);
			b = (const buf_rec *)(void *)h1->buffer.data;
			n = h1->buffer.len;
			for (i = 0; i < n; i++) {
				size_t bvlen;
				const void *bv = __dtfl_slices_get(
				    &h1->edits.vals, b[i].value_index, &bvlen);
				uint64_t jt[DTFL_TIME_MAX];

				if (tw != 0)
					dtfl_time_join(tc, b[i].time, et, jt);
				emit(ctx, bv, bvlen, ev, evlen, jt, b[i].diff,
				    ed);
			}
			value_history_step(h2);
		}
	}
}

/*
 * think (JoinThinker::think): for reasonably sized edits, do the dead-simple
 * cross product; otherwise replay the two histories as a bilinear wave.  The
 * threshold is upstream's 10 edits on either side.
 */
void
__dtfl_join_think(__dtfl_value_history *h1, __dtfl_value_history *h2,
    __dtfl_join_emit_fn emit, void *ctx)
{
	if (edit_list_nedits(&h1->edits) < 10 ||
	    edit_list_nedits(&h2->edits) < 10)
		join_cross(h1, h2, emit, ctx);
	else
		join_wave(h1, h2, emit, ctx);
}

/* ------------------------------------------------------------------ */
/* Integer-proxy support: ValueHistory::load_iter and step_through      */
/* (operators/mod.rs), bilinear_wave, TimeHistory, tile_descriptions,   */
/* KeyView, DiscoverScratch and discover_times (common.rs).             */
/* ------------------------------------------------------------------ */

const uint64_t *
__dtfl_value_history_time(const __dtfl_value_history *h)
{
	return value_history_time(h);
}

const uint64_t *
__dtfl_value_history_meet(const __dtfl_value_history *h)
{
	return value_history_meet(h);
}

void
__dtfl_value_history_step(__dtfl_value_history *h)
{
	value_history_step(h);
}

/* Step edits while the next time equals `time`; true iff any did. */
bool
__dtfl_value_history_step_while_time_is(__dtfl_value_history *h,
    const uint64_t *time)
{
	bool found = false;

	while (value_history_time(h) != NULL &&
	    __dtfl_words_cmp(value_history_time(h), time, h->tw) == 0) {
		found = true;
		value_history_step(h);
	}
	return found;
}

/*
 * Step edits while the next time is <= `time` in the TOTAL order (a superset
 * of the partially-ordered downset; readers filter the buffer by less_equal
 * themselves).
 */
void
__dtfl_value_history_step_through(__dtfl_value_history *h,
    const uint64_t *time)
{
	while (value_history_time(h) != NULL &&
	    __dtfl_words_cmp(value_history_time(h), time, h->tw) <= 0)
		value_history_step(h);
}

void
__dtfl_value_history_advance_buffer_by(__dtfl_value_history *h,
    const uint64_t *meet)
{
	value_history_advance_buffer_by(h, meet);
}

size_t
__dtfl_value_history_buffer_len(const __dtfl_value_history *h)
{
	return h->buffer.len;
}

const void *
__dtfl_value_history_buffer_at(const __dtfl_value_history *h, size_t i,
    size_t *vlen, const uint64_t **time, const void **diff)
{
	const buf_rec *b = &((const buf_rec *)(void *)h->buffer.data)[i];

	*time = b->time;
	*diff = b->diff;
	return __dtfl_slices_get(&h->edits.vals, b->value_index, vlen);
}

/*
 * load_iter: edits grouped by consecutive value (the presentation order),
 * each time joined with advance_by if supplied, then build.  Ungrouped input
 * is still correct, only less compact.  The value is the id as 8 bytes of
 * dtfl_put_u64, so value order is id order.
 */
void
__dtfl_value_history_load_iter(__dtfl_value_history *h,
    const __dtfl_bridge_upd *rows, size_t n, const uint64_t *advance_by)
{
	unsigned char cur[8];
	bool have = false;
	size_t i;

	edit_list_clear(&h->edits);
	for (i = 0; i < n; i++) {
		if (!have || dtfl_get_u64(cur) != rows[i].id) {
			if (have)
				edit_list_seal(&h->edits, cur, sizeof cur);
			dtfl_put_u64(cur, rows[i].id);
			have = true;
		}
		edit_list_push(&h->edits, rows[i].time, rows[i].diff,
		    advance_by);
	}
	if (have)
		edit_list_seal(&h->edits, cur, sizeof cur);
	value_history_build(h);
}

/* bilinear_wave: the join of two histories, in time order. */
void
__dtfl_bilinear_wave(__dtfl_value_history *h0, __dtfl_value_history *h1,
    __dtfl_join_emit_fn emit, void *ctx)
{
	join_wave(h0, h1, emit, ctx);
}

/* Compares two __dtfl_time_val by Ord, for idx_msort. */
typedef struct tv_sort_ctx {
	const __dtfl_time_val *v;
	unsigned tw;
} tv_sort_ctx;

static int
tv_cmp_idx(void *vctx, size_t a, size_t b)
{
	tv_sort_ctx *c = vctx;

	return __dtfl_words_cmp(c->v[a].t, c->v[b].t, c->tw);
}

/* Sort `v` (__dtfl_time_val) ascending, or descending if `desc`. */
static void
times_sort(__dtfl_vec *v, unsigned tw, bool desc)
{
	size_t n = v->len, i;
	__dtfl_time_val *t = (__dtfl_time_val *)(void *)v->data, *sorted;
	size_t *idx;
	tv_sort_ctx cx = { t, tw };

	if (n < 2)
		return;
	idx = __dtfl_alloc(__dtfl_mul_size(n, 2 * sizeof *idx));
	sorted = __dtfl_alloc(__dtfl_mul_size(n, sizeof *sorted));
	for (i = 0; i < n; i++)
		idx[i] = i;
	idx_msort(idx, idx + n, n, tv_cmp_idx, &cx);
	for (i = 0; i < n; i++)
		sorted[i] = t[idx[desc ? n - 1 - i : i]];
	memcpy(t, sorted, n * sizeof *sorted);
	__dtfl_free(sorted);
	__dtfl_free(idx);
}

/* Drop adjacent duplicates (Vec::dedup). */
static void
times_dedup(__dtfl_vec *v, unsigned tw)
{
	__dtfl_time_val *t = (__dtfl_time_val *)(void *)v->data;
	size_t i, w = 0;

	for (i = 0; i < v->len; i++)
		if (w == 0 || __dtfl_words_cmp(t[w - 1].t, t[i].t, tw) != 0)
			t[w++] = t[i];
	v->len = w;
}

/* reduce.rs sort_dedup. */
void
__dtfl_times_sort_dedup(__dtfl_vec *v, unsigned tw)
{
	times_sort(v, tw, false);
	times_dedup(v, tw);
}

static void
times_push(__dtfl_vec *v, const uint64_t *t, unsigned tw)
{
	__dtfl_time_val x;

	memset(&x, 0, sizeof x);
	if (tw != 0)
		memcpy(x.t, t, (size_t)tw * sizeof(uint64_t));
	__dtfl_vec_push(v, &x);
}

static __dtfl_time_val *
times_at(const __dtfl_vec *v, size_t i)
{
	return &((__dtfl_time_val *)(void *)v->data)[i];
}

typedef struct bridge_sort_ctx {
	const __dtfl_bridge_upd *r;
	unsigned tw;
} bridge_sort_ctx;

/* Orders bridge rows by ((hash, id), time). */
int
__dtfl_bridge_cmp(const __dtfl_bridge_upd *x, const __dtfl_bridge_upd *y,
    unsigned tw)
{
	if (x->hash != y->hash)
		return x->hash < y->hash ? -1 : 1;
	if (x->id != y->id)
		return x->id < y->id ? -1 : 1;
	return __dtfl_words_cmp(x->time, y->time, tw);
}

static int
bridge_cmp_idx(void *vctx, size_t a, size_t b)
{
	bridge_sort_ctx *c = vctx;

	return __dtfl_bridge_cmp(&c->r[a], &c->r[b], c->tw);
}

void
__dtfl_bridge_sort(__dtfl_vec *rows, unsigned tw)
{
	__dtfl_bridge_upd *r = (__dtfl_bridge_upd *)(void *)rows->data;
	size_t n = rows->len, i;
	bridge_sort_ctx cx = { r, tw };
	size_t *idx;
	__dtfl_bridge_upd *sorted;

	if (n < 2)
		return;
	idx = __dtfl_alloc(__dtfl_mul_size(n, 2 * sizeof *idx));
	sorted = __dtfl_alloc(__dtfl_mul_size(n, sizeof *sorted));
	for (i = 0; i < n; i++)
		idx[i] = i;
	idx_msort(idx, idx + n, n, bridge_cmp_idx, &cx);
	for (i = 0; i < n; i++)
		sorted[i] = r[idx[i]];
	memcpy(r, sorted, n * sizeof *sorted);
	__dtfl_free(sorted);
	__dtfl_free(idx);
}

void
__dtfl_bridge_consolidate(__dtfl_vec *rows, unsigned tw,
    const __dtfl_diff_class *dc)
{
	__dtfl_bridge_upd *r;
	size_t n = rows->len, i, j, w = 0;

	__dtfl_bridge_sort(rows, tw);
	r = (__dtfl_bridge_upd *)(void *)rows->data;
	for (i = 0; i < n; i = j) {
		__dtfl_bridge_upd acc = r[i];
		bridge_sort_ctx cx = { r, tw };

		for (j = i + 1; j < n && bridge_cmp_idx(&cx, i, j) == 0; j++)
			if (dc->width != 0)
				dc->plus_equals(dc, acc.diff, r[j].diff);
		if (dc->width == 0 || !dc->is_zero(dc, acc.diff))
			r[w++] = acc;
	}
	rows->len = w;
}

/* TimeHistory: (time, meet) pairs, sorted descending by time. */
typedef struct time_meet {
	uint64_t time[DTFL_TIME_MAX];
	uint64_t meet[DTFL_TIME_MAX];
} time_meet;

void
__dtfl_time_history_init(__dtfl_time_history *h, const dtfl_time_class *tc)
{
	__dtfl_vec_init(&h->history, sizeof(time_meet));
	__dtfl_vec_init(&h->buffer, sizeof(__dtfl_time_val));
	h->tc = tc;
	h->tw = dtfl_time_depth(tc);
}

void
__dtfl_time_history_free(__dtfl_time_history *h)
{
	__dtfl_vec_free(&h->history);
	__dtfl_vec_free(&h->buffer);
}

static int
tm_desc_idx(void *vctx, size_t a, size_t b)
{
	struct { const time_meet *r; unsigned tw; } *c = vctx;

	return __dtfl_words_cmp(c->r[b].time, c->r[a].time, c->tw);
}

/* Load times, advancing each by advance_by; sort + suffix meets. */
void
__dtfl_time_history_load(__dtfl_time_history *h, const uint64_t *first,
    size_t n, size_t stride, const uint64_t *advance_by)
{
	time_meet *r;
	size_t i;

	__dtfl_vec_clear(&h->history);
	__dtfl_vec_clear(&h->buffer);
	for (i = 0; i < n; i++) {
		time_meet x;
		const uint64_t *t = (const uint64_t *)(const void *)
		    ((const unsigned char *)first + i * stride);

		memset(&x, 0, sizeof x);
		if (h->tw != 0) {
			memcpy(x.time, t, (size_t)h->tw * sizeof(uint64_t));
			if (advance_by != NULL)
				dtfl_time_join(h->tc, x.time, advance_by,
				    x.time);
			memcpy(x.meet, x.time, (size_t)h->tw * sizeof(uint64_t));
		}
		__dtfl_vec_push(&h->history, &x);
	}
	n = h->history.len;
	r = (time_meet *)(void *)h->history.data;
	if (n > 1) {
		struct { const time_meet *r; unsigned tw; } cx = { r, h->tw };
		size_t *idx = __dtfl_alloc(__dtfl_mul_size(n, 2 * sizeof *idx));
		time_meet *sorted = __dtfl_alloc(__dtfl_mul_size(n,
		    sizeof *sorted));

		for (i = 0; i < n; i++)
			idx[i] = i;
		idx_msort(idx, idx + n, n, tm_desc_idx, &cx);
		for (i = 0; i < n; i++)
			sorted[i] = r[idx[i]];
		memcpy(r, sorted, n * sizeof *sorted);
		__dtfl_free(sorted);
		__dtfl_free(idx);
	}
	if (h->tw != 0)
		for (i = 1; i < n; i++)
			dtfl_time_meet(h->tc, r[i].meet, r[i - 1].meet,
			    r[i].meet);
}

/* The next (least) un-replayed time. */
const uint64_t *
__dtfl_time_history_time(const __dtfl_time_history *h)
{
	if (h->history.len == 0)
		return NULL;
	return ((const time_meet *)(void *)h->history.data)
	    [h->history.len - 1].time;
}

/* The meet of all un-replayed times. */
const uint64_t *
__dtfl_time_history_meet(const __dtfl_time_history *h)
{
	if (h->history.len == 0)
		return NULL;
	return ((const time_meet *)(void *)h->history.data)
	    [h->history.len - 1].meet;
}

/* Step times while the next equals `time`; true iff any did. */
bool
__dtfl_time_history_step_while_time_is(__dtfl_time_history *h,
    const uint64_t *time)
{
	bool found = false;

	while (__dtfl_time_history_time(h) != NULL &&
	    __dtfl_words_cmp(__dtfl_time_history_time(h), time, h->tw) == 0) {
		found = true;
		times_push(&h->buffer, __dtfl_time_history_time(h), h->tw);
		h->history.len--;
	}
	return found;
}

/*
 * Advance buffered times by `meet` and deduplicate: the collapse that keeps
 * replay linear.
 */
void
__dtfl_time_history_advance_buffer_by(__dtfl_time_history *h,
    const uint64_t *meet)
{
	size_t i;

	if (h->tw != 0)
		for (i = 0; i < h->buffer.len; i++)
			dtfl_time_join(h->tc, times_at(&h->buffer, i)->t, meet,
			    times_at(&h->buffer, i)->t);
	__dtfl_times_sort_dedup(&h->buffer, h->tw);
}

/*
 * tile_descriptions: the i-th cut point is the frontier formed by inserting
 * held[i+1..] into upper, so description i covers the part of the interval
 * not greater-or-equal any held time after held[i] (and not covered by an
 * earlier description).  Descriptions whose interval is empty are skipped.
 */
size_t
__dtfl_tile_descriptions(const __dtfl_antichain *lower,
    const __dtfl_antichain *upper, const __dtfl_antichain *held,
    __dtfl_description *descs, size_t *tile_held, size_t *tile_of)
{
	const dtfl_time_class *tc = lower->tc;
	size_t nheld = __dtfl_antichain_len(held), index, i, ntiles = 0;
	__dtfl_antichain out_lower;
	uint64_t minimum[DTFL_TIME_MAX] = { 0 };

	dtfl_time_minimum(tc, minimum);
	__dtfl_antichain_init(&out_lower, tc);
	__dtfl_antichain_copy(&out_lower, lower);
	for (index = 0; index < nheld; index++) {
		__dtfl_antichain out_upper;

		tile_of[index] = SIZE_MAX;
		__dtfl_antichain_init(&out_upper, tc);
		__dtfl_antichain_copy(&out_upper, upper);
		for (i = index + 1; i < nheld; i++)
			__dtfl_antichain_insert(&out_upper,
			    __dtfl_antichain_at(held, i));
		if (!__dtfl_antichain_eq(&out_upper, &out_lower)) {
			__dtfl_antichain lo, hi, since;

			tile_of[index] = ntiles;
			__dtfl_antichain_init(&lo, tc);
			__dtfl_antichain_copy(&lo, &out_lower);
			__dtfl_antichain_init(&hi, tc);
			__dtfl_antichain_copy(&hi, &out_upper);
			__dtfl_antichain_init_elem(&since, tc, minimum);
			__dtfl_description_init(&descs[ntiles], &lo, &hi,
			    &since);
			tile_held[ntiles] = index;
			ntiles++;
			__dtfl_antichain_copy(&out_lower, &out_upper);
		}
		__dtfl_antichain_free(&out_upper);
	}
	__dtfl_antichain_free(&out_lower);
	return ntiles;
}

void
__dtfl_discover_scratch_init(__dtfl_discover_scratch *s,
    const dtfl_time_class *tc, const __dtfl_diff_class *rin)
{
	__dtfl_time_history_init(&s->batch_replay, tc);
	__dtfl_value_history_init(&s->input_replay, tc, rin);
	__dtfl_time_history_init(&s->output_replay, tc);
	__dtfl_vec_init(&s->synth, sizeof(__dtfl_time_val));
	__dtfl_vec_init(&s->times_current, sizeof(__dtfl_time_val));
	__dtfl_vec_init(&s->temporary, sizeof(__dtfl_time_val));
	__dtfl_vec_init(&s->meets, sizeof(__dtfl_time_val));
}

void
__dtfl_discover_scratch_free(__dtfl_discover_scratch *s)
{
	__dtfl_time_history_free(&s->batch_replay);
	__dtfl_value_history_free(&s->input_replay);
	__dtfl_time_history_free(&s->output_replay);
	__dtfl_vec_free(&s->synth);
	__dtfl_vec_free(&s->times_current);
	__dtfl_vec_free(&s->temporary);
	__dtfl_vec_free(&s->meets);
}

/* Updates an optional meet by an optional time. */
static void
dt_update_meet(const dtfl_time_class *tc, uint64_t *meet, bool *has,
    const uint64_t *other)
{
	if (other == NULL)
		return;
	if (*has)
		dtfl_time_meet(tc, meet, other, meet);
	else {
		memcpy(meet, other, (size_t)dtfl_time_depth(tc) *
		    sizeof(uint64_t));
		*has = true;
	}
}

/* t joined with every element... of `src` not <= next, onto `dst`. */
static void
push_joins_not_le(const dtfl_time_class *tc, __dtfl_vec *dst,
    const uint64_t *t, const uint64_t *next)
{
	uint64_t j[DTFL_TIME_MAX] = { 0 };

	if (!dtfl_time_less_equal(tc, t, next)) {
		dtfl_time_join(tc, next, t, j);
		times_push(dst, j, dtfl_time_depth(tc));
	}
}

/*
 * discover_times: replays the key's seed_times and pending times in
 * ascending order, marking those that carry updates and closing the set
 * under joins with the input and output histories' times and with each
 * other.  No input collection is materialized, so peak memory is O(times);
 * buffers are advanced by the meet of the times still to come.
 *
 * seed_times must be the novel batch's own time support for this key.
 * Seeding from a consolidated view is unsound: compaction may advance a
 * history record onto a novel time, where consolidation cancels the novel
 * update and its interesting time is missed.
 */
void
__dtfl_discover_times(const __dtfl_key_view *key, const uint64_t *seed_times,
    size_t nseed, size_t seed_stride, const uint64_t *out_times, size_t nout,
    size_t out_stride, const uint64_t *upper, size_t nupper,
    __dtfl_discover_scratch *scratch, __dtfl_vec *moments, __dtfl_vec *pended)
{
	__dtfl_time_history *batch_replay = &scratch->batch_replay;
	__dtfl_value_history *input_replay = &scratch->input_replay;
	__dtfl_time_history *output_replay = &scratch->output_replay;
	__dtfl_vec *synth = &scratch->synth;
	__dtfl_vec *times_current = &scratch->times_current;
	__dtfl_vec *temporary = &scratch->temporary;
	__dtfl_vec *meets = &scratch->meets;
	const dtfl_time_class *tc = batch_replay->tc;
	unsigned tw = batch_replay->tw;
	uint64_t meet[DTFL_TIME_MAX] = { 0 };
	bool has_meet = false;
	size_t times_pos = 0, i;

	__dtfl_vec_clear(synth);
	__dtfl_vec_clear(times_current);
	__dtfl_vec_clear(temporary);

	__dtfl_time_history_load(batch_replay, seed_times, nseed, seed_stride,
	    NULL);

	__dtfl_vec_clear(meets);
	for (i = 0; i < key->npending; i++)
		__dtfl_vec_push(meets, &key->pending[i]);
	for (i = meets->len; i-- > 1;)
		dtfl_time_meet(tc, times_at(meets, i - 1)->t,
		    times_at(meets, i)->t, times_at(meets, i - 1)->t);

	dt_update_meet(tc, meet, &has_meet, meets->len != 0 ?
	    times_at(meets, 0)->t : NULL);
	dt_update_meet(tc, meet, &has_meet,
	    __dtfl_time_history_meet(batch_replay));

	/*
	 * The merged (history and novel) run, replayed for its TIMES only
	 * (join base), never accumulated.  Output times likewise: base joins,
	 * never seeds.
	 */
	__dtfl_value_history_load_iter(input_replay, key->p_in + key->i0,
	    key->i1 - key->i0, has_meet ? meet : NULL);
	__dtfl_time_history_load(output_replay, out_times, nout, out_stride,
	    has_meet ? meet : NULL);

	for (;;) {
		const uint64_t *cand[5];
		uint64_t next_time[DTFL_TIME_MAX] = { 0 };
		const uint64_t *least = NULL;
		bool interesting;
		size_t c;

		cand[0] = __dtfl_time_history_time(batch_replay);
		cand[1] = times_pos < key->npending ?
		    key->pending[times_pos].t : NULL;
		cand[2] = __dtfl_value_history_time(input_replay);
		cand[3] = __dtfl_time_history_time(output_replay);
		cand[4] = synth->len != 0 ?
		    times_at(synth, synth->len - 1)->t : NULL;
		for (c = 0; c < 5; c++)
			if (cand[c] != NULL && (least == NULL ||
			    __dtfl_words_cmp(cand[c], least, tw) < 0))
				least = cand[c];
		if (least == NULL)
			break;
		if (tw != 0)
			memcpy(next_time, least, (size_t)tw * sizeof(uint64_t));

		(void)__dtfl_value_history_step_while_time_is(input_replay,
		    next_time);
		(void)__dtfl_time_history_step_while_time_is(output_replay,
		    next_time);
		interesting = __dtfl_time_history_step_while_time_is(
		    batch_replay, next_time);
		if (interesting && has_meet)
			__dtfl_time_history_advance_buffer_by(batch_replay,
			    meet);
		while (synth->len != 0 && __dtfl_words_cmp(times_at(synth,
		    synth->len - 1)->t, next_time, tw) == 0) {
			__dtfl_vec_push(times_current,
			    times_at(synth, synth->len - 1));
			synth->len--;
			interesting = true;
		}
		while (times_pos < key->npending && __dtfl_words_cmp(
		    key->pending[times_pos].t, next_time, tw) == 0) {
			__dtfl_vec_push(times_current, &key->pending[times_pos]);
			times_pos++;
			interesting = true;
		}
		for (i = 0; !interesting && i < batch_replay->buffer.len; i++)
			interesting = dtfl_time_less_equal(tc,
			    times_at(&batch_replay->buffer, i)->t, next_time);
		for (i = 0; !interesting && i < times_current->len; i++)
			interesting = dtfl_time_less_equal(tc,
			    times_at(times_current, i)->t, next_time);

		if (!__dtfl_frontier_less_equal(tc, upper, nupper,
		    next_time)) {
			size_t synth_len;

			if (interesting) {
				/*
				 * Synthesize joins against the input/output
				 * histories (times only, no accumulation), then
				 * record next_time as an interesting moment.
				 */
				if (has_meet)
					__dtfl_value_history_advance_buffer_by(
					    input_replay, meet);
				for (i = 0; i < __dtfl_value_history_buffer_len(
				    input_replay); i++) {
					size_t vl;
					const uint64_t *t;
					const void *d;

					(void)__dtfl_value_history_buffer_at(
					    input_replay, i, &vl, &t, &d);
					push_joins_not_le(tc, temporary, t,
					    next_time);
				}
				if (has_meet)
					__dtfl_time_history_advance_buffer_by(
					    output_replay, meet);
				for (i = 0; i < output_replay->buffer.len; i++)
					push_joins_not_le(tc, temporary,
					    times_at(&output_replay->buffer,
					    i)->t, next_time);
				times_push(moments, next_time, tw);
			}
			for (i = 0; i < batch_replay->buffer.len; i++)
				push_joins_not_le(tc, temporary,
				    times_at(&batch_replay->buffer, i)->t,
				    next_time);
			for (i = 0; i < times_current->len; i++)
				push_joins_not_le(tc, temporary,
				    times_at(times_current, i)->t, next_time);
			__dtfl_times_sort_dedup(temporary, tw);
			synth_len = synth->len;
			for (i = 0; i < temporary->len; i++) {
				const uint64_t *t = times_at(temporary, i)->t;

				if (__dtfl_frontier_less_equal(tc, upper,
				    nupper, t))
					times_push(pended, t, tw);
				else
					times_push(synth, t, tw);
			}
			__dtfl_vec_clear(temporary);
			if (synth->len > synth_len) {
				times_sort(synth, tw, true);
				times_dedup(synth, tw);
			}
		} else if (interesting)
			times_push(pended, next_time, tw);

		has_meet = false;
		dt_update_meet(tc, meet, &has_meet,
		    __dtfl_time_history_meet(batch_replay));
		dt_update_meet(tc, meet, &has_meet,
		    __dtfl_value_history_meet(input_replay));
		dt_update_meet(tc, meet, &has_meet,
		    __dtfl_time_history_meet(output_replay));
		for (i = 0; i < synth->len; i++)
			dt_update_meet(tc, meet, &has_meet,
			    times_at(synth, i)->t);
		dt_update_meet(tc, meet, &has_meet, times_pos < meets->len ?
		    times_at(meets, times_pos)->t : NULL);
		if (has_meet)
			for (i = 0; i < times_current->len; i++)
				dtfl_time_join(tc, times_at(times_current,
				    i)->t, meet, times_at(times_current, i)->t);
		__dtfl_times_sort_dedup(times_current, tw);
	}
	__dtfl_times_sort_dedup(pended, tw);
}
