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

dtfl_message *
__dtfl_message_new(const dtfl_time_class *tc, const uint64_t *time,
    __dtfl_batch *data)
{
	dtfl_message *m = __dtfl_alloc(sizeof *m);

	m->depth = tc->depth;
	__dtfl_time_copy(tc, m->time, time);
	m->data = data;
	m->from = 0;
	m->seq = 0;
	return m;
}

void
__dtfl_message_free(dtfl_message *m)
{
	if (m == NULL)
		return;
	__dtfl_batch_release(m->data);
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

typedef struct msg_class_ctx {
	__dtfl_arc ref;
	const dtfl_time_class *tc;
} msg_class_ctx;

static size_t
msg_encoded_len(const __dtfl_msg_class *mc, const void *msg)
{
	const msg_class_ctx *c = mc->ctx;
	const dtfl_message *m = msg;

	/* from, seq, the time words, then the batch. */
	return 16 + (size_t)c->tc->depth * 8 + __dtfl_batch_encoded_len(m->data);
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
	const msg_class_ctx *c = mc->ctx;
	const dtfl_message *m = msg;
	unsigned i;

	put_u64_le(out, (uint64_t)m->from);
	put_u64_le(out + 8, (uint64_t)m->seq);
	for (i = 0; i < c->tc->depth; i++)
		put_u64_le(out + 16 + (size_t)i * 8, m->time[i]);
	__dtfl_batch_encode(m->data, out + 16 + (size_t)c->tc->depth * 8);
}

static void *
msg_decode(const __dtfl_msg_class *mc, const __dtfl_bytes *b)
{
	const msg_class_ctx *c = mc->ctx;
	size_t header = 16 + (size_t)c->tc->depth * 8;
	dtfl_message *m;
	__dtfl_batch *data;
	unsigned i;

	if (b->len < header)
		return NULL;
	data = __dtfl_batch_decode(b->ptr + header, b->len - header);
	if (data == NULL)
		return NULL;
	m = __dtfl_alloc(sizeof *m);
	m->depth = c->tc->depth;
	m->from = (size_t)get_u64_le(b->ptr);
	m->seq = (size_t)get_u64_le(b->ptr + 8);
	for (i = 0; i < c->tc->depth; i++)
		m->time[i] = get_u64_le(b->ptr + 16 + (size_t)i * 8);
	m->data = data;
	return m;
}

static void *
msg_clone(const __dtfl_msg_class *mc, const void *msg)
{
	const msg_class_ctx *c = mc->ctx;
	const dtfl_message *m = msg;
	dtfl_message *out = __dtfl_alloc(sizeof *out);

	(void)c;
	*out = *m;
	out->data = __dtfl_batch_retain(m->data);	/* share, don't copy (D2) */
	return out;
}

static void
msg_free(const __dtfl_msg_class *mc, void *msg)
{
	(void)mc;
	__dtfl_message_free(msg);
}

__dtfl_msg_class *
__dtfl_message_class_new(const dtfl_time_class *tc)
{
	__dtfl_msg_class *mc = __dtfl_alloc(sizeof *mc);
	msg_class_ctx *c = __dtfl_alloc(sizeof *c);

	__dtfl_arc_init(&c->ref);
	c->tc = dtfl_time_class_retain(tc);
	mc->name = "dtfl_message";
	mc->encoded_len = msg_encoded_len;
	mc->encode = msg_encode;
	mc->decode = msg_decode;
	mc->clone = msg_clone;
	mc->free = msg_free;
	mc->ctx = c;
	return mc;
}

__dtfl_msg_class *
__dtfl_message_class_retain(__dtfl_msg_class *mc)
{
	msg_class_ctx *c = mc->ctx;

	__dtfl_arc_retain(&c->ref);
	return mc;
}

void
__dtfl_message_class_free(__dtfl_msg_class *mc)
{
	msg_class_ctx *c;

	if (mc == NULL)
		return;
	c = mc->ctx;
	if (!__dtfl_arc_release(&c->ref))
		return;
	dtfl_time_class_free(c->tc);
	__dtfl_free(c);
	__dtfl_free(mc);
}
