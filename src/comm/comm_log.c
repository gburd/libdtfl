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
 * Emission scope: the events are logged through the worker's
 * "communication" logger, the same registry path the timely logger
 * uses, so a subscriber receives them.  The data-plane send and receive
 * loops (zc_tcp.c) do NOT call these yet: at the pin a comm log sink is
 * handed to the allocators (Hooks.log_fn), and that plumbing from the
 * worker's registry down to the network threads is not modeled (the
 * zc_tcp.c ledger row records the same gap).  So the "communication"
 * stream exists and is byte-faithful and decodable, but is populated
 * only by whatever calls __dtfl_worker_log_comm_* (today: tests and
 * any caller that logs explicitly); the automatic per-message points
 * land when the allocator log sink is plumbed.
 */
#include "comm/comm_int.h"
#include "comm/zc_int.h"

#include "timely/log_codec_int.h"
#include "timely/worker_int.h"

enum comm_event_tag {
	CE_MESSAGE = 0,
	CE_STATE = 1,
	CE_SETUP = 2
};

/* Logs the encoded event in `buf` to the "communication" logger, if any. */
static void
emit(__dtfl_worker *w, __dtfl_vec *buf)
{
	__dtfl_logger *l = __dtfl_worker_logger(w, "communication");

	if (l != NULL) {
		__dtfl_logger_log(l, buf->data, buf->len);
		__dtfl_logger_release(l);
	}
	__dtfl_vec_free(buf);
}

static bool
logging(__dtfl_worker *w)
{
	__dtfl_logger *l = __dtfl_worker_logger(w, "communication");

	if (l == NULL)
		return false;
	__dtfl_logger_release(l);
	return true;
}

/* CommunicationEvent::Message(MessageEvent { is_send, header }). */
void
__dtfl_worker_log_comm_message(__dtfl_worker *w, bool is_send,
    const __dtfl_message_header *h)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, CE_MESSAGE);
	__dtfl_bc_u8(&b, is_send ? 1 : 0);
	__dtfl_bc_u64(&b, h->channel);
	__dtfl_bc_u64(&b, h->source);
	__dtfl_bc_u64(&b, h->target_lower);
	__dtfl_bc_u64(&b, h->target_upper);
	__dtfl_bc_u64(&b, h->length);
	__dtfl_bc_u64(&b, h->seqno);
	emit(w, &b);
}

/* CommunicationEvent::State(StateEvent { send, process, remote, start }). */
void
__dtfl_worker_log_comm_state(__dtfl_worker *w, bool send, size_t process,
    size_t remote, bool start)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, CE_STATE);
	__dtfl_bc_u8(&b, send ? 1 : 0);
	__dtfl_bc_u64(&b, process);
	__dtfl_bc_u64(&b, remote);
	__dtfl_bc_u8(&b, start ? 1 : 0);
	emit(w, &b);
}

/*
 * CommunicationEvent::Setup(CommunicationSetup { sender, process,
 * remote: Option<usize> }).  has_remote false encodes None.
 */
void
__dtfl_worker_log_comm_setup(__dtfl_worker *w, bool sender, size_t process,
    bool has_remote, size_t remote)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
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
	emit(w, &b);
}
