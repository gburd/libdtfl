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

	s->stride = 0;
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
	if (s->stride == 0) {
		__dtfl_vec_clear(&s->offs);
		__dtfl_vec_push(&s->offs, &zero);
	}
}

void
__dtfl_slices_push(__dtfl_slices *s, const void *p, size_t len)
{
	size_t end;

	if (s->stride != 0) {
		/*
		 * Columnar path: fixed-width records concatenated, no offsets.
		 * Every value is exactly stride bytes, as the type promised.
		 */
		DTFL_ASSERT(len == s->stride);
		__dtfl_vec_push_n(&s->arena, p, len);
		return;
	}
	if (len == sizeof(uint64_t)) {
		uint64_t x;

		/* Common case: an 8-byte key or value, stored as one word. */
		memcpy(&x, p, sizeof x);
		__dtfl_vec_push_8bytes(&s->arena, x);
	} else if (len != 0)
		__dtfl_vec_push_n(&s->arena, p, len);
	end = s->arena.len;
	__dtfl_vec_push_size(&s->offs, end);
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

#ifdef DTFL_COLUMNAR
/*
 * Big-endian loads for the columnar fast path.  Keys are stored big-endian
 * (base/record.c be_put), so a big-endian load yields the logical unsigned
 * value, and comparing those values reproduces the memcmp of the bytes.
 * The byte assembly folds to a single load plus bswap on common targets.
 */
static inline uint64_t
__dtfl_be64_load(const void *p)
{
	const unsigned char *b = (const unsigned char *)p;

	return (uint64_t)b[0] << 56 | (uint64_t)b[1] << 48 |
	    (uint64_t)b[2] << 40 | (uint64_t)b[3] << 32 |
	    (uint64_t)b[4] << 24 | (uint64_t)b[5] << 16 |
	    (uint64_t)b[6] << 8 | (uint64_t)b[7];
}

static inline uint32_t
__dtfl_be32_load(const void *p)
{
	const unsigned char *b = (const unsigned char *)p;

	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
	    (uint32_t)b[2] << 8 | (uint32_t)b[3];
}

/*
 * Columnar fast path for __dtfl_slices_advance: the same exponential +
 * binary search, but values are a flat stride column, so the predicate is
 * a direct strided compare (an integer load for width 8/4, else a memcmp of
 * `stride` bytes) with no offsets indirection and no length bookkeeping.
 * The key is also exactly `stride` bytes (klen == stride), so comparing
 * stride bytes reproduces cmp_raw exactly: the result is byte-identical to
 * the faithful path.  Deliberate non-fidelity divergence (PLAN D2), gated.
 */
static size_t
slices_advance_col(const __dtfl_slices *s, size_t start, size_t end,
    const void *key)
{
	const size_t small_limit = 8;
	const unsigned char *base = (const unsigned char *)s->arena.data;
	unsigned stride = s->stride;
	size_t index, step;

#define COL_LESS(i)							\
	(stride == 8							\
	    ? __dtfl_be64_load(base + (size_t)stride * (i)) <		\
		__dtfl_be64_load(key)					\
	    : stride == 4						\
		? __dtfl_be32_load(base + (size_t)stride * (i)) <	\
		    __dtfl_be32_load(key)				\
		: memcmp(base + (size_t)stride * (i), key, stride) < 0)

	if (end > start + small_limit && COL_LESS(start + small_limit)) {
		index = small_limit + 1;
		if (start + index < end && COL_LESS(start + index)) {
			step = 1;
			while (start + index + step < end &&
			    COL_LESS(start + index + step)) {
				index += step;
				step <<= 1;
			}
			step >>= 1;
			while (step > 0) {
				if (start + index + step < end &&
				    COL_LESS(start + index + step))
					index += step;
				step >>= 1;
			}
			index += 1;
		}
		return index;
	} else {
		size_t limit = DTFL_MIN(end, start + small_limit), i, count = 0;

		for (i = start; i < limit; i++) {
			if (COL_LESS(i))
				count++;
		}
		return count;
	}
#undef COL_LESS
}
#endif /* DTFL_COLUMNAR */

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

#ifdef DTFL_COLUMNAR
	if (s->stride != 0) {
		DTFL_ASSERT(klen == s->stride);
		return slices_advance_col(s, start, end, key);
	}
#endif

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
	__dtfl_vec_push_size(&o->v, val);
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
	void *slot;

	if (b->tw == 1)
		__dtfl_vec_push_u64(&b->stash_t, time[0]);
	else {
		slot = __dtfl_vec_push(&b->stash_t, NULL);
		if (b->tw != 0)
			memcpy(slot, time, (size_t)b->tw * sizeof(uint64_t));
	}
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

	/*
	 * Sort stash indices by time (stable insertion sort).
	 * shortcut: O(n^2) in the updates of one (key, val), fine while a
	 * value's history is short; switch to a merge sort if a hot key's
	 * history grows to thousands of times (merges re-seal it each time).
	 */
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

	/*
	 * Apply the permutation, as consolidate_slice sorts the stash itself:
	 * the in-place compaction below then reads slot i >= out, so it never
	 * reads a slot it has already overwritten.
	 */
	{
		__dtfl_vec st, sd;

		__dtfl_vec_init(&st, b->stash_t.elem);
		__dtfl_vec_init(&sd, b->stash_d.elem);
		for (i = 0; i < n; i++) {
			__dtfl_vec_push(&st, __dtfl_vec_at(&b->stash_t,
			    order[i]));
			__dtfl_vec_push(&sd, __dtfl_vec_at(&b->stash_d,
			    order[i]));
		}
		__dtfl_vec_swap(&b->stash_t, &st);
		__dtfl_vec_swap(&b->stash_d, &sd);
		__dtfl_vec_free(&st);
		__dtfl_vec_free(&sd);
	}
	__dtfl_free(order);

	/* Consolidate equal-time runs in place, compacting to the front. */
	accum = __dtfl_alloc(DTFL_MAX(b->dw, 1u));
	for (i = 0; i < n; i = k) {
		if (b->dw != 0)
			memcpy(accum, stash_diff(b, i), b->dw);
		for (k = i + 1; k < n && stash_cmp(b, i, k) == 0; k++)
			b->dc->plus_equals(b->dc, accum, stash_diff(b, k));
		if (b->dc->is_zero(b->dc, accum))
			continue;
		/* Move survivor to slot `out` (out <= i). */
		if (b->tw != 0)
			memmove(stash_time(b, out), stash_time(b, i),
			    (size_t)b->tw * sizeof(uint64_t));
		if (b->dw != 0)
			memcpy(stash_diff(b, out), accum, b->dw);
		out++;
	}
	__dtfl_free(accum);

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
		void *ts, *ds;

		if (u->tw == 1)
			__dtfl_vec_push_u64(&u->times, stash_time(b, i)[0]);
		else {
			ts = __dtfl_vec_push(&u->times, NULL);
			if (u->tw != 0)
				memcpy(ts, stash_time(b, i),
				    (size_t)u->tw * sizeof(uint64_t));
		}
		ds = __dtfl_vec_push(&u->diffs, NULL);
		if (u->dw != 0)
			memcpy(ds, stash_diff(b, i), u->dw);
		u->nupds++;
	}
	stash_clear(b);
	__dtfl_offsets_push(&u->offs, u->nupds);
	return true;
}
