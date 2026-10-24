/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dogs3-v0.25.1
 *   dogsdogsdogs/src/operators/validate.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dogs/validate.c: validate.
 *
 * validate keeps each prefix||value extension whose (key(prefix), value)
 * pair is present in the arrangement (a (key, value) -> unit trace arranged
 * by self).  It is lookup_map with the lookup key the pair key(prefix)||
 * value and the output the extension unchanged (upstream closure
 * |(pre,val)| (key_selector(pre), val) for the key, |(pre,val),r| ((pre,
 * val), r) for the output).
 */
#include "dogs/dogs_int.h"
#include "dd/common_int.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"

#include <string.h>

typedef struct validate_ctx {
	dtfl_key_selector_fn key;
	void *user;
	void (*user_free)(void *);
	const dtfl_type *prefix_type;	/* splits prefix||value, retained */
	const dtfl_type *val_type;	/* the extension value type */
	const __dtfl_diff_class *odc;	/* output diff class (prefix diff) */
} validate_ctx;

/*
 * The lookup key is key_selector(prefix) || value.  The extension record is
 * prefix||value; split it with the prefix type to find the value part.
 */
static void
validate_key(void *vctx, const void *ext, size_t elen, void *key, size_t cap,
    size_t *klen)
{
	validate_ctx *c = vctx;
	size_t plen = dtfl_type_value_len(c->prefix_type, ext, elen);
	const unsigned char *val;
	size_t vlen;
	size_t kprefix = 0;

	if (plen == SIZE_MAX || plen > elen)
		plen = elen;
	val = (const unsigned char *)ext + plen;
	vlen = elen - plen;

	c->key(c->user, ext, plen, key, cap, &kprefix);
	if (kprefix + vlen <= cap && vlen != 0)
		memcpy((unsigned char *)key + kprefix, val, vlen);
	*klen = kprefix + vlen;
}

/* Output: the extension unchanged, with the prefix diff unchanged. */
static bool
validate_output(void *vctx, const void *ext, size_t elen, const void *diff,
    const void *value, size_t vlen, const void *count, void *out_rec,
    size_t cap, size_t *out_len, void *out_diff)
{
	validate_ctx *c = vctx;

	(void)value; (void)vlen; (void)count;
	if (elen > cap)
		return false;
	if (elen != 0)
		memcpy(out_rec, ext, elen);
	*out_len = elen;
	if (c->odc->width != 0)
		memcpy(out_diff, diff, c->odc->width);
	return true;
}

static void
validate_free(void *vctx)
{
	validate_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	dtfl_type_free(c->prefix_type);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_dogs_validate(dtfl_collection *extensions, dtfl_arranged *arr,
    const dtfl_type *out_type, const dtfl_type *prefix_type,
    const dtfl_type *val_type, dtfl_key_selector_fn key, dtfl_ctx ctx)
{
	const __dtfl_diff_class *odc = extensions->diff->cls;
	validate_ctx *c = __dtfl_alloc(sizeof *c);

	c->key = key;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	c->prefix_type = dtfl_type_retain(prefix_type);
	c->val_type = val_type;
	c->odc = odc;
	return __dtfl_dogs_lookup_map(extensions, arr, out_type, odc,
	    validate_key, c, validate_free, validate_output, c, NULL);
}
