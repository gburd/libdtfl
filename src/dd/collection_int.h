/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/dd/collection_int.h: shared internals of the differential model.
 *
 * A collection update (record, time, diff) travels on a timely stream as
 * one record laid out as record bytes, then the scope's time words
 * (depth u64, native order so word compares and the batch time column
 * match), then the diff bytes.  update_type() builds the dtfl_type that
 * describes this layout and hashes by the record prefix, so exchange
 * routes by the record the way differential dataflow routes by the key.
 */
#ifndef DTFL_DD_COLLECTION_INT_H
#define DTFL_DD_COLLECTION_INT_H

#include "dd/dd_int.h"
#include "dtfl/collection.h"

/* dtfl_diff wraps an internal diff class and reference counts the pair. */
struct dtfl_diff {
	__dtfl_arc ref;
	const __dtfl_diff_class *cls;	/* retained by this handle */
};

/*
 * A collection handle: the timely stream of update records, the record
 * type and diff class it carries, and the scope's time depth (the time
 * words embedded in each update record).  type and diff are retained.
 */
struct dtfl_collection {
	dtfl_stream *stream;		/* owned by the scope */
	const dtfl_type *type;		/* retained */
	dtfl_diff *diff;		/* retained */
	unsigned depth;			/* time words per update */
};

/* Builds a dtfl_diff that wraps `cls` (retains it). */
dtfl_diff *__dtfl_diff_wrap(const __dtfl_diff_class *cls);

/*
 * The dtfl_type for update records of (type, depth, diff width).  The
 * caller frees it with dtfl_type_free; it retains `rec`.
 */
const dtfl_type *__dtfl_update_type(const dtfl_type *rec, unsigned depth,
    size_t dw);

/* Byte offsets inside an update record of total length `len`. */
static inline size_t
__dtfl_update_rec_len(unsigned depth, size_t dw, size_t total)
{
	return total - (size_t)depth * sizeof(uint64_t) - dw;
}

static inline const uint64_t *
__dtfl_update_time(const void *upd, unsigned depth, size_t dw, size_t total)
{
	const unsigned char *p = upd;

	return (const uint64_t *)(const void *)(p +
	    __dtfl_update_rec_len(depth, dw, total));
}

static inline const void *
__dtfl_update_diff(const void *upd, size_t dw, size_t total)
{
	const unsigned char *p = upd;

	return p + (total - dw);
}

/* Wraps an existing stream as a collection (as_collection). */
dtfl_collection *__dtfl_as_collection(dtfl_stream *st, const dtfl_type *rec,
    dtfl_diff *diff, unsigned depth);

#endif /* DTFL_DD_COLLECTION_INT_H */
