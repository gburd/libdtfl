/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/timely_log.c: timely event logging.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/logging.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Events are logged to the worker's "timely" logger as the bincode-1
 * fixint little-endian encoding of TimelyEvent (PLAN D9), so a log
 * stream written by libdtfl decodes with the pinned Rust types and vice
 * versa.  Enum variants are u32 tags in declaration order; usize is u64;
 * bool and the Option tag are one byte; String and Vec are a u64 length
 * then elements; Duration is u64 seconds then u32 nanoseconds.
 */
#include "timely/worker_int.h"

#include "timely/log_codec_int.h"

enum timely_event_tag {
	TE_OPERATES = 0,
	TE_CHANNELS = 1,
	TE_PUSH_PROGRESS = 2,
	TE_MESSAGES = 3,
	TE_SCHEDULE = 4,
	TE_SHUTDOWN = 5,
	TE_COMM_CHANNELS = 6,
	TE_PARK = 7,
	TE_TEXT = 8
};

void
__dtfl_bc_u8(__dtfl_vec *o, uint8_t v)
{
	__dtfl_vec_push(o, &v);
}

void
__dtfl_bc_u32(__dtfl_vec *o, uint32_t v)
{
	unsigned char b[4];
	int i;

	for (i = 0; i < 4; i++)
		b[i] = (unsigned char)(v >> (8 * i));
	__dtfl_vec_push_n(o, b, 4);
}

void
__dtfl_bc_u64(__dtfl_vec *o, uint64_t v)
{
	unsigned char b[8];
	int i;

	for (i = 0; i < 8; i++)
		b[i] = (unsigned char)(v >> (8 * i));
	__dtfl_vec_push_n(o, b, 8);
}

void
__dtfl_bc_str(__dtfl_vec *o, const char *s)
{
	size_t n = strlen(s);

	__dtfl_bc_u64(o, n);
	__dtfl_vec_push_n(o, s, n);
}

void
__dtfl_bc_usizes(__dtfl_vec *o, const size_t *v, size_t n)
{
	size_t i;

	__dtfl_bc_u64(o, n);
	for (i = 0; i < n; i++)
		__dtfl_bc_u64(o, v[i]);
}

void
__dtfl_bc_duration(__dtfl_vec *o, uint64_t ns)
{
	__dtfl_bc_u64(o, ns / UINT64_C(1000000000));
	__dtfl_bc_u32(o, (uint32_t)(ns % UINT64_C(1000000000)));
}

void
__dtfl_bc_reader_init(__dtfl_bc_reader *r, const void *data, size_t len)
{
	r->p = data;
	r->len = len;
	r->off = 0;
	r->bad = false;
}

/* Returns false and sets bad if `n` more bytes are not available. */
static bool
bc_want(__dtfl_bc_reader *r, size_t n)
{
	if (r->bad || n > r->len - r->off) {
		r->bad = true;
		return false;
	}
	return true;
}

uint8_t
__dtfl_bc_read_u8(__dtfl_bc_reader *r)
{
	if (!bc_want(r, 1))
		return 0;
	return r->p[r->off++];
}

uint32_t
__dtfl_bc_read_u32(__dtfl_bc_reader *r)
{
	uint32_t v = 0;
	int i;

	if (!bc_want(r, 4))
		return 0;
	for (i = 0; i < 4; i++)
		v |= (uint32_t)r->p[r->off + (size_t)i] << (8 * i);
	r->off += 4;
	return v;
}

uint64_t
__dtfl_bc_read_u64(__dtfl_bc_reader *r)
{
	uint64_t v = 0;
	int i;

	if (!bc_want(r, 8))
		return 0;
	for (i = 0; i < 8; i++)
		v |= (uint64_t)r->p[r->off + (size_t)i] << (8 * i);
	r->off += 8;
	return v;
}

int64_t
__dtfl_bc_read_i64(__dtfl_bc_reader *r)
{
	return (int64_t)__dtfl_bc_read_u64(r);
}

uint64_t
__dtfl_bc_read_duration(__dtfl_bc_reader *r)
{
	uint64_t secs = __dtfl_bc_read_u64(r);
	uint32_t nanos = __dtfl_bc_read_u32(r);

	return secs * UINT64_C(1000000000) + nanos;
}

const char *
__dtfl_bc_read_str(__dtfl_bc_reader *r, size_t *slen)
{
	uint64_t n = __dtfl_bc_read_u64(r);
	const char *s;

	*slen = 0;
	if (r->bad || n > r->len - r->off) {
		r->bad = true;
		return NULL;
	}
	s = (const char *)(r->p + r->off);
	r->off += (size_t)n;
	*slen = (size_t)n;
	return s;
}

size_t
__dtfl_bc_read_vec_len(__dtfl_bc_reader *r)
{
	return (size_t)__dtfl_bc_read_u64(r);
}

/* Logs the encoded event in `buf` to the "timely" logger, if any. */
static void
emit(__dtfl_worker *w, __dtfl_vec *buf)
{
	__dtfl_logger *l = __dtfl_worker_logger(w, "timely");

	if (l != NULL) {
		__dtfl_logger_log(l, buf->data, buf->len);
		__dtfl_logger_release(l);
	}
	__dtfl_vec_free(buf);
}

static bool
logging(__dtfl_worker *w)
{
	__dtfl_logger *l = __dtfl_worker_logger(w, "timely");

	if (l == NULL)
		return false;
	__dtfl_logger_release(l);
	return true;
}

void
__dtfl_worker_log_operates(__dtfl_worker *w, size_t id, const size_t *addr,
    size_t addr_len, const char *name)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, TE_OPERATES);
	__dtfl_bc_u64(&b, id);
	__dtfl_bc_usizes(&b, addr, addr_len);
	__dtfl_bc_str(&b, name);
	emit(w, &b);
}

void
__dtfl_worker_log_channel(__dtfl_worker *w, size_t id, const size_t *scope,
    size_t scope_len, size_t src_node, size_t src_port, size_t tgt_node,
    size_t tgt_port, const char *typ)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, TE_CHANNELS);
	__dtfl_bc_u64(&b, id);
	__dtfl_bc_usizes(&b, scope, scope_len);
	__dtfl_bc_u64(&b, src_node);
	__dtfl_bc_u64(&b, src_port);
	__dtfl_bc_u64(&b, tgt_node);
	__dtfl_bc_u64(&b, tgt_port);
	__dtfl_bc_str(&b, typ);
	emit(w, &b);
}

void
__dtfl_worker_log_push_progress(__dtfl_worker *w, size_t op_id)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, TE_PUSH_PROGRESS);
	__dtfl_bc_u64(&b, op_id);
	emit(w, &b);
}

void
__dtfl_worker_log_messages(__dtfl_worker *w, bool is_send, size_t channel,
    size_t source, size_t target, size_t seq, int64_t record_count)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, TE_MESSAGES);
	__dtfl_bc_u8(&b, is_send ? 1 : 0);
	__dtfl_bc_u64(&b, channel);
	__dtfl_bc_u64(&b, source);
	__dtfl_bc_u64(&b, target);
	__dtfl_bc_u64(&b, seq);
	__dtfl_bc_u64(&b, (uint64_t)record_count);
	emit(w, &b);
}

void
__dtfl_worker_log_schedule(__dtfl_worker *w, size_t id, bool start)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, TE_SCHEDULE);
	__dtfl_bc_u64(&b, id);
	__dtfl_bc_u32(&b, start ? 0 : 1);
	emit(w, &b);
}

void
__dtfl_worker_log_shutdown(__dtfl_worker *w, size_t id)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, TE_SHUTDOWN);
	__dtfl_bc_u64(&b, id);
	emit(w, &b);
}

void
__dtfl_worker_log_comm_channel(__dtfl_worker *w, size_t id, bool progress)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, TE_COMM_CHANNELS);
	__dtfl_bc_u64(&b, id);
	__dtfl_bc_u32(&b, progress ? 0 : 1);
	emit(w, &b);
}

/* ParkEvent::Park(Option<Duration>) or ParkEvent::Unpark. */
void
__dtfl_worker_log_park(__dtfl_worker *w, bool park, uint64_t timeout_ns)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, TE_PARK);
	if (park) {
		__dtfl_bc_u32(&b, 0);
		if (timeout_ns == UINT64_MAX) {
			__dtfl_bc_u8(&b, 0);
		} else {
			__dtfl_bc_u8(&b, 1);
			__dtfl_bc_duration(&b, timeout_ns);
		}
	} else {
		__dtfl_bc_u32(&b, 1);
	}
	emit(w, &b);
}

void
__dtfl_worker_log_text(__dtfl_worker *w, const char *text)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, TE_TEXT);
	__dtfl_bc_str(&b, text);
	emit(w, &b);
}
