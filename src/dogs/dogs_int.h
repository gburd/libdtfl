/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/dogs/dogs_int.h: library-internal delta-query / WCOJ primitives.
 *
 * The dogsdogsdogs operators (count, propose, validate) are thin wrappers
 * over lookup_map, which walks an arranged trace with a cursor for each
 * prefix.  This header declares the shared lookup_map entry point and the
 * key-selector shape the operators and the generic join reuse.  Nothing
 * here is installed or exported.
 */
#ifndef DTFL_DOGS_INT_H
#define DTFL_DOGS_INT_H

#include "dd/arrange_int.h"
#include "dd/collection_int.h"
#include "dd/trace/trace_int.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/dogs.h"

/* The largest key a selector may produce (bounds the staging buffer). */
#define DTFL_DOGS_KEY_MAX 256

/*
 * The lookup_map output callback (operators/lookup_map.rs S closure): for a
 * matched (prefix, diff, value, count) it produces one output (record, diff)
 * pair.  `prefix` is the prefix record bytes (plen long), `diff` is the
 * prefix diff (dc width), `value` is the matched value bytes (vlen long),
 * `count` is the accumulated matched diff at times <= the prefix time (dc
 * width).  The callback writes the output record bytes to `out_rec` (up to
 * `cap`, storing the length in *out_len) and the output diff to `out_diff`
 * (odc width).  Returning false drops the output (a zero diff).
 */
typedef bool (*__dtfl_lookup_output_fn)(void *ctx, const void *prefix,
    size_t plen, const void *diff, const void *value, size_t vlen,
    const void *count, void *out_rec, size_t cap, size_t *out_len,
    void *out_diff);

/*
 * lookup_map (operators/lookup_map.rs): for each prefix in `prefixes`,
 * select a key with `key`, seek it in `arr`, and for every value under that
 * key accumulate the matched diffs at trace times less or equal to the
 * prefix's own time; when the accumulation is non-zero, apply `output_func`
 * and ship (output_record, prefix_time, output_diff).  `out_type` and `odc`
 * describe the output records and their diff class.  The caller still owns
 * `arr`.  Returns a new collection the caller frees.
 */
dtfl_collection *__dtfl_dogs_lookup_map(dtfl_collection *prefixes,
    dtfl_arranged *arr, const dtfl_type *out_type, const __dtfl_diff_class *odc,
    dtfl_key_selector_fn key, void *key_ctx, void (*key_free)(void *),
    __dtfl_lookup_output_fn output_func, void *out_ctx,
    void (*out_free)(void *));

#endif /* DTFL_DOGS_INT_H */
