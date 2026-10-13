/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dogs3-v0.25.1
 *   dogsdogsdogs/src/operators/propose.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dogs/propose.c: propose and propose_distinct.
 *
 * propose extends each prefix with every value the arrangement holds under
 * the prefix's key, scaling the prefix diff by the matched count (the
 * number of extensions the relation offers).  propose_distinct does the
 * same but leaves the diff unscaled.  Both are lookup_map with the prefix
 * key as the lookup key and an output record prefix||value.
 */
#include "dogs/dogs_int.h"
#include "dd/common_int.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"

#include <string.h>

typedef struct propose_ctx {
	dtfl_key_selector_fn key;
	void *user;
	void (*user_free)(void *);
	const __dtfl_diff_class *pdc;	/* prefix diff class */
	const __dtfl_diff_class *mdc;	/* matched (trace) diff class */
	const __dtfl_diff_class *odc;	/* output diff class */
	bool scale;			/* true: multiply diff by count */
} propose_ctx;

/* The lookup key for propose is just the key selected from the prefix. */
static void
propose_key(void *vctx, const void *prefix, size_t plen, void *key, size_t cap,
    size_t *klen)
{
	propose_ctx *c = vctx;

	c->key(c->user, prefix, plen, key, cap, klen);
}

/*
 * Output: record = prefix||value; diff = prefix_diff multiplied by the
 * matched count (propose) or the prefix diff unchanged (propose_distinct).
 */
static bool
propose_output(void *vctx, const void *prefix, size_t plen, const void *diff,
    const void *value, size_t vlen, const void *count, void *out_rec,
    size_t cap, size_t *out_len, void *out_diff)
{
	propose_ctx *c = vctx;
	size_t total = plen + vlen;

	if (total > cap)
		return false;
	if (plen != 0)
		memcpy(out_rec, prefix, plen);
	if (vlen != 0)
		memcpy((unsigned char *)out_rec + plen, value, vlen);
	*out_len = total;

	if (c->scale) {
		const __dtfl_diff_class *oc;

		if (__dtfl_diff_multiply(c->pdc, c->mdc, diff, count, out_diff,
		    &oc) != DTFL_OK)
			return false;
	} else {
		if (c->odc->width != 0)
			memcpy(out_diff, diff, c->odc->width);
	}
	return true;
}

static dtfl_collection *
propose_impl(dtfl_collection *prefixes, dtfl_arranged *arr,
    const dtfl_type *out_type, dtfl_key_selector_fn key, dtfl_ctx ctx,
    bool scale)
{
	const __dtfl_diff_class *pdc = prefixes->diff->cls;
	const __dtfl_diff_class *mdc = __dtfl_arranged_diff_class(arr);
	const __dtfl_diff_class *odc = scale
	    ? __dtfl_diff_multiply_class(pdc, mdc) : pdc;
	propose_ctx *c;

	if (odc == NULL) {
		(void)__dtfl_err(DTFL_EALGEBRA,
		    "propose: no multiply for %s * %s", pdc->name, mdc->name);
		return NULL;
	}
	c = __dtfl_alloc(sizeof *c);
	c->key = key;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	c->pdc = pdc;
	c->mdc = mdc;
	c->odc = odc;
	c->scale = scale;
	return __dtfl_dogs_lookup_map(prefixes, arr, out_type, odc,
	    propose_key, c, __dtfl_free, propose_output, c, NULL);
}

dtfl_collection *
dtfl_dogs_propose(dtfl_collection *prefixes, dtfl_arranged *arr,
    const dtfl_type *out_type, dtfl_key_selector_fn key, dtfl_ctx ctx)
{
	return propose_impl(prefixes, arr, out_type, key, ctx, true);
}

dtfl_collection *
dtfl_dogs_propose_distinct(dtfl_collection *prefixes, dtfl_arranged *arr,
    const dtfl_type *out_type, dtfl_key_selector_fn key, dtfl_ctx ctx)
{
	return propose_impl(prefixes, arr, out_type, key, ctx, false);
}
