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
