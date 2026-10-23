/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/zc_inline.c: the inline network driver, which runs the work
 * of the cluster's send and receive threads (zc_tcp.c) on the worker's
 * own thread, over a dtfl_transport (ADR-0013).
 *
 * A TcpAllocator talks to its network threads through MergeQueues: one
 * send queue per remote process, which the worker's SendEndpoint
 * extends and the send thread drains to the stream, and one receive
 * queue per remote process, which the receive thread extends from the
 * stream and the worker drains.  This driver holds the far end of those
 * same queues, and the allocator calls __dtfl_inline_pump at the start
 * of every receive and the end of every release:
 *
 *   send half    drain each send queue into a stash, then write the
 *                stash to the transport as far as it accepts; the rest
 *                (and the offset into a partly written view) waits for
 *                the next pump.
 *   receive half read whatever the transport has from each peer into a
 *                BytesSlab, carve out complete frames, and extend the
 *                receive queue with them, exactly as recv_loop does.
 *
 * The worker parks on a parker whose sleep is the transport's wait and
 * whose wake is its wake, so arriving bytes, room on a full stream and
 * a dtfl_sync_activator from another thread all end the park.
 *
 * Shutdown.  When the worker is freed, its allocator drops the
 * SendEndpoints (flushing the last bytes into the queues) and the
 * receive readers.  __dtfl_inline_finish then does what the network
 * threads do at the end: it writes everything left, a zero-length
 * header and a shutdown to every peer, and reads each peer until its
 * zero-length header, checking that nothing follows, as recv_loop does.
 * Data arriving after the worker is gone is dropped, as upstream drops
 * it for a channel already dropped.
 *
 * shortcut: the "communication" log stream gets no events from this
 * driver (the network threads log StateEvent and MessageEvent); add
 * them here when a host needs that stream in inline mode.
 *
 * Threads.  Everything here belongs to the worker's thread.  The only
 * other thread that touches the driver is one that unparks the worker,
 * which reaches the transport's wake through the parker; the detach in
 * finish stops that before the driver is freed.
 */
#include "comm/zc_int.h"
#include "comm/init_int.h"

/* recv_loop's slab starts at 1 << 20, as upstream BytesSlab::new(20). */
#define SLAB_SHIFT 20

typedef struct inline_peer {
	size_t process;			/* the remote process index */
	__dtfl_merge_queue *send_rd;	/* reader of the worker's send queue */
	__dtfl_merge_queue *recv_wr;	/* writer of the worker's recv queue */
	__dtfl_vec stash;		/* __dtfl_bytes waiting to be written */
	size_t stash_head;		/* first unwritten view in stash */
	size_t stash_off;		/* bytes of stash[head] already written */
	__dtfl_bytes_slab slab;		/* receive buffer */
	__dtfl_vec staged;		/* __dtfl_bytes frames for recv_wr */
	__dtfl_merge_queue *pending_writer;	/* until posted to the worker */
	bool eos_sent;			/* the zero-length header went out */
	bool eos_seen;			/* the peer's zero-length header came */
} inline_peer;

struct __dtfl_inline_net {
	const dtfl_transport *t;	/* borrowed for the execution */
	size_t process;			/* this process (the worker index) */
	inline_peer *peers;		/* [processes - 1], self skipped */
	size_t npeers;
	__dtfl_parker *parker;		/* one reference; the worker's */
	__dtfl_mpsc **promises;		/* [npeers], held until attach */
	bool worker_gone;		/* the allocator has been destroyed */
};

/* tcp_panic, as zc_tcp.c: the prefix downstream code sniffs for. */
DTFL_NORETURN static void
net_panic(const char *context, int rc)
{
	__dtfl_panic("timely communication error: %s: %s", context,
	    rc == DTFL_EOF ? "unexpected end of stream" :
	    dtfl_last_error_detail());
}

/* MergeQueue buzz for the send queues: nothing to wake, we poll them. */
static void
buzz_none(void *ctx)
{
	(void)ctx;
}

/* The parker's callbacks: the transport's wait and wake. */
static void
transport_sleep(void *ctx, uint64_t timeout_ns)
{
	const dtfl_transport *t = ctx;

	t->wait(t->ctx, timeout_ns);
}

static void
transport_wake(void *ctx)
{
	const dtfl_transport *t = ctx;

	t->wake(t->ctx);
}

/* ------------------------------------------------------------------ */
/* Send half (send_loop)                                               */
/* ------------------------------------------------------------------ */

/* Drops the written prefix of the stash once it is long. */
static void
stash_compact(inline_peer *ip)
{
	__dtfl_bytes *base = (__dtfl_bytes *)(void *)ip->stash.data;

	if (ip->stash_head == 0)
		return;
	memmove(base, base + ip->stash_head,
	    (ip->stash.len - ip->stash_head) * sizeof *base);
	ip->stash.len -= ip->stash_head;
	ip->stash_head = 0;
}

/*
 * Writes the stash as far as the transport takes it.  Returns true if
 * everything was written.
 */
static bool
flush_stash(__dtfl_inline_net *n, inline_peer *ip)
{
	while (ip->stash_head < ip->stash.len) {
		__dtfl_bytes *b = DTFL_VEC_AT(&ip->stash, __dtfl_bytes,
		    ip->stash_head);
		size_t sent;
		int rc;

		rc = n->t->send(n->t->ctx, ip->process, b->ptr + ip->stash_off,
		    b->len - ip->stash_off, &sent);
		if (rc != 0)
			net_panic("writing data", rc);
		ip->stash_off += sent;
		if (ip->stash_off < b->len)
			return false;	/* short: offer the rest next time */
		__dtfl_bytes_release(b);
		ip->stash_head++;
		ip->stash_off = 0;
	}
	__dtfl_vec_clear(&ip->stash);
	ip->stash_head = 0;
	return true;
}

static void
pump_send(__dtfl_inline_net *n, inline_peer *ip)
{
	if (ip->send_rd != NULL) {
		if (ip->stash_head > 64)
			stash_compact(ip);
		__dtfl_merge_queue_drain_into(ip->send_rd, &ip->stash);
	}
	(void)flush_stash(n, ip);
}

/* ------------------------------------------------------------------ */
/* Receive half (recv_loop)                                            */
/* ------------------------------------------------------------------ */

/*
 * Carves complete frames off the front of the slab into staged.  A
 * zero-length header is the peer's end of stream.
 */
static void
carve_frames(__dtfl_inline_net *n, inline_peer *ip)
{
	__dtfl_message_header header;
	unsigned char *valid;
	size_t valid_len;

	for (;;) {
		__dtfl_bytes bytes;

		valid = __dtfl_slab_valid(&ip->slab, &valid_len);
		if (!__dtfl_header_try_read(&header, valid, valid_len))
			break;
		if (ip->eos_seen)
			__dtfl_panic("Clean shutdown followed by data.");
		__dtfl_slab_extract(&ip->slab,
		    __dtfl_header_required_bytes(&header), &bytes);
		if (header.length == 0) {
			ip->eos_seen = true;
			__dtfl_bytes_release(&bytes);
			continue;
		}
		/* One worker per process: the target range is this worker. */
		DTFL_ASSERT(header.target_lower == n->process &&
		    header.target_upper == n->process + 1);
		__dtfl_vec_push(&ip->staged, &bytes);
	}
	valid = __dtfl_slab_valid(&ip->slab, &valid_len);
	if (ip->eos_seen && valid_len != 0)
		__dtfl_panic("Clean shutdown followed by data.");
}

/* Reads what the peer has; returns true if any bytes arrived. */
static bool
pump_recv(__dtfl_inline_net *n, inline_peer *ip)
{
	bool any = false;

	for (;;) {
		unsigned char *empty;
		size_t cap, got;
		int rc;

		__dtfl_slab_ensure_capacity(&ip->slab, 1);
		empty = __dtfl_slab_empty(&ip->slab, &cap);
		rc = n->t->recv(n->t->ctx, ip->process, empty, cap, &got);
		if (rc == DTFL_EOF) {
			if (!ip->eos_seen)
				net_panic("reading data", rc);
			break;
		}
		if (rc != 0)
			net_panic("reading data", rc);
		if (got == 0)
			break;
		any = true;
		__dtfl_slab_make_valid(&ip->slab, got);
		carve_frames(n, ip);
		if (ip->eos_seen)
			break;
	}
	if (ip->staged.len > 0) {
		if (ip->recv_wr != NULL)
			__dtfl_merge_queue_extend(ip->recv_wr,
			    (__dtfl_bytes *)(void *)ip->staged.data,
			    ip->staged.len);
		else	/* the worker is gone: drop late data */
			while (ip->staged.len > 0) {
				ip->staged.len--;
				__dtfl_bytes_release(DTFL_VEC_AT(&ip->staged,
				    __dtfl_bytes, ip->staged.len));
			}
		__dtfl_vec_clear(&ip->staged);
	}
	return any;
}

/* ------------------------------------------------------------------ */
/* The pump and its lifecycle                                          */
/* ------------------------------------------------------------------ */

void
__dtfl_inline_pump(void *ctx)
{
	__dtfl_inline_net *n = ctx;
	size_t i;

	if (n->worker_gone)
		return;
	for (i = 0; i < n->npeers; i++) {
		pump_recv(n, &n->peers[i]);
		pump_send(n, &n->peers[i]);
	}
}

/* The driver for `process`, parking the worker on `parker` (retained). */
static __dtfl_inline_net *
inline_net_new(const dtfl_transport *t, size_t process, size_t processes,
    __dtfl_spill_fn *spill, __dtfl_parker *parker)
{
	__dtfl_inline_net *n = __dtfl_calloc(1, sizeof *n);
	size_t i, k = 0;

	n->t = t;
	n->process = process;
	n->npeers = processes - 1;
	n->parker = __dtfl_parker_retain(parker);
	n->peers = __dtfl_calloc(DTFL_MAX(n->npeers, 1), sizeof *n->peers);
	for (i = 0; i < processes; i++) {
		inline_peer *ip;
		__dtfl_merge_queue *writer, *reader;
		__dtfl_spill_policy *wp, *rp;

		if (i == process)
			continue;
		ip = &n->peers[k++];
		ip->process = i;
		__dtfl_vec_init(&ip->stash, sizeof(__dtfl_bytes));
		__dtfl_vec_init(&ip->staged, sizeof(__dtfl_bytes));
		__dtfl_slab_init(&ip->slab, SLAB_SHIFT);
		/* send_loop's queue pair; the writer goes to the worker. */
		__dtfl_spill_fn_make(spill, &wp, &rp);
		__dtfl_merge_queue_new_pair(buzz_none, NULL, NULL, wp, rp,
		    &writer, &reader);
		ip->send_rd = reader;
		ip->pending_writer = writer;
	}
	return n;
}

/* send_loop: post each send queue's writer to the worker's builder. */
static void
inline_post_sends(__dtfl_inline_net *n, __dtfl_mpsc **net_futures)
{
	size_t k;

	for (k = 0; k < n->npeers; k++) {
		__dtfl_mq_handoff h;

		h.queue = n->peers[k].pending_writer;
		n->peers[k].pending_writer = NULL;
		if (!__dtfl_mpsc_send(net_futures[k], &h))
			__dtfl_panic("inline driver: worker gone before build");
		__dtfl_mpsc_release(net_futures[k]);
	}
}

/* recv_loop's handoff: take the recv queues' writers the build posted. */
static void
inline_net_attach(__dtfl_inline_net *n, __dtfl_mpsc **net_promises)
{
	size_t k;

	/* recv_loop: the worker's build posted its recv queues' writers. */
	for (k = 0; k < n->npeers; k++) {
		__dtfl_mq_handoff h;

		if (!__dtfl_mpsc_recv(net_promises[k], &h))
			__dtfl_panic("inline driver: no receive queue posted");
		n->peers[k].recv_wr = h.queue;
		__dtfl_mpsc_release(net_promises[k]);
	}
}

/* Every peer that still owes us bytes or end of stream. */
static bool
peers_pending(__dtfl_inline_net *n)
{
	size_t i;

	for (i = 0; i < n->npeers; i++)
		if (!n->peers[i].eos_seen ||
		    n->peers[i].stash_head < n->peers[i].stash.len)
			return true;
	return false;
}

void
__dtfl_inline_net_finish(__dtfl_inline_net *n)
{
	__dtfl_message_header eos;
	unsigned char eos_bytes[DTFL_HEADER_BYTES];
	size_t i;

	n->worker_gone = true;
	memset(&eos, 0, sizeof eos);
	__dtfl_header_write_to(&eos, eos_bytes);
	for (i = 0; i < n->npeers; i++) {
		inline_peer *ip = &n->peers[i];
		__dtfl_bytes b;

		/*
		 * The worker dropped its writer (flushing it); take the rest,
		 * then queue the zero-length header after it.
		 */
		__dtfl_merge_queue_drain_into(ip->send_rd, &ip->stash);
		if (!__dtfl_merge_queue_is_complete(ip->send_rd))
			__dtfl_panic("inline driver: send queue still open");
		__dtfl_merge_queue_drop(ip->send_rd, false);
		ip->send_rd = NULL;
		__dtfl_bytes_new(&b, sizeof eos_bytes);
		memcpy(b.ptr, eos_bytes, sizeof eos_bytes);
		__dtfl_bytes_freeze(&b);
		__dtfl_vec_push(&ip->stash, &b);
		__dtfl_merge_queue_drop(ip->recv_wr, false);
		ip->recv_wr = NULL;
	}
	/*
	 * Drive every peer until our bytes are out and theirs have ended.
	 * The worker is gone, so no other thread can unpark the parker;
	 * the transport's own wait is enough.
	 */
	while (peers_pending(n)) {
		bool progress = false;

		for (i = 0; i < n->npeers; i++) {
			inline_peer *ip = &n->peers[i];

			if (!ip->eos_sent && flush_stash(n, ip)) {
				ip->eos_sent = true;
				if (n->t->shutdown != NULL &&
				    n->t->shutdown(n->t->ctx, ip->process) != 0)
					net_panic("shutting down writer",
					    DTFL_EIO);
				progress = true;
			}
			if (!ip->eos_seen && pump_recv(n, ip))
				progress = true;
		}
		if (!progress && peers_pending(n))
			n->t->wait(n->t->ctx, UINT64_MAX);
	}
}

void
__dtfl_inline_net_free(__dtfl_inline_net *n)
{
	size_t i;

	if (n == NULL)
		return;
	/* No unpark may reach the transport once its owner may free it. */
	__dtfl_parker_detach(n->parker);
	for (i = 0; i < n->npeers; i++) {
		inline_peer *ip = &n->peers[i];
		size_t k;

		__dtfl_merge_queue_drop(ip->pending_writer, false);
		__dtfl_merge_queue_drop(ip->send_rd, false);
		__dtfl_merge_queue_drop(ip->recv_wr, false);
		for (k = ip->stash_head; k < ip->stash.len; k++)
			__dtfl_bytes_release(DTFL_VEC_AT(&ip->stash,
			    __dtfl_bytes, k));
		__dtfl_vec_free(&ip->stash);
		__dtfl_vec_free(&ip->staged);
		__dtfl_slab_free(&ip->slab);
	}
	__dtfl_free(n->peers);
	__dtfl_parker_release(n->parker);
	__dtfl_free(n);
}

/*
 * dtfl_execute_worker's builder: a TcpBuilder over one typed Process
 * builder whose parker is the driver's, so the worker parks in the
 * transport's wait.  The network grids are [npeers][1].
 */
__dtfl_allocator_builder *
__dtfl_inline_builder(const dtfl_config *cfg, size_t index, size_t peers,
    const dtfl_transport *t, __dtfl_inline_net **net_out)
{
	size_t npeers = peers - 1, k;
	__dtfl_allocator_builder *inner, *b;
	__dtfl_mpsc ***net_promises, ***net_futures;
	__dtfl_mpsc **futures;
	__dtfl_spill_fn *spill = cfg != NULL ? __dtfl_config_spill(cfg) : NULL;
	__dtfl_inline_net *n;
	__dtfl_parker *parker;

	*net_out = NULL;
	if (peers == 1)
		return __dtfl_thread_builder();
	net_promises = __dtfl_calloc(npeers, sizeof *net_promises);
	net_futures = __dtfl_calloc(npeers, sizeof *net_futures);
	for (k = 0; k < npeers; k++) {
		net_promises[k] = __dtfl_calloc(1, sizeof **net_promises);
		net_futures[k] = __dtfl_calloc(1, sizeof **net_futures);
	}
	/* The driver's parker exists before the inner builder needs it. */
	parker = __dtfl_parker_new_with(transport_sleep, transport_wake,
	    (void *)t);
	__dtfl_process_builders_parked(1, &parker, &inner);
	__dtfl_tcp_builders(&inner, 1, index, peers, spill, &b, net_promises,
	    net_futures);
	futures = __dtfl_calloc(npeers, sizeof *futures);
	n = inline_net_new(t, index, peers, spill, parker);
	for (k = 0; k < npeers; k++)
		futures[k] = net_futures[k][0];
	inline_post_sends(n, futures);
	n->promises = __dtfl_calloc(npeers, sizeof *n->promises);
	for (k = 0; k < npeers; k++) {
		n->promises[k] = net_promises[k][0];
		__dtfl_free(net_promises[k]);
		__dtfl_free(net_futures[k]);
	}
	__dtfl_free(net_promises);
	__dtfl_free(net_futures);
	__dtfl_free(futures);
	__dtfl_parker_release(parker);
	__dtfl_tcp_builder_set_pump(b, __dtfl_inline_pump, n);
	*net_out = n;
	return b;
}

void
__dtfl_inline_net_attach_built(__dtfl_inline_net *n)
{
	if (n == NULL)
		return;
	inline_net_attach(n, n->promises);
	__dtfl_free(n->promises);
	n->promises = NULL;
}
