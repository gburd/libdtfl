/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dogs3-v0.25.1
 *   dogsdogsdogs/src/operators/count.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dogs/count.c: count.
 *
 * count annotates each prefix triple (prefix, count, index) with the fewer
 * of its current count and the number of extensions the arrangement offers
 * under the prefix's key; when the arrangement offers fewer, the triple's
 * index is overwritten with this extender's index.  The generic join uses
 * it to route each prefix to the extender offering the fewest extensions.
 * It is lookup_map with the prefix's key as the lookup key, over a key ->
 * unit trace whose accumulated diff per key is the number of distinct
 * extensions; the output record is prefix||new_count||new_index with the
 * prefix diff unchanged.
 */
#include "dogs/dogs_int.h"
#include "dd/common_int.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"

#include <string.h>

typedef struct count_ctx {
	dtfl_key_selector_fn key;
	void *user;
	void (*user_free)(void *);
	const dtfl_type *prefix_type;	/* splits prefix||count||index */
	const __dtfl_diff_class *mdc;	/* matched (trace) diff class */
	const __dtfl_diff_class *odc;	/* output diff class (prefix diff) */
	uint64_t index;			/* this extender's index */
} count_ctx;

/* The lookup key is key_selector(prefix_part); the triple's count and index
 * follow the prefix as two u64 words. */
static void
count_key(void *vctx, const void *rec, size_t rlen, void *key, size_t cap,
    size_t *klen)
{
	count_ctx *c = vctx;
	size_t plen = dtfl_type_value_len(c->prefix_type, rec, rlen);

	if (plen == SIZE_MAX || plen > rlen)
		plen = rlen >= 2 * sizeof(uint64_t) ? rlen - 2 * sizeof(uint64_t)
		    : 0;
	c->key(c->user, rec, plen, key, cap, klen);
}

/* Read the matched diff as a non-negative count (count_trace is isize). */
static uint64_t
count_as_usize(count_ctx *c, const void *count)
{
	int64_t v = 0;

	/* The count trace diff is signed; a distinct count is non-negative. */
	if (c->mdc->width == sizeof(int64_t))
		memcpy(&v, count, sizeof v);
	else if (c->mdc->width == sizeof(int32_t)) {
		int32_t w = 0;

		memcpy(&w, count, sizeof w);
		v = w;
	}
	return v < 0 ? 0 : (uint64_t)v;
}

/*
 * Output: record = prefix || min(current_count, s) || index-if-s-smaller;
 * diff = the prefix diff unchanged.
 */
static bool
count_output(void *vctx, const void *rec, size_t rlen, const void *diff,
    const void *value, size_t vlen, const void *count, void *out_rec,
    size_t cap, size_t *out_len, void *out_diff)
{
	count_ctx *c = vctx;
	size_t plen;
	uint64_t cur_count, cur_index, s;
	const unsigned char *p = rec;

	(void)value; (void)vlen;
	if (rlen > cap || rlen < 2 * sizeof(uint64_t))
		return false;
	plen = rlen - 2 * sizeof(uint64_t);
	cur_count = dtfl_get_u64(p + plen);
	cur_index = dtfl_get_u64(p + plen + sizeof(uint64_t));
	s = count_as_usize(c, count);

	memcpy(out_rec, rec, plen);
	if (cur_count < s) {
		dtfl_put_u64((unsigned char *)out_rec + plen, cur_count);
		dtfl_put_u64((unsigned char *)out_rec + plen + sizeof(uint64_t),
		    cur_index);
	} else {
		dtfl_put_u64((unsigned char *)out_rec + plen, s);
		dtfl_put_u64((unsigned char *)out_rec + plen + sizeof(uint64_t),
		    c->index);
	}
	*out_len = rlen;
	if (c->odc->width != 0)
		memcpy(out_diff, diff, c->odc->width);
	return true;
}

static void
count_free(void *vctx)
{
	count_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	dtfl_type_free(c->prefix_type);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_dogs_count(dtfl_collection *prefixes, dtfl_arranged *arr,
    const dtfl_type *prefix_type, dtfl_key_selector_fn key, dtfl_ctx ctx,
    uint64_t index)
{
	const __dtfl_diff_class *odc = prefixes->diff->cls;
	count_ctx *c = __dtfl_alloc(sizeof *c);

	c->key = key;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	c->prefix_type = dtfl_type_retain(prefix_type);
	c->mdc = __dtfl_arranged_diff_class(arr);
	c->odc = odc;
	c->index = index;
	/* The output record type equals the input type (prefix||count||index). */
	return __dtfl_dogs_lookup_map(prefixes, arr, prefixes->type, odc,
	    count_key, c, count_free, count_output, c, NULL);
}
