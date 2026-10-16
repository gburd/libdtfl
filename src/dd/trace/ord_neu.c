/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/implementations/ord_neu.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/ord_neu.c: the ordered batch's Builder and Merger.  The
 * Builder turns a sorted, consolidated chain of (key, val, time, diff)
 * updates into the layered trie (keys -> vals -> updates), applying the
 * singleton update optimization through UpdsBuilder.  The Merger fuels a
 * key-by-key merge of two batches, advancing copied times by the merged
 * description's since and dropping values and keys whose updates cancel.
 * A key batch (OrdKeyBatch) is the value-less specialization: no value
 * layer, updates indexed by key.
 */
#include "dd/trace/trace_int.h"

/* ------------------------------------------------------------------ */
/* Builder                                                             */
/* ------------------------------------------------------------------ */

void
__dtfl_builder_ord_init(__dtfl_builder_ord *bld, bool keyed,
    const dtfl_type *kt, const dtfl_type *vt, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc, size_t keys, size_t vals, size_t upds)
{
	(void)keys;
	(void)vals;
	(void)upds;
	bld->result.keyed = keyed;
	__dtfl_slices_init(&bld->result.keys);
	__dtfl_slices_set_stride(&bld->result.keys,
	    __dtfl_slices_stride_for(kt));
	__dtfl_vals_init(&bld->result.vals);
	__dtfl_upds_init(&bld->result.upds, dtfl_time_depth(tc),
	    (unsigned)dc->width);
	__dtfl_upds_builder_init(&bld->staging, tc, dc);
	bld->have_last = false;
	bld->kt = kt;
	bld->vt = vt;
	bld->tc = tc;
	bld->dc = dc;
}

/* The last key / value pushed into the result, for the continuation test. */
static bool
last_key_eq(const __dtfl_builder_ord *bld, const void *key, size_t klen)
{
	size_t n = __dtfl_slices_len(&bld->result.keys), len;
	const void *k;

	if (n == 0)
		return false;
	k = __dtfl_slices_get(&bld->result.keys, n - 1, &len);
	return dtfl_type_cmp(bld->kt, k, len, key, klen) == 0;
}

static bool
last_val_eq(const __dtfl_builder_ord *bld, const void *val, size_t vlen)
{
	size_t n = __dtfl_slices_len(&bld->result.vals.vals), len;
	const void *v;

	if (n == 0)
		return false;
	v = __dtfl_slices_get(&bld->result.vals.vals, n - 1, &len);
	return dtfl_type_cmp(bld->vt, v, len, val, vlen) == 0;
}

/*
 * OrdValBuilder::push / OrdKeyBuilder::push for one update.  Mirrors the
 * four cases: first update, continuation of the current value, new value
 * for the current key, new key.
 */
void
__dtfl_builder_ord_push(__dtfl_builder_ord *bld, const void *key, size_t klen,
    const void *val, size_t vlen, const uint64_t *time, const void *diff)
{
	__dtfl_ord_storage *r = &bld->result;

	if (bld->result.keyed) {
		if (!bld->have_last) {
			__dtfl_slices_push(&r->keys, key, klen);
			__dtfl_upds_builder_push(&bld->staging, time, diff);
			bld->have_last = true;
		} else if (last_key_eq(bld, key, klen)) {
			__dtfl_upds_builder_push(&bld->staging, time, diff);
		} else {
			__dtfl_upds_builder_seal(&bld->staging, &r->upds);
			__dtfl_upds_builder_push(&bld->staging, time, diff);
			__dtfl_slices_push(&r->keys, key, klen);
		}
		return;
	}

	if (!bld->have_last) {
		__dtfl_slices_push(&r->vals.vals, val, vlen);
		__dtfl_slices_push(&r->keys, key, klen);
		__dtfl_upds_builder_push(&bld->staging, time, diff);
		bld->have_last = true;
	} else if (last_key_eq(bld, key, klen)) {
		if (last_val_eq(bld, val, vlen)) {
			__dtfl_upds_builder_push(&bld->staging, time, diff);
		} else {
			__dtfl_upds_builder_seal(&bld->staging, &r->upds);
			__dtfl_upds_builder_push(&bld->staging, time, diff);
			__dtfl_slices_push(&r->vals.vals, val, vlen);
		}
	} else {
		__dtfl_upds_builder_seal(&bld->staging, &r->upds);
		__dtfl_upds_builder_push(&bld->staging, time, diff);
		__dtfl_offsets_push(&r->vals.offs,
		    __dtfl_slices_len(&r->vals.vals));
		__dtfl_slices_push(&r->vals.vals, val, vlen);
		__dtfl_slices_push(&r->keys, key, klen);
	}
}

__dtfl_batch_ord *
__dtfl_builder_ord_done(__dtfl_builder_ord *bld, __dtfl_description *desc)
{
	__dtfl_batch_ord *b = __dtfl_alloc(sizeof *b);

	__dtfl_upds_builder_seal(&bld->staging, &bld->result.upds);
	if (!bld->result.keyed)
		__dtfl_offsets_push(&bld->result.vals.offs,
		    __dtfl_slices_len(&bld->result.vals.vals));

	__dtfl_arc_init(&b->ref);
	b->storage = bld->result;	/* move */
	b->updates = bld->staging.total;
	b->kt = bld->kt;
	b->vt = bld->vt;
	b->tc = bld->tc;
	b->dc = bld->dc;
	b->description = *desc;		/* move */
	memset(desc, 0, sizeof *desc);
	__dtfl_upds_builder_free(&bld->staging);
	return b;
}

/*
 * Builder::seal over a chain of consolidated chunks.  Each chunk is a
 * __dtfl_batch of (key||val, time, diff) rows; kw splits key||val.  For a
 * key batch kw is 0 (the whole record is the key).
 */
__dtfl_batch_ord *
__dtfl_builder_ord_seal(bool keyed, const dtfl_type *kt, const dtfl_type *vt,
    const dtfl_time_class *tc, const __dtfl_diff_class *dc,
    __dtfl_batch **chunks, size_t nchunks, __dtfl_description *desc)
{
	__dtfl_builder_ord bld;
	size_t ci, i;

	__dtfl_builder_ord_init(&bld, keyed, kt, vt, tc, dc, 0, 0, 0);
	for (ci = 0; ci < nchunks; ci++) {
		__dtfl_batch *chunk = chunks[ci];
		size_t n = __dtfl_batch_len(chunk);

		for (i = 0; i < n; i++) {
			size_t len, klen, vlen;
			const void *rec = __dtfl_batch_rec(chunk, i, &len);
			const void *key, *val;

			if (keyed) {
				key = rec;
				klen = len;
				val = "";
				vlen = 0;
			} else {
				/* key || val: split at the key's length. */
				klen = dtfl_type_value_len(kt, rec, len);
				DTFL_ASSERT(klen != SIZE_MAX && klen <= len);
				key = rec;
				val = (const unsigned char *)rec + klen;
				vlen = len - klen;
			}
			__dtfl_builder_ord_push(&bld, key, klen, val, vlen,
			    __dtfl_batch_time(chunk, i),
			    __dtfl_batch_diff(chunk, i));
		}
	}
	return __dtfl_builder_ord_done(&bld, desc);
}

/* ------------------------------------------------------------------ */
/* Merger                                                              */
/* ------------------------------------------------------------------ */

/*
 * Merger::new: since = batch1.since join batch2.since join compaction.
 * Asserts batch1.upper == batch2.lower.
 */
void
__dtfl_merger_ord_new(__dtfl_merger_ord *m, __dtfl_batch_ord *src1,
    __dtfl_batch_ord *src2, const __dtfl_antichain *compaction_frontier)
{
	__dtfl_antichain lower, upper, since;
	const __dtfl_antichain *s1 = __dtfl_description_since(&src1->description);
	const __dtfl_antichain *s2 = __dtfl_description_since(&src2->description);

	DTFL_ASSERT(__dtfl_antichain_eq(__dtfl_batch_ord_upper(src1),
	    __dtfl_batch_ord_lower(src2)));

	/*
	 * since = s1.join(s2).join(compaction_frontier) (lattice.rs
	 * Lattice::join for Antichain: the antichain of the pairwise joins of
	 * every element of one with every element of the other, inserted so
	 * the result stays an antichain).  Join against the whole
	 * compaction_frontier at once; folding its elements in one at a time
	 * would collapse a multi-element frontier to a single joined time
	 * (e.g. {((0,0),3),((0,1),1)} would become {((0,1),3)}), advancing
	 * edit times to the wrong place.
	 */
	__dtfl_antichain_init(&since, src1->tc);
	__dtfl_antichain_join_into(src1->tc, __dtfl_ac_base(s1),
	    __dtfl_antichain_len(s1), __dtfl_ac_base(s2),
	    __dtfl_antichain_len(s2), &since);
	{
		__dtfl_antichain joined;

		__dtfl_antichain_init(&joined, src1->tc);
		__dtfl_antichain_join_into(src1->tc,
		    __dtfl_ac_base(&since), __dtfl_antichain_len(&since),
		    __dtfl_ac_base(compaction_frontier),
		    __dtfl_antichain_len(compaction_frontier), &joined);
		__dtfl_antichain_free(&since);
		since = joined;
	}

	__dtfl_antichain_init(&lower, src1->tc);
	__dtfl_antichain_copy(&lower, __dtfl_batch_ord_lower(src1));
	__dtfl_antichain_init(&upper, src1->tc);
	__dtfl_antichain_copy(&upper, __dtfl_batch_ord_upper(src2));
	__dtfl_description_init(&m->description, &lower, &upper, &since);

	m->key_cursor1 = 0;
	m->key_cursor2 = 0;
	m->result.keyed = src1->storage.keyed;
	__dtfl_slices_init(&m->result.keys);
	__dtfl_slices_set_stride(&m->result.keys,
	    __dtfl_slices_stride_for(src1->kt));
	__dtfl_vals_init(&m->result.vals);
	__dtfl_upds_init(&m->result.upds, dtfl_time_depth(src1->tc),
	    (unsigned)src1->dc->width);
	__dtfl_upds_builder_init(&m->staging, src1->tc, src1->dc);
	m->src1 = __dtfl_batch_ord_retain(src1);
	m->src2 = __dtfl_batch_ord_retain(src2);
	m->keyed = src1->storage.keyed;
	m->kt = src1->kt;
	m->vt = src1->vt;
	m->tc = src1->tc;
	m->dc = src1->dc;
}

/*
 * stash_updates_for_val / _for_key: push every update of the value (or
 * key) at `index` into staging, advanced by the description's since.
 */
static void
stash_updates(__dtfl_merger_ord *m, const __dtfl_ord_storage *src, size_t index)
{
	const __dtfl_antichain *since =
	    __dtfl_description_since(&m->description);
	size_t lo, hi, i;
	unsigned tw = dtfl_time_depth(m->tc);

	__dtfl_upds_bounds(&src->upds, index, &lo, &hi);
	for (i = lo; i < hi; i++) {
		uint64_t t[DTFL_TIME_MAX];

		if (tw != 0)
			memcpy(t, __dtfl_upds_time(&src->upds, i),
			    (size_t)tw * sizeof(uint64_t));
		__dtfl_time_advance_by(m->tc, t, __dtfl_ac_base(since),
		    __dtfl_antichain_len(since));
		__dtfl_upds_builder_push(&m->staging, t,
		    __dtfl_upds_diff(&src->upds, i));
	}
}

/* copy_key for a key batch (no value layer). */
static void
copy_key_keyed(__dtfl_merger_ord *m, const __dtfl_ord_storage *src,
    size_t cursor)
{
	size_t len;
	const void *k;

	stash_updates(m, src, cursor);
	if (__dtfl_upds_builder_seal(&m->staging, &m->result.upds)) {
		k = __dtfl_slices_get(&src->keys, cursor, &len);
		__dtfl_slices_push(&m->result.keys, k, len);
	}
}

/*
 * merge_vals: merge two value ranges into the result, returning the new
 * upper val offset if non-empty (Some(off)), or SIZE_MAX for None.
 */
static size_t
merge_vals(__dtfl_merger_ord *m, const __dtfl_ord_storage *s1, size_t lo1,
    size_t hi1, const __dtfl_ord_storage *s2, size_t lo2, size_t hi2)
{
	size_t init = __dtfl_slices_len(&m->result.vals.vals);

	while (lo1 < hi1 && lo2 < hi2) {
		size_t l1, l2;
		const void *v1 = __dtfl_slices_get(&s1->vals.vals, lo1, &l1);
		const void *v2 = __dtfl_slices_get(&s2->vals.vals, lo2, &l2);
		int r = dtfl_type_cmp(m->vt, v1, l1, v2, l2);

		if (r < 0) {
			stash_updates(m, s1, lo1);
			if (__dtfl_upds_builder_seal(&m->staging,
			    &m->result.upds))
				__dtfl_slices_push(&m->result.vals.vals, v1, l1);
			lo1++;
		} else if (r == 0) {
			stash_updates(m, s1, lo1);
			stash_updates(m, s2, lo2);
			if (__dtfl_upds_builder_seal(&m->staging,
			    &m->result.upds))
				__dtfl_slices_push(&m->result.vals.vals, v1, l1);
			lo1++;
			lo2++;
		} else {
			stash_updates(m, s2, lo2);
			if (__dtfl_upds_builder_seal(&m->staging,
			    &m->result.upds))
				__dtfl_slices_push(&m->result.vals.vals, v2, l2);
			lo2++;
		}
	}
	while (lo1 < hi1) {
		size_t l1;
		const void *v1 = __dtfl_slices_get(&s1->vals.vals, lo1, &l1);

		stash_updates(m, s1, lo1);
		if (__dtfl_upds_builder_seal(&m->staging, &m->result.upds))
			__dtfl_slices_push(&m->result.vals.vals, v1, l1);
		lo1++;
	}
	while (lo2 < hi2) {
		size_t l2;
		const void *v2 = __dtfl_slices_get(&s2->vals.vals, lo2, &l2);

		stash_updates(m, s2, lo2);
		if (__dtfl_upds_builder_seal(&m->staging, &m->result.upds))
			__dtfl_slices_push(&m->result.vals.vals, v2, l2);
		lo2++;
	}
	if (__dtfl_slices_len(&m->result.vals.vals) > init)
		return __dtfl_slices_len(&m->result.vals.vals);
	return SIZE_MAX;
}

/* copy_key for a value batch: copy every value of the key, then the key. */
static void
copy_key_val(__dtfl_merger_ord *m, const __dtfl_ord_storage *src, size_t cursor)
{
	size_t init = __dtfl_slices_len(&m->result.vals.vals);
	size_t lo, hi;

	__dtfl_vals_bounds(&src->vals, cursor, &lo, &hi);
	while (lo < hi) {
		size_t vlen;
		const void *v = __dtfl_slices_get(&src->vals.vals, lo, &vlen);

		stash_updates(m, src, lo);
		if (__dtfl_upds_builder_seal(&m->staging, &m->result.upds))
			__dtfl_slices_push(&m->result.vals.vals, v, vlen);
		lo++;
	}
	if (__dtfl_slices_len(&m->result.vals.vals) > init) {
		size_t klen;
		const void *k = __dtfl_slices_get(&src->keys, cursor, &klen);

		__dtfl_slices_push(&m->result.keys, k, klen);
		__dtfl_offsets_push(&m->result.vals.offs,
		    __dtfl_slices_len(&m->result.vals.vals));
	}
}

/* merge_key: merge the current key of each source. */
static void
merge_key(__dtfl_merger_ord *m, const __dtfl_ord_storage *s1,
    const __dtfl_ord_storage *s2)
{
	size_t l1, l2;
	const void *k1 = __dtfl_slices_get(&s1->keys, m->key_cursor1, &l1);
	const void *k2 = __dtfl_slices_get(&s2->keys, m->key_cursor2, &l2);
	int r = dtfl_type_cmp(m->kt, k1, l1, k2, l2);

	if (r < 0) {
		if (m->keyed)
			copy_key_keyed(m, s1, m->key_cursor1);
		else
			copy_key_val(m, s1, m->key_cursor1);
		m->key_cursor1++;
	} else if (r == 0) {
		if (m->keyed) {
			stash_updates(m, s1, m->key_cursor1);
			stash_updates(m, s2, m->key_cursor2);
			if (__dtfl_upds_builder_seal(&m->staging,
			    &m->result.upds))
				__dtfl_slices_push(&m->result.keys, k1, l1);
		} else {
			size_t lo1, hi1, lo2, hi2, off;

			__dtfl_vals_bounds(&s1->vals, m->key_cursor1, &lo1,
			    &hi1);
			__dtfl_vals_bounds(&s2->vals, m->key_cursor2, &lo2,
			    &hi2);
			off = merge_vals(m, s1, lo1, hi1, s2, lo2, hi2);
			if (off != SIZE_MAX) {
				__dtfl_slices_push(&m->result.keys, k1, l1);
				__dtfl_offsets_push(&m->result.vals.offs, off);
			}
		}
		m->key_cursor1++;
		m->key_cursor2++;
	} else {
		if (m->keyed)
			copy_key_keyed(m, s2, m->key_cursor2);
		else
			copy_key_val(m, s2, m->key_cursor2);
		m->key_cursor2++;
	}
}

static void
copy_key(__dtfl_merger_ord *m, const __dtfl_ord_storage *src, size_t cursor)
{
	if (m->keyed)
		copy_key_keyed(m, src, cursor);
	else
		copy_key_val(m, src, cursor);
}

/*
 * Merger::work: single-key merges while both sources have keys and fuel
 * remains, then key-by-key copies.  Effort is the growth of staging.total.
 */
void
__dtfl_merger_ord_work(__dtfl_merger_ord *m, int64_t *fuel)
{
	const __dtfl_ord_storage *s1 = &m->src1->storage;
	const __dtfl_ord_storage *s2 = &m->src2->storage;
	size_t n1 = __dtfl_slices_len(&s1->keys);
	size_t n2 = __dtfl_slices_len(&s2->keys);
	size_t starting = m->staging.total;
	int64_t effort = 0;

	while (m->key_cursor1 < n1 && m->key_cursor2 < n2 && effort < *fuel) {
		merge_key(m, s1, s2);
		effort = (int64_t)(m->staging.total - starting);
	}
	while (m->key_cursor1 < n1 && effort < *fuel) {
		copy_key(m, s1, m->key_cursor1);
		m->key_cursor1++;
		effort = (int64_t)(m->staging.total - starting);
	}
	while (m->key_cursor2 < n2 && effort < *fuel) {
		copy_key(m, s2, m->key_cursor2);
		m->key_cursor2++;
		effort = (int64_t)(m->staging.total - starting);
	}
	*fuel -= effort;
}

__dtfl_batch_ord *
__dtfl_merger_ord_done(__dtfl_merger_ord *m)
{
	__dtfl_batch_ord *b = __dtfl_alloc(sizeof *b);

	__dtfl_arc_init(&b->ref);
	b->storage = m->result;		/* move */
	b->updates = m->staging.total;
	b->kt = m->kt;
	b->vt = m->vt;
	b->tc = m->tc;
	b->dc = m->dc;
	b->description = m->description;	/* move */
	memset(&m->description, 0, sizeof m->description);
	__dtfl_upds_builder_free(&m->staging);
	__dtfl_batch_ord_release(m->src1);
	__dtfl_batch_ord_release(m->src2);
	return b;
}
