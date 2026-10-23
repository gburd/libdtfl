/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/message.c: timestamped messages on a channel.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/channels/mod.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A Message<T, C> becomes a dtfl_message: a time (depth words of the
 * scope's class), a refcounted dtfl_batch (the container), and the
 * from / seq fields the pusher stamps.  Records are already bytes, so
 * serialization is from (u64 le), seq (u64 le), the time words, then the
 * batch encoding, which replaces upstream's bincode of T and the
 * ContainerBytes of C.  The time words are 8-aligned, so no slop is
 * needed between the header and the batch.
 */
#include "timely/timely_int.h"

/* Fills a fresh block; every field is set, as a recycled block is stale. */
static dtfl_message *
message_init(dtfl_message *m, uintptr_t pool, const dtfl_time_class *tc,
    const uint64_t *time, __dtfl_batch *data)
{
	m->depth = tc->depth;
	__dtfl_time_copy(tc, m->time, time);
	m->data = data;
	m->from = 0;
	m->seq = 0;
	m->pool = pool;
	return m;
}

dtfl_message *
__dtfl_message_new(const dtfl_time_class *tc, const uint64_t *time,
    __dtfl_batch *data)
{
	return message_init(__dtfl_alloc(sizeof(dtfl_message)), 0, tc, time,
	    data);
}

/*
 * Message::new for a message sent per container: upstream moves it
 * through the channel by value, so it costs no allocation there; here
 * the block comes from the sending worker's free list.
 */
dtfl_message *
__dtfl_message_new_pooled(__dtfl_freelist *pool, const dtfl_time_class *tc,
    const uint64_t *time, __dtfl_batch *data)
{
	return message_init(__dtfl_freelist_get(pool), (uintptr_t)pool, tc,
	    time, data);
}

void
__dtfl_message_free(dtfl_message *m)
{
	if (m == NULL)
		return;
	__dtfl_batch_release(m->data);
	__dtfl_free(m);
}

void
__dtfl_message_free_local(dtfl_message *m, __dtfl_freelist *pool)
{
	if (m == NULL)
		return;
	__dtfl_batch_release(m->data);
	if (m->pool == (uintptr_t)pool)
		__dtfl_freelist_put(pool, m);
	else
		__dtfl_free(m);
}

/* ------------------------------------------------------------------ */
/* Read accessors (timely.h)                                           */
/* ------------------------------------------------------------------ */

const uint64_t *
dtfl_message_time(const dtfl_message *m)
{
	return m->time;
}

size_t
dtfl_message_len(const dtfl_message *m)
{
	return __dtfl_batch_len(m->data);
}

const void *
dtfl_message_record(const dtfl_message *m, size_t i, size_t *len)
{
	return __dtfl_batch_rec(m->data, i, len);
}

/* ------------------------------------------------------------------ */
/* Message class (serialization for exchange)                          */
/* ------------------------------------------------------------------ */

/*
 * One static class serves every data channel, as the progcaster's class
 * does: the time depth is encoded in each message, so decode needs no
 * context and every worker passes the same class pointer (the process
 * allocator keys a channel by that pointer).
 */

static size_t
msg_encoded_len(const __dtfl_msg_class *mc, const void *msg)
{
	const dtfl_message *m = msg;

	(void)mc;
	/* from, seq, depth, the time words, then the batch. */
	return 24 + (size_t)m->depth * 8 + __dtfl_batch_encoded_len(m->data);
}

static void
put_u64_le(unsigned char *p, uint64_t v)
{
	size_t i;

	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t
get_u64_le(const unsigned char *p)
{
	uint64_t v = 0;
	size_t i;

	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static void
msg_encode(const __dtfl_msg_class *mc, const void *msg, unsigned char *out)
{
	const dtfl_message *m = msg;
	unsigned i;

	(void)mc;
	put_u64_le(out, (uint64_t)m->from);
	put_u64_le(out + 8, (uint64_t)m->seq);
	put_u64_le(out + 16, (uint64_t)m->depth);
	for (i = 0; i < m->depth; i++)
		put_u64_le(out + 24 + (size_t)i * 8, m->time[i]);
	__dtfl_batch_encode(m->data, out + 24 + (size_t)m->depth * 8);
}

static void *
msg_decode(const __dtfl_msg_class *mc, const __dtfl_bytes *b)
{
	uint64_t depth;
	size_t header;
	dtfl_message *m;
	__dtfl_batch *data;
	unsigned i;

	(void)mc;
	if (b->len < 24)
		return NULL;
	depth = get_u64_le(b->ptr + 16);
	if (depth > DTFL_TIME_MAX)
		return NULL;
	header = 24 + (size_t)depth * 8;
	if (b->len < header)
		return NULL;
	data = __dtfl_batch_decode(b->ptr + header, b->len - header);
	if (data == NULL)
		return NULL;
	m = __dtfl_alloc(sizeof *m);
	m->pool = 0;
	m->depth = (unsigned)depth;
	m->from = (size_t)get_u64_le(b->ptr);
	m->seq = (size_t)get_u64_le(b->ptr + 8);
	for (i = 0; i < m->depth; i++)
		m->time[i] = get_u64_le(b->ptr + 24 + (size_t)i * 8);
	m->data = data;
	return m;
}

static void *
msg_clone(const __dtfl_msg_class *mc, const void *msg)
{
	const dtfl_message *m = msg;
	dtfl_message *out = __dtfl_alloc(sizeof *out);

	(void)mc;
	*out = *m;
	out->pool = 0;	/* a hook allocation, not a block of m's list */
	out->data = __dtfl_batch_retain(m->data);	/* share, don't copy (D2) */
	return out;
}

static void
msg_free(const __dtfl_msg_class *mc, void *msg)
{
	(void)mc;
	__dtfl_message_free(msg);
}

static const __dtfl_msg_class message_class = {
	"dtfl_message", msg_encoded_len, msg_encode, msg_decode, msg_clone,
	msg_free, NULL
};

const __dtfl_msg_class *
__dtfl_message_class(void)
{
	return &message_class;
}
