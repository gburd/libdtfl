/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/reduce.rs
 *   differential-dataflow/src/operators/mod.rs (EditList, ValueHistory)
 *   differential-dataflow/src/collection.rs (reduce, reduce_named,
 *     reduce_abelian, reduce_core, distinct, threshold, count methods)
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/reduce.c: the reduce operator and the reduce family of collection
 * methods (reduce, count, distinct, threshold).
 *
 * reduce reads one arranged (key -> val) input and produces an arranged
 * (key -> out_val) output.  Per key, per interesting time, it assembles the
 * accumulated input multiset and the previously produced output multiset,
 * applies the user logic to compute the output change, and ships that change
 * as the output batch.  reduce_abelian wraps the logic to retract the prior
 * output (negate and append), so the user logic produces only the new output
 * and the delta becomes retract-old then assert-new.  The per-key engine is
 * the history replayer: it walks the input, output and new-batch histories in
 * time order, discovering the interesting times (joins of input/output/seed
 * times, bounded by advancing to running meets) and evaluating the logic at
 * each, exactly as reduce.rs CursorTactic / HistoryReplayer do.  The
 * per-interval work goes through a ReduceTactic seam (rd_tactic_ops): the
 * cursor tactic is the default, and the model-derived reference tactic
 * (reduce.rs mod reference, ReferenceThinker) is the second engine, reached
 * only through __dtfl_arranged_reduce_trace_reference for differential tests.
 *
 * The batch/capability driver that arranges the input by key is private to
 * this file (arrange_by_key): it exchanges updates by key, batches each
 * frontier interval into a keyed value trace, and shares it through a trace
 * agent, mirroring arrangement.c's arrange_core but splitting key || val.
 */
#include "dd/arrange_int.h"
#include "dd/collection_int.h"
#include "dtfl/arrange.h"
#include "dtfl/reduce.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

/* ================================================================== */
/* Fixed-depth time vectors.                                           */
/* ================================================================== */

/*
 * Times are runs of `tw` uint64_t words (the scope depth).  A reduce call
 * fixes tw, so a time is TW words; buffers of times are flat runs.  The
 * engine compares by Ord (word order) and uses the time class for the
 * lattice (join, meet, less_equal).
 */

/* A dynamic vector of times, tw words each. */
typedef struct timevec {
	__dtfl_vec words;	/* uint64_t, tw per time */
	const dtfl_time_class *tc;
	unsigned tw;
} timevec;

static void
tv_init(timevec *v, const dtfl_time_class *tc, unsigned tw)
{
	__dtfl_vec_init(&v->words, sizeof(uint64_t));
	v->tc = tc;
	v->tw = tw;
}

static void
tv_free(timevec *v)
{
	__dtfl_vec_free(&v->words);
}

static void
tv_clear(timevec *v)
{
	__dtfl_vec_clear(&v->words);
}

static size_t
tv_len(const timevec *v)
{
	return v->tw != 0 ? v->words.len / v->tw : v->words.len;
}

static uint64_t *
tv_at(const timevec *v, size_t i)
{
	if (v->tw == 0)
		return (uint64_t *)(void *)v->words.data;	/* unit: no words */
	return (uint64_t *)(void *)v->words.data + (size_t)v->tw * i;
}

static void
tv_push(timevec *v, const uint64_t *t)
{
	size_t i;

	if (v->tw == 0) {
		/* Unit time: track a logical count via a dummy word. */
		uint64_t z = 0;

		__dtfl_vec_push(&v->words, &z);
		return;
	}
	for (i = 0; i < v->tw; i++)
		__dtfl_vec_push(&v->words, &t[i]);
}

static void
tv_truncate(timevec *v, size_t n)
{
	__dtfl_vec_truncate(&v->words, v->tw != 0 ? (size_t)v->tw * n : n);
}

/* qsort comparator state for sorting an index array by time words. */
typedef struct tsort_ctx {
	const timevec *v;
} tsort_ctx;

/* Sort times ascending by Ord, dedup in place (sort_dedup). */
static void
tv_sort_dedup(timevec *v)
{
	size_t n = tv_len(v), i, w;
	uint64_t *tmp;
	size_t *idx;

	if (n < 2) {
		if (v->tw == 0 && n == 1)
			return;
		return;
	}
	/* Insertion sort of row indices (n is small per key in practice). */
	idx = __dtfl_alloc(n * sizeof *idx);
	for (i = 0; i < n; i++)
		idx[i] = i;
	for (i = 1; i < n; i++) {
		size_t key = idx[i];
		size_t j = i;

		while (j > 0 && __dtfl_words_cmp(tv_at(v, idx[j - 1]),
		    tv_at(v, key), v->tw) > 0) {
			idx[j] = idx[j - 1];
			j--;
		}
		idx[j] = key;
	}
	/* Rebuild sorted and deduplicated. */
	tmp = __dtfl_alloc((size_t)v->tw * n * sizeof(uint64_t) +
	    sizeof(uint64_t));
	w = 0;
	for (i = 0; i < n; i++) {
		const uint64_t *t = tv_at(v, idx[i]);

		if (w > 0 && __dtfl_words_cmp(tmp + (w - 1) * v->tw, t,
		    v->tw) == 0)
			continue;
		memcpy(tmp + w * (size_t)v->tw, t,
		    (size_t)v->tw * sizeof(uint64_t));
		w++;
	}
	__dtfl_vec_clear(&v->words);
	for (i = 0; i < w; i++)
		tv_push(v, tmp + i * (size_t)v->tw);
	__dtfl_free(tmp);
	__dtfl_free(idx);
}

/* ================================================================== */
/* Interned value table.                                               */
/* ================================================================== */

/*
 * Values are opaque byte strings; the engine needs them Copy + Ord, so each
 * compute interns distinct values into a table and refers to them by index.
 * The index order is not the value order, so comparisons go through cmp on
 * the bytes.  The table lives for one key's computation.
 */
typedef struct valtab {
	__dtfl_slices slices;	/* value i bytes */
	const dtfl_type *vt;
} valtab;

static void
valtab_init(valtab *t, const dtfl_type *vt)
{
	__dtfl_slices_init(&t->slices);
	t->vt = vt;
}

static void
valtab_free(valtab *t)
{
	__dtfl_slices_free(&t->slices);
}

static void
valtab_clear(valtab *t)
{
	__dtfl_slices_clear(&t->slices);
}

/* Intern a value, returning its index (appends; no dedup, caller dedups). */
static size_t
valtab_intern(valtab *t, const void *val, size_t vlen)
{
	size_t i = __dtfl_slices_len(&t->slices);

	__dtfl_slices_push(&t->slices, val, vlen);
	return i;
}

static const void *
valtab_get(const valtab *t, size_t i, size_t *len)
{
	return __dtfl_slices_get(&t->slices, i, len);
}

static int
valtab_cmp(const valtab *t, size_t a, size_t b)
{
	size_t al, bl;
	const void *ap = valtab_get(t, a, &al);
	const void *bp = valtab_get(t, b, &bl);

	return dtfl_type_cmp(t->vt, ap, al, bp, bl);
}

/* ================================================================== */
/* (value, time, diff) buffer with consolidation.                     */
/* ================================================================== */

/*
 * A buffer of ((value_index, time), diff) updates, as ValueHistory::buffer
 * and the input/output buffers.  Values index a valtab; times are tw words;
 * diffs are dw bytes.  consolidate sorts by (value under cmp, time) and
 * folds equal runs, dropping zeros (consolidation.rs).
 */
typedef struct vtd_buf {
	__dtfl_vec vals;	/* size_t value index */
	timevec times;		/* tw words per entry */
	__dtfl_vec diffs;	/* dw bytes per entry */
	const __dtfl_diff_class *dc;
	const valtab *tab;	/* for value order; not owned */
	unsigned dw;
} vtd_buf;

static void
vtd_init(vtd_buf *b, const dtfl_time_class *tc, unsigned tw,
    const __dtfl_diff_class *dc, const valtab *tab)
{
	__dtfl_vec_init(&b->vals, sizeof(size_t));
	tv_init(&b->times, tc, tw);
	__dtfl_vec_init(&b->diffs, DTFL_MAX(dc->width, 1u));
	b->dc = dc;
	b->tab = tab;
	b->dw = (unsigned)dc->width;
}

static void
vtd_free(vtd_buf *b)
{
	__dtfl_vec_free(&b->vals);
	tv_free(&b->times);
	__dtfl_vec_free(&b->diffs);
}

static void
vtd_clear(vtd_buf *b)
{
	__dtfl_vec_clear(&b->vals);
	tv_clear(&b->times);
	__dtfl_vec_clear(&b->diffs);
}

static size_t
vtd_len(const vtd_buf *b)
{
	return b->vals.len;
}

static size_t
vtd_val(const vtd_buf *b, size_t i)
{
	return *DTFL_VEC_AT(&b->vals, size_t, i);
}

static const uint64_t *
vtd_time(const vtd_buf *b, size_t i)
{
	return tv_at(&b->times, i);
}

static void *
vtd_diff(const vtd_buf *b, size_t i)
{
	return __dtfl_vec_at(&b->diffs, i);
}

static void
vtd_push(vtd_buf *b, size_t val, const uint64_t *time, const void *diff)
{
	char zero[1] = { 0 };

	__dtfl_vec_push(&b->vals, &val);
	tv_push(&b->times, time);
	__dtfl_vec_push(&b->diffs, b->dw != 0 ? diff : zero);
}

/* Compare entries i and j by (value, time). */
static int
vtd_cmp(const vtd_buf *b, size_t i, size_t j)
{
	int r = valtab_cmp(b->tab, vtd_val(b, i), vtd_val(b, j));

	if (r != 0)
		return r;
	return __dtfl_words_cmp(vtd_time(b, i), vtd_time(b, j), b->times.tw);
}

/* consolidate: sort by (value, time), fold equal runs, drop zeros. */
static void
vtd_consolidate(vtd_buf *b)
{
	size_t n = vtd_len(b), i, k, w;
	size_t *idx;
	vtd_buf out;
	unsigned char *accum;

	if (n < 2 && n != 0) {
		if (b->dc->is_zero(b->dc, b->dw != 0 ? vtd_diff(b, 0) : NULL))
			vtd_clear(b);
		return;
	}
	if (n == 0)
		return;
	idx = __dtfl_alloc(n * sizeof *idx);
	for (i = 0; i < n; i++)
		idx[i] = i;
	/* Insertion sort by (value, time). */
	for (i = 1; i < n; i++) {
		size_t key = idx[i];
		size_t j = i;

		while (j > 0 && vtd_cmp(b, idx[j - 1], key) > 0) {
			idx[j] = idx[j - 1];
			j--;
		}
		idx[j] = key;
	}
	vtd_init(&out, b->times.tc, b->times.tw, b->dc, b->tab);
	accum = __dtfl_alloc(DTFL_MAX(b->dw, 1u));
	w = 0;
	(void)w;
	for (i = 0; i < n; i = k) {
		if (b->dw != 0)
			memcpy(accum, vtd_diff(b, idx[i]), b->dw);
		for (k = i + 1; k < n && vtd_cmp(b, idx[i], idx[k]) == 0; k++) {
			if (b->dw != 0)
				b->dc->plus_equals(b->dc, accum,
				    vtd_diff(b, idx[k]));
		}
		if (b->dc->is_zero(b->dc, b->dw != 0 ? accum : NULL))
			continue;
		vtd_push(&out, vtd_val(b, idx[i]), vtd_time(b, idx[i]), accum);
	}
	/* Move out into b. */
	__dtfl_vec_swap(&b->vals, &out.vals);
	__dtfl_vec_swap(&b->times.words, &out.times.words);
	__dtfl_vec_swap(&b->diffs, &out.diffs);
	vtd_free(&out);
	__dtfl_free(accum);
	__dtfl_free(idx);
}

/* advance_buffer_by(meet): join each time with meet, then consolidate. */
static void
vtd_advance_by(vtd_buf *b, const uint64_t *meet)
{
	size_t i, n = vtd_len(b);

	if (b->times.tw == 0)
		return;
	for (i = 0; i < n; i++) {
		uint64_t *t = tv_at(&b->times, i);

		dtfl_time_join(b->times.tc, t, meet, t);
	}
	vtd_consolidate(b);
}

/* ================================================================== */
/* ValueHistory (operators/mod.rs).                                    */
/* ================================================================== */

/*
 * A loaded, time-ordered replay of one key's (value, time, diff) edits.
 * edits are value-grouped (EditList); history is sorted descending by Ord so
 * last() is the least un-replayed time, with precomputed suffix meets; buffer
 * accumulates stepped-in edits, collapsible by advance_buffer_by.  Values
 * reference the shared per-compute valtab (so Copy + Ord by index).
 */
typedef struct hist_entry {
	size_t edit;		/* index into edit_times / edit_diffs */
	size_t value;		/* value index (valtab) */
} hist_entry;

typedef struct value_history {
	/* EditList: grouped edits. */
	timevec edit_times;	/* tw words per edit */
	__dtfl_vec edit_diffs;	/* dw bytes per edit */
	__dtfl_vec edit_vals;	/* size_t value index per edit */
	/* history: sorted descending by Ord; parallel meets. */
	__dtfl_vec hist;	/* hist_entry */
	timevec hist_time;	/* the edit time per history entry */
	timevec hist_meet;	/* suffix meet per history entry */
	size_t pos;		/* history cursor from the end (last = least) */
	vtd_buf buffer;		/* stepped-in edits */
	const dtfl_time_class *tc;
	unsigned tw;
	unsigned dw;
	const __dtfl_diff_class *dc;
	valtab *tab;		/* shared, not owned */
} value_history;

static void
vh_init(value_history *h, const dtfl_time_class *tc, unsigned tw,
    const __dtfl_diff_class *dc, valtab *tab)
{
	tv_init(&h->edit_times, tc, tw);
	__dtfl_vec_init(&h->edit_diffs, DTFL_MAX(dc->width, 1u));
	__dtfl_vec_init(&h->edit_vals, sizeof(size_t));
	__dtfl_vec_init(&h->hist, sizeof(hist_entry));
	tv_init(&h->hist_time, tc, tw);
	tv_init(&h->hist_meet, tc, tw);
	h->pos = 0;
	vtd_init(&h->buffer, tc, tw, dc, tab);
	h->tc = tc;
	h->tw = tw;
	h->dw = (unsigned)dc->width;
	h->dc = dc;
	h->tab = tab;
}

static void
vh_free(value_history *h)
{
	tv_free(&h->edit_times);
	__dtfl_vec_free(&h->edit_diffs);
	__dtfl_vec_free(&h->edit_vals);
	__dtfl_vec_free(&h->hist);
	tv_free(&h->hist_time);
	tv_free(&h->hist_meet);
	vtd_free(&h->buffer);
}

static void
vh_clear(value_history *h)
{
	tv_clear(&h->edit_times);
	__dtfl_vec_clear(&h->edit_diffs);
	__dtfl_vec_clear(&h->edit_vals);
	__dtfl_vec_clear(&h->hist);
	tv_clear(&h->hist_time);
	tv_clear(&h->hist_meet);
	h->pos = 0;
	vtd_clear(&h->buffer);
}

/* Push one edit (time, diff) for the value appended last by vh_seal below. */
static void
vh_edit_push(value_history *h, const uint64_t *time, const void *diff,
    size_t value)
{
	char zero[1] = { 0 };

	tv_push(&h->edit_times, time);
	__dtfl_vec_push(&h->edit_diffs, h->dw != 0 ? diff : zero);
	__dtfl_vec_push(&h->edit_vals, &value);
}

/*
 * build(): sort the edits into history descending by Ord, then compute suffix
 * meets.  history.sort_by(|x,y| y.cmp(x)) is descending Ord, so last() is the
 * least.  The meet reduction runs front to back joining each with the next's
 * meet; here with the descending order, hist[i].meet = meet of hist[i..].
 */
static void
vh_build(value_history *h)
{
	size_t n = tv_len(&h->edit_times), i;
	size_t *idx;

	vtd_clear(&h->buffer);
	__dtfl_vec_clear(&h->hist);
	tv_clear(&h->hist_time);
	tv_clear(&h->hist_meet);
	h->pos = 0;
	if (n == 0)
		return;
	idx = __dtfl_alloc(n * sizeof *idx);
	for (i = 0; i < n; i++)
		idx[i] = i;
	/* Descending Ord insertion sort (stable enough; ties keep order). */
	for (i = 1; i < n; i++) {
		size_t key = idx[i];
		size_t j = i;

		while (j > 0 && __dtfl_words_cmp(tv_at(&h->edit_times,
		    idx[j - 1]), tv_at(&h->edit_times, key), h->tw) < 0) {
			idx[j] = idx[j - 1];
			j--;
		}
		idx[j] = key;
	}
	for (i = 0; i < n; i++) {
		hist_entry e;

		e.edit = idx[i];
		e.value = *DTFL_VEC_AT(&h->edit_vals, size_t, idx[i]);
		__dtfl_vec_push(&h->hist, &e);
		tv_push(&h->hist_time, tv_at(&h->edit_times, idx[i]));
		tv_push(&h->hist_meet, tv_at(&h->edit_times, idx[i]));
	}
	/*
	 * Rust: history.iter_mut().reduce(|prev, cur| meet cur with prev).
	 * The vector is ascending-sorted-descending (index 0 is greatest);
	 * iter order is 0..n, so meets propagate from the greatest down.
	 * meet[i] = meet(time[0..=i]).  last() (least time) carries the full
	 * meet, which is what meet() must return.
	 */
	if (h->tw != 0) {
		for (i = 1; i < n; i++) {
			uint64_t *cur = tv_at(&h->hist_meet, i);
			const uint64_t *prev = tv_at(&h->hist_meet, i - 1);

			dtfl_time_meet(h->tc, cur, prev, cur);
		}
	}
	__dtfl_free(idx);
}

static size_t
vh_hist_len(const value_history *h)
{
	return h->hist.len;
}

/* The least un-replayed time, or NULL. */
static const uint64_t *
vh_time(const value_history *h)
{
	size_t n = vh_hist_len(h);

	if (h->pos >= n)
		return NULL;
	return tv_at(&h->hist_time, n - 1 - h->pos);
}

/* The meet of all un-replayed times, or NULL. */
static const uint64_t *
vh_meet(const value_history *h)
{
	size_t n = vh_hist_len(h);

	if (h->pos >= n)
		return NULL;
	return tv_at(&h->hist_meet, n - 1 - h->pos);
}

/* Move the next (least) edit into the buffer. */
static void
vh_step(value_history *h)
{
	size_t n = vh_hist_len(h);
	size_t i = n - 1 - h->pos;
	const hist_entry *e = DTFL_VEC_AT(&h->hist, hist_entry, i);

	vtd_push(&h->buffer, e->value, tv_at(&h->hist_time, i),
	    h->dw != 0 ? __dtfl_vec_at(&h->edit_diffs, e->edit) : NULL);
	h->pos++;
}

/* Step edits while the next time equals `time`; true iff any did. */
static bool
vh_step_while_time_is(value_history *h, const uint64_t *time)
{
	bool found = false;
	const uint64_t *t;

	while ((t = vh_time(h)) != NULL &&
	    __dtfl_words_cmp(t, time, h->tw) == 0) {
		found = true;
		vh_step(h);
	}
	return found;
}

/* The buffered (stepped-in, advanced, consolidated) edits. */
static vtd_buf *
vh_buffer(value_history *h)
{
	return &h->buffer;
}

/* advance_buffer_by. */
static void
vh_advance_buffer_by(value_history *h, const uint64_t *meet)
{
	vtd_advance_by(&h->buffer, meet);
}

/*
 * replay_key: seek the cursor list to `key` and load its (val, time, diff)
 * edits into the history, advancing each time by `meet` if given, then build.
 * The value bytes are interned into the shared valtab.  If the key is absent
 * the history is empty.  map_times is driven per value through a trampoline.
 */
typedef struct load_ctx {
	value_history *h;
	size_t value;		/* current interned value index */
	const uint64_t *meet;	/* NULL or a time to join each edit time with */
} load_ctx;

static void
load_on_time(const uint64_t *time, const void *diff, void *vctx)
{
	load_ctx *lc = vctx;
	value_history *h = lc->h;

	if (lc->meet != NULL && h->tw != 0) {
		uint64_t joined[DTFL_TIME_MAX];

		dtfl_time_join(h->tc, time, lc->meet, joined);
		vh_edit_push(h, joined, diff, lc->value);
	} else {
		vh_edit_push(h, time, diff, lc->value);
	}
}

static void
vh_replay_key(value_history *h, __dtfl_cursor_list *cl, const void *key,
    size_t klen, const uint64_t *meet)
{
	load_ctx lc;

	vh_clear(h);
	lc.h = h;
	lc.meet = meet;
	__dtfl_cursor_list_rewind_keys(cl);
	__dtfl_cursor_list_seek_key(cl, key, klen);
	if (__dtfl_cursor_list_key_valid(cl)) {
		size_t curklen;
		const void *curkey = __dtfl_cursor_list_key(cl, &curklen);

		/* Load only if the cursor landed exactly on `key` (klen may be 0). */
		if (curklen == klen && (klen == 0 ||
		    memcmp(curkey, key, klen) == 0)) {
			while (__dtfl_cursor_list_val_valid(cl)) {
				size_t vlen;
				const void *val = __dtfl_cursor_list_val(cl,
				    &vlen);

				lc.value = valtab_intern(h->tab, val, vlen);
				__dtfl_cursor_list_map_times(cl, load_on_time,
				    &lc);
				__dtfl_cursor_list_step_val(cl);
			}
		}
	}
	vh_build(h);
}

/* ================================================================== */
/* The per-key engine (HistoryReplayer::compute).                     */
/* ================================================================== */

/*
 * One output buffer per held capability time: the (record, time, diff)
 * updates the engine produced at times in this capability's interval.  The
 * record is key || out_val (the push closure's ((key, v), t, r)); recs is a
 * slices arena of those record bytes, times the tw words, diffs the output
 * class.  tag is the held time this buffer ships at.
 */
typedef struct out_buffer {
	uint64_t tag[DTFL_TIME_MAX];	/* the held time */
	__dtfl_slices recs;		/* key || out_val bytes */
	timevec times;			/* output tw words */
	__dtfl_vec diffs;		/* output dw bytes */
} out_buffer;

/* The user logic as the engine presents it, plus its emit buffer. */
struct __dtfl_reduce_change {
	__dtfl_vec vals;	/* size_t output value index */
	__dtfl_vec diffs;	/* out_dw bytes */
	valtab *out_tab;	/* interns pushed output values */
	unsigned out_dw;
};

void
__dtfl_reduce_change_push(__dtfl_reduce_change *ch, const void *val,
    size_t vlen, const void *diff)
{
	char zero[1] = { 0 };
	size_t vi = valtab_intern(ch->out_tab, val, vlen);

	__dtfl_vec_push(&ch->vals, &vi);
	__dtfl_vec_push(&ch->diffs, ch->out_dw != 0 ? diff : zero);
}

/*
 * The engine's reusable state (HistoryReplayer fields).  input/output/batch
 * histories, the input/output accumulation buffers, the update buffer, the
 * output_produced running set, and the time scratch vectors.  Output values
 * live in a separate valtab from input values (different type).
 */
typedef struct replayer {
	value_history input_history;
	value_history output_history;
	value_history batch_history;
	/* input_buffer: (input value, input diff) at next_time (consolidated) */
	vtd_buf input_buffer;	/* input valtab, input dc */
	vtd_buf output_buffer;	/* output valtab, output dc */
	vtd_buf update_buffer;	/* output valtab, output dc */
	vtd_buf output_produced; /* output valtab, output dc, carries time */
	timevec synth_times;
	timevec meets;
	timevec times_current;
	timevec temporary;
	/* classes / types */
	const dtfl_time_class *tc;
	unsigned tw;
	const dtfl_type *ivt;	/* input value type */
	const __dtfl_diff_class *idc;
	const dtfl_type *ovt;	/* output value type */
	const __dtfl_diff_class *odc;
	valtab *in_tab;
	valtab *out_tab;
} replayer;

static void
replayer_init(replayer *r, const dtfl_time_class *tc, unsigned tw,
    const dtfl_type *ivt, const __dtfl_diff_class *idc, const dtfl_type *ovt,
    const __dtfl_diff_class *odc, valtab *in_tab, valtab *out_tab)
{
	vh_init(&r->input_history, tc, tw, idc, in_tab);
	vh_init(&r->output_history, tc, tw, odc, out_tab);
	vh_init(&r->batch_history, tc, tw, idc, in_tab);
	vtd_init(&r->input_buffer, tc, tw, idc, in_tab);
	vtd_init(&r->output_buffer, tc, tw, odc, out_tab);
	vtd_init(&r->update_buffer, tc, tw, odc, out_tab);
	vtd_init(&r->output_produced, tc, tw, odc, out_tab);
	tv_init(&r->synth_times, tc, tw);
	tv_init(&r->meets, tc, tw);
	tv_init(&r->times_current, tc, tw);
	tv_init(&r->temporary, tc, tw);
	r->tc = tc;
	r->tw = tw;
	r->ivt = ivt;
	r->idc = idc;
	r->ovt = ovt;
	r->odc = odc;
	r->in_tab = in_tab;
	r->out_tab = out_tab;
}

static void
replayer_free(replayer *r)
{
	vh_free(&r->input_history);
	vh_free(&r->output_history);
	vh_free(&r->batch_history);
	vtd_free(&r->input_buffer);
	vtd_free(&r->output_buffer);
	vtd_free(&r->update_buffer);
	vtd_free(&r->output_produced);
	tv_free(&r->synth_times);
	tv_free(&r->meets);
	tv_free(&r->times_current);
	tv_free(&r->temporary);
}

/* update_meet(meet, has_meet, other): meet with other if present. */
static void
update_meet(replayer *r, uint64_t *meet, bool *has, const uint64_t *other)
{
	if (other == NULL)
		return;
	if (*has) {
		if (r->tw != 0)
			dtfl_time_meet(r->tc, meet, other, meet);
	} else {
		if (r->tw != 0)
			memcpy(meet, other, (size_t)r->tw * sizeof(uint64_t));
		*has = true;
	}
}

/* The logic callback and its context, threaded to compute. */
typedef struct engine_logic {
	__dtfl_reduce_logic_fn fn;
	void *ctx;
} engine_logic;

/*
 * Build the arrays the user logic expects from a consolidated vtd_buf and its
 * valtab, calling fn, draining the change into update_buffer at next_time.
 * input (ivt/idc) and output (ovt/odc) buffers are passed; change collects
 * (output value, output diff) pairs.
 */
static void
run_logic(replayer *r, engine_logic *lg, const void *key, size_t klen,
    const uint64_t *next_time)
{
	size_t ni = vtd_len(&r->input_buffer);
	size_t no = vtd_len(&r->output_buffer);
	const void **ivals = NULL, **ovals = NULL;
	size_t *ivlens = NULL, *ovlens = NULL;
	unsigned char *idiffs = NULL, *odiffs = NULL;
	unsigned char *ovals_arena = NULL;
	size_t i;
	__dtfl_reduce_change ch;

	if (ni != 0) {
		ivals = __dtfl_alloc(ni * sizeof *ivals);
		ivlens = __dtfl_alloc(ni * sizeof *ivlens);
		if (r->idc->width != 0)
			idiffs = __dtfl_alloc(ni * r->idc->width);
		for (i = 0; i < ni; i++) {
			ivals[i] = valtab_get(r->in_tab, vtd_val(&r->input_buffer,
			    i), &ivlens[i]);
			if (r->idc->width != 0)
				memcpy(idiffs + i * r->idc->width,
				    vtd_diff(&r->input_buffer, i),
				    r->idc->width);
		}
	}
	if (no != 0) {
		size_t total = 0, off = 0;
		unsigned char *arena;

		ovals = __dtfl_alloc(no * sizeof *ovals);
		ovlens = __dtfl_alloc(no * sizeof *ovlens);
		if (r->odc->width != 0)
			odiffs = __dtfl_alloc(no * r->odc->width);
		/*
		 * Copy the output values into a stable arena: the logic may push
		 * new output values into r->out_tab (reduce_abelian re-interns the
		 * negated prior output), which can realloc out_tab's arena and
		 * dangle a direct valtab_get pointer.  Copying first keeps ovals
		 * valid across the logic call.
		 */
		for (i = 0; i < no; i++) {
			size_t l;

			(void)valtab_get(r->out_tab,
			    vtd_val(&r->output_buffer, i), &l);
			ovlens[i] = l;
			total += l;
		}
		arena = __dtfl_alloc(total != 0 ? total : 1);
		ovals_arena = arena;
		for (i = 0; i < no; i++) {
			size_t l;
			const void *v = valtab_get(r->out_tab,
			    vtd_val(&r->output_buffer, i), &l);

			if (l != 0)
				memcpy(arena + off, v, l);
			ovals[i] = arena + off;
			off += l;
			if (r->odc->width != 0)
				memcpy(odiffs + i * r->odc->width,
				    vtd_diff(&r->output_buffer, i),
				    r->odc->width);
		}
	}
	__dtfl_vec_init(&ch.vals, sizeof(size_t));
	__dtfl_vec_init(&ch.diffs, DTFL_MAX(r->odc->width, 1u));
	ch.out_tab = r->out_tab;
	ch.out_dw = (unsigned)r->odc->width;

	lg->fn(lg->ctx, key, klen, ni, ivals, ivlens, idiffs, no, ovals,
	    ovlens, odiffs, &ch);

	/* Move the change into update_buffer at next_time. */
	for (i = 0; i < ch.vals.len; i++) {
		size_t vi = *DTFL_VEC_AT(&ch.vals, size_t, i);
		const void *d = r->odc->width != 0 ?
		    __dtfl_vec_at(&ch.diffs, i) : NULL;

		vtd_push(&r->update_buffer, vi, next_time, d);
	}
	__dtfl_vec_free(&ch.vals);
	__dtfl_vec_free(&ch.diffs);
	if (ivals != NULL) {
		__dtfl_free(ivals);
		__dtfl_free(ivlens);
		if (idiffs != NULL)
			__dtfl_free(idiffs);
	}
	if (ovals != NULL) {
		__dtfl_free(ovals);
		__dtfl_free(ovlens);
		if (odiffs != NULL)
			__dtfl_free(odiffs);
		if (ovals_arena != NULL)
			__dtfl_free(ovals_arena);
	}
}

/*
 * compute: the per-key history replay (HistoryReplayer::compute).
 *   source/output/batch: the three loaded histories (replay_key already done)
 *   times: the interesting times (sorted, in [lower, upper))
 *   lg: the user logic
 *   upper: upper_limit frontier words (nupper elements, tw each)
 *   outputs: one buffer per held capability time
 *   new_interesting: times to carry to the next activation (timevec)
 * The engine walks input/output/batch/seed/synth times in Ord order,
 * discovers interesting times, and evaluates the logic at each in-band one.
 */
static void
reduce_compute(replayer *r, const void *key, size_t klen, const timevec *times,
    engine_logic *lg, const uint64_t *upper, size_t nupper,
    out_buffer *outputs, size_t noutputs, timevec *new_interesting)
{
	uint64_t meet[DTFL_TIME_MAX];
	bool has_meet = false;
	size_t times_pos = 0;
	timevec meets;		/* suffix meets of `times` */
	value_history *ih = &r->input_history;
	value_history *oh = &r->output_history;
	value_history *bh = &r->batch_history;

	/* Suffix meets of `times`: meets[i] = meet(times[i..]). */
	tv_init(&meets, r->tc, r->tw);
	{
		size_t n = tv_len(times), i;

		for (i = 0; i < n; i++)
			tv_push(&meets, tv_at((timevec *)times, i));
		if (r->tw != 0)
			for (i = n; i-- > 1;) {
				uint64_t *a = tv_at(&meets, i - 1);
				const uint64_t *b = tv_at(&meets, i);

				dtfl_time_meet(r->tc, a, b, a);
			}
	}

	/* meet of batch and times (seed meet). */
	update_meet(r, meet, &has_meet, tv_len(&meets) > 0 ?
	    tv_at(&meets, 0) : NULL);
	update_meet(r, meet, &has_meet, vh_meet(bh));

	/*
	 * Having the meet, (re)load input and output histories advanced by it.
	 * replay_key was already called with meet==NULL by the driver; here we
	 * re-advance the loaded buffers lazily (upstream reloads with meet).
	 * We instead advance buffers as we go (advance_buffer_by below), which
	 * is equivalent for accumulation because join preservation holds.
	 */
	tv_clear(&r->synth_times);
	tv_clear(&r->times_current);
	vtd_clear(&r->output_produced);

	for (;;) {
		uint64_t next_time[DTFL_TIME_MAX];
		bool have_next = false;
		const uint64_t *cand;
		bool interesting;

		/* next_time = min over batch, times, input, output, synth. */
		cand = vh_time(bh);
		if (cand != NULL) { memcpy(next_time, cand, (size_t)r->tw * 8); have_next = true; }
		if (times_pos < tv_len(times)) {
			const uint64_t *t = tv_at((timevec *)times, times_pos);
			if (!have_next || __dtfl_words_cmp(t, next_time, r->tw) < 0) {
				memcpy(next_time, t, (size_t)r->tw * 8); have_next = true;
			}
		}
		cand = vh_time(ih);
		if (cand != NULL && (!have_next ||
		    __dtfl_words_cmp(cand, next_time, r->tw) < 0)) {
			memcpy(next_time, cand, (size_t)r->tw * 8); have_next = true;
		}
		cand = vh_time(oh);
		if (cand != NULL && (!have_next ||
		    __dtfl_words_cmp(cand, next_time, r->tw) < 0)) {
			memcpy(next_time, cand, (size_t)r->tw * 8); have_next = true;
		}
		if (tv_len(&r->synth_times) > 0) {
			/* synth_times is sorted descending, last() is least. */
			const uint64_t *t = tv_at(&r->synth_times,
			    tv_len(&r->synth_times) - 1);
			if (!have_next || __dtfl_words_cmp(t, next_time, r->tw) < 0) {
				memcpy(next_time, t, (size_t)r->tw * 8); have_next = true;
			}
		}
		if (!have_next)
			break;

		vh_step_while_time_is(ih, next_time);
		vh_step_while_time_is(oh, next_time);
		interesting = vh_step_while_time_is(bh, next_time);
		if (interesting && has_meet)
			vh_advance_buffer_by(bh, meet);

		/* synth_times at next_time -> times_current, interesting. */
		while (tv_len(&r->synth_times) > 0) {
			const uint64_t *t = tv_at(&r->synth_times,
			    tv_len(&r->synth_times) - 1);
			if (__dtfl_words_cmp(t, next_time, r->tw) != 0)
				break;
			tv_push(&r->times_current, t);
			tv_truncate(&r->synth_times, tv_len(&r->synth_times) - 1);
			interesting = true;
		}
		/* times at next_time -> times_current, interesting. */
		while (times_pos < tv_len(times) &&
		    __dtfl_words_cmp(tv_at((timevec *)times, times_pos),
		    next_time, r->tw) == 0) {
			tv_push(&r->times_current,
			    tv_at((timevec *)times, times_pos));
			times_pos++;
			interesting = true;
		}

		/* interesting if a batch-buffer or times_current time <= next. */
		if (!interesting) {
			size_t i, n = vtd_len(vh_buffer(bh));

			for (i = 0; i < n; i++)
				if (dtfl_time_less_equal(r->tc,
				    vtd_time(vh_buffer(bh), i), next_time)) {
					interesting = true;
					break;
				}
		}
		if (!interesting) {
			size_t i, n = tv_len(&r->times_current);

			for (i = 0; i < n; i++)
				if (dtfl_time_less_equal(r->tc,
				    tv_at(&r->times_current, i), next_time)) {
					interesting = true;
					break;
				}
		}

		if (!__dtfl_frontier_less_equal(r->tc, upper, nupper, next_time)) {
			/* DETERMINATION: synthetic times. */
			size_t i, n;

			tv_clear(&r->temporary);
			/* join batch-buffer and times_current (always). */
			n = vtd_len(vh_buffer(bh));
			for (i = 0; i < n; i++) {
				const uint64_t *t = vtd_time(vh_buffer(bh), i);
				if (!dtfl_time_less_equal(r->tc, t, next_time)) {
					uint64_t j[DTFL_TIME_MAX];
					dtfl_time_join(r->tc, t, next_time, j);
					tv_push(&r->temporary, j);
				}
			}
			n = tv_len(&r->times_current);
			for (i = 0; i < n; i++) {
				const uint64_t *t = tv_at(&r->times_current, i);
				if (!dtfl_time_less_equal(r->tc, t, next_time)) {
					uint64_t j[DTFL_TIME_MAX];
					dtfl_time_join(r->tc, t, next_time, j);
					tv_push(&r->temporary, j);
				}
			}
			if (interesting) {
				if (has_meet) {
					vh_advance_buffer_by(ih, meet);
					vh_advance_buffer_by(oh, meet);
				}
				n = vtd_len(vh_buffer(ih));
				for (i = 0; i < n; i++) {
					const uint64_t *t = vtd_time(vh_buffer(ih), i);
					if (!dtfl_time_less_equal(r->tc, t, next_time)) {
						uint64_t j[DTFL_TIME_MAX];
						dtfl_time_join(r->tc, t, next_time, j);
						tv_push(&r->temporary, j);
					}
				}
				n = vtd_len(vh_buffer(oh));
				for (i = 0; i < n; i++) {
					const uint64_t *t = vtd_time(vh_buffer(oh), i);
					if (!dtfl_time_less_equal(r->tc, t, next_time)) {
						uint64_t j[DTFL_TIME_MAX];
						dtfl_time_join(r->tc, t, next_time, j);
						tv_push(&r->temporary, j);
					}
				}
				n = vtd_len(&r->output_produced);
				for (i = 0; i < n; i++) {
					const uint64_t *t = vtd_time(&r->output_produced, i);
					if (!dtfl_time_less_equal(r->tc, t, next_time)) {
						uint64_t j[DTFL_TIME_MAX];
						dtfl_time_join(r->tc, t, next_time, j);
						tv_push(&r->temporary, j);
					}
				}
			}
			tv_sort_dedup(&r->temporary);

			/* introduce synthetic times. */
			{
				size_t before = tv_len(&r->synth_times);
				size_t m = tv_len(&r->temporary);

				for (i = 0; i < m; i++) {
					const uint64_t *t = tv_at(&r->temporary, i);
					if (__dtfl_frontier_less_equal(r->tc,
					    upper, nupper, t))
						tv_push(new_interesting, t);
					else
						tv_push(&r->synth_times, t);
				}
				if (tv_len(&r->synth_times) > before) {
					/* sort descending, dedup. */
					tv_sort_dedup(&r->synth_times);
					/* reverse to descending */
					{
						size_t lo = 0, hi = tv_len(&r->synth_times);
						size_t half = hi / 2, s;
						uint64_t tmp[DTFL_TIME_MAX];
						for (s = 0; s < half; s++) {
							uint64_t *a = tv_at(&r->synth_times, lo + s);
							uint64_t *b = tv_at(&r->synth_times, hi - 1 - s);
							if (r->tw != 0) {
								memcpy(tmp, a, (size_t)r->tw * 8);
								memcpy(a, b, (size_t)r->tw * 8);
								memcpy(b, tmp, (size_t)r->tw * 8);
							}
						}
					}
				}
			}

			/* EVALUATION. */
			if (interesting) {
				vtd_clear(&r->input_buffer);
				vtd_clear(&r->output_buffer);
				/* input at next_time from input and batch buffers. */
				n = vtd_len(vh_buffer(ih));
				for (i = 0; i < n; i++)
					if (dtfl_time_less_equal(r->tc,
					    vtd_time(vh_buffer(ih), i), next_time))
						vtd_push(&r->input_buffer,
						    vtd_val(vh_buffer(ih), i),
						    next_time,
						    r->idc->width ? vtd_diff(vh_buffer(ih), i) : NULL);
				n = vtd_len(vh_buffer(bh));
				for (i = 0; i < n; i++)
					if (dtfl_time_less_equal(r->tc,
					    vtd_time(vh_buffer(bh), i), next_time))
						vtd_push(&r->input_buffer,
						    vtd_val(vh_buffer(bh), i),
						    next_time,
						    r->idc->width ? vtd_diff(vh_buffer(bh), i) : NULL);
				vtd_consolidate(&r->input_buffer);
				/* output at next_time from output buffer and output_produced. */
				n = vtd_len(vh_buffer(oh));
				for (i = 0; i < n; i++)
					if (dtfl_time_less_equal(r->tc,
					    vtd_time(vh_buffer(oh), i), next_time))
						vtd_push(&r->output_buffer,
						    vtd_val(vh_buffer(oh), i),
						    next_time,
						    r->odc->width ? vtd_diff(vh_buffer(oh), i) : NULL);
				n = vtd_len(&r->output_produced);
				for (i = 0; i < n; i++)
					if (dtfl_time_less_equal(r->tc,
					    vtd_time(&r->output_produced, i), next_time))
						vtd_push(&r->output_buffer,
						    vtd_val(&r->output_produced, i),
						    next_time,
						    r->odc->width ? vtd_diff(&r->output_produced, i) : NULL);
				vtd_consolidate(&r->output_buffer);

				if (vtd_len(&r->input_buffer) != 0 ||
				    vtd_len(&r->output_buffer) != 0) {
					vtd_clear(&r->update_buffer);
					run_logic(r, lg, key, klen, next_time);
					vtd_consolidate(&r->update_buffer);
					if (vtd_len(&r->update_buffer) != 0) {
						/* find output buffer with tag <= next_time. */
						size_t idx = SIZE_MAX;
						size_t u;
						for (u = noutputs; u-- > 0;)
							if (dtfl_time_less_equal(r->tc,
							    outputs[u].tag, next_time)) {
								idx = u;
								break;
							}
						DTFL_ASSERT(idx != SIZE_MAX);
						n = vtd_len(&r->update_buffer);
						for (i = 0; i < n; i++) {
							size_t vi = vtd_val(&r->update_buffer, i);
							const void *d = r->odc->width ?
							    vtd_diff(&r->update_buffer, i) : NULL;
							size_t ovl;
							const void *ov = valtab_get(r->out_tab, vi, &ovl);
							char z[1] = { 0 };
							size_t rl = klen + ovl;
							unsigned char *rec = __dtfl_alloc(rl != 0 ? rl : 1);

							/* output_produced carries next_time. */
							vtd_push(&r->output_produced, vi, next_time, d);
							/* ship key || out_val into the capability buffer. */
							if (klen != 0)
								memcpy(rec, key, klen);
							if (ovl != 0)
								memcpy(rec + klen, ov, ovl);
							__dtfl_slices_push(&outputs[idx].recs, rec, klen + ovl);
							__dtfl_free(rec);
							tv_push(&outputs[idx].times, next_time);
							__dtfl_vec_push(&outputs[idx].diffs,
							    r->odc->width ? d : z);
						}
						if (has_meet) {
							n = vtd_len(&r->output_produced);
							for (i = 0; i < n; i++) {
								uint64_t *t = tv_at(&r->output_produced.times, i);
								if (r->tw != 0)
									dtfl_time_join(r->tc, t, meet, t);
							}
							vtd_consolidate(&r->output_produced);
						}
					}
				}
			}
		} else if (interesting) {
			tv_push(new_interesting, next_time);
		}

		/* Update meet. */
		has_meet = false;
		update_meet(r, meet, &has_meet, vh_meet(bh));
		update_meet(r, meet, &has_meet, vh_meet(ih));
		update_meet(r, meet, &has_meet, vh_meet(oh));
		{
			size_t i, n = tv_len(&r->synth_times);
			for (i = 0; i < n; i++)
				update_meet(r, meet, &has_meet,
				    tv_at(&r->synth_times, i));
		}
		if (times_pos < tv_len(&meets))
			update_meet(r, meet, &has_meet, tv_at(&meets, times_pos));

		/* times_current join meet. */
		if (has_meet && r->tw != 0) {
			size_t i, n = tv_len(&r->times_current);
			for (i = 0; i < n; i++) {
				uint64_t *t = tv_at(&r->times_current, i);
				dtfl_time_join(r->tc, t, meet, t);
			}
		}
		tv_sort_dedup(&r->times_current);
	}

	tv_sort_dedup(new_interesting);
	tv_free(&meets);
}

/* ================================================================== */
/* The reference tactic's per-key engine (reference::ReferenceThinker). */
/* ================================================================== */

/*
 * TimeReplay (operators/mod.rs): a time-only, non-destructive walk over an
 * already-built value_history.  It reads hist_time / hist_meet from the end
 * (the least time) through its own position, leaving the history and its
 * value walk position untouched, and accumulates only times into `buffer`,
 * which the caller owns.
 */
typedef struct time_replay {
	const value_history *h;	/* borrowed */
	size_t pos;		/* entries consumed from the end */
	timevec *buffer;	/* accumulated (and compacted) times */
} time_replay;

/* ValueHistory::replay_times: a fresh time-only walk, clearing `buffer`. */
static void
tr_init(time_replay *tr, const value_history *h, timevec *buffer)
{
	tr->h = h;
	tr->pos = 0;
	tr->buffer = buffer;
	tv_clear(buffer);
}

static const uint64_t *
tr_time(const time_replay *tr)
{
	size_t n = vh_hist_len(tr->h);

	if (tr->pos >= n)
		return NULL;
	return tv_at(&tr->h->hist_time, n - 1 - tr->pos);
}

static const uint64_t *
tr_meet(const time_replay *tr)
{
	size_t n = vh_hist_len(tr->h);

	if (tr->pos >= n)
		return NULL;
	return tv_at(&tr->h->hist_meet, n - 1 - tr->pos);
}

static void
tr_step(time_replay *tr)
{
	tv_push(tr->buffer, tr_time(tr));
	tr->pos++;
}

static bool
tr_step_while_time_is(time_replay *tr, const uint64_t *time)
{
	bool found = false;
	const uint64_t *t;

	while ((t = tr_time(tr)) != NULL &&
	    __dtfl_words_cmp(t, time, tr->h->tw) == 0) {
		found = true;
		tr_step(tr);
	}
	return found;
}

/* Join buffered times with `meet`, then sort and dedup (time-only consolidate). */
static void
tr_advance_buffer_by(time_replay *tr, const uint64_t *meet)
{
	size_t i, n = tv_len(tr->buffer);

	if (tr->h->tw != 0)
		for (i = 0; i < n; i++) {
			uint64_t *t = tv_at(tr->buffer, i);

			dtfl_time_join(tr->h->tc, t, meet, t);
		}
	tv_sort_dedup(tr->buffer);
}

/* ValueHistory::walk: a fresh value walk over the intact, built history. */
static void
vh_walk(value_history *h)
{
	vtd_clear(&h->buffer);
	h->pos = 0;
}

/* Push t join next for every t in `src` not <= next (the temporary extends). */
static void
ref_extend_joins(replayer *r, timevec *dst, const timevec *src,
    const uint64_t *next_time)
{
	size_t i, n = tv_len(src);

	for (i = 0; i < n; i++) {
		const uint64_t *t = tv_at(src, i);

		if (!dtfl_time_less_equal(r->tc, t, next_time)) {
			uint64_t j[DTFL_TIME_MAX];

			if (r->tw != 0)
				dtfl_time_join(r->tc, t, next_time, j);
			tv_push(dst, j);
		}
	}
}

/* Suffix meets: meets[i] = meet(src[i..]). */
static void
ref_suffix_meets(replayer *r, timevec *meets, const timevec *src)
{
	size_t n = tv_len(src), i;

	tv_clear(meets);
	for (i = 0; i < n; i++)
		tv_push(meets, tv_at(src, i));
	if (r->tw != 0)
		for (i = n; i-- > 1;) {
			uint64_t *a = tv_at(meets, i - 1);

			dtfl_time_meet(r->tc, a, tv_at(meets, i), a);
		}
}

/*
 * ReferenceThinker::compute: the two-phase per-key engine.
 *
 * Phase 1 (determination) reads the input/output/seed *times* and closes them
 * into `active` (the interesting times) and the pended set (new_interesting),
 * the model's `Reached`, directly.  Phase 2 (application) walks the same,
 * still-loaded histories for *values* and emits corrections.  The three
 * histories were loaded with replay_key(.., None) by the retire driver, as
 * upstream's compute does first thing.  Same signature as reduce_compute so
 * the shared retire body can drive either engine.
 */
static void
reference_compute(replayer *r, const void *key, size_t klen,
    const timevec *times, engine_logic *lg, const uint64_t *upper,
    size_t nupper, out_buffer *outputs, size_t noutputs,
    timevec *new_interesting)
{
	value_history *ih = &r->input_history;
	value_history *oh = &r->output_history;
	value_history *bh = &r->batch_history;
	timevec batch_times, input_times, output_times, active;
	size_t i, n;

	tv_init(&batch_times, r->tc, r->tw);
	tv_init(&input_times, r->tc, r->tw);
	tv_init(&output_times, r->tc, r->tw);
	tv_init(&active, r->tc, r->tw);

	/*
	 * PHASE 1: DETERMINATION (`Reached`, compacted).  Walk the input,
	 * output and seed times in increasing order; a time is *reached* only
	 * via a seed (a batch update, a due pending time, or a synthetic join of
	 * earlier reached times); a reached in-band time joins against the live
	 * partners to spawn more reached times, and a join beyond `upper` is
	 * pended.  The live partner sets are kept an antichain by
	 * advance_buffer_by(meet).  Time-only: the time_replay walks read the
	 * histories without stepping them, so phase 2 can walk them.
	 */
	{
		time_replay batch_replay, input_replay, output_replay;
		size_t times_pos = 0;	/* times_slice and meets_slice */
		uint64_t meet[DTFL_TIME_MAX];
		bool has_meet = false;

		/* Suffix-meets of the supplied (due pending) times. */
		ref_suffix_meets(r, &r->meets, times);

		tr_init(&batch_replay, bh, &batch_times);
		tr_init(&input_replay, ih, &input_times);
		tr_init(&output_replay, oh, &output_times);

		tv_clear(&r->synth_times);
		tv_clear(&r->times_current);
		tv_clear(&r->temporary);
		tv_clear(&active);

		for (;;) {
			uint64_t next_time[DTFL_TIME_MAX];
			const uint64_t *cand[5];
			bool have_next = false, interesting;
			size_t c;

			cand[0] = tr_time(&batch_replay);
			cand[1] = times_pos < tv_len(times) ?
			    tv_at(times, times_pos) : NULL;
			cand[2] = tr_time(&input_replay);
			cand[3] = tr_time(&output_replay);
			cand[4] = tv_len(&r->synth_times) > 0 ?
			    tv_at(&r->synth_times,
			    tv_len(&r->synth_times) - 1) : NULL;
			for (c = 0; c < 5; c++)
				if (cand[c] != NULL && (!have_next ||
				    __dtfl_words_cmp(cand[c], next_time,
				    r->tw) < 0)) {
					if (r->tw != 0)
						memcpy(next_time, cand[c],
						    (size_t)r->tw * 8);
					have_next = true;
				}
			if (!have_next)
				break;

			tr_step_while_time_is(&input_replay, next_time);
			tr_step_while_time_is(&output_replay, next_time);

			/*
			 * Reached via a seed: a batch update, a due pending
			 * time, or a synthetic join.  (Input/output times alone
			 * are not reached; they are only join partners.)
			 */
			interesting = tr_step_while_time_is(&batch_replay,
			    next_time);
			if (interesting && has_meet)
				tr_advance_buffer_by(&batch_replay, meet);
			while (tv_len(&r->synth_times) > 0 &&
			    __dtfl_words_cmp(tv_at(&r->synth_times,
			    tv_len(&r->synth_times) - 1), next_time,
			    r->tw) == 0) {
				tv_push(&r->times_current,
				    tv_at(&r->synth_times,
				    tv_len(&r->synth_times) - 1));
				tv_truncate(&r->synth_times,
				    tv_len(&r->synth_times) - 1);
				interesting = true;
			}
			while (times_pos < tv_len(times) &&
			    __dtfl_words_cmp(tv_at(times, times_pos),
			    next_time, r->tw) == 0) {
				tv_push(&r->times_current, next_time);
				times_pos++;
				interesting = true;
			}
			/* Absorb: a time at or above a reached time is reached. */
			for (i = 0; !interesting &&
			    i < tv_len(batch_replay.buffer); i++)
				if (dtfl_time_less_equal(r->tc,
				    tv_at(batch_replay.buffer, i), next_time))
					interesting = true;
			for (i = 0; !interesting &&
			    i < tv_len(&r->times_current); i++)
				if (dtfl_time_less_equal(r->tc,
				    tv_at(&r->times_current, i), next_time))
					interesting = true;

			if (!__dtfl_frontier_less_equal(r->tc, upper, nupper,
			    next_time)) {
				size_t synth_len;

				/*
				 * A reached in-band time is `active`; join it
				 * against the live partners, input/output
				 * (joinBase) and reached-so-far (joinActive),
				 * for new times.
				 */
				if (interesting) {
					tv_push(&active, next_time);
					if (has_meet) {
						tr_advance_buffer_by(
						    &input_replay, meet);
						tr_advance_buffer_by(
						    &output_replay, meet);
					}
					ref_extend_joins(r, &r->temporary,
					    input_replay.buffer, next_time);
					ref_extend_joins(r, &r->temporary,
					    output_replay.buffer, next_time);
				}
				ref_extend_joins(r, &r->temporary,
				    batch_replay.buffer, next_time);
				ref_extend_joins(r, &r->temporary,
				    &r->times_current, next_time);
				tv_sort_dedup(&r->temporary);

				synth_len = tv_len(&r->synth_times);
				n = tv_len(&r->temporary);
				for (i = 0; i < n; i++) {
					const uint64_t *t = tv_at(
					    &r->temporary, i);

					if (__dtfl_frontier_less_equal(r->tc,
					    upper, nupper, t))
						tv_push(new_interesting, t);	/* pended */
					else
						tv_push(&r->synth_times, t);	/* reached, later */
				}
				tv_clear(&r->temporary);
				if (tv_len(&r->synth_times) > synth_len) {
					/* sort descending (last() least), dedup. */
					size_t lo, hi;
					uint64_t tmp[DTFL_TIME_MAX];

					tv_sort_dedup(&r->synth_times);
					hi = tv_len(&r->synth_times);
					for (lo = 0; r->tw != 0 && lo < hi / 2;
					    lo++) {
						uint64_t *a = tv_at(
						    &r->synth_times, lo);
						uint64_t *b = tv_at(
						    &r->synth_times,
						    hi - 1 - lo);

						memcpy(tmp, a,
						    (size_t)r->tw * 8);
						memcpy(a, b,
						    (size_t)r->tw * 8);
						memcpy(b, tmp,
						    (size_t)r->tw * 8);
					}
				}
			} else if (interesting) {
				tv_push(new_interesting, next_time);
			}

			/*
			 * Running meet (a lower bound on every time still to
			 * visit); compact the reached partners times_current.
			 */
			has_meet = false;
			update_meet(r, meet, &has_meet, tr_meet(&batch_replay));
			update_meet(r, meet, &has_meet, tr_meet(&input_replay));
			update_meet(r, meet, &has_meet,
			    tr_meet(&output_replay));
			for (i = 0; i < tv_len(&r->synth_times); i++)
				update_meet(r, meet, &has_meet,
				    tv_at(&r->synth_times, i));
			update_meet(r, meet, &has_meet,
			    times_pos < tv_len(&r->meets) ?
			    tv_at(&r->meets, times_pos) : NULL);
			if (has_meet && r->tw != 0)
				for (i = 0; i < tv_len(&r->times_current);
				    i++) {
					uint64_t *t = tv_at(&r->times_current,
					    i);

					dtfl_time_join(r->tc, t, meet, t);
				}
			tv_sort_dedup(&r->times_current);
		}

		tv_sort_dedup(&active);
	}

	/*
	 * PHASE 2: APPLICATION (emit_correct).  Walk `active` in order over the
	 * SAME per-key edits (a fresh value walk, no cursor), keep the
	 * input/output accumulations tight by advancing to the meet of the times
	 * still to be produced, apply `logic`, and emit corrections.
	 */
	{
		size_t index;

		ref_suffix_meets(r, &r->meets, &active);

		vh_walk(bh);
		vh_walk(ih);
		vh_walk(oh);

		vtd_clear(&r->output_produced);

		for (index = 0; index < tv_len(&active); index++) {
			uint64_t next_time[DTFL_TIME_MAX];
			uint64_t meet[DTFL_TIME_MAX];
			const uint64_t *t;

			if (r->tw != 0) {
				memcpy(next_time, tv_at(&active, index),
				    (size_t)r->tw * 8);
				memcpy(meet, tv_at(&r->meets, index),
				    (size_t)r->tw * 8);
			}

			/*
			 * Phase 2 visits only the active times, so at each we
			 * must catch up the histories to include every edit that
			 * will contribute to the accumulation at `next_time`
			 * (edits at non-active times count too).  history is
			 * sorted by the total Ord and step pops the least, so we
			 * step the Ord-prefix `t <= next_time`, NOT the partial
			 * `t.less_equal(next_time)`: the partial order
			 * interleaves with the sort, so an Ord-earlier time
			 * incomparable to `next_time` would halt a less_equal
			 * walk early and strand later edits that *are*
			 * less_equal(next_time).  Stepping the Ord-prefix takes a
			 * superset; the less_equal filter below then selects the
			 * true `<= next_time` edits.
			 */
			while ((t = vh_time(ih)) != NULL &&
			    __dtfl_words_cmp(t, next_time, r->tw) <= 0)
				vh_step(ih);
			while ((t = vh_time(bh)) != NULL &&
			    __dtfl_words_cmp(t, next_time, r->tw) <= 0)
				vh_step(bh);
			while ((t = vh_time(oh)) != NULL &&
			    __dtfl_words_cmp(t, next_time, r->tw) <= 0)
				vh_step(oh);
			vh_advance_buffer_by(ih, meet);
			vh_advance_buffer_by(bh, meet);
			vh_advance_buffer_by(oh, meet);

			vtd_clear(&r->input_buffer);
			vtd_clear(&r->output_buffer);
			n = vtd_len(vh_buffer(ih));
			for (i = 0; i < n; i++)
				if (dtfl_time_less_equal(r->tc,
				    vtd_time(vh_buffer(ih), i), next_time))
					vtd_push(&r->input_buffer,
					    vtd_val(vh_buffer(ih), i), next_time,
					    r->idc->width ?
					    vtd_diff(vh_buffer(ih), i) : NULL);
			n = vtd_len(vh_buffer(bh));
			for (i = 0; i < n; i++)
				if (dtfl_time_less_equal(r->tc,
				    vtd_time(vh_buffer(bh), i), next_time))
					vtd_push(&r->input_buffer,
					    vtd_val(vh_buffer(bh), i), next_time,
					    r->idc->width ?
					    vtd_diff(vh_buffer(bh), i) : NULL);
			vtd_consolidate(&r->input_buffer);

			n = vtd_len(vh_buffer(oh));
			for (i = 0; i < n; i++)
				if (dtfl_time_less_equal(r->tc,
				    vtd_time(vh_buffer(oh), i), next_time))
					vtd_push(&r->output_buffer,
					    vtd_val(vh_buffer(oh), i), next_time,
					    r->odc->width ?
					    vtd_diff(vh_buffer(oh), i) : NULL);
			n = vtd_len(&r->output_produced);
			for (i = 0; i < n; i++)
				if (dtfl_time_less_equal(r->tc,
				    vtd_time(&r->output_produced, i),
				    next_time))
					vtd_push(&r->output_buffer,
					    vtd_val(&r->output_produced, i),
					    next_time, r->odc->width ?
					    vtd_diff(&r->output_produced, i) :
					    NULL);
			vtd_consolidate(&r->output_buffer);

			if (vtd_len(&r->input_buffer) == 0 &&
			    vtd_len(&r->output_buffer) == 0)
				continue;
			vtd_clear(&r->update_buffer);
			run_logic(r, lg, key, klen, next_time);
			vtd_consolidate(&r->update_buffer);
			if (vtd_len(&r->update_buffer) != 0) {
				size_t idx = SIZE_MAX, u;

				for (u = noutputs; u-- > 0;)
					if (dtfl_time_less_equal(r->tc,
					    outputs[u].tag, next_time)) {
						idx = u;
						break;
					}
				if (idx == SIZE_MAX)
					__dtfl_panic("reduce reference: "
					    "failed to find index");
				n = vtd_len(&r->update_buffer);
				for (i = 0; i < n; i++) {
					size_t vi = vtd_val(&r->update_buffer,
					    i);
					const void *d = r->odc->width ?
					    vtd_diff(&r->update_buffer, i) :
					    NULL;
					size_t ovl;
					const void *ov = valtab_get(r->out_tab,
					    vi, &ovl);
					char z[1] = { 0 };
					size_t rl = klen + ovl;
					unsigned char *rec = __dtfl_alloc(
					    rl != 0 ? rl : 1);

					vtd_push(&r->output_produced, vi,
					    next_time, d);
					if (klen != 0)
						memcpy(rec, key, klen);
					if (ovl != 0)
						memcpy(rec + klen, ov, ovl);
					__dtfl_slices_push(&outputs[idx].recs,
					    rec, rl);
					__dtfl_free(rec);
					tv_push(&outputs[idx].times,
					    next_time);
					__dtfl_vec_push(&outputs[idx].diffs,
					    r->odc->width ? d : z);
				}
				n = vtd_len(&r->output_produced);
				if (r->tw != 0)
					for (i = 0; i < n; i++) {
						uint64_t *ot = tv_at(
						    &r->output_produced.times,
						    i);

						dtfl_time_join(r->tc, ot, meet,
						    ot);
					}
				vtd_consolidate(&r->output_produced);
			}
		}
	}

	tv_free(&batch_times);
	tv_free(&input_times);
	tv_free(&output_times);
	tv_free(&active);
}

/* ================================================================== */
/* The interval driver (CursorTactic::retire) and reduce operator.    */
/* ================================================================== */

/*
 * Pending (key, time) moments carried across activations.  Stored as a
 * parallel key-slice and time-vector, sorted by (key, time), the lower
 * envelope of interesting times the operator still holds (the pending_keys /
 * pending_time of CursorTactic).
 */
typedef struct pending {
	__dtfl_slices keys;	/* key bytes per entry */
	timevec times;		/* tw words per entry */
} pending;

static void
pending_init(pending *p, const dtfl_time_class *tc, unsigned tw)
{
	__dtfl_slices_init(&p->keys);
	tv_init(&p->times, tc, tw);
}

static void
pending_free(pending *p)
{
	__dtfl_slices_free(&p->keys);
	tv_free(&p->times);
}

static void
pending_clear(pending *p)
{
	__dtfl_slices_clear(&p->keys);
	tv_clear(&p->times);
}

static size_t
pending_len(const pending *p)
{
	return __dtfl_slices_len(&p->keys);
}

static void
pending_push(pending *p, const void *key, size_t klen, const uint64_t *t)
{
	__dtfl_slices_push(&p->keys, key, klen);
	tv_push(&p->times, t);
}

/*
 * The reduce operator's state.  source: the shared input trace agent (read
 * for pre-existing input batches).  out_reader/out_writer: the output trace
 * agent and writer.  pending: carried moments.  caps: held capabilities (the
 * CapabilitySet).  The input batch stream carries DTFL_BATCH_PTR records.
 */
typedef struct reduce_op_ctx {
	__dtfl_trace_agent source;	/* clone of input agent */
	__dtfl_trace_agent out_reader;	/* operator's reader (shared) */
	__dtfl_trace_writer out_writer;
	pending pend;
	__dtfl_vec caps;		/* dtfl_cap * (CapabilitySet) */
	__dtfl_antichain upper_limit;
	__dtfl_antichain lower_limit;
	engine_logic logic;
	void *logic_ctx_free_arg;
	void (*logic_ctx_free)(void *);
	const dtfl_time_class *tc;
	unsigned tw;
	unsigned depth;			/* == tw */
	const dtfl_type *ikt;		/* input key type */
	const dtfl_type *ivt;		/* input value type */
	const __dtfl_diff_class *idc;
	const dtfl_type *ovt;		/* output value type */
	const __dtfl_diff_class *odc;
	bool abelian_wrap;		/* reduce_abelian: wrap the logic */
	__dtfl_vec batch_storage;	/* __dtfl_batch_ord * (new input batches) */
	const struct rd_tactic_ops *tactic;	/* ReduceTactic vtable */
	void *tactic_state;		/* owned; freed by tactic->free */
} reduce_op_ctx;

/*
 * A produced output batch, tagged with the held time to ship it at (the
 * (time, batch) pairs a ReduceTactic returns).  The batch reference is owned
 * by this struct until the operator ships and commits it.
 */
typedef struct produced_batch {
	uint64_t tag[DTFL_TIME_MAX];
	__dtfl_batch_ord *batch;	/* owned */
} produced_batch;

/*
 * The ReduceTactic seam (reduce.rs ReduceTactic::retire): the driver
 * (reduce_logic, reduce_with_tactic) hands the tactic the interval
 * [c->lower_limit, upper) and reads back the output batches, each tagged with
 * a held time, plus the new frontier of interesting times to hold.  The
 * tactic reads the pre-existing input (c->source), output (c->out_reader)
 * and new batches (c->batch_storage) from the operator state, and `held` is
 * the capability set c->caps.  produced has room for one batch per held
 * time.  The contract is upstream's: ordered output tiling [lower, upper),
 * shipped at held times, and an empty frontier when nothing is withheld.
 */
typedef struct rd_tactic_ops {
	void (*retire)(reduce_op_ctx *c, const uint64_t *upper, size_t nupper,
	    produced_batch *produced, size_t *nproduced,
	    __dtfl_antichain *new_frontier);
	void (*free)(void *state);
} rd_tactic_ops;

/* The abelian wrapper: run inner logic (if input non-empty), then subtract
 * the tentative output (negate and append), consolidate.  The engine already
 * consolidates update_buffer, so we only append the negated output here. */
typedef struct abelian_logic {
	__dtfl_reduce_logic_fn inner;
	void *inner_ctx;
	void (*inner_ctx_free)(void *);
	const __dtfl_diff_class *odc;
} abelian_logic;

static void
abelian_logic_fn(void *vctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, __dtfl_reduce_change *ch)
{
	abelian_logic *a = vctx;
	size_t i;
	size_t dw = a->odc->width;
	unsigned char *neg = dw != 0 ? __dtfl_alloc(dw) : NULL;

	if (n != 0)
		a->inner(a->inner_ctx, key, klen, n, in_vals, in_vlens,
		    in_diffs, 0, NULL, NULL, NULL, ch);
	/* change.extend(output.map(|(x, d)| (x, -d))). */
	for (i = 0; i < m; i++) {
		const unsigned char *d = (const unsigned char *)out_diffs +
		    i * dw;

		if (dw != 0) {
			memcpy(neg, d, dw);
			a->odc->negate(a->odc, neg);
		}
		__dtfl_reduce_change_push(ch, out_vals[i], out_vlens[i],
		    dw != 0 ? neg : NULL);
	}
	if (neg != NULL)
		__dtfl_free(neg);
	/* The engine consolidates ch via update_buffer after we return. */
}

/* Find the capability for `t` among caps, or SIZE_MAX. */
static size_t
rd_find_cap(reduce_op_ctx *c, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < c->caps.len; i++)
		if (__dtfl_words_cmp(dtfl_cap_time(*DTFL_VEC_AT(&c->caps,
		    dtfl_cap *, i)), t, c->tw) == 0)
			return i;
	return SIZE_MAX;
}

/* Comparator state for sorting one output buffer by (record, time). */
typedef struct ob_sort {
	out_buffer *ob;
	reduce_op_ctx *c;
} ob_sort;

static int
ob_cmp(const ob_sort *s, size_t i, size_t j)
{
	size_t al, bl;
	const void *a = __dtfl_slices_get(&s->ob->recs, i, &al);
	const void *b = __dtfl_slices_get(&s->ob->recs, j, &bl);
	/* The record is key || out_val; compare as a tuple of key then val. */
	size_t ka = dtfl_type_value_len(s->c->ikt, a, al);
	size_t kb = dtfl_type_value_len(s->c->ikt, b, bl);
	int r = dtfl_type_cmp(s->c->ikt, a, ka, b, kb);

	if (r != 0)
		return r;
	r = dtfl_type_cmp(s->c->ovt, (const char *)a + ka, al - ka,
	    (const char *)b + kb, bl - kb);
	if (r != 0)
		return r;
	return __dtfl_words_cmp(tv_at(&s->ob->times, i),
	    tv_at(&s->ob->times, j), s->c->tw);
}

/*
 * Build output batches, one per held capability, tiling [lower, upper).
 * output batch index's upper is `upper` joined with the held times after it;
 * a batch is emitted only when its [lower, upper) is non-degenerate.  Follows
 * CursorTactic's builder loop.  Appends (held_time, batch) to *produced.
 */
static void
rd_build_batches(reduce_op_ctx *c, const __dtfl_antichain *held,
    out_buffer *outputs, size_t nheld, produced_batch *produced,
    size_t *nproduced)
{
	__dtfl_antichain out_lower, out_upper;
	size_t index, i;

	__dtfl_antichain_init(&out_lower, c->tc);
	__dtfl_antichain_init(&out_upper, c->tc);
	__dtfl_antichain_copy(&out_lower, &c->lower_limit);

	for (index = 0; index < nheld; index++) {
		__dtfl_antichain_clear(&out_upper);
		for (i = 0; i < __dtfl_antichain_len(&c->upper_limit); i++)
			__dtfl_antichain_insert(&out_upper,
			    __dtfl_antichain_at(&c->upper_limit, i));
		for (i = index + 1; i < nheld; i++)
			__dtfl_antichain_insert(&out_upper,
			    __dtfl_antichain_at(held, i));

		if (!__dtfl_antichain_eq(&out_upper, &out_lower)) {
			__dtfl_description desc;
			__dtfl_antichain lo, hi, since;
			__dtfl_builder_ord bld;
			out_buffer *ob = &outputs[index];
			size_t n = __dtfl_slices_len(&ob->recs), j;
			uint64_t minimum[DTFL_TIME_MAX] = { 0 };
			size_t *idx = NULL;
			ob_sort ss = { ob, c };

			__dtfl_antichain_init(&lo, c->tc);
			__dtfl_antichain_init(&hi, c->tc);
			__dtfl_antichain_copy(&lo, &out_lower);
			__dtfl_antichain_copy(&hi, &out_upper);
			__dtfl_antichain_init_elem(&since, c->tc, minimum);
			__dtfl_description_init(&desc, &lo, &hi, &since);

			/* Sort rows by (record, time); builder needs them in
			 * (key, val) order with each group contiguous. */
			if (n != 0) {
				idx = __dtfl_alloc(n * sizeof *idx);
				for (j = 0; j < n; j++)
					idx[j] = j;
				for (j = 1; j < n; j++) {
					size_t key = idx[j], k = j;

					while (k > 0 &&
					    ob_cmp(&ss, idx[k - 1], key) > 0) {
						idx[k] = idx[k - 1];
						k--;
					}
					idx[k] = key;
				}
			}
			__dtfl_builder_ord_init(&bld, false, c->ikt, c->ovt,
			    c->tc, c->odc, 0, 0, n);
			for (j = 0; j < n; j++) {
				size_t oi = idx[j];
				size_t rl, kl;
				const void *rec = __dtfl_slices_get(&ob->recs,
				    oi, &rl);
				const void *d = c->odc->width ?
				    __dtfl_vec_at(&ob->diffs, oi) : NULL;

				kl = dtfl_type_value_len(c->ikt, rec, rl);
				__dtfl_builder_ord_push(&bld, rec, kl,
				    (const char *)rec + kl, rl - kl,
				    tv_at(&ob->times, oi), d);
			}
			if (idx != NULL)
				__dtfl_free(idx);
			produced[*nproduced].batch =
			    __dtfl_builder_ord_done(&bld, &desc);
			if (c->tw != 0)
				memcpy(produced[*nproduced].tag,
				    __dtfl_antichain_at(held, index),
				    (size_t)c->tw * 8);
			(*nproduced)++;

			__dtfl_antichain_clear(&out_lower);
			for (i = 0; i < __dtfl_antichain_len(&out_upper); i++)
				__dtfl_antichain_insert(&out_lower,
				    __dtfl_antichain_at(&out_upper, i));
		}
	}
	__dtfl_antichain_free(&out_lower);
	__dtfl_antichain_free(&out_upper);
}

/*
 * retire: run the interval [lower, upper) through the engine.  Builds a
 * cursor list over the source (pre-existing input), output (pre-existing
 * output) and the new input batches, marches keys (merging the batch cursor
 * with pending keys), evaluates each key, and fills *produced with the output
 * batches to ship plus the new capability frontier.  Follows CursorTactic.
 */
/*
 * The per-key engine a retire drives: reduce_compute (CursorTactic's
 * HistoryReplayer) or reference_compute (ReferenceTactic's ReferenceThinker).
 */
typedef void (*rd_compute_fn)(replayer *r, const void *key, size_t klen,
    const timevec *times, engine_logic *lg, const uint64_t *upper,
    size_t nupper, out_buffer *outputs, size_t noutputs,
    timevec *new_interesting);

static void
rd_retire(reduce_op_ctx *c, const uint64_t *frontier, size_t fn,
    produced_batch *produced, size_t *nproduced,
    __dtfl_antichain *new_frontier, rd_compute_fn compute)
{
	__dtfl_cursor_list src_cl, out_cl, batch_cl;
	bool have_src, have_out;
	size_t nheld, i, h;
	out_buffer *outputs;
	valtab in_tab, out_tab;
	replayer eng;
	pending next;
	__dtfl_antichain lower_cp;
	/* The held times, an antichain of the capabilities. */
	__dtfl_antichain held;
	size_t pend_pos;
	bool any_in_interval;

	/* held = { cap.time } as an antichain. */
	__dtfl_antichain_init(&held, c->tc);
	for (i = 0; i < c->caps.len; i++)
		__dtfl_antichain_insert(&held,
		    dtfl_cap_time(*DTFL_VEC_AT(&c->caps, dtfl_cap *, i)));

	/*
	 * Need compute only if some held time is not in advance of upper.  When
	 * none is, upstream's CursorTactic::retire takes the no-work branch and
	 * returns `(produced, held.clone())` (reduce.rs:493-496): the new
	 * frontier is the held antichain itself, so the driver's capability
	 * downgrade is a no-op and every capability is preserved.  The port must
	 * do the same.  Returning without setting *new_frontier would leave it
	 * empty, and the unconditional downgrade in reduce_logic would then drop
	 * every capability while pending times still reference them; a later
	 * activation whose frontier re-enters that region would hold no
	 * capability covering a pending interesting time, tripping the
	 * output-tag assertion in the engine (idx != SIZE_MAX).
	 */
	any_in_interval = false;
	for (i = 0; i < __dtfl_antichain_len(&held); i++)
		if (!__dtfl_frontier_less_equal(c->tc, frontier, fn,
		    __dtfl_antichain_at(&held, i))) {
			any_in_interval = true;
			break;
		}
	if (!any_in_interval) {
		/* No work: leave the held times unchanged (upstream held.clone()). */
		__dtfl_antichain_clear(new_frontier);
		for (i = 0; i < __dtfl_antichain_len(&held); i++)
			__dtfl_antichain_insert(new_frontier,
			    __dtfl_antichain_at(&held, i));
		__dtfl_antichain_free(&held);
		return;
	}

	/* lower_limit as an antichain (for batches_through). */
	__dtfl_antichain_init(&lower_cp, c->tc);
	__dtfl_antichain_copy(&lower_cp, &c->lower_limit);

	/* Cursors over source and output batches preceding lower. */
	have_src = __dtfl_trace_agent_cursor_through(&c->source, &lower_cp,
	    &src_cl);
	have_out = __dtfl_trace_agent_cursor_through(&c->out_reader, &lower_cp,
	    &out_cl);
	DTFL_ASSERT(have_src && have_out);

	/* Cursor over the new input batches. */
	{
		size_t nb = c->batch_storage.len;
		__dtfl_batch_ord **arr = nb != 0 ?
		    __dtfl_alloc(nb * sizeof *arr) : NULL;

		for (i = 0; i < nb; i++)
			arr[i] = __dtfl_batch_ord_retain(*DTFL_VEC_AT(
			    &c->batch_storage, __dtfl_batch_ord *, i));
		__dtfl_cursor_list_init(&batch_cl, arr, nb, c->ikt, c->ivt);
		if (arr != NULL)
			__dtfl_free(arr);
	}

	/* One output buffer and builder slot per held capability. */
	nheld = __dtfl_antichain_len(&held);
	outputs = __dtfl_alloc(DTFL_MAX(nheld, 1u) * sizeof *outputs);
	for (h = 0; h < nheld; h++) {
		memset(&outputs[h], 0, sizeof outputs[h]);
		if (c->tw != 0)
			memcpy(outputs[h].tag, __dtfl_antichain_at(&held, h),
			    (size_t)c->tw * 8);
		__dtfl_slices_init(&outputs[h].recs);
		tv_init(&outputs[h].times, c->tc, c->tw);
		__dtfl_vec_init(&outputs[h].diffs, DTFL_MAX(c->odc->width, 1u));
	}

	valtab_init(&in_tab, c->ivt);
	valtab_init(&out_tab, c->ovt);
	replayer_init(&eng, c->tc, c->tw, c->ivt, c->idc, c->ovt, c->odc,
	    &in_tab, &out_tab);
	pending_init(&next, c->tc, c->tw);

	/* March keys: merge batch cursor and pending keys. */
	pend_pos = 0;
	__dtfl_cursor_list_rewind_keys(&batch_cl);
	while (__dtfl_cursor_list_key_valid(&batch_cl) ||
	    pend_pos < pending_len(&c->pend)) {
		const void *key1 = NULL, *key2 = NULL, *key;
		size_t klen1 = 0, klen2 = 0, klen;
		bool have1, have2;
		size_t prior_pos;
		timevec itimes;		/* interesting times */

		/* A 0-byte (unit) key is a valid key with a NULL pointer, so
		 * use the explicit validity flags, not pointer-NULL tests. */
		have1 = pend_pos < pending_len(&c->pend);
		have2 = __dtfl_cursor_list_key_valid(&batch_cl);
		if (have1)
			key1 = __dtfl_slices_get(&c->pend.keys, pend_pos, &klen1);
		if (have2)
			key2 = __dtfl_cursor_list_key(&batch_cl, &klen2);
		if (have1 && have2) {
			int r = dtfl_type_cmp(c->ikt, key1, klen1, key2, klen2);
			if (r <= 0) { key = key1; klen = klen1; }
			else { key = key2; klen = klen2; }
		} else if (have1) { key = key1; klen = klen1; }
		else { key = key2; klen = klen2; }

		/* Gather interesting times for `key` not beyond upper. */
		tv_init(&itimes, c->tc, c->tw);
		prior_pos = pend_pos;
		while (pend_pos < pending_len(&c->pend)) {
			size_t pk;
			const void *k = __dtfl_slices_get(&c->pend.keys,
			    pend_pos, &pk);
			if (!(pk == klen && (klen == 0 ||
			    memcmp(k, key, klen) == 0)))
				break;
			{
				const uint64_t *t = tv_at(&c->pend.times,
				    pend_pos);
				if (!__dtfl_frontier_less_equal(c->tc, frontier,
				    fn, t))
					tv_push(&itimes, t);
			}
			pend_pos++;
		}
		tv_sort_dedup(&itimes);

		{
			bool batch_here = have2 && klen2 == klen &&
			    (klen == 0 || memcmp(key2, key, klen) == 0);

			if (batch_here || tv_len(&itimes) != 0) {
				timevec new_times;

				tv_init(&new_times, c->tc, c->tw);
				/* reload the three histories for this key.  Clear
				 * the valtabs first: replay interns values into
				 * them, and each key starts with a fresh table. */
				valtab_clear(&in_tab);
				valtab_clear(&out_tab);
				vh_replay_key(&eng.batch_history, &batch_cl,
				    key, klen, NULL);
				vh_replay_key(&eng.input_history, &src_cl,
				    key, klen, NULL);
				vh_replay_key(&eng.output_history, &out_cl,
				    key, klen, NULL);

				compute(&eng, key, klen, &itimes,
				    &c->logic, frontier, fn, outputs, nheld,
				    &new_times);

				if (batch_here)
					__dtfl_cursor_list_step_key(&batch_cl);

				/* carry prior pending times still beyond upper. */
				for (i = prior_pos; i < pend_pos; i++) {
					const uint64_t *t = tv_at(
					    &c->pend.times, i);
					if (__dtfl_frontier_less_equal(c->tc,
					    frontier, fn, t))
						tv_push(&new_times, t);
				}
				tv_sort_dedup(&new_times);
				for (i = 0; i < tv_len(&new_times); i++)
					pending_push(&next, key, klen,
					    tv_at(&new_times, i));
				tv_free(&new_times);
			} else {
				/* copy pending key/times forward. */
				for (i = prior_pos; i < pend_pos; i++)
					pending_push(&next, key, klen,
					    tv_at(&c->pend.times, i));
			}
		}
		tv_free(&itimes);
	}

	/* Build output batches, tiling [lower, upper). */
	rd_build_batches(c, &held, outputs, nheld, produced, nproduced);

	/* Swap pending. */
	pending_clear(&c->pend);
	{
		pending tmp = c->pend;
		c->pend = next;
		next = tmp;
	}
	pending_free(&next);

	/* New capability frontier = the lower envelope of pending times. */
	__dtfl_antichain_clear(new_frontier);
	for (i = 0; i < pending_len(&c->pend); i++)
		__dtfl_antichain_insert(new_frontier,
		    tv_at(&c->pend.times, i));

	replayer_free(&eng);
	valtab_free(&in_tab);
	valtab_free(&out_tab);
	for (h = 0; h < nheld; h++) {
		__dtfl_slices_free(&outputs[h].recs);
		tv_free(&outputs[h].times);
		__dtfl_vec_free(&outputs[h].diffs);
	}
	__dtfl_free(outputs);
	__dtfl_cursor_list_free(&src_cl);
	__dtfl_cursor_list_free(&out_cl);
	__dtfl_cursor_list_free(&batch_cl);
	__dtfl_antichain_free(&held);
	__dtfl_antichain_free(&lower_cp);
}

/*
 * CursorTactic and ReferenceTactic are structural twins (reduce.rs: "only the
 * per-key engine differs"): both run rd_retire over the pending (key, time)
 * moments in c->pend, differing only in the engine.  Neither carries state
 * beyond c->pend, so tactic_state is NULL for both.
 */
static void
cursor_tactic_retire(reduce_op_ctx *c, const uint64_t *upper, size_t nupper,
    produced_batch *produced, size_t *nproduced,
    __dtfl_antichain *new_frontier)
{
	rd_retire(c, upper, nupper, produced, nproduced, new_frontier,
	    reduce_compute);
}

static void
reference_tactic_retire(reduce_op_ctx *c, const uint64_t *upper,
    size_t nupper, produced_batch *produced, size_t *nproduced,
    __dtfl_antichain *new_frontier)
{
	rd_retire(c, upper, nupper, produced, nproduced, new_frontier,
	    reference_compute);
}

static const rd_tactic_ops cursor_tactic_ops = {
	cursor_tactic_retire, NULL
};
static const rd_tactic_ops reference_tactic_ops = {
	reference_tactic_retire, NULL
};

/* ================================================================== */
/* The reduce operator and arrangement-level entry points.            */
/* ================================================================== */

static bool
reduce_logic(dtfl_op *op, void *vctx)
{
	reduce_op_ctx *c = vctx;
	dtfl_message *m;
	const uint64_t *frontier;
	size_t fn, i;
	uint64_t minimum[DTFL_TIME_MAX] = { 0 };
	produced_batch *produced;
	size_t nproduced = 0, nheld;
	__dtfl_antichain new_frontier;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* Downgrade previous upper limit to current lower limit. */
	__dtfl_antichain_clear(&c->lower_limit);
	for (i = 0; i < __dtfl_antichain_len(&c->upper_limit); i++)
		__dtfl_antichain_insert(&c->lower_limit,
		    __dtfl_antichain_at(&c->upper_limit, i));

	/* Drain input batches, capturing capabilities and the last upper. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		size_t n = dtfl_message_len(m);

		__dtfl_caps_insert(&c->caps, c->tc, cap);
		for (i = 0; i < n; i++) {
			size_t len;
			const void *recp = dtfl_message_record(m, i, &len);
			__dtfl_batch_ord *batch;

			memcpy(&batch, recp, sizeof batch);
			/* upper_limit = batch.upper (batches are in order). */
			__dtfl_antichain_clear(&c->upper_limit);
			{
				const __dtfl_antichain *bu =
				    __dtfl_batch_ord_upper(batch);
				size_t j;

				for (j = 0; j < __dtfl_antichain_len(bu); j++)
					__dtfl_antichain_insert(&c->upper_limit,
					    __dtfl_antichain_at(bu, j));
			}
			{
				__dtfl_batch_ord *ret =
				    __dtfl_batch_ord_retain(batch);

				__dtfl_vec_push(&c->batch_storage, &ret);
			}
			/*
			 * The record is a borrow (the input message owns the batch
			 * reference); reduce keeps its own reference in
			 * batch_storage for the deferred retire.
			 */
		}
	}

	/* The input frontier as a flat run. */
	frontier = dtfl_op_frontier(op, 0, &fn);

	/*
	 * Incorporate the input frontier guarantees: upper_limit =
	 * upper_limit join frontier (antichain join).  Advance upper to the
	 * frontier where the source trace has sealed further; here the source
	 * trace advance is folded into the input frontier the operator sees.
	 */
	{
		__dtfl_antichain joined;
		size_t a, b;

		__dtfl_antichain_init(&joined, c->tc);
		/*
		 * antichain_join_into: the pairwise joins of the elements, kept
		 * minimal (lattice.rs).  The empty antichain is the maximum, so
		 * joining with it yields the empty antichain (no pairs): when the
		 * input frontier is empty after close, upper_limit becomes empty
		 * and the operator seals the output to empty and releases.
		 */
		for (a = 0; a < __dtfl_antichain_len(&c->upper_limit); a++)
			for (b = 0; b < fn; b++) {
				uint64_t j[DTFL_TIME_MAX];

				if (c->tw != 0)
					dtfl_time_join(c->tc,
					    __dtfl_antichain_at(
					    &c->upper_limit, a),
					    frontier + (size_t)c->tw * b,
					    j);
				__dtfl_antichain_insert(&joined, j);
			}
		__dtfl_antichain_clear(&c->upper_limit);
		for (a = 0; a < __dtfl_antichain_len(&joined); a++)
			__dtfl_antichain_insert(&c->upper_limit,
			    __dtfl_antichain_at(&joined, a));
		__dtfl_antichain_free(&joined);
	}

	/* Only retire when [lower, upper) is non-empty. */
	if (__dtfl_antichain_eq(&c->upper_limit, &c->lower_limit))
		goto done;

	nheld = c->caps.len;
	produced = __dtfl_alloc(DTFL_MAX(nheld, 1u) * sizeof *produced);
	__dtfl_antichain_init(&new_frontier, c->tc);

	c->tactic->retire(c, (const uint64_t *)(void *)c->upper_limit.elems.data,
	    __dtfl_antichain_len(&c->upper_limit), produced, &nproduced,
	    &new_frontier);

	/* Ship each produced batch at a delayed capability and commit it. */
	for (i = 0; i < nproduced; i++) {
		size_t ci = rd_find_cap(c, produced[i].tag);
		dtfl_cap *src, *d;
		dtfl_session *s;
		unsigned char *dst;

		DTFL_ASSERT(ci != SIZE_MAX);
		src = *DTFL_VEC_AT(&c->caps, dtfl_cap *, ci);
		d = dtfl_cap_delayed(src, produced[i].tag);
		s = dtfl_op_session(op, 0, d);
		__dtfl_session_set_batch_ptr(s);
		/*
		 * writer.insert(batch.clone(), ...) then output.give(batch):
		 * the writer takes one reference into the output trace, and the
		 * stream record owns a second (retained here, released when the
		 * stream message frees).  The downstream reader must survive the
		 * shared spine merging and dropping the trace's reference.
		 */
		__dtfl_trace_writer_insert(&c->out_writer, produced[i].batch,
		    produced[i].tag);
		__dtfl_batch_ord_retain(produced[i].batch);
		dst = __dtfl_session_reserve(s, DTFL_BATCH_PTR_SIZE);
		memcpy(dst, &produced[i].batch, sizeof produced[i].batch);
		dtfl_session_close(s);
		dtfl_cap_drop(d);
	}
	__dtfl_free(produced);

	/* Downgrade capabilities to new_frontier (drop those below it). */
	{
		__dtfl_vec newcaps;
		size_t j;

		__dtfl_vec_init(&newcaps, sizeof(dtfl_cap *));
		for (j = 0; j < __dtfl_antichain_len(&new_frontier); j++) {
			const uint64_t *t = __dtfl_antichain_at(&new_frontier,
			    j);
			dtfl_cap *found = NULL;
			size_t ci;

			for (ci = 0; ci < c->caps.len; ci++) {
				dtfl_cap *cap = *DTFL_VEC_AT(&c->caps,
				    dtfl_cap *, ci);

				if (dtfl_time_less_equal(c->tc,
				    dtfl_cap_time(cap), t)) {
					found = cap;
					break;
				}
			}
			if (found == NULL)
				__dtfl_panic("reduce: failed to find capability");
			{
				dtfl_cap *nc = dtfl_cap_delayed(found, t);

				__dtfl_vec_push(&newcaps, &nc);
			}
		}
		for (j = 0; j < c->caps.len; j++)
			dtfl_cap_drop(*DTFL_VEC_AT(&c->caps, dtfl_cap *, j));
		__dtfl_vec_free(&c->caps);
		c->caps = newcaps;
	}

	/* Seal the output writer to upper_limit. */
	__dtfl_trace_writer_seal(&c->out_writer, &c->upper_limit);

	/* Advance compaction on both traces. */
	__dtfl_trace_agent_set_logical(&c->source,
	    (const uint64_t *)(void *)c->upper_limit.elems.data,
	    __dtfl_antichain_len(&c->upper_limit));
	__dtfl_trace_agent_set_physical(&c->source,
	    (const uint64_t *)(void *)c->upper_limit.elems.data,
	    __dtfl_antichain_len(&c->upper_limit));
	__dtfl_trace_agent_set_logical(&c->out_reader,
	    (const uint64_t *)(void *)c->upper_limit.elems.data,
	    __dtfl_antichain_len(&c->upper_limit));
	__dtfl_trace_agent_set_physical(&c->out_reader,
	    (const uint64_t *)(void *)c->upper_limit.elems.data,
	    __dtfl_antichain_len(&c->upper_limit));

	/* Release the consumed new input batches. */
	for (i = 0; i < c->batch_storage.len; i++)
		__dtfl_batch_ord_release(*DTFL_VEC_AT(&c->batch_storage,
		    __dtfl_batch_ord *, i));
	__dtfl_vec_clear(&c->batch_storage);
	__dtfl_antichain_free(&new_frontier);
	(void)minimum;

done:
	return c->caps.len != 0;
}

static void
reduce_op_free(void *vctx)
{
	reduce_op_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->caps.len; i++)
		dtfl_cap_drop(*DTFL_VEC_AT(&c->caps, dtfl_cap *, i));
	__dtfl_vec_free(&c->caps);
	for (i = 0; i < c->batch_storage.len; i++)
		__dtfl_batch_ord_release(*DTFL_VEC_AT(&c->batch_storage,
		    __dtfl_batch_ord *, i));
	__dtfl_vec_free(&c->batch_storage);
	pending_free(&c->pend);
	__dtfl_antichain_free(&c->upper_limit);
	__dtfl_antichain_free(&c->lower_limit);
	__dtfl_trace_writer_free(&c->out_writer);
	__dtfl_trace_agent_free(&c->out_reader);
	__dtfl_trace_agent_free(&c->source);
	if (c->logic_ctx_free != NULL)
		c->logic_ctx_free(c->logic_ctx_free_arg);
	if (c->tactic->free != NULL)
		c->tactic->free(c->tactic_state);
	__dtfl_free(c);
}

/*
 * reduce_with_tactic: build the reduce operator over the input arrangement
 * `a`, producing an output arrangement (key -> ovt, odc diffs), with the
 * per-interval work routed through `tactic` (which takes tactic_state).
 */
static dtfl_arranged *
reduce_with_tactic(dtfl_arranged *a, const char *name, const dtfl_type *ovt,
    const __dtfl_diff_class *odc, __dtfl_reduce_logic_fn logic, void *ctx,
    void (*ctx_free)(void *), const rd_tactic_ops *tactic,
    void *tactic_state)
{
	dtfl_stream *in_stream = __dtfl_arranged_stream(a);
	dtfl_scope *scope = dtfl_stream_scope(in_stream);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	unsigned depth = __dtfl_arranged_depth(a);
	__dtfl_trace_agent *src_agent = __dtfl_arranged_agent(a);
	const dtfl_type *ikt = dtfl_arranged_type(a);
	const dtfl_type *ivt = src_agent->vt;
	const __dtfl_diff_class *idc = __dtfl_arranged_diff_class(a);
	reduce_op_ctx *c = __dtfl_alloc(sizeof *c);
	__dtfl_trace *out_trace;
	dtfl_op_builder *b;
	dtfl_stream *out_stream;
	dtfl_arranged *out;
	__dtfl_trace_agent out_arr_reader;
	uint64_t minimum[DTFL_TIME_MAX] = { 0 };
	dtfl_diff *odiff = __dtfl_diff_wrap(odc);

	/* The output trace is a value trace: key -> out_val, odc diffs. */
	out_trace = __dtfl_trace_new(false, ikt, ovt, tc, odc);
	b = dtfl_op_builder_new(scope, name != NULL ? name : "Reduce");
	/* Acquire a logger for arrange events. */
	__dtfl_dd_logging_install(scope, b, out_trace, NULL);

	__dtfl_trace_agent_clone(src_agent, &c->source);
	__dtfl_trace_agent_new(out_trace, false, ikt, ovt, tc, odc,
	    &c->out_reader, &c->out_writer);
	/* A second reader for the output arrangement handle. */
	__dtfl_trace_agent_clone(&c->out_reader, &out_arr_reader);

	pending_init(&c->pend, tc, depth);
	__dtfl_vec_init(&c->caps, sizeof(dtfl_cap *));
	__dtfl_antichain_init_elem(&c->upper_limit, tc, minimum);
	__dtfl_antichain_init_elem(&c->lower_limit, tc, minimum);
	c->logic.fn = logic;
	c->logic.ctx = ctx;
	c->logic_ctx_free = ctx_free;
	c->logic_ctx_free_arg = ctx;
	c->tc = tc;
	c->tw = depth;
	c->depth = depth;
	c->ikt = ikt;
	c->ivt = ivt;
	c->idc = idc;
	c->ovt = ovt;
	c->odc = odc;
	c->abelian_wrap = false;
	__dtfl_vec_init(&c->batch_storage, sizeof(__dtfl_batch_ord *));
	c->tactic = tactic;
	c->tactic_state = tactic_state;

	dtfl_op_builder_input(b, in_stream, dtfl_pact_pipeline());
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
	out_stream = dtfl_op_builder_output(b, __dtfl_batch_ptr_type());
	dtfl_op_builder_build(b, reduce_logic, c, reduce_op_free);

	out = __dtfl_arranged_from_agent(out_stream, &out_arr_reader, ikt,
	    odiff, depth, false);
	return out;
}

/* reduce_trace: reduce_with_tactic with the cursor tactic. */
dtfl_arranged *
__dtfl_arranged_reduce_core(dtfl_arranged *a, const char *name,
    const dtfl_type *ovt, const __dtfl_diff_class *odc,
    __dtfl_reduce_logic_fn logic, void *ctx, void (*ctx_free)(void *))
{
	return reduce_with_tactic(a, name, ovt, odc, logic, ctx, ctx_free,
	    &cursor_tactic_ops, NULL);
}

/*
 * reduce_trace_reference (reference::reduce_trace_reference): the same
 * reduce driven by the model-derived reference tactic.  Same result
 * contract as __dtfl_arranged_reduce_core; a testing oracle, not a stable
 * entry point (upstream exports it doc-hidden).
 */
dtfl_arranged *
__dtfl_arranged_reduce_trace_reference(dtfl_arranged *a, const char *name,
    const dtfl_type *ovt, const __dtfl_diff_class *odc,
    __dtfl_reduce_logic_fn logic, void *ctx, void (*ctx_free)(void *))
{
	return reduce_with_tactic(a, name, ovt, odc, logic, ctx, ctx_free,
	    &reference_tactic_ops, NULL);
}

/* The abelian wrapper's context; frees the inner ctx then the wrapper. */
static void
abelian_ctx_free(void *v)
{
	abelian_logic *a = v;

	if (a->inner_ctx_free != NULL)
		a->inner_ctx_free(a->inner_ctx);
	__dtfl_free(a);
}

dtfl_arranged *
__dtfl_arranged_reduce_abelian(dtfl_arranged *a, const char *name,
    const dtfl_type *ovt, const __dtfl_diff_class *odc,
    __dtfl_reduce_logic_fn logic, void *ctx, void (*ctx_free)(void *))
{
	abelian_logic *aw = __dtfl_alloc(sizeof *aw);

	DTFL_ASSERT(odc->negate != NULL);	/* reduce_abelian needs Abelian */
	aw->inner = logic;
	aw->inner_ctx = ctx;
	aw->inner_ctx_free = ctx_free;
	aw->odc = odc;
	return __dtfl_arranged_reduce_core(a, name, ovt, odc, abelian_logic_fn,
	    aw, abelian_ctx_free);
}

/* ================================================================== */
/* Private arrange-by-key driver (collection.rs arrange_by_key).       */
/* ================================================================== */

/*
 * reduce arranges its (key || val) input by key before reduce_core.  This is
 * arrange_core with keyed = false, splitting each record into key and value
 * (the batcher and builder split by the key type's length), and exchanging
 * by the key prefix rather than the whole record.  It is kept private to this
 * file so reduce's batch / capability driver does not leak into the public
 * arrangement API.
 */
/* Exchange route: hash the key prefix of an update record. */
typedef struct abk_route {
	const dtfl_type *kt;
	unsigned depth;
	size_t dw;
} abk_route;

typedef struct abk_ctx {
	__dtfl_merge_batcher batcher;
	__dtfl_chunker chunker;
	__dtfl_trace_agent reader;
	__dtfl_trace_writer writer;
	__dtfl_vec caps;		/* dtfl_cap * */
	__dtfl_antichain prev_frontier;
	const dtfl_type *rt;		/* key || val record type (retained) */
	const dtfl_type *kt;		/* retained */
	const dtfl_type *vt;		/* retained */
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
	unsigned depth;
	size_t dw;
	abk_route route;		/* exchange route ctx (referenced by edge) */
} abk_ctx;

static uint64_t
abk_route_fn(const void *rec, size_t len, void *vctx)
{
	abk_route *r = vctx;
	size_t rlen = __dtfl_update_rec_len(r->depth, r->dw, len);
	size_t klen = dtfl_type_value_len(r->kt, rec, rlen);

	return dtfl_type_hash(r->kt, rec, klen);
}

static __dtfl_batch_ord *
abk_seal_batch(abk_ctx *c, const __dtfl_antichain *upper)
{
	__dtfl_batch **chain;
	size_t nchain, i;
	__dtfl_description desc;
	__dtfl_batch_ord *batch;

	__dtfl_merge_batcher_seal(&c->batcher, upper, &chain, &nchain, &desc);
	batch = __dtfl_builder_ord_seal(false, c->kt, c->vt, c->tc, c->dc,
	    chain, nchain, &desc);
	for (i = 0; i < nchain; i++)
		__dtfl_batch_release(chain[i]);
	if (chain != NULL)
		__dtfl_free(chain);
	return batch;
}

static bool
abk_logic(dtfl_op *op, void *vctx)
{
	abk_ctx *c = vctx;
	dtfl_message *m;
	size_t fn, i, j;
	const uint64_t *frontier;
	bool any_behind;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t k = dtfl_message_len(m);
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		__dtfl_batch *chunk;

		__dtfl_caps_insert(&c->caps, c->tc, cap);
		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw,
			    len);
			uint64_t etbuf[DTFL_TIME_MAX];
			const uint64_t *et = __dtfl_update_time_copy(rec,
			    c->depth, c->dw, len, etbuf);
			const void *diff = c->dw != 0 ?
			    __dtfl_update_diff(rec, c->dw, len) : NULL;

			__dtfl_chunker_push(&c->chunker, rec, rlen, et, diff);
		}
		while ((chunk = __dtfl_chunker_extract(&c->chunker)) != NULL)
			__dtfl_merge_batcher_push(&c->batcher, chunk);
	}

	frontier = dtfl_op_frontier(op, 0, &fn);

	{
		__dtfl_antichain cur;
		bool changed;

		__dtfl_antichain_init(&cur, c->tc);
		for (i = 0; i < fn; i++)
			__dtfl_antichain_insert(&cur,
			    frontier + (size_t)c->depth * i);
		changed = !__dtfl_antichain_eq(&c->prev_frontier, &cur);
		__dtfl_antichain_free(&cur);
		if (!changed)
			return c->caps.len != 0;
	}

	{
		__dtfl_batch *chunk;

		while ((chunk = __dtfl_chunker_finish(&c->chunker)) != NULL)
			__dtfl_merge_batcher_push(&c->batcher, chunk);
	}

	any_behind = false;
	for (i = 0; i < c->caps.len; i++) {
		dtfl_cap *cap = *DTFL_VEC_AT(&c->caps, dtfl_cap *, i);

		if (!__dtfl_frontier_less_equal(c->tc, frontier, fn,
		    dtfl_cap_time(cap))) {
			any_behind = true;
			break;
		}
	}

	if (any_behind) {
		__dtfl_antichain upper;

		__dtfl_antichain_init(&upper, c->tc);
		for (i = 0; i < c->caps.len; i++) {
			dtfl_cap *cap = *DTFL_VEC_AT(&c->caps, dtfl_cap *, i);
			__dtfl_batch_ord *batch;
			dtfl_session *s;
			unsigned char *dst;

			if (__dtfl_frontier_less_equal(c->tc, frontier, fn,
			    dtfl_cap_time(cap)))
				continue;

			__dtfl_antichain_clear(&upper);
			for (j = 0; j < fn; j++)
				__dtfl_antichain_insert(&upper,
				    frontier + (size_t)c->depth * j);
			for (j = i + 1; j < c->caps.len; j++)
				__dtfl_antichain_insert(&upper,
				    dtfl_cap_time(*DTFL_VEC_AT(&c->caps,
				    dtfl_cap *, j)));

			batch = abk_seal_batch(c, &upper);
			/*
			 * writer.insert(batch.clone()) then output.give(batch): the
			 * writer takes one reference into the trace and the stream
			 * record owns a second, released when the message frees.
			 */
			__dtfl_trace_writer_insert(&c->writer, batch,
			    dtfl_cap_time(cap));
			__dtfl_batch_ord_retain(batch);
			s = dtfl_op_session(op, 0, cap);
			__dtfl_session_set_batch_ptr(s);
			dst = __dtfl_session_reserve(s, DTFL_BATCH_PTR_SIZE);
			memcpy(dst, &batch, sizeof batch);
			dtfl_session_close(s);
		}

		{
			const __dtfl_antichain *bf =
			    __dtfl_merge_batcher_frontier(&c->batcher);
			__dtfl_vec newcaps;
			size_t ci;

			__dtfl_vec_init(&newcaps, sizeof(dtfl_cap *));
			for (j = 0; j < __dtfl_antichain_len(bf); j++) {
				const uint64_t *t = __dtfl_antichain_at(bf, j);
				dtfl_cap *found = NULL;

				for (ci = 0; ci < c->caps.len; ci++) {
					dtfl_cap *cap = *DTFL_VEC_AT(&c->caps,
					    dtfl_cap *, ci);

					if (__dtfl_frontier_less_equal(c->tc,
					    dtfl_cap_time(cap), 1, t)) {
						found = cap;
						break;
					}
				}
				if (found == NULL)
					__dtfl_panic("arrange_by_key: no "
					    "capability");
				{
					dtfl_cap *nc = dtfl_cap_delayed(found,
					    t);

					__dtfl_vec_push(&newcaps, &nc);
				}
			}
			for (ci = 0; ci < c->caps.len; ci++)
				dtfl_cap_drop(*DTFL_VEC_AT(&c->caps,
				    dtfl_cap *, ci));
			__dtfl_vec_free(&c->caps);
			c->caps = newcaps;
		}
		__dtfl_antichain_free(&upper);
	} else {
		__dtfl_antichain front;
		__dtfl_batch **chain;
		size_t nchain, bi;
		__dtfl_description desc;

		__dtfl_antichain_init(&front, c->tc);
		for (i = 0; i < fn; i++)
			__dtfl_antichain_insert(&front,
			    frontier + (size_t)c->depth * i);
		__dtfl_merge_batcher_seal(&c->batcher, &front, &chain, &nchain,
		    &desc);
		for (bi = 0; bi < nchain; bi++)
			__dtfl_batch_release(chain[bi]);
		if (chain != NULL)
			__dtfl_free(chain);
		__dtfl_description_free(&desc);
		__dtfl_trace_writer_seal(&c->writer, &front);
		__dtfl_antichain_free(&front);
	}

	__dtfl_antichain_clear(&c->prev_frontier);
	for (i = 0; i < fn; i++)
		__dtfl_antichain_insert(&c->prev_frontier,
		    frontier + (size_t)c->depth * i);

	return c->caps.len != 0;
}

static void
abk_free(void *vctx)
{
	abk_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->caps.len; i++)
		dtfl_cap_drop(*DTFL_VEC_AT(&c->caps, dtfl_cap *, i));
	__dtfl_vec_free(&c->caps);
	__dtfl_antichain_free(&c->prev_frontier);
	__dtfl_trace_writer_free(&c->writer);
	__dtfl_trace_agent_free(&c->reader);
	__dtfl_merge_batcher_free(&c->batcher);
	__dtfl_chunker_free(&c->chunker);
	dtfl_type_free(c->rt);
	dtfl_type_free(c->kt);
	dtfl_type_free(c->vt);
	__dtfl_free(c);
}

/* The route ctx is small and freed with the operator; stash it in abk_ctx. */

static dtfl_arranged *
arrange_by_key(dtfl_collection *coll, const dtfl_type *key_type,
    const dtfl_type *val_type, const char *name)
{
	dtfl_scope *scope = dtfl_stream_scope(coll->stream);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	unsigned depth = coll->depth;
	const __dtfl_diff_class *dc = coll->diff->cls;
	size_t dw = dc->width;
	abk_ctx *c = __dtfl_alloc(sizeof *c);
	__dtfl_trace *trace;
	dtfl_op_builder *b;
	dtfl_arranged *a;
	__dtfl_trace_agent arr_reader;
	uint64_t minimum[DTFL_TIME_MAX] = { 0 };

	trace = __dtfl_trace_new(false, key_type, val_type, tc, dc);

	/* Retained, as arrange_core does: the caller may free its handles. */
	c->rt = dtfl_type_retain(coll->type);
	c->kt = dtfl_type_retain(key_type);
	c->vt = dtfl_type_retain(val_type);
	c->tc = tc;
	c->dc = dc;
	c->depth = depth;
	c->dw = dw;
	__dtfl_merge_batcher_init(&c->batcher, coll->type, tc, dc);
	__dtfl_chunker_init(&c->chunker, coll->type, depth, dc);
	__dtfl_vec_init(&c->caps, sizeof(dtfl_cap *));
	__dtfl_antichain_init_elem(&c->prev_frontier, tc, minimum);

	b = dtfl_op_builder_new(scope, name != NULL ? name : "ArrangeByKey");
	/* Acquire a logger for arrange events. */
	__dtfl_dd_logging_install(scope, b, trace, &c->batcher);
	__dtfl_trace_agent_new(trace, false, key_type, val_type, tc, dc,
	    &c->reader, &c->writer);
	__dtfl_trace_agent_clone(&c->reader, &arr_reader);

	c->route.kt = key_type;
	c->route.depth = depth;
	c->route.dw = dw;

	dtfl_op_builder_input(b, coll->stream,
	    dtfl_pact_exchange(abk_route_fn, &c->route));
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
	{
		dtfl_stream *st = dtfl_op_builder_output(b,
		    __dtfl_batch_ptr_type());

		dtfl_op_builder_build(b, abk_logic, c, abk_free);
		a = __dtfl_arranged_from_agent(st, &arr_reader, key_type,
		    dtfl_diff_retain(coll->diff), depth, false);
	}
	return a;
}

/* ================================================================== */
/* Collection-level methods (collection.rs).                           */
/* ================================================================== */

/* The user reduce logic plus the input/output types it needs. */
typedef struct user_reduce {
	dtfl_reduce_fn fn;
	void *ctx;
	void (*ctx_free)(void *);
} user_reduce;

struct dtfl_reduce_emit {
	__dtfl_reduce_change *ch;
};

void
dtfl_reduce_emit_pair(dtfl_reduce_emit *e, const void *val, size_t vlen,
    const void *diff)
{
	__dtfl_reduce_change_push(e->ch, val, vlen, diff);
}

/* Adapter: present the engine's arrays to the user reduce fn. */
static void
user_reduce_logic(void *vctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, __dtfl_reduce_change *ch)
{
	user_reduce *u = vctx;
	dtfl_reduce_emit e;

	(void)m; (void)out_vals; (void)out_vlens; (void)out_diffs;
	e.ch = ch;
	u->fn(u->ctx, key, klen, n, in_vals, in_vlens, in_diffs, &e);
}

static void
user_reduce_free(void *vctx)
{
	user_reduce *u = vctx;

	if (u->ctx_free != NULL)
		u->ctx_free(u->ctx);
	__dtfl_free(u);
}

/* as_collection kv fn: emit key || val as the output record. */
static void
kv_concat(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, dtfl_arranged_emit *out)
{
	unsigned char *rec;
	size_t rl = klen + vlen;

	(void)ctx;
	rec = __dtfl_alloc(rl != 0 ? rl : 1);
	if (klen != 0)
		memcpy(rec, key, klen);
	if (vlen != 0)
		memcpy(rec + klen, val, vlen);
	dtfl_arranged_emit_record(out, rec, rl);
	__dtfl_free(rec);
}

dtfl_collection *
dtfl_collection_reduce_named(dtfl_collection *c, const char *name,
    const dtfl_type *key_type, const dtfl_type *val_type,
    const dtfl_type *out_val, const dtfl_diff *out_diff, dtfl_reduce_fn logic,
    dtfl_ctx ctx)
{
	dtfl_arranged *arr, *red;
	dtfl_collection *out;
	user_reduce *u = __dtfl_alloc(sizeof *u);
	const dtfl_type *fields[2];
	const dtfl_type *out_rec_type;

	u->fn = logic;
	u->ctx = ctx.ptr;
	u->ctx_free = ctx.free;

	arr = arrange_by_key(c, key_type, val_type,
	    name != NULL ? name : "Reduce");
	red = __dtfl_arranged_reduce_abelian(arr,
	    name != NULL ? name : "Reduce", out_val, out_diff->cls,
	    user_reduce_logic, u, user_reduce_free);
	dtfl_arranged_free(arr);

	/* The output record is key || out_val. */
	fields[0] = key_type;
	fields[1] = out_val;
	out_rec_type = dtfl_type_tuple(fields, 2);
	out = dtfl_arranged_as_collection(red, out_rec_type, kv_concat,
	    DTFL_NOCTX);
	dtfl_type_free(out_rec_type);
	dtfl_arranged_free(red);
	return out;
}

dtfl_collection *
dtfl_collection_reduce(dtfl_collection *c, const dtfl_type *key_type,
    const dtfl_type *val_type, const dtfl_type *out_val,
    const dtfl_diff *out_diff, dtfl_reduce_fn logic, dtfl_ctx ctx)
{
	return dtfl_collection_reduce_named(c, "Reduce", key_type, val_type,
	    out_val, out_diff, logic, ctx);
}

/* ------------------------------------------------------------------ */
/* Public reduce over an arrangement (arrangement.rs reduce_core,       */
/* reduce_abelian): forwarders to the internal entry points.           */
/* ------------------------------------------------------------------ */

/* The user reduce-core logic plus its context. */
typedef struct user_reduce_core {
	dtfl_reduce_core_fn fn;
	void *ctx;
	void (*ctx_free)(void *);
} user_reduce_core;

/* Adapter: present the engine's arrays to the user reduce-core fn. */
static void
user_reduce_core_logic(void *vctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    size_t m, const void *const *out_vals, const size_t *out_vlens,
    const void *out_diffs, __dtfl_reduce_change *ch)
{
	user_reduce_core *u = vctx;
	dtfl_reduce_emit e;

	e.ch = ch;
	u->fn(u->ctx, key, klen, n, in_vals, in_vlens, in_diffs, m, out_vals,
	    out_vlens, out_diffs, &e);
}

static void
user_reduce_core_free(void *vctx)
{
	user_reduce_core *u = vctx;

	if (u->ctx_free != NULL)
		u->ctx_free(u->ctx);
	__dtfl_free(u);
}

dtfl_arranged *
dtfl_arranged_reduce_core(dtfl_arranged *a, const char *name,
    const dtfl_type *out_val, const dtfl_diff *out_diff,
    dtfl_reduce_core_fn logic, dtfl_ctx ctx)
{
	user_reduce_core *u = __dtfl_alloc(sizeof *u);

	u->fn = logic;
	u->ctx = ctx.ptr;
	u->ctx_free = ctx.free;
	return __dtfl_arranged_reduce_core(a, name, out_val, out_diff->cls,
	    user_reduce_core_logic, u, user_reduce_core_free);
}

dtfl_arranged *
dtfl_arranged_reduce_abelian(dtfl_arranged *a, const char *name,
    const dtfl_type *out_val, const dtfl_diff *out_diff,
    dtfl_reduce_core_fn logic, dtfl_ctx ctx)
{
	user_reduce_core *u = __dtfl_alloc(sizeof *u);

	u->fn = logic;
	u->ctx = ctx.ptr;
	u->ctx_free = ctx.free;
	return __dtfl_arranged_reduce_abelian(a, name, out_val, out_diff->cls,
	    user_reduce_core_logic, u, user_reduce_core_free);
}

/* ------------------------------------------------------------------ */
/* count: for each key, emit (key, summed_diff) with output diff one.   */
/* ------------------------------------------------------------------ */

typedef struct count_ctx {
	const __dtfl_diff_class *idc;	/* input diff class (the value) */
	const __dtfl_diff_class *odc;	/* output diff class (one) */
} count_ctx;

/*
 * Render a diff value (native, as stored in the diff column) as the bytes of
 * its record descriptor (D1 order-preserving encoding).  count uses this to
 * turn the summed input diff into an output value.  Supports the built-in
 * integer classes, whose record type is the matching integer descriptor; a
 * class without an integer record type yields the empty value (unit).
 */
static size_t
count_value_bytes(const __dtfl_diff_class *dc, const void *native,
    unsigned char *buf)
{
	if (native == NULL || dc->width == 0)
		return 0;
	if (dc->kind == DTFL_DIFF_KIND_SIGNED ||
	    dc->kind == DTFL_DIFF_KIND_WRAPPING) {
		switch (dc->width) {
		case 1: { int8_t v; memcpy(&v, native, 1); return dtfl_put_i8(buf, v); }
		case 2: { int16_t v; memcpy(&v, native, 2); return dtfl_put_i16(buf, v); }
		case 4: { int32_t v; memcpy(&v, native, 4); return dtfl_put_i32(buf, v); }
		default: { int64_t v; memcpy(&v, native, 8); return dtfl_put_i64(buf, v); }
		}
	}
	if (dc->kind == DTFL_DIFF_KIND_UNSIGNED) {
		switch (dc->width) {
		case 1: { uint8_t v; memcpy(&v, native, 1); return dtfl_put_u8(buf, v); }
		case 2: { uint16_t v; memcpy(&v, native, 2); return dtfl_put_u16(buf, v); }
		case 4: { uint32_t v; memcpy(&v, native, 4); return dtfl_put_u32(buf, v); }
		default: { uint64_t v; memcpy(&v, native, 8); return dtfl_put_u64(buf, v); }
		}
	}
	/* A user class with its own record type carries the native bytes. */
	memcpy(buf, native, dc->width);
	return dc->width;
}

/*
 * count logic: the input accumulation at a key is one (unit, summed) pair
 * (arrange_by_self consolidates to a single unit value).  Emit the summed
 * input diff rendered as a value of the input diff class's record type, with
 * an output diff of one (collection.rs count_core: t.push((s[0].1, R2::one)).
 */
static void
count_logic(void *vctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    dtfl_reduce_emit *out)
{
	count_ctx *cc = vctx;
	size_t odw = cc->odc->width;
	unsigned char *one = odw != 0 ? __dtfl_alloc(odw) : NULL;
	unsigned char valbuf[16] = { 0 };
	size_t vlen;

	(void)key; (void)klen; (void)n; (void)in_vals; (void)in_vlens;
	/* in_diffs[0] is the summed input diff; render it as a value. */
	vlen = count_value_bytes(cc->idc,
	    cc->idc->width != 0 ? in_diffs : NULL, valbuf);
	if (odw != 0)
		cc->odc->one(cc->odc, one);
	dtfl_reduce_emit_pair(out, valbuf, vlen, odw != 0 ? one : NULL);
	if (one != NULL)
		__dtfl_free(one);
}

dtfl_collection *
dtfl_collection_count(dtfl_collection *c, const dtfl_diff *out_diff)
{
	const dtfl_type *kt = dtfl_collection_type(c);
	const __dtfl_diff_class *idc = dtfl_collection_diff(c)->cls;
	const __dtfl_diff_class *odc = out_diff->cls;
	const dtfl_type *out_val = __dtfl_diff_type(idc);
	count_ctx *cc = __dtfl_alloc(sizeof *cc);
	dtfl_arranged *arr, *red;
	dtfl_collection *out;
	const dtfl_type *fields[2];
	const dtfl_type *out_rec_type;
	user_reduce *u = __dtfl_alloc(sizeof *u);

	DTFL_ASSERT(odc->negate != NULL && odc->one != NULL);
	cc->idc = idc;
	cc->odc = odc;
	u->fn = count_logic;
	u->ctx = cc;
	u->ctx_free = __dtfl_free;

	arr = dtfl_collection_arrange(c, "Arrange: Count");
	red = __dtfl_arranged_reduce_abelian(arr, "Count", out_val, odc,
	    user_reduce_logic, u, user_reduce_free);
	dtfl_arranged_free(arr);

	fields[0] = kt;
	fields[1] = out_val;
	out_rec_type = dtfl_type_tuple(fields, 2);
	out = dtfl_arranged_as_collection(red, out_rec_type, kv_concat,
	    DTFL_NOCTX);
	dtfl_type_free(out_rec_type);
	dtfl_arranged_free(red);
	return out;
}

/* ------------------------------------------------------------------ */
/* threshold / distinct.                                                */
/* ------------------------------------------------------------------ */

typedef struct thresh_ctx {
	dtfl_threshold_fn fn;
	void *ctx;
	void (*ctx_free)(void *);
	const __dtfl_diff_class *idc;
	const __dtfl_diff_class *odc;
} thresh_ctx;

/*
 * threshold logic: the input accumulation is one (unit, summed_diff) pair;
 * apply thresh to the summed input diff and, if it emits, push ((), out_diff)
 * (collection.rs threshold_named: t.push(((), thresh(k, s[0].1)))).
 */
static void
thresh_logic(void *vctx, const void *key, size_t klen, size_t n,
    const void *const *in_vals, const size_t *in_vlens, const void *in_diffs,
    dtfl_reduce_emit *out)
{
	thresh_ctx *tc = vctx;
	size_t odw = tc->odc->width;
	unsigned char *od = odw != 0 ? __dtfl_alloc(odw) : NULL;

	(void)n; (void)in_vals; (void)in_vlens;
	if (tc->fn(tc->ctx, key, klen, tc->idc->width != 0 ? in_diffs : NULL,
	    odw != 0 ? od : NULL))
		dtfl_reduce_emit_pair(out, NULL, 0, odw != 0 ? od : NULL);
	if (od != NULL)
		__dtfl_free(od);
}

static void
thresh_ctx_free(void *v)
{
	thresh_ctx *tc = v;

	if (tc->ctx_free != NULL)
		tc->ctx_free(tc->ctx);
	__dtfl_free(tc);
}

dtfl_collection *
dtfl_collection_threshold_named(dtfl_collection *c, const char *name,
    const dtfl_diff *out_diff, dtfl_threshold_fn thresh, dtfl_ctx ctx)
{
	const dtfl_type *kt = dtfl_collection_type(c);
	const __dtfl_diff_class *idc = dtfl_collection_diff(c)->cls;
	const __dtfl_diff_class *odc = out_diff->cls;
	thresh_ctx *tc = __dtfl_alloc(sizeof *tc);
	user_reduce *u = __dtfl_alloc(sizeof *u);
	dtfl_arranged *arr, *red;
	dtfl_collection *out;

	DTFL_ASSERT(odc->negate != NULL);
	tc->fn = thresh;
	tc->ctx = ctx.ptr;
	tc->ctx_free = ctx.free;
	tc->idc = idc;
	tc->odc = odc;
	u->fn = thresh_logic;
	u->ctx = tc;
	u->ctx_free = thresh_ctx_free;

	arr = dtfl_collection_arrange(c, name != NULL ? name : "Threshold");
	red = __dtfl_arranged_reduce_abelian(arr,
	    name != NULL ? name : "Threshold", dtfl_type_unit(), odc,
	    user_reduce_logic, u, user_reduce_free);
	dtfl_arranged_free(arr);

	/* as_collection emits the key (NULL fn: the stored record is the key). */
	out = dtfl_arranged_as_collection(red, kt, NULL, DTFL_NOCTX);
	dtfl_arranged_free(red);
	return out;
}

dtfl_collection *
dtfl_collection_threshold(dtfl_collection *c, const dtfl_diff *out_diff,
    dtfl_threshold_fn thresh, dtfl_ctx ctx)
{
	return dtfl_collection_threshold_named(c, "Threshold", out_diff,
	    thresh, ctx);
}

/* distinct: threshold that maps any non-zero accumulation to one. */
static bool
distinct_thresh(void *ctx, const void *key, size_t klen, const void *acc_diff,
    void *out)
{
	const __dtfl_diff_class *odc = ctx;

	(void)key; (void)klen; (void)acc_diff;
	/* The input accumulation is already non-zero (consolidation drops
	 * zeros), so distinct always emits one. */
	if (odc->width != 0)
		odc->one(odc, out);
	return true;
}

dtfl_collection *
dtfl_collection_distinct(dtfl_collection *c, const dtfl_diff *out_diff)
{
	DTFL_ASSERT(out_diff->cls->one != NULL);
	return dtfl_collection_threshold_named(c, "Distinct", out_diff,
	    distinct_thresh, DTFL_CTX((void *)out_diff->cls, NULL));
}
