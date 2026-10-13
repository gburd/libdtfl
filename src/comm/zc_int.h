/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/zc_int.h: the zero-copy byte layer shared by the byte
 * allocators (ProcessBinary now, Tcp later).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/networking.rs
 *   communication/src/allocator/zero_copy/bytes_slab.rs
 *   communication/src/allocator/zero_copy/bytes_exchange.rs
 *   communication/src/allocator/zero_copy/spill.rs
 *   communication/src/allocator/zero_copy/push_pull.rs
 *   communication/src/allocator/zero_copy/allocator_process.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Threads.  A MergeQueue pair is the only cross-thread object here:
 * the writer handle lives on the sending worker, the reader handle on
 * the receiving worker, and the entries between them are guarded by a
 * mutex.  Everything else (slabs, send endpoints, pushers, pullers,
 * local FIFOs) belongs to one worker thread.  Byte views cross threads
 * only as frozen (read-only) __dtfl_bytes, whose store count is atomic.
 */
#ifndef DTFL_ZC_INT_H
#define DTFL_ZC_INT_H

#include <pthread.h>

#include "comm/comm_int.h"

/* ------------------------------------------------------------------ */
/* networking.rs: MessageHeader                                        */
/* ------------------------------------------------------------------ */

/*
 * Framing for each transmission: a typed channel, the source and
 * destination workers, and the payload length in bytes.  On the wire it
 * is six big-endian u64s in field order (48 bytes), then `length` bytes.
 */
typedef struct __dtfl_message_header {
	size_t channel;		/* index of channel */
	size_t source;		/* index of worker sending message */
	size_t target_lower;	/* lower bound of receiving workers */
	size_t target_upper;	/* upper bound (exclusive) */
	size_t length;		/* number of bytes in message */
	size_t seqno;		/* sequence number */
} __dtfl_message_header;

#define DTFL_HEADER_FIELDS 6
#define DTFL_HEADER_BYTES (DTFL_HEADER_FIELDS * 8)

bool __dtfl_header_try_read(__dtfl_message_header *h,
    const unsigned char *bytes, size_t len);
void __dtfl_header_write_to(const __dtfl_message_header *h,
    unsigned char *out);
size_t __dtfl_header_required_bytes(const __dtfl_message_header *h);
size_t __dtfl_header_header_bytes(const __dtfl_message_header *h);

/* ------------------------------------------------------------------ */
/* bytes_slab.rs: BytesSlab                                            */
/* ------------------------------------------------------------------ */

/*
 * A large writable allocation with a valid (written) prefix.  Extracted
 * views go to readers; the retired buffers they came from wait in
 * in_progress until every reader is done, then move to the stash.
 * Owned by one worker thread.
 */
typedef struct __dtfl_bytes_slab {
	__dtfl_bytes buffer;	/* current writable working buffer */
	__dtfl_vec in_progress;	/* __dtfl_bytes: retired, readers hold */
	__dtfl_vec stash;	/* __dtfl_bytes: reclaimed, each 1 << shift */
	size_t shift;		/* current buffer allocation size (log2) */
	size_t valid;		/* buffer.ptr[0 .. valid) are written */
} __dtfl_bytes_slab;

void __dtfl_slab_init(__dtfl_bytes_slab *s, size_t shift);
void __dtfl_slab_free(__dtfl_bytes_slab *s);
unsigned char *__dtfl_slab_empty(__dtfl_bytes_slab *s, size_t *len);
unsigned char *__dtfl_slab_valid(__dtfl_bytes_slab *s, size_t *len);
void __dtfl_slab_make_valid(__dtfl_bytes_slab *s, size_t bytes);
void __dtfl_slab_extract(__dtfl_bytes_slab *s, size_t bytes,
    __dtfl_bytes *out);
void __dtfl_slab_ensure_capacity(__dtfl_bytes_slab *s, size_t capacity);

/* ------------------------------------------------------------------ */
/* bytes_exchange.rs: QueueEntry and its deque                         */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_bytes_fetch __dtfl_bytes_fetch;

/* Bytes resident in memory, or a handle to bytes a SpillPolicy wrote out. */
typedef struct __dtfl_queue_entry {
	enum { DTFL_QE_BYTES, DTFL_QE_PAGED } kind;
	union {
		__dtfl_bytes bytes;		/* owned view */
		__dtfl_bytes_fetch *paged;	/* owned handle */
	} u;
} __dtfl_queue_entry;

/* VecDeque<QueueEntry>: entries buf[head .. head + len).  Owns its entries. */
typedef struct __dtfl_queue_entries {
	__dtfl_queue_entry *buf;
	size_t head;
	size_t len;
	size_t cap;
} __dtfl_queue_entries;

void __dtfl_qe_init(__dtfl_queue_entries *q);
void __dtfl_qe_free(__dtfl_queue_entries *q);
void __dtfl_qe_entry_drop(__dtfl_queue_entry *e);

static inline __dtfl_queue_entry *
__dtfl_qe_at(__dtfl_queue_entries *q, size_t i)
{
	DTFL_DEBUG_ASSERT(i < q->len);
	return &q->buf[q->head + i];
}

void __dtfl_qe_push_back(__dtfl_queue_entries *q, __dtfl_queue_entry e);
__dtfl_queue_entry __dtfl_qe_pop_back(__dtfl_queue_entries *q);
__dtfl_queue_entry __dtfl_qe_pop_front(__dtfl_queue_entries *q);
void __dtfl_qe_insert(__dtfl_queue_entries *q, size_t i, __dtfl_queue_entry e);
__dtfl_queue_entry __dtfl_qe_remove(__dtfl_queue_entries *q, size_t i);

/* ------------------------------------------------------------------ */
/* spill.rs: SpillPolicy, BytesSpill, BytesFetch                       */
/* ------------------------------------------------------------------ */

/*
 * Inspects and optionally rewrites a MergeQueue's entries.  apply()
 * runs under the queue's mutex, on whichever thread holds the handle
 * that owns the policy.
 */
typedef struct __dtfl_spill_policy __dtfl_spill_policy;
typedef struct __dtfl_spill_policy_ops {
	void (*apply)(__dtfl_spill_policy *p, __dtfl_queue_entries *q);
	void (*destroy)(__dtfl_spill_policy *p);
} __dtfl_spill_policy_ops;
struct __dtfl_spill_policy {
	const __dtfl_spill_policy_ops *ops;
};

/*
 * Moves bytes to backing storage.  spill() takes ownership of a prefix
 * of `chunks` (__dtfl_bytes), removes it from the front of the vector,
 * and appends one handle per spilled chunk to `handles`
 * (__dtfl_bytes_fetch *).  It may stop partway on failure; what is left
 * in `chunks` still belongs to the caller.
 */
typedef struct __dtfl_bytes_spill __dtfl_bytes_spill;
typedef struct __dtfl_bytes_spill_ops {
	void (*spill)(__dtfl_bytes_spill *s, __dtfl_vec *chunks,
	    __dtfl_vec *handles);
	void (*destroy)(__dtfl_bytes_spill *s);
} __dtfl_bytes_spill_ops;
struct __dtfl_bytes_spill {
	const __dtfl_bytes_spill_ops *ops;
};

/*
 * A consume-once handle to spilled bytes.  fetch() returns true after
 * appending the payload (__dtfl_bytes) to `out` and freeing the handle;
 * on false the handle is unchanged and still owned by the caller.
 * destroy() drops an unfetched handle.
 */
typedef struct __dtfl_bytes_fetch_ops {
	bool (*fetch)(__dtfl_bytes_fetch *h, __dtfl_vec *out);
	void (*destroy)(__dtfl_bytes_fetch *h);
} __dtfl_bytes_fetch_ops;
struct __dtfl_bytes_fetch {
	const __dtfl_bytes_fetch_ops *ops;
};

/* threshold::Threshold: writer side, spills the middle of the queue. */
typedef struct __dtfl_spill_threshold {
	__dtfl_spill_policy base;
	__dtfl_bytes_spill *strategy;	/* owned */
	size_t threshold_bytes;		/* default 256 MB */
	size_t head_reserve_bytes;	/* default 64 MB */
} __dtfl_spill_threshold;

/* prefetch::PrefetchPolicy: reader side, pages the front back in. */
typedef struct __dtfl_spill_prefetch {
	__dtfl_spill_policy base;
	size_t budget;		/* bytes materialized per apply */
} __dtfl_spill_prefetch;

__dtfl_spill_threshold *__dtfl_spill_threshold_new(
    __dtfl_bytes_spill *strategy);
__dtfl_spill_prefetch *__dtfl_spill_prefetch_new(size_t budget);
void __dtfl_spill_policy_destroy(__dtfl_spill_policy *p);

/* ------------------------------------------------------------------ */
/* bytes_exchange.rs: MergeQueue and SendEndpoint                      */
/* ------------------------------------------------------------------ */

/*
 * One handle on a point-to-point byte queue between two threads.  A
 * handle belongs to one thread; the pair shares the entries.
 */
typedef struct __dtfl_merge_queue __dtfl_merge_queue;

/*
 * MergeQueue::new_pair.  buzz(ctx) wakes the receiving thread; it may
 * run on either thread.  ctx_drop(ctx), if not NULL, runs once after
 * both handles are gone.  The writer runs writer_policy after each
 * extend, the reader runs reader_policy before each drain; each handle
 * owns its policy (NULL for none).
 */
void __dtfl_merge_queue_new_pair(void (*buzz)(void *ctx), void *ctx,
    void (*ctx_drop)(void *ctx), __dtfl_spill_policy *writer_policy,
    __dtfl_spill_policy *reader_policy, __dtfl_merge_queue **writer,
    __dtfl_merge_queue **reader);
bool __dtfl_merge_queue_is_complete(__dtfl_merge_queue *q);
void __dtfl_merge_queue_extend(__dtfl_merge_queue *q, __dtfl_bytes *items,
    size_t n);
void __dtfl_merge_queue_drain_into(__dtfl_merge_queue *q, __dtfl_vec *out);
/*
 * Drop.  `panicking` stands in for std::thread::panicking(): true
 * poisons the queue (unclean shutdown); false panics if the peer
 * poisoned it.  Either way the receiver is buzzed.
 */
void __dtfl_merge_queue_drop(__dtfl_merge_queue *q, bool panicking);

/*
 * SendEndpoint<MergeQueue>: stages writes in a slab and publishes them
 * to a queue.  Shared by the pushers to one target and the allocator,
 * as upstream Rc<RefCell<_>>: a worker-local reference count.
 */
typedef struct __dtfl_send_endpoint {
	size_t rc;			/* worker-local */
	__dtfl_merge_queue *send;	/* owned writer handle */
	__dtfl_bytes_slab buffer;
} __dtfl_send_endpoint;

__dtfl_send_endpoint *__dtfl_send_endpoint_new(__dtfl_merge_queue *queue);
__dtfl_send_endpoint *__dtfl_send_endpoint_retain(__dtfl_send_endpoint *e);
void __dtfl_send_endpoint_release(__dtfl_send_endpoint *e);
unsigned char *__dtfl_send_endpoint_reserve(__dtfl_send_endpoint *e,
    size_t capacity, size_t *len);
void __dtfl_send_endpoint_make_valid(__dtfl_send_endpoint *e, size_t bytes);
void __dtfl_send_endpoint_publish(__dtfl_send_endpoint *e);

/* ------------------------------------------------------------------ */
/* push_pull.rs                                                        */
/* ------------------------------------------------------------------ */

/*
 * Rc<RefCell<VecDeque<Bytes>>>: a worker-local FIFO of payload views,
 * items[head .. len), shared by the allocator and one puller through a
 * worker-local count.  Owns its views.
 */
typedef struct __dtfl_bytes_fifo {
	size_t rc;		/* worker-local */
	__dtfl_vec items;	/* __dtfl_bytes */
	size_t head;
} __dtfl_bytes_fifo;

__dtfl_bytes_fifo *__dtfl_bytes_fifo_new(void);
__dtfl_bytes_fifo *__dtfl_bytes_fifo_retain(__dtfl_bytes_fifo *f);
void __dtfl_bytes_fifo_release(__dtfl_bytes_fifo *f);
/* Moves *b in (b is zeroed). */
void __dtfl_bytes_fifo_push(__dtfl_bytes_fifo *f, __dtfl_bytes *b);
/* Moves the front view to *out; false if empty. */
bool __dtfl_bytes_fifo_pop(__dtfl_bytes_fifo *f, __dtfl_bytes *out);

/*
 * canary.rs plus its Rc<RefCell<Vec<usize>>>: identifiers of channels
 * whose puller dropped, shared by the allocator and its pullers through
 * a worker-local count, so either may be destroyed first.
 */
typedef struct __dtfl_zc_canaries {
	size_t rc;		/* worker-local */
	__dtfl_vec dropped;	/* size_t */
} __dtfl_zc_canaries;

/*
 * Pusher: writes header then mc's encoding into `sender`.  Retains
 * sender.  `header` is the template; seqno counts up from it.
 */
__dtfl_push *__dtfl_zc_pusher_new(const __dtfl_message_header *header,
    __dtfl_send_endpoint *sender, const __dtfl_msg_class *mc);
/*
 * Puller wrapped in counters.rs Puller and a Canary: pops from
 * `receiver` (retained) and decodes with mc; reports `identifier` to
 * `events` (the allocator's, so the allocator must outlive every pull)
 * when a run of messages ends, and to `canaries` (retained) on destroy.
 */
__dtfl_pull *__dtfl_zc_puller_new(__dtfl_bytes_fifo *receiver,
    const __dtfl_msg_class *mc, size_t identifier, __dtfl_events *events,
    __dtfl_zc_canaries *canaries);

/*
 * push_pull.rs PullerInner wrapped in counters.rs Puller and a Canary:
 * pulls first from `inner` (the process-local typed puller, ownership
 * taken), then from `receiver` (the FIFO of inter-process payload
 * views, retained), decoding with mc.  Reports `identifier` to `events`
 * (the allocator's) when a run of messages ends, and to `canaries`
 * (retained) on destroy.  The Tcp allocator uses this; the plain Puller
 * above has no inner.
 */
__dtfl_pull *__dtfl_zc_puller_inner_new(__dtfl_pull *inner,
    __dtfl_bytes_fifo *receiver, const __dtfl_msg_class *mc,
    size_t identifier, __dtfl_events *events, __dtfl_zc_canaries *canaries);

/* ------------------------------------------------------------------ */
/* tcp.rs: the send and receive loops                                  */
/* ------------------------------------------------------------------ */

/*
 * One MergeQueue writer handle in transit between a worker thread and a
 * network thread.  The queues move through __dtfl_mpsc channels (the
 * promise_futures rendezvous): element type is one of these.
 */
typedef struct __dtfl_mq_handoff {
	__dtfl_merge_queue *queue;
} __dtfl_mq_handoff;

/*
 * recv_loop: read framed messages from socket `fd` and route the
 * payload bytes to the target worker's queue.  `targets` is a vector of
 * __dtfl_mpsc * (one per local worker, size_t threads); each yields the
 * writer handle its worker posted.  Byte views for a message whose
 * target range is [lower, upper) go to targets[t - worker_offset] for
 * each t in the range.  A zero-length header is end of stream; a short
 * or malformed frame, a zero read, or data after a clean shutdown
 * panics (upstream's tcp_panic).  Takes ownership of the `targets`
 * vector's storage and the mpsc references in it; does not close `fd`.
 */
void __dtfl_tcp_recv_loop(int fd, __dtfl_mpsc **targets, size_t ntargets,
    size_t worker_offset);

/*
 * send_loop: drain each local worker's outgoing queue to socket `fd` as
 * framed messages, then write a final zero-length header and shut down
 * the write half.  `sources` is a vector of __dtfl_mpsc * (one per local
 * worker); the loop creates a MergeQueue pair per source, posts the
 * writer to the worker, and drains the reader.  A write failure panics.
 * Takes ownership of the `sources` vector's storage and references; does
 * not close `fd`.
 */
void __dtfl_tcp_send_loop(int fd, __dtfl_mpsc **sources, size_t nsources);

#endif /* DTFL_ZC_INT_H */
