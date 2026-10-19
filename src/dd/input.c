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
 * The buffer is a __dtfl_batch of update records laid out exactly as the
 * collection operators consume them (collection_int.h: record bytes, time
 * words, diff bytes), so it ships to the timely input as one message
 * (Handle::send_batch) with no per-row copy.
 */
#include "dd/collection_int.h"
#include "timely/timely_int.h"

/*
 * InputSession::update reserves 1024 more updates whenever its buffer is
 * full, after shipping it, so the session sends 1024 updates at a time.
 */
#define SESSION_BATCH 1024

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
	__dtfl_batch *buffer;		/* buffered update records; owned */
};

/* A fresh buffer with room for SESSION_BATCH update records. */
static __dtfl_batch *
session_buffer(const dtfl_input_session *s)
{
	__dtfl_batch *b = __dtfl_batch_new(0, 0);
	size_t w = __dtfl_type_const_width(s->type);

	__dtfl_batch_reserve(b, SESSION_BATCH, w == SIZE_MAX ? 0 :
	    SESSION_BATCH * (w + (size_t)s->depth * sizeof(uint64_t) + s->dw));
	return b;
}

/* Handle::send_batch of the buffer; leaves a fresh, empty buffer. */
static void
session_send(dtfl_input_session *s)
{
	if (__dtfl_batch_len(s->buffer) == 0)
		return;
	__dtfl_input_send_batch(s->input, s->buffer);
	s->buffer = session_buffer(s);
}

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
	s->buffer = session_buffer(s);
	dtfl_time_minimum(tc, s->epoch);
	s->have_epoch = true;

	st = dtfl_input_to_stream(s->input, scope);
	return __dtfl_as_collection(st, s->type, dtfl_diff_retain(s->diff),
	    s->depth);
}

/*
 * update / update_at: ship the buffer if it is full, then append one
 * (record, time, diff) update record.
 */
static void
buffer_update(dtfl_input_session *s, const void *record, size_t len,
    const uint64_t *time, const void *change)
{
	size_t tw = (size_t)s->depth * sizeof(uint64_t);
	unsigned char *p;

	if (!s->have_epoch)
		__dtfl_panic("dtfl_input_session: no collection yet; call "
		    "dtfl_input_session_to_collection first");
	if (__dtfl_batch_len(s->buffer) >= SESSION_BATCH)
		session_send(s);
	p = __dtfl_batch_push_uninit(s->buffer, len + tw + s->dw, NULL, NULL);
	if (len == 8 && tw == 8 && s->dw == 8) {
		/* (u64 record, u64 time, i64 diff): three fixed copies. */
		memcpy(p, record, 8);
		memcpy(p + 8, time, 8);
		memcpy(p + 16, change, 8);
		return;
	}
	if (len != 0)
		memcpy(p, record, len);
	if (tw != 0)
		memcpy(p + len, time, tw);
	if (s->dw != 0)
		memcpy(p + len + tw, change, s->dw);
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
 * flush: ship the buffered updates into the timely input as one message,
 * then advance the input frontier to the session epoch (input.rs flush:
 * the handle sends under its current capability and advances if behind).
 */
void
dtfl_input_session_flush(dtfl_input_session *s)
{
	if (!s->have_epoch)
		return;
	session_send(s);
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
