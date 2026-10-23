/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/tcp.rs
 *   communication/src/allocator/zero_copy/stream.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/zc_tcp.c: the TCP send and receive loops.
 *
 * One network thread per remote process runs each loop over a socket to
 * that process.
 *
 *   recv_loop reads a sequence of (header, payload) frames from the
 *   socket into a BytesSlab, carves out each complete message, and hands
 *   its payload views to the local workers named by the header's target
 *   range.  A zero-length header ends the stream.
 *
 *   send_loop drains every local worker's outgoing MergeQueue (whose
 *   payloads are already framed by the Pusher) into the socket, parking
 *   when there is nothing to send, and finishes with a zero-length
 *   header and a write shutdown so the peer's recv_loop exits cleanly.
 *
 * The stream abstraction (stream.rs) folds in here: a socket fd supports
 * read, write, shutdown and blocking mode directly.
 *
 * Communication logging (logging.rs MessageEvent/StateEvent) is not
 * wired: the network threads have no worker and no comm log sink is
 * plumbed to them (as the slab's BytesRefill and the spill Hooks were
 * simplified).  The event encoding lands in comm_log.c for a future
 * sink; here the log points are omitted.
 *
 * Threads.  Each loop owns its socket fd and its slab or stash.  The
 * only cross-thread state is the MergeQueues it shares with the workers
 * (zc_bytes_exchange.c documents their synchronization) and the mpsc
 * channels it exchanges them through (comm_int.h).  A parked send thread
 * is woken by the worker's Pusher buzzing the queue (the queue's buzz
 * callback unparks this thread's parker).
 */
#define _GNU_SOURCE	/* MSG_NOSIGNAL, via base/sock_int.h */

#include "comm/zc_int.h"
#include "comm/init_int.h"
#include "base/os_thread.h"
#include "base/sock_int.h"

#include <errno.h>
#include <sys/socket.h>

/*
 * tcp_panic: upstream aborts the computation on any socket error with a
 * message beginning "timely communication error:" (downstream crates
 * sniff for it).  Keep the prefix.
 */
DTFL_NORETURN static void
tcp_panic(const char *context, int err)
{
	__dtfl_panic("timely communication error: %s: %s", context,
	    strerror(err));
}

/* ------------------------------------------------------------------ */
/* Blocking socket IO (stream.rs read/write/shutdown)                  */
/* ------------------------------------------------------------------ */

/*
 * read(2) into buf, retrying EINTR.  Returns the count (> 0), or panics
 * on error or a zero read (the peer closed without a clean shutdown).
 */
static size_t
sock_read(int fd, unsigned char *buf, size_t cap, const char *context)
{
	ssize_t r;

	do {
		r = recv(fd, buf, cap, 0);
	} while (r < 0 && errno == EINTR);
	if (r < 0)
		tcp_panic(context, errno);
	if (r == 0)
		tcp_panic(context, 0);	/* UnexpectedEof: socket closed */
	return (size_t)r;
}

/* read(2) that tolerates a zero read, used to confirm EOF after shutdown. */
static size_t
sock_read_eof(int fd, unsigned char *buf, size_t cap, const char *context)
{
	ssize_t r;

	do {
		r = recv(fd, buf, cap, 0);
	} while (r < 0 && errno == EINTR);
	if (r < 0)
		tcp_panic(context, errno);
	return (size_t)r;
}

/* write(2) all of buf, retrying EINTR and short writes.  Panics on error. */
static void
sock_write_all(int fd, const unsigned char *buf, size_t len,
    const char *context)
{
	size_t off = 0;
	ssize_t w;

	while (off < len) {
		do {
			w = send(fd, buf + off, len - off, DTFL_SEND_NOSIGNAL);
		} while (w < 0 && errno == EINTR);
		if (w < 0)
			tcp_panic(context, errno);
		off += (size_t)w;
	}
}

/* ------------------------------------------------------------------ */
/* recv_loop                                                           */
/* ------------------------------------------------------------------ */

void
__dtfl_tcp_recv_loop(int fd, __dtfl_mpsc **targets, size_t ntargets,
    size_t worker_offset, size_t process, size_t remote,
    __dtfl_logger *logger)
{
	__dtfl_merge_queue **queues = __dtfl_calloc(DTFL_MAX(ntargets, 1),
	    sizeof *queues);
	__dtfl_vec *stageds = __dtfl_calloc(DTFL_MAX(ntargets, 1),
	    sizeof *stageds);
	__dtfl_bytes_slab buffer;
	__dtfl_message_header header;
	bool active = true;
	size_t i, t;

	/* Log the receive thread's start. */
	if (logger != NULL)
		__dtfl_comm_log_state(logger, false, process, remote, true);

	/* Receive each local worker's queue writer (it posted it in build). */
	for (i = 0; i < ntargets; i++) {
		__dtfl_mq_handoff h;

		while (!__dtfl_mpsc_recv(targets[i], &h)) {
			/*
			 * The worker always posts before stepping.  A spin is
			 * acceptable: the post is near-immediate at build time.
			 */
			__dtfl_yield();
		}
		queues[i] = h.queue;
		__dtfl_vec_init(&stageds[i], sizeof(__dtfl_bytes));
	}

	/* 1 << 20 initial slab, as upstream BytesSlab::new(20). */
	__dtfl_slab_init(&buffer, 20);

	while (active) {
		unsigned char *empty;
		size_t cap, read, valid_len;

		__dtfl_slab_ensure_capacity(&buffer, 1);
		empty = __dtfl_slab_empty(&buffer, &cap);
		DTFL_ASSERT(cap > 0);

		read = sock_read(fd, empty, cap, "reading data");
		__dtfl_slab_make_valid(&buffer, read);

		/* Consume complete messages from the front of the buffer. */
		for (;;) {
			unsigned char *valid = __dtfl_slab_valid(&buffer,
			    &valid_len);
			__dtfl_bytes bytes;
			size_t peeled;

			if (!__dtfl_header_try_read(&header, valid, valid_len))
				break;
			peeled = __dtfl_header_required_bytes(&header);
			__dtfl_slab_extract(&buffer, peeled, &bytes);

			/* Record message receipt. */
			if (logger != NULL)
				__dtfl_comm_log_message(logger, false, &header);

			if (header.length > 0) {
				for (t = header.target_lower;
				    t < header.target_upper; t++) {
					__dtfl_bytes copy;
					size_t idx = t - worker_offset;

					DTFL_ASSERT(idx < ntargets);
					__dtfl_bytes_clone(&bytes, &copy);
					__dtfl_vec_push(&stageds[idx], &copy);
				}
				__dtfl_bytes_release(&bytes);
			} else {
				/* Clean shutdown; confirm no trailing data. */
				active = false;
				__dtfl_bytes_release(&bytes);
				valid = __dtfl_slab_valid(&buffer, &valid_len);
				if (valid_len != 0)
					__dtfl_panic("Clean shutdown followed "
					    "by data.");
				__dtfl_slab_ensure_capacity(&buffer, 1);
				empty = __dtfl_slab_empty(&buffer, &cap);
				if (sock_read_eof(fd, empty, cap,
				    "reading EOF") > 0)
					__dtfl_panic("Clean shutdown followed "
					    "by data.");
				break;
			}
		}

		/* Pass staged views along to their target queues. */
		for (i = 0; i < ntargets; i++) {
			__dtfl_vec *s = &stageds[i];

			if (s->len > 0) {
				__dtfl_merge_queue_extend(queues[i],
				    (__dtfl_bytes *)(void *)s->data, s->len);
				__dtfl_vec_clear(s);
			}
		}
	}

	__dtfl_slab_free(&buffer);
	for (i = 0; i < ntargets; i++) {
		/* Clean shutdown of the receive side of each queue. */
		__dtfl_merge_queue_drop(queues[i], false);
		__dtfl_vec_free(&stageds[i]);
		__dtfl_mpsc_release(targets[i]);
	}
	__dtfl_free(queues);
	__dtfl_free(stageds);
	__dtfl_free(targets);

	/* Log the receive thread's end. */
	if (logger != NULL) {
		__dtfl_comm_log_state(logger, false, process, remote, false);
		__dtfl_logger_release(logger);
	}
}

/* ------------------------------------------------------------------ */
/* send_loop                                                           */
/* ------------------------------------------------------------------ */

/* MergeQueue buzz: wakes this send thread's parker. */
static void
buzz_parker(void *ctx)
{
	__dtfl_parker_unpark(ctx);
}

/* MergeQueue ctx_drop: releases the send thread's parker reference. */
static void
drop_parker(void *ctx)
{
	__dtfl_parker_release(ctx);
}

void
__dtfl_tcp_send_loop(int fd, __dtfl_mpsc **sources, size_t nsources,
    size_t process, size_t remote, __dtfl_logger *logger,
    __dtfl_spill_fn *spill)
{
	__dtfl_parker *parker = __dtfl_parker_new();
	__dtfl_merge_queue **readers = __dtfl_calloc(DTFL_MAX(nsources, 1),
	    sizeof *readers);
	size_t live = nsources;
	__dtfl_vec stash;	/* __dtfl_bytes */
	__dtfl_message_header eos;
	unsigned char eos_bytes[DTFL_HEADER_BYTES];
	size_t i;

	__dtfl_vec_init(&stash, sizeof(__dtfl_bytes));

	/* Log the send thread's start. */
	if (logger != NULL)
		__dtfl_comm_log_state(logger, true, process, remote, true);

	/*
	 * Send the policy-bearing queue to the worker (writer); keep a
	 * reader-side handle for ourselves.
	 */
	for (i = 0; i < nsources; i++) {
		__dtfl_merge_queue *writer, *reader;
		__dtfl_spill_policy *wp, *rp;
		__dtfl_mq_handoff h;

		__dtfl_spill_fn_make(spill, &wp, &rp);
		__dtfl_merge_queue_new_pair(buzz_parker,
		    __dtfl_parker_retain(parker), drop_parker, wp, rp,
		    &writer, &reader);
		readers[i] = reader;
		h.queue = writer;
		if (!__dtfl_mpsc_send(sources[i], &h)) {
			/* The worker never built; drop the unsent writer. */
			__dtfl_merge_queue_drop(writer, false);
			readers[i] = NULL;
		}
	}

	while (live > 0) {
		bool any = false;

		for (i = 0; i < nsources; i++)
			if (readers[i] != NULL)
				__dtfl_merge_queue_drain_into(readers[i],
				    &stash);

		if (stash.len == 0) {
			/*
			 * Nothing to send.  Retire queues whose worker handle
			 * has dropped and that are empty, then park unless
			 * every source is gone.
			 */
			live = 0;
			for (i = 0; i < nsources; i++) {
				if (readers[i] == NULL)
					continue;
				if (__dtfl_merge_queue_is_complete(readers[i])) {
					__dtfl_merge_queue_drop(readers[i],
					    false);
					readers[i] = NULL;
				} else {
					live++;
				}
			}
			if (live > 0)
				__dtfl_parker_park(parker, UINT64_MAX);
		} else {
			for (i = 0; i < stash.len; i++) {
				__dtfl_bytes *b = DTFL_VEC_AT(&stash,
				    __dtfl_bytes, i);

				/* Record message sends. */
				if (logger != NULL) {
					__dtfl_message_header h;
					size_t off = 0;

					while (__dtfl_header_try_read(&h,
					    b->ptr + off, b->len - off)) {
						__dtfl_comm_log_message(logger,
						    true, &h);
						off += __dtfl_header_required_bytes(
						    &h);
					}
				}
				sock_write_all(fd, b->ptr, b->len,
				    "writing data");
				any = true;
				__dtfl_bytes_release(b);
			}
			__dtfl_vec_clear(&stash);
			(void)any;
		}
	}

	/*
	 * Final zero-length header: the peer's recv_loop reads it as end of
	 * stream.  Then shut down the write half so its next read returns 0.
	 */
	eos.channel = 0;
	eos.source = 0;
	eos.target_lower = 0;
	eos.target_upper = 0;
	eos.length = 0;
	eos.seqno = 0;
	__dtfl_header_write_to(&eos, eos_bytes);
	sock_write_all(fd, eos_bytes, sizeof eos_bytes, "writing data");
	if (shutdown(fd, SHUT_WR) != 0)
		tcp_panic("shutting down writer", errno);
	if (logger != NULL)
		__dtfl_comm_log_message(logger, true, &eos);

	__dtfl_vec_free(&stash);
	for (i = 0; i < nsources; i++)
		__dtfl_mpsc_release(sources[i]);
	__dtfl_free(readers);
	__dtfl_free(sources);
	__dtfl_parker_release(parker);

	/* Log the send thread's end. */
	if (logger != NULL) {
		__dtfl_comm_log_state(logger, true, process, remote, false);
		__dtfl_logger_release(logger);
	}
}
