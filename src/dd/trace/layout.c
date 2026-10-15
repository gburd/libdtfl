/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/implementations/mod.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/layout.c: the columnar storage behind the ord_neu trie
 * (PLAN D2).  Upstream's Layout picks a BatchContainer for keys, vals,
 * times, diffs and offsets; libdtfl has one container, so keys and vals
 * are byte arenas with offset arrays (SliceContainer), offsets are a
 * size_t column (OffsetList, whose u32/u64 packing is a space-only
 * optimization), and times and diffs are fixed-width columns.  Vals and
 * Upds are the two trie layers; UpdsBuilder consolidates and seals the
 * per-value update lists, with the singleton optimization kept.
 */
#include "dd/trace/trace_int.h"

/* ------------------------------------------------------------------ */
/* Slices (SliceContainer)                                             */
/* ------------------------------------------------------------------ */

void
__dtfl_slices_init(__dtfl_slices *s)
{
	size_t zero = 0;

	__dtfl_vec_init(&s->arena, 1);
	__dtfl_vec_init(&s->offs, sizeof(size_t));
	__dtfl_vec_push(&s->offs, &zero);
}

void
__dtfl_slices_free(__dtfl_slices *s)
{
	__dtfl_vec_free(&s->arena);
	__dtfl_vec_free(&s->offs);
}

void
__dtfl_slices_clear(__dtfl_slices *s)
{
	size_t zero = 0;

	__dtfl_vec_clear(&s->arena);
	__dtfl_vec_clear(&s->offs);
	__dtfl_vec_push(&s->offs, &zero);
}

void
__dtfl_slices_push(__dtfl_slices *s, const void *p, size_t len)
{
	size_t end;

	if (len != 0)
		__dtfl_vec_push_n(&s->arena, p, len);
	end = s->arena.len;
	__dtfl_vec_push(&s->offs, &end);
}

/*
 * BatchContainer::advance (containers.rs): the number of indices in
 * [start, end) whose value is strictly less than `key`.  Exponential
 * search when the answer is past the small limit; a linear scan
 * otherwise.  The predicate "value < key" must stay false once false,
 * which holds because the values are sorted.
 */
/* cmp_raw(a, b) < 0 for a memcmp-ordered type: by prefix, then length. */
static inline bool
slice_less_raw(const void *a, size_t alen, const void *b, size_t blen)
{
	size_t m = alen < blen ? alen : blen;
	int c = m != 0 ? memcmp(a, b, m) : 0;

	if (c != 0)
		return c < 0;
	return alen < blen;
}
size_t
__dtfl_slices_advance(const __dtfl_slices *s, const dtfl_type *t, size_t start,
    size_t end, const void *key, size_t klen)
{
	const size_t small_limit = 8;
	size_t index, step, len;
	const void *v;
	/*
	 * A memcmp-ordered type (cmp == NULL) compares by cmp_raw: memcmp
	 * over the common prefix, then by length.  Inline it so the hot seek
	 * loop skips the dtfl_type_cmp dispatch; the result is identical.
	 */
	const bool raw = t->cmp == NULL;

#define LESS(i)								\
	(v = __dtfl_slices_get(s, (i), &len),				\
	    raw ? slice_less_raw(v, len, key, klen)			\
		: dtfl_type_cmp(t, v, len, key, klen) < 0)

	if (end > start + small_limit && LESS(start + small_limit)) {
		index = small_limit + 1;
		if (start + index < end && LESS(start + index)) {
			step = 1;
			while (start + index + step < end &&
			    LESS(start + index + step)) {
				index += step;
				step <<= 1;
			}
			step >>= 1;
			while (step > 0) {
				if (start + index + step < end &&
				    LESS(start + index + step))
					index += step;
				step >>= 1;
			}
			index += 1;
		}
		return index;
	} else {
		size_t limit = DTFL_MIN(end, start + small_limit), i, count = 0;

		for (i = start; i < limit; i++) {
			if (LESS(i))
				count++;
		}
		return count;
	}
#undef LESS
}

/* ------------------------------------------------------------------ */
/* Offsets (OffsetList)                                                */
/* ------------------------------------------------------------------ */

void
__dtfl_offsets_init(__dtfl_offsets *o)
{
	__dtfl_vec_init(&o->v, sizeof(size_t));
}

void
__dtfl_offsets_free(__dtfl_offsets *o)
{
	__dtfl_vec_free(&o->v);
}

void
__dtfl_offsets_clear(__dtfl_offsets *o)
{
	__dtfl_vec_clear(&o->v);
}

void
__dtfl_offsets_push(__dtfl_offsets *o, size_t val)
{
	__dtfl_vec_push(&o->v, &val);
}

/* ------------------------------------------------------------------ */
/* Vals                                                                */
/* ------------------------------------------------------------------ */

void
__dtfl_vals_init(__dtfl_vals *v)
{
	__dtfl_offsets_init(&v->offs);
	__dtfl_offsets_push(&v->offs, 0);
	__dtfl_slices_init(&v->vals);
}

void
__dtfl_vals_free(__dtfl_vals *v)
{
	__dtfl_offsets_free(&v->offs);
	__dtfl_slices_free(&v->vals);
}

void
__dtfl_vals_bounds(const __dtfl_vals *v, size_t index, size_t *lo, size_t *hi)
{
	*lo = __dtfl_offsets_at(&v->offs, index);
	*hi = __dtfl_offsets_at(&v->offs, index + 1);
}

/* ------------------------------------------------------------------ */
/* Upds                                                                */
/* ------------------------------------------------------------------ */

void
__dtfl_upds_init(__dtfl_upds *u, unsigned tw, unsigned dw)
{
	__dtfl_offsets_init(&u->offs);
	__dtfl_offsets_push(&u->offs, 0);
	__dtfl_vec_init(&u->times, DTFL_MAX((size_t)tw * sizeof(uint64_t), 1));
	__dtfl_vec_init(&u->diffs, DTFL_MAX((size_t)dw, 1));
	u->nupds = 0;
	u->tw = tw;
	u->dw = dw;
}

void
__dtfl_upds_free(__dtfl_upds *u)
{
	__dtfl_offsets_free(&u->offs);
	__dtfl_vec_free(&u->times);
	__dtfl_vec_free(&u->diffs);
}

/*
 * Upds::bounds: equal lower and upper encodes "singleton; just before
 * here", so return (lower - 1, upper) in that case.  lower must then be
 * greater than zero, as upstream asserts.
 */
void
__dtfl_upds_bounds(const __dtfl_upds *u, size_t index, size_t *lo, size_t *hi)
{
	size_t lower = __dtfl_offsets_at(&u->offs, index);
	size_t upper = __dtfl_offsets_at(&u->offs, index + 1);

	if (lower == upper) {
		DTFL_ASSERT(lower > 0);
		lower -= 1;
	}
	*lo = lower;
	*hi = upper;
}

/* The last stored update time, for the singleton comparison. */
static const uint64_t *
upds_last_time(const __dtfl_upds *u)
{
	if (u->tw == 0 || u->nupds == 0)
		return NULL;
	return __dtfl_upds_time(u, u->nupds - 1);
}

static const void *
upds_last_diff(const __dtfl_upds *u)
{
	if (u->dw == 0 || u->nupds == 0)
		return NULL;
	return __dtfl_upds_diff(u, u->nupds - 1);
}

/* ------------------------------------------------------------------ */
/* UpdsBuilder                                                         */
/* ------------------------------------------------------------------ */

void
__dtfl_upds_builder_init(__dtfl_upds_builder *b, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc)
{
	b->tc = tc;
	b->dc = dc;
	b->tw = dtfl_time_depth(tc);
	b->dw = (unsigned)dc->width;
	__dtfl_vec_init(&b->stash_t, DTFL_MAX((size_t)b->tw * sizeof(uint64_t), 1));
	__dtfl_vec_init(&b->stash_d, DTFL_MAX((size_t)b->dw, 1));
	b->total = 0;
}

void
__dtfl_upds_builder_free(__dtfl_upds_builder *b)
{
	__dtfl_vec_free(&b->stash_t);
	__dtfl_vec_free(&b->stash_d);
}

void
__dtfl_upds_builder_push(__dtfl_upds_builder *b, const uint64_t *time,
    const void *diff)
{
	void *slot = __dtfl_vec_push(&b->stash_t, NULL);

	if (b->tw != 0)
		memcpy(slot, time, (size_t)b->tw * sizeof(uint64_t));
	slot = __dtfl_vec_push(&b->stash_d, NULL);
	if (b->dw != 0)
		memcpy(slot, diff, b->dw);
}

/* Access to the i'th staged (time, diff). */
static uint64_t *
stash_time(__dtfl_upds_builder *b, size_t i)
{
	return (uint64_t *)(void *)b->stash_t.data + (size_t)b->tw * i;
}

static void *
stash_diff(__dtfl_upds_builder *b, size_t i)
{
	return b->stash_d.data + (size_t)b->dw * i;
}

static size_t
stash_len(const __dtfl_upds_builder *b)
{
	/*
	 * Each staged update is one element of stash_t (tw words) and one of
	 * stash_d (dw bytes); a __dtfl_vec counts elements, so the staged
	 * count is the element count of whichever stash carries width.  When
	 * tw == 0 (unit time) stash_t has no width, so read stash_d.
	 */
	return b->tw == 0 ? b->stash_d.len : b->stash_t.len;
}

/* consolidation::consolidate over the stash by (time, diff order). */
static int
stash_cmp(__dtfl_upds_builder *b, size_t i, size_t j)
{
	int r;

	if (b->tw != 0) {
		r = __dtfl_words_cmp(stash_time(b, i), stash_time(b, j), b->tw);
		if (r != 0)
			return r;
	}
	return 0;
}

static void
stash_clear(__dtfl_upds_builder *b)
{
	__dtfl_vec_clear(&b->stash_t);
	__dtfl_vec_clear(&b->stash_d);
}

/*
 * UpdsBuilder::seal: consolidate the stash by time, drop zero diffs, and
 * push the survivors into `u`.  The singleton optimization: a single
 * survivor equal to the last stored update is encoded by pushing an
 * offset equal to the current times length (empty slice).
 */
bool
__dtfl_upds_builder_seal(__dtfl_upds_builder *b, __dtfl_upds *u)
{
	size_t n = stash_len(b), i, k, out = 0;
	unsigned char *accum;
	size_t *order;

	if (n == 0)
		return false;

	/* Sort stash indices by time (stable insertion sort; n is small). */
	order = __dtfl_alloc(n * sizeof *order);
	for (i = 0; i < n; i++)
		order[i] = i;
	for (i = 1; i < n; i++) {
		size_t key = order[i], j = i;

		while (j > 0 && stash_cmp(b, order[j - 1], key) > 0) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = key;
	}

	/* Consolidate equal-time runs in place, compacting to the front. */
	accum = __dtfl_alloc(DTFL_MAX(b->dw, 1u));
	for (i = 0; i < n; i = k) {
		size_t src = order[i];

		if (b->dw != 0)
			memcpy(accum, stash_diff(b, src), b->dw);
		for (k = i + 1; k < n && stash_cmp(b, src, order[k]) == 0; k++)
			b->dc->plus_equals(b->dc, accum,
			    stash_diff(b, order[k]));
		if (b->dc->is_zero(b->dc, accum))
			continue;
		/* Move survivor to slot `out`. */
		if (b->tw != 0)
			memmove(stash_time(b, out), stash_time(b, src),
			    (size_t)b->tw * sizeof(uint64_t));
		if (b->dw != 0)
			memcpy(stash_diff(b, out), accum, b->dw);
		out++;
	}
	__dtfl_free(accum);
	__dtfl_free(order);

	if (out == 0) {
		stash_clear(b);
		return false;
	}

	/* Singleton optimization: one survivor equal to the last update. */
	if (out == 1) {
		const uint64_t *lt = upds_last_time(u);
		const void *ld = upds_last_diff(u);
		bool time_eq = (b->tw == 0) ||
		    (lt != NULL &&
		     __dtfl_words_cmp(lt, stash_time(b, 0), b->tw) == 0);
		bool diff_eq = (b->dw == 0) ||
		    (ld != NULL &&
		     memcmp(ld, stash_diff(b, 0), b->dw) == 0);

		if (time_eq && diff_eq) {
			b->total += 1;
			stash_clear(b);
			__dtfl_offsets_push(&u->offs, u->nupds);
			return true;
		}
	}

	/* Conventional: append the survivors. */
	b->total += out;
	for (i = 0; i < out; i++) {
		void *ts = __dtfl_vec_push(&u->times, NULL);
		void *ds;

		if (u->tw != 0)
			memcpy(ts, stash_time(b, i),
			    (size_t)u->tw * sizeof(uint64_t));
		ds = __dtfl_vec_push(&u->diffs, NULL);
		if (u->dw != 0)
			memcpy(ds, stash_diff(b, i), u->dw);
		u->nupds++;
	}
	stash_clear(b);
	__dtfl_offsets_push(&u->offs, u->nupds);
	return true;
}
