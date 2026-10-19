/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/comm_log.c: communication event logging.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/logging.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * The CommunicationEvent encoder: the bincode-1 fixint little-endian
 * encoding of CommunicationEvent (Message, State, Setup) under the
 * "communication" stream (PLAN D9), so the subscriber API (dtfl/log.h)
 * can decode it with the pinned Rust types.  Enum variants are u32 tags
 * in declaration order; usize is u64; bool and the Option tag are one
 * byte.  A MessageEvent carries the networking MessageHeader, whose six
 * usize fields serialize as six u64 (this is the serde form inside the
 * log record, distinct from the big-endian 48-byte on-the-wire framing
 * in networking.rs that the data plane uses).
 *
 * Emission: as at the pin, the network threads log through their own
 * loggers, which Hooks.log_fn hands each thread at start
 * (__dtfl_comm_log_hook, zc_initialize.c).  Each logger is owned by and
 * used on one network thread, so no logger state is shared across
 * threads.  The send and receive loops (zc_tcp.c) log StateEvent at
 * start and end and a MessageEvent per header; with no hook the logger
 * is NULL and each emit point is one pointer test.  The
 * __dtfl_worker_log_comm_* forms log to a worker's "communication"
 * logger instead.
 */
#include "comm/comm_int.h"
#include "comm/zc_int.h"
#include "comm/init_int.h"

#include "timely/log_codec_int.h"
#include "timely/worker_int.h"

enum comm_event_tag {
	CE_MESSAGE = 0,
	CE_STATE = 1,
	CE_SETUP = 2
};

/* Logs the encoded event in `buf` to `l` and frees the buffer. */
static void
emit(__dtfl_logger *l, __dtfl_vec *buf)
{
	__dtfl_logger_log(l, buf->data, buf->len);
	__dtfl_vec_free(buf);
}

void
__dtfl_comm_log_message(__dtfl_logger *l, bool is_send,
    const __dtfl_message_header *h)
{
	__dtfl_vec b;

	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, CE_MESSAGE);
	__dtfl_bc_u8(&b, is_send ? 1 : 0);
	__dtfl_bc_u64(&b, h->channel);
	__dtfl_bc_u64(&b, h->source);
	__dtfl_bc_u64(&b, h->target_lower);
	__dtfl_bc_u64(&b, h->target_upper);
	__dtfl_bc_u64(&b, h->length);
	__dtfl_bc_u64(&b, h->seqno);
	emit(l, &b);
}

void
__dtfl_comm_log_state(__dtfl_logger *l, bool send, size_t process,
    size_t remote, bool start)
{
	__dtfl_vec b;

	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, CE_STATE);
	__dtfl_bc_u8(&b, send ? 1 : 0);
	__dtfl_bc_u64(&b, process);
	__dtfl_bc_u64(&b, remote);
	__dtfl_bc_u8(&b, start ? 1 : 0);
	emit(l, &b);
}

void
__dtfl_comm_log_setup(__dtfl_logger *l, bool sender, size_t process,
    bool has_remote, size_t remote)
{
	__dtfl_vec b;

	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, CE_SETUP);
	__dtfl_bc_u8(&b, sender ? 1 : 0);
	__dtfl_bc_u64(&b, process);
	if (has_remote) {
		__dtfl_bc_u8(&b, 1);
		__dtfl_bc_u64(&b, remote);
	} else {
		__dtfl_bc_u8(&b, 0);
	}
	emit(l, &b);
}

/*
 * Hooks.log_fn for a "communication" config subscription: a fresh
 * subscriber logger for this network thread.  The subscription's ctx is
 * shared by every network and worker thread (dtfl/log.h: the subscriber
 * synchronizes shared state itself).
 */
__dtfl_logger *
__dtfl_comm_subscriber_log_fn(void *ctx, const __dtfl_comm_setup *setup)
{
	const __dtfl_log_sub *sub = ctx;

	return __dtfl_log_subscriber_logger("communication", setup->process,
	    sub->fn, sub->ctx);
}

/* ------------------------------------------------------------------ */
/* Worker forms: log to the worker's "communication" logger, if bound. */
/* ------------------------------------------------------------------ */

void
__dtfl_worker_log_comm_message(__dtfl_worker *w, bool is_send,
    const __dtfl_message_header *h)
{
	__dtfl_logger *l = __dtfl_worker_logger(w, "communication");

	if (l != NULL) {
		__dtfl_comm_log_message(l, is_send, h);
		__dtfl_logger_release(l);
	}
}

void
__dtfl_worker_log_comm_state(__dtfl_worker *w, bool send, size_t process,
    size_t remote, bool start)
{
	__dtfl_logger *l = __dtfl_worker_logger(w, "communication");

	if (l != NULL) {
		__dtfl_comm_log_state(l, send, process, remote, start);
		__dtfl_logger_release(l);
	}
}

void
__dtfl_worker_log_comm_setup(__dtfl_worker *w, bool sender, size_t process,
    bool has_remote, size_t remote)
{
	__dtfl_logger *l = __dtfl_worker_logger(w, "communication");

	if (l != NULL) {
		__dtfl_comm_log_setup(l, sender, process, has_remote, remote);
		__dtfl_logger_release(l);
	}
}
