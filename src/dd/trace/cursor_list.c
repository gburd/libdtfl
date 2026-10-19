/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/cursor/cursor_list.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/cursor_list.c: a cursor over a sequence of batches, merging
 * their per-batch cursors (cursor_list::CursorList).  It tracks the indices
 * of cursors at the minimum key, and among those the indices at the minimum
 * value, so navigation and map_times fire across every batch holding the
 * current (key, val).  Keys and values compare through their dtfl_type.
 */
#include "dd/trace/trace_int.h"

/*
 * min_key / min_val hold at most n indices, so pushing into a vec sized n
 * (cursor_list_init reserves it) needs no growth check.
 */
static inline void
push_index(__dtfl_vec *v, size_t idx)
{
	((size_t *)(void *)v->data)[v->len++] = idx;
}

/*
 * minimize_vals: among the min-key cursors, find those whose current value
 * is the minimum.  Equal-key cursors with no value are skipped.
 */
static void
minimize_vals(__dtfl_cursor_list *cl)
{
	const void *min = NULL;
	size_t minlen = 0, i;
	bool have_min = false;

	__dtfl_vec_clear(&cl->min_val);
	if (cl->all_keyed) {
		/*
		 * A key batch's one value is the unit, and units compare
		 * equal, so every min-key cursor whose value is valid ties
		 * at the minimum.
		 */
		for (i = 0; i < cl->min_key.len; i++) {
			size_t idx = ((size_t *)(void *)cl->min_key.data)[i];

			if (__dtfl_cursor_val_valid(&cl->cursors[idx],
			    cl->batches[idx]))
				push_index(&cl->min_val, idx);
		}
		return;
	}
	for (i = 0; i < cl->min_key.len; i++) {
		size_t idx = *DTFL_VEC_AT(&cl->min_key, size_t, i);
		const void *val;
		size_t vlen;
		int r;

		if (!__dtfl_cursor_val_valid(&cl->cursors[idx], cl->batches[idx]))
			continue;
		val = __dtfl_cursor_val(&cl->cursors[idx], cl->batches[idx],
		    &vlen);
		if (!have_min) {
			__dtfl_vec_push(&cl->min_val, &idx);
			min = val;
			minlen = vlen;
			have_min = true;
			continue;
		}
		r = __dtfl_type_cmp(cl->vt, val, vlen, min, minlen);
		if (r < 0) {
			__dtfl_vec_clear(&cl->min_val);
			__dtfl_vec_push(&cl->min_val, &idx);
			min = val;
			minlen = vlen;
		} else if (r == 0) {
			__dtfl_vec_push(&cl->min_val, &idx);
		}
	}
}

/*
 * minimize_keys for a memcmp-ordered key type when every batch's keys are
 * 8 bytes (all8): the same scan, comparing keys as big-endian words, so
 * the order and ties are cmp_raw's.  Then the key-batch minimize_vals:
 * every min-key cursor whose single unit value is not yet stepped.
 */
static bool
minimize_keys_all8(__dtfl_cursor_list *cl)
{
	uint64_t min = 0;
	size_t i;
	bool have_min = false;

	for (i = 0; i < cl->n; i++)
		if (!cl->batches[i]->storage.keys.all8 ||
		    cl->batches[i]->storage.keys.stride != 0)
			return false;
	cl->min_key.len = 0;
	for (i = 0; i < cl->n; i++) {
		const __dtfl_slices *ks = &cl->batches[i]->storage.keys;
		size_t kc = cl->cursors[i].key_cursor;
		uint64_t k;

		if (kc >= ks->arena.len / 8)
			continue;
		memcpy(&k, ks->arena.data + 8 * kc, 8);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		k = __builtin_bswap64(k);
#endif
		if (!have_min || k < min) {
			cl->min_key.len = 0;
			min = k;
			have_min = true;
		} else if (k > min)
			continue;
		push_index(&cl->min_key, i);
	}
	return true;
}

/*
 * minimize_keys: the indices of cursors at the minimum valid key, then
 * minimize_vals().
 */
static void
minimize_keys(__dtfl_cursor_list *cl)
{
	const void *min = NULL;
	size_t minlen = 0, i;
	bool have_min = false;

#if defined(__BYTE_ORDER__) && (defined(__GNUC__) || defined(__clang__))
	if (cl->kt->cmp == NULL && minimize_keys_all8(cl)) {
		minimize_vals(cl);
		return;
	}
#endif
	__dtfl_vec_clear(&cl->min_key);
	for (i = 0; i < cl->n; i++) {
		const void *key;
		size_t klen;
		int r;

		if (!__dtfl_cursor_key_valid(&cl->cursors[i], cl->batches[i]))
			continue;
		key = __dtfl_cursor_key(&cl->cursors[i], cl->batches[i], &klen);
		if (!have_min) {
			__dtfl_vec_push(&cl->min_key, &i);
			min = key;
			minlen = klen;
			have_min = true;
			continue;
		}
		r = __dtfl_type_cmp(cl->kt, key, klen, min, minlen);
		if (r < 0) {
			__dtfl_vec_clear(&cl->min_key);
			__dtfl_vec_push(&cl->min_key, &i);
			min = key;
			minlen = klen;
		} else if (r == 0) {
			__dtfl_vec_push(&cl->min_key, &i);
		}
	}
	minimize_vals(cl);
}

void
__dtfl_cursor_list_init(__dtfl_cursor_list *cl, __dtfl_batch_ord **batches,
    size_t n, const dtfl_type *kt, const dtfl_type *vt)
{
	size_t i;

	cl->n = n;
	cl->kt = kt;
	cl->vt = vt;
	cl->batches = n == 0 ? NULL : __dtfl_alloc(n * sizeof *cl->batches);
	cl->cursors = n == 0 ? NULL : __dtfl_alloc(n * sizeof *cl->cursors);
	cl->all_keyed = true;
	for (i = 0; i < n; i++) {
		cl->batches[i] = batches[i];	/* ownership moves in */
		__dtfl_cursor_init(&cl->cursors[i], cl->batches[i]);
		cl->all_keyed = cl->all_keyed && cl->batches[i]->storage.keyed;
	}
	__dtfl_vec_init(&cl->min_key, sizeof(size_t));
	__dtfl_vec_init(&cl->min_val, sizeof(size_t));
	__dtfl_vec_reserve(&cl->min_key, n);
	__dtfl_vec_reserve(&cl->min_val, n);
	minimize_keys(cl);
}

void
__dtfl_cursor_list_free(__dtfl_cursor_list *cl)
{
	size_t i;

	for (i = 0; i < cl->n; i++)
		__dtfl_batch_ord_release(cl->batches[i]);
	__dtfl_free(cl->batches);
	__dtfl_free(cl->cursors);
	__dtfl_vec_free(&cl->min_key);
	__dtfl_vec_free(&cl->min_val);
}

bool
__dtfl_cursor_list_key_valid(const __dtfl_cursor_list *cl)
{
	return cl->min_key.len != 0;
}

bool
__dtfl_cursor_list_val_valid(const __dtfl_cursor_list *cl)
{
	return cl->min_val.len != 0;
}

const void *
__dtfl_cursor_list_key(const __dtfl_cursor_list *cl, size_t *len)
{
	size_t idx = *DTFL_VEC_AT(&cl->min_key, size_t, 0);

	return __dtfl_cursor_key(&cl->cursors[idx], cl->batches[idx], len);
}

const void *
__dtfl_cursor_list_val(const __dtfl_cursor_list *cl, size_t *len)
{
	size_t idx = *DTFL_VEC_AT(&cl->min_val, size_t, 0);

	return __dtfl_cursor_val(&cl->cursors[idx], cl->batches[idx], len);
}

void
__dtfl_cursor_list_map_times(__dtfl_cursor_list *cl,
    void (*logic)(const uint64_t *, const void *, void *), void *ctx)
{
	size_t i;

	for (i = 0; i < cl->min_val.len; i++) {
		size_t idx = *DTFL_VEC_AT(&cl->min_val, size_t, i);

		__dtfl_cursor_map_times(&cl->cursors[idx], cl->batches[idx],
		    logic, ctx);
	}
}

void
__dtfl_cursor_list_step_key(__dtfl_cursor_list *cl)
{
	size_t i;

	for (i = 0; i < cl->min_key.len; i++) {
		size_t idx = *DTFL_VEC_AT(&cl->min_key, size_t, i);

		__dtfl_cursor_step_key(&cl->cursors[idx], cl->batches[idx]);
	}
	minimize_keys(cl);
}

void
__dtfl_cursor_list_seek_key(__dtfl_cursor_list *cl, const void *key,
    size_t klen)
{
	size_t i;

	for (i = 0; i < cl->n; i++)
		__dtfl_cursor_seek_key(&cl->cursors[i], cl->batches[i], key,
		    klen);
	minimize_keys(cl);
}

void
__dtfl_cursor_list_step_val(__dtfl_cursor_list *cl)
{
	size_t i;

	for (i = 0; i < cl->min_val.len; i++) {
		size_t idx = *DTFL_VEC_AT(&cl->min_val, size_t, i);

		__dtfl_cursor_step_val(&cl->cursors[idx], cl->batches[idx]);
	}
	minimize_vals(cl);
}

void
__dtfl_cursor_list_seek_val(__dtfl_cursor_list *cl, const void *val,
    size_t vlen)
{
	size_t i;

	for (i = 0; i < cl->n; i++)
		__dtfl_cursor_seek_val(&cl->cursors[i], cl->batches[i], val,
		    vlen);
	minimize_vals(cl);
}

void
__dtfl_cursor_list_rewind_keys(__dtfl_cursor_list *cl)
{
	size_t i;

	for (i = 0; i < cl->n; i++)
		__dtfl_cursor_rewind_keys(&cl->cursors[i], cl->batches[i]);
	minimize_keys(cl);
}

void
__dtfl_cursor_list_rewind_vals(__dtfl_cursor_list *cl)
{
	size_t i;

	for (i = 0; i < cl->min_key.len; i++) {
		size_t idx = *DTFL_VEC_AT(&cl->min_key, size_t, i);

		__dtfl_cursor_rewind_vals(&cl->cursors[idx], cl->batches[idx]);
	}
	minimize_vals(cl);
}
