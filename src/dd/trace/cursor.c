/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/cursor/mod.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/cursor.c: the Cursor over a single ord_neu batch, and the
 * refcounted batch itself.  A cursor navigates keys and, per key, values,
 * and visits the (time, diff) updates of the current value (Cursor's
 * key/val stepping, seek and map_times).  The batch stores a layered trie
 * (keys -> vals -> updates); seek uses the exponential-search advance of
 * the key and value containers.  The batch is the cursor's storage and is
 * shared through an atomic refcount (upstream wraps it in Rc).
 */
#include "dd/trace/trace_int.h"

/* ------------------------------------------------------------------ */
/* Batch lifecycle                                                     */
/* ------------------------------------------------------------------ */

static void
storage_init(__dtfl_ord_storage *s, bool keyed, unsigned tw, unsigned dw)
{
	__dtfl_slices_init(&s->keys);
	__dtfl_vals_init(&s->vals);
	__dtfl_upds_init(&s->upds, tw, dw);
	s->keyed = keyed;
}

static void
storage_free(__dtfl_ord_storage *s)
{
	__dtfl_slices_free(&s->keys);
	__dtfl_vals_free(&s->vals);
	__dtfl_upds_free(&s->upds);
}

static __dtfl_batch_ord *
batch_ord_alloc(bool keyed, const dtfl_type *kt, const dtfl_type *vt,
    const dtfl_time_class *tc, const __dtfl_diff_class *dc)
{
	__dtfl_batch_ord *b = __dtfl_alloc(sizeof *b);

	__dtfl_arc_init(&b->ref);
	storage_init(&b->storage, keyed, dtfl_time_depth(tc),
	    (unsigned)dc->width);
	b->updates = 0;
	b->kt = kt;
	b->vt = vt;
	b->tc = tc;
	b->dc = dc;
	memset(&b->description, 0, sizeof b->description);
	return b;
}

__dtfl_batch_ord *
__dtfl_batch_ord_retain(__dtfl_batch_ord *b)
{
	__dtfl_arc_retain(&b->ref);
	return b;
}

void
__dtfl_batch_ord_release(__dtfl_batch_ord *b)
{
	if (b == NULL || !__dtfl_arc_release(&b->ref))
		return;
	storage_free(&b->storage);
	__dtfl_description_free(&b->description);
	__dtfl_free(b);
}

const __dtfl_antichain *
__dtfl_batch_ord_lower(const __dtfl_batch_ord *b)
{
	return __dtfl_description_lower(&b->description);
}

const __dtfl_antichain *
__dtfl_batch_ord_upper(const __dtfl_batch_ord *b)
{
	return __dtfl_description_upper(&b->description);
}

__dtfl_batch_ord *
__dtfl_batch_ord_empty(bool keyed, const dtfl_type *kt, const dtfl_type *vt,
    const dtfl_time_class *tc, const __dtfl_diff_class *dc,
    __dtfl_antichain *lower, __dtfl_antichain *upper)
{
	__dtfl_batch_ord *b = batch_ord_alloc(keyed, kt, vt, tc, dc);
	__dtfl_antichain since;
	uint64_t min[DTFL_TIME_MAX];

	dtfl_time_minimum(tc, min);
	__dtfl_antichain_init_elem(&since, tc, min);
	__dtfl_description_init(&b->description, lower, upper, &since);
	return b;
}

/* ------------------------------------------------------------------ */
/* Cursor over one batch                                               */
/* ------------------------------------------------------------------ */

/*
 * The value index that bounds the current key's values (bounds().1).  When
 * the key cursor is past the last key (an exhausted cursor, which CursorList
 * can produce by seeking a key a batch does not hold) there is no value
 * range, so report the total value count: val_valid is then false and seek_val
 * is a no-op, matching the upstream cursor's behavior on an exhausted key.
 */
static size_t
val_upper(const __dtfl_cursor *c, const __dtfl_batch_ord *b)
{
	size_t lo, hi;

	if (c->key_cursor >= __dtfl_slices_len(&b->storage.keys))
		return __dtfl_slices_len(&b->storage.vals.vals);
	__dtfl_vals_bounds(&b->storage.vals, c->key_cursor, &lo, &hi);
	return hi;
}

void
__dtfl_cursor_init(__dtfl_cursor *c, const __dtfl_batch_ord *b)
{
	c->key_cursor = 0;
	c->val_cursor = 0;
	c->val_stepped = false;
	__dtfl_cursor_rewind_keys(c, b);
}

bool
__dtfl_cursor_key_valid(const __dtfl_cursor *c, const __dtfl_batch_ord *b)
{
	return c->key_cursor < __dtfl_slices_len(&b->storage.keys);
}

bool
__dtfl_cursor_val_valid(const __dtfl_cursor *c, const __dtfl_batch_ord *b)
{
	if (b->storage.keyed)
		return !c->val_stepped;
	return c->val_cursor < val_upper(c, b);
}

const void *
__dtfl_cursor_key(const __dtfl_cursor *c, const __dtfl_batch_ord *b, size_t *len)
{
	DTFL_ASSERT(__dtfl_cursor_key_valid(c, b));
	return __dtfl_slices_get(&b->storage.keys, c->key_cursor, len);
}

const void *
__dtfl_cursor_val(const __dtfl_cursor *c, const __dtfl_batch_ord *b, size_t *len)
{
	DTFL_ASSERT(__dtfl_cursor_val_valid(c, b));
	if (b->storage.keyed) {
		*len = 0;	/* the unit value */
		return "";
	}
	return __dtfl_slices_get(&b->storage.vals.vals, c->val_cursor, len);
}

void
__dtfl_cursor_map_times(__dtfl_cursor *c, const __dtfl_batch_ord *b,
    void (*logic)(const uint64_t *, const void *, void *), void *ctx)
{
	size_t index = b->storage.keyed ? c->key_cursor : c->val_cursor;
	size_t lo, hi, i;

	__dtfl_upds_bounds(&b->storage.upds, index, &lo, &hi);
	for (i = lo; i < hi; i++)
		logic(__dtfl_upds_time(&b->storage.upds, i),
		    __dtfl_upds_diff(&b->storage.upds, i), ctx);
}

void
__dtfl_cursor_step_key(__dtfl_cursor *c, const __dtfl_batch_ord *b)
{
	c->key_cursor += 1;
	if (__dtfl_cursor_key_valid(c, b))
		__dtfl_cursor_rewind_vals(c, b);
	else
		c->key_cursor = __dtfl_slices_len(&b->storage.keys);
}

void
__dtfl_cursor_seek_key(__dtfl_cursor *c, const __dtfl_batch_ord *b,
    const void *key, size_t klen)
{
	c->key_cursor += __dtfl_slices_advance(&b->storage.keys, b->kt,
	    c->key_cursor, __dtfl_slices_len(&b->storage.keys), key, klen);
	if (__dtfl_cursor_key_valid(c, b))
		__dtfl_cursor_rewind_vals(c, b);
}

void
__dtfl_cursor_step_val(__dtfl_cursor *c, const __dtfl_batch_ord *b)
{
	if (b->storage.keyed) {
		c->val_stepped = true;
		return;
	}
	c->val_cursor += 1;
	if (!__dtfl_cursor_val_valid(c, b))
		c->val_cursor = val_upper(c, b);
}

void
__dtfl_cursor_seek_val(__dtfl_cursor *c, const __dtfl_batch_ord *b,
    const void *val, size_t vlen)
{
	if (b->storage.keyed)
		return;
	c->val_cursor += __dtfl_slices_advance(&b->storage.vals.vals, b->vt,
	    c->val_cursor, val_upper(c, b), val, vlen);
}

void
__dtfl_cursor_rewind_keys(__dtfl_cursor *c, const __dtfl_batch_ord *b)
{
	c->key_cursor = 0;
	if (__dtfl_cursor_key_valid(c, b))
		__dtfl_cursor_rewind_vals(c, b);
}

void
__dtfl_cursor_rewind_vals(__dtfl_cursor *c, const __dtfl_batch_ord *b)
{
	size_t lo, hi;

	if (b->storage.keyed) {
		c->val_stepped = false;
		return;
	}
	__dtfl_vals_bounds(&b->storage.vals, c->key_cursor, &lo, &hi);
	c->val_cursor = lo;
}
