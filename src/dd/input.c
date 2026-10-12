/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/input.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/input.c: the differential InputSession.
 *
 * An input session batches collection updates and ships them with
 * coarsened timely capabilities.  It buffers (record, time, diff)
 * triples at the current epoch (or a future time for update_at), and
 * flush drives them into a timely input (ops_input.c) under the session
 * epoch's capability and advances the input frontier.  to_collection
 * registers the input in a scope and wraps the resulting stream as a
 * collection (input.rs new_collection / to_collection).
 *
 * The buffered updates are rows of a __dtfl_batch laid out exactly like
 * the update records the collection operators consume (collection_int.h:
 * record bytes, time words, diff bytes), so flush sends each row
 * verbatim and no reserialization is needed.
 */
#include "dd/collection_int.h"

struct dtfl_input_session {
	const dtfl_type *type;		/* record type; retained */
	dtfl_diff *diff;		/* retained */
	const dtfl_type *update_type;	/* update record type; retained */
	dtfl_input *input;		/* timely input over update_type; owned */
	const dtfl_time_class *tc;	/* set at to_collection; not retained */
	unsigned depth;
	size_t dw;
	uint64_t epoch[DTFL_TIME_MAX];	/* current logical time */
	bool have_epoch;
	__dtfl_batch *buffer;		/* buffered update rows; owned */
};

dtfl_input_session *
dtfl_input_session_new(const dtfl_type *type, const dtfl_diff *diff)
{
	dtfl_input_session *s = __dtfl_calloc(1, sizeof *s);

	s->type = dtfl_type_retain(type);
	s->diff = dtfl_diff_retain((dtfl_diff *)diff);
	s->dw = diff->cls->width;
	/* depth and update_type are fixed when the scope's class is known. */
	s->input = NULL;
	s->buffer = NULL;
	s->have_epoch = false;
	return s;
}

dtfl_collection *
dtfl_input_session_to_collection(dtfl_input_session *s, dtfl_scope *scope)
{
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	dtfl_stream *st;

	if (s->input != NULL)
		__dtfl_panic("dtfl_input_session_to_collection: already added");
	s->tc = tc;
	s->depth = dtfl_time_depth(tc);
	s->update_type = __dtfl_update_type(s->type, s->depth, s->dw);
	s->input = dtfl_input_new(s->update_type);
	s->buffer = __dtfl_batch_new(s->depth, (unsigned)s->dw);
	dtfl_time_minimum(tc, s->epoch);
	s->have_epoch = true;

	st = dtfl_input_to_stream(s->input, scope);
	return __dtfl_as_collection(st, s->type, dtfl_diff_retain(s->diff),
	    s->depth);
}

/* push_into: buffer one (record, time, diff) update row. */
static void
buffer_update(dtfl_input_session *s, const void *record, size_t len,
    const uint64_t *time, const void *change)
{
	if (!s->have_epoch)
		__dtfl_panic("dtfl_input_session: no collection yet; call "
		    "dtfl_input_session_to_collection first");
	__dtfl_batch_push(s->buffer, record, len, time,
	    s->dw != 0 ? change : NULL);
}

void
dtfl_input_session_update(dtfl_input_session *s, const void *record, size_t len,
    const void *change)
{
	buffer_update(s, record, len, s->epoch, change);
}

void
dtfl_input_session_update_at(dtfl_input_session *s, const void *record,
    size_t len, const uint64_t *at, const void *change)
{
	if (!s->have_epoch)
		__dtfl_panic("dtfl_input_session_update_at: no collection yet");
	if (!dtfl_time_less_equal(s->tc, s->epoch, at))
		__dtfl_panic("dtfl_input_session_update_at: time precedes the "
		    "current epoch");
	buffer_update(s, record, len, at, change);
}

void
dtfl_input_session_insert(dtfl_input_session *s, const void *record, size_t len)
{
	unsigned char one[8];

	if (s->diff->cls->one == NULL)
		__dtfl_panic("dtfl_input_session_insert: the diff class has no "
		    "unit value");
	s->diff->cls->one(s->diff->cls, one);
	dtfl_input_session_update(s, record, len, one);
}

void
dtfl_input_session_remove(dtfl_input_session *s, const void *record, size_t len)
{
	unsigned char neg_one[8];

	if (s->diff->cls->one == NULL || s->diff->cls->negate == NULL)
		__dtfl_panic("dtfl_input_session_remove: the diff class is not "
		    "Abelian with a unit value");
	s->diff->cls->one(s->diff->cls, neg_one);
	s->diff->cls->negate(s->diff->cls, neg_one);
	dtfl_input_session_update(s, record, len, neg_one);
}

const uint64_t *
dtfl_input_session_time(const dtfl_input_session *s)
{
	return s->have_epoch ? s->epoch : NULL;
}

void
dtfl_input_session_advance_to(dtfl_input_session *s, const uint64_t *t)
{
	if (!s->have_epoch)
		__dtfl_panic("dtfl_input_session_advance_to: no collection yet");
	if (!dtfl_time_less_equal(s->tc, s->epoch, t))
		__dtfl_panic("dtfl_input_session_advance_to: time does not "
		    "advance the current epoch");
	__dtfl_time_copy(s->tc, s->epoch, t);
}

/*
 * flush: send every buffered update row into the timely input, then
 * advance the input frontier to the session epoch (input.rs flush: the
 * handle sends under its current capability and advances if behind).
 */
void
dtfl_input_session_flush(dtfl_input_session *s)
{
	size_t i, n, scratch_cap = 0;
	unsigned char *scratch = NULL;

	if (!s->have_epoch)
		return;
	n = __dtfl_batch_len(s->buffer);
	for (i = 0; i < n; i++) {
		size_t rlen;
		const void *rec = __dtfl_batch_rec(s->buffer, i, &rlen);
		size_t tw = (size_t)s->depth * sizeof(uint64_t);
		size_t total = rlen + tw + s->dw;

		/* Rebuild the flat update record from the batch columns. */
		if (total > scratch_cap) {
			scratch = __dtfl_realloc(scratch, total);
			scratch_cap = total;
		}
		memcpy(scratch, rec, rlen);
		if (tw != 0)
			memcpy(scratch + rlen, __dtfl_batch_time(s->buffer, i),
			    tw);
		if (s->dw != 0)
			memcpy(scratch + rlen + tw,
			    __dtfl_batch_diff(s->buffer, i), s->dw);
		dtfl_input_send(s->input, scratch, total);
	}
	__dtfl_free(scratch);
	__dtfl_batch_clear(s->buffer);
	dtfl_input_flush(s->input);
	if (dtfl_time_less_than(s->tc, dtfl_input_time(s->input), s->epoch))
		dtfl_input_advance_to(s->input, s->epoch);
}

void
dtfl_input_session_close(dtfl_input_session *s)
{
	if (s == NULL)
		return;
	if (s->input != NULL) {
		dtfl_input_session_flush(s);
		dtfl_input_close(s->input);
		__dtfl_batch_release(s->buffer);
		dtfl_type_free(s->update_type);
	}
	dtfl_type_free(s->type);
	dtfl_diff_free(s->diff);
	__dtfl_free(s);
}
