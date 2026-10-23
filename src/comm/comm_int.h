/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/comm_int.h: the communication layer (timely_communication).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/lib.rs
 *   communication/src/allocator/mod.rs
 *   communication/src/allocator/zero_copy/mod.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Rust is generic over the element type T of a channel.  In C a channel
 * carries opaque message pointers (void *) described by a __dtfl_msg_class
 * that supplies what the Rust trait bounds supply: Bytesable
 * (encode/decode), Clone (for broadcast), and Drop (free).  A typed
 * allocator (Thread, Process) moves the pointer; a byte allocator
 * (ProcessBinary, Tcp) encodes on push and decodes on pull.
 *
 * Push and Pull keep upstream's protocol exactly:
 *   push(&msg): `msg` is never NULL; it is Rust's `&mut Option<T>`.
 *               *msg != NULL sends it (ownership moves to the channel)
 *               and the pusher stores a recycled element or NULL back
 *               into *msg; *msg == NULL (__dtfl_push_done) is a flush.
 *   pull():     returns the next message (ownership to the caller) or
 *               NULL if none is available now.
 *
 * Ownership: every message has exactly one owner at a time.  A pusher
 * that drops a message (remote endpoint gone) frees it with the class.
 *
 * Threads: pushers and pullers belong to their worker thread.  The
 * queues between workers (process.c, zc_bytes_exchange.c) are the only
 * cross-thread state, and each documents its synchronization.
 */
#ifndef DTFL_COMM_INT_H
#define DTFL_COMM_INT_H

#include "base/base_int.h"
#include "base/os_sync.h"
#include "base/batch_int.h"

/* ------------------------------------------------------------------ */
/* Message classes (Bytesable + Clone + Drop)                          */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_msg_class {
	const char *name;
	/* Bytes that encode() will write.  Must be > 0. */
	size_t (*encoded_len)(const struct __dtfl_msg_class *mc, const void *msg);
	/* Writes exactly encoded_len bytes. */
	void (*encode)(const struct __dtfl_msg_class *mc, const void *msg,
	    unsigned char *out);
	/*
	 * Builds a message from bytes.  `b` is a view the decoder may keep
	 * (zero copy) by cloning it, or copy out of.  Returns NULL on
	 * malformed input.
	 */
	void *(*decode)(const struct __dtfl_msg_class *mc, const __dtfl_bytes *b);
	/* Deep or shared copy, for broadcast and Tee fan-out. */
	void *(*clone)(const struct __dtfl_msg_class *mc, const void *msg);
	void (*free)(const struct __dtfl_msg_class *mc, void *msg);
	void *ctx;
} __dtfl_msg_class;

/* ------------------------------------------------------------------ */
/* Push and Pull                                                       */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_push __dtfl_push;
typedef struct __dtfl_pull __dtfl_pull;

typedef struct __dtfl_push_ops {
	/* Upstream Push::push.  *msg == NULL is done(). */
	void (*push)(__dtfl_push *p, void **msg);
	void (*destroy)(__dtfl_push *p);
} __dtfl_push_ops;

struct __dtfl_push {
	const __dtfl_push_ops *ops;
};

typedef struct __dtfl_pull_ops {
	/* Upstream Pull::pull then take(): returns an owned message or NULL. */
	void *(*pull)(__dtfl_pull *p);
	void (*destroy)(__dtfl_pull *p);
} __dtfl_pull_ops;

struct __dtfl_pull {
	const __dtfl_pull_ops *ops;
};

/* Push::send: moves `msg` in; frees any recycled element returned. */
void __dtfl_push_send(__dtfl_push *p, const __dtfl_msg_class *mc, void *msg);
/*
 * Push::done: flush.  No pusher at the pin recycles on a flush, so a
 * recycled element here is a contract violation and panics.
 */
void __dtfl_push_done(__dtfl_push *p);

static inline void
__dtfl_push_push(__dtfl_push *p, void **msg)
{
	p->ops->push(p, msg);
}

static inline void *
__dtfl_pull_recv(__dtfl_pull *p)
{
	return p->ops->pull(p);
}

void __dtfl_push_destroy(__dtfl_push *p);
void __dtfl_pull_destroy(__dtfl_pull *p);

typedef struct __dtfl_parker __dtfl_parker;

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

/*
 * The worker-local list of channel identifiers with activity
 * (upstream Rc<RefCell<Vec<usize>>>).  Pushers and pullers append; the
 * worker drains it in poll_events.
 */
typedef struct __dtfl_events {
	__dtfl_vec ids;	/* size_t */
} __dtfl_events;

static inline void
__dtfl_events_push(__dtfl_events *e, size_t id)
{
	__dtfl_vec_push(&e->ids, &id);
}

/* ------------------------------------------------------------------ */
/* FIFO and MPSC queues (VecDeque<T>, mpsc::channel<T>)                */
/* ------------------------------------------------------------------ */

/*
 * A FIFO of fixed-size elements, live in v[head .. v.len).  Not
 * synchronized: one owner (a worker, or whoever holds the lock around
 * it) uses it.
 */
typedef struct __dtfl_fifo {
	__dtfl_vec v;
	size_t head;
} __dtfl_fifo;

void __dtfl_fifo_init(__dtfl_fifo *q, size_t elem);
void __dtfl_fifo_push(__dtfl_fifo *q, const void *elem);
/* Pops the front element into *out; false when empty. */
bool __dtfl_fifo_pop(__dtfl_fifo *q, void *out);
void __dtfl_fifo_free(__dtfl_fifo *q);

static inline size_t
__dtfl_fifo_len(const __dtfl_fifo *q)
{
	return q->v.len - q->head;
}

/*
 * A many-producer, one-consumer queue between threads: a lock-free port
 * of the unbounded list channel behind std::sync::mpsc::channel at the
 * pin's toolchain (rust 1.99.1, library/std/src/sync/mpmc/list.rs).
 * Messages live in a linked list of blocks of 31 slots.  A sender
 * reserves a slot by advancing the tail index with a CAS, copies the
 * element in, and sets the slot's WRITE bit with a Release fetch_or;
 * the receiver advances the head index and waits for WRITE with an
 * Acquire load before copying the element out.  That pair makes a value
 * sent happen-before the receive that returns it.  Senders never take a
 * lock; a sender or receiver spins (then yields) only while another
 * thread is inside a few instructions of installing a block or writing
 * a reserved slot.  Reference counted (atomic): every Sender clone and
 * the Receiver hold one reference.
 *
 * One receiver: recv, drain and close are called by one thread at a
 * time.  Any number of threads may send concurrently.
 *
 * After __dtfl_mpsc_close (the Receiver dropped), sends fail and the
 * sender keeps ownership of the value, as mpsc::Sender::send returns
 * it in the error.  Close sets the disconnected mark on the tail, so
 * every send is ordered either before it (the element is queued, and
 * close drops it) or after it (send returns false).
 */
typedef struct __dtfl_mpsc __dtfl_mpsc;

/*
 * `drop` (may be NULL) disposes of an element the receiver never takes:
 * those queued at close, and those left when the last reference goes.
 */
__dtfl_mpsc *__dtfl_mpsc_new(size_t elem, void (*drop)(void *elem, void *ctx),
    void *ctx);
__dtfl_mpsc *__dtfl_mpsc_retain(__dtfl_mpsc *q);
void __dtfl_mpsc_release(__dtfl_mpsc *q);
/* Returns false (and does not take *elem) if the receiver closed. */
bool __dtfl_mpsc_send(__dtfl_mpsc *q, const void *elem);
/* try_recv: pops the front element into *out; false when empty. */
bool __dtfl_mpsc_recv(__dtfl_mpsc *q, void *out);
/*
 * Appends queued elements to `out` in send order until the queue is
 * empty (process.rs receive: `while let Ok(x) = try_recv()`).
 */
void __dtfl_mpsc_drain(__dtfl_mpsc *q, __dtfl_vec *out);
/*
 * Receiver side: the Receiver is dropped.  Queued elements are dropped
 * with `drop`, on the calling thread with no internal lock held, and
 * later sends fail.
 */
void __dtfl_mpsc_close(__dtfl_mpsc *q);

/* ------------------------------------------------------------------ */
/* Counters and canaries (counters.rs, canary.rs)                      */
/* ------------------------------------------------------------------ */

/*
 * counters.rs Pusher: appends `identifier` to `events` on every push
 * (data or flush), then pushes into `inner`.  Takes ownership of inner.
 * `events` belongs to the worker's allocator and must outlive the pusher.
 */
__dtfl_push *__dtfl_count_pusher_new(__dtfl_push *inner, size_t identifier,
    __dtfl_events *events);
/*
 * counters.rs Puller: counts messages pulled from `inner`, and when a
 * pull comes back empty after a nonempty run, appends `identifier` to
 * `events`.  Takes ownership of inner.
 */
__dtfl_pull *__dtfl_count_puller_new(__dtfl_pull *inner, size_t identifier,
    __dtfl_events *events);

/*
 * counters.rs ArcPusher: pushes into `inner`, then sends `identifier`
 * to the target worker's id queue (an __dtfl_mpsc of size_t), then
 * unparks the target, in that order (upstream: any other order is
 * defective with several threads).  Takes ownership of inner and
 * retains `ids` and `parker`.
 */
__dtfl_push *__dtfl_arc_pusher_new(__dtfl_push *inner, size_t identifier,
    __dtfl_mpsc *ids, __dtfl_parker *parker);

/*
 * canary.rs: reports `index` into a worker-local list when dropped.
 * `queue` (size_t elements) belongs to the allocator and must outlive
 * the canary.
 */
typedef struct __dtfl_canary {
	size_t index;
	__dtfl_vec *queue;
} __dtfl_canary;

static inline void
__dtfl_canary_init(__dtfl_canary *c, size_t index, __dtfl_vec *queue)
{
	c->index = index;
	c->queue = queue;
}

void __dtfl_canary_drop(__dtfl_canary *c);

/* ------------------------------------------------------------------ */
/* Buzzer (thread wakeup)                                              */
/* ------------------------------------------------------------------ */

/*
 * A parker owned by one worker thread.  Any thread may buzz it; only
 * the owner parks.  A buzz before park makes the next park return
 * immediately (the semantics of std::thread::park/unpark).
 * Shared by reference count across threads.  Sleeps through the sync
 * layer (src/base/os_sync.h): a futex-style word wait where one exists,
 * elsewhere a mutex and condition variable.
 */

__dtfl_parker *__dtfl_parker_new(void);
__dtfl_parker *__dtfl_parker_retain(__dtfl_parker *p);
void __dtfl_parker_release(__dtfl_parker *p);
/* Unpark: wake the owner (or make its next park return at once). */
void __dtfl_parker_unpark(__dtfl_parker *p);
/* Park for at most timeout_ns; UINT64_MAX waits indefinitely. */
void __dtfl_parker_park(__dtfl_parker *p, uint64_t timeout_ns);

/* ------------------------------------------------------------------ */
/* Allocators                                                          */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_allocator __dtfl_allocator;

typedef struct __dtfl_allocator_ops {
	/*
	 * Allocate::allocate.  Returns `peers` pushers (one per target
	 * worker, indexed by worker) in pushers[] and this worker's puller.
	 * Channel identifiers must be allocated in the same order on every
	 * worker; byte allocators assert they increase.
	 */
	void (*allocate)(__dtfl_allocator *a, size_t identifier,
	    const __dtfl_msg_class *mc, __dtfl_push **pushers,
	    __dtfl_pull **puller);
	/*
	 * Allocate::broadcast override (TcpAllocator has one).  NULL uses
	 * the trait default: allocate, then wrap the pushers in a
	 * Broadcaster.
	 */
	void (*broadcast)(__dtfl_allocator *a, size_t identifier,
	    const __dtfl_msg_class *mc, __dtfl_push **pusher,
	    __dtfl_pull **puller);
	/* Moves cross-thread or network arrivals into local queues. */
	void (*receive)(__dtfl_allocator *a);
	/* Publishes buffered outgoing data. */
	void (*release)(__dtfl_allocator *a);
	/* Parks until an event arrives or timeout (UINT64_MAX: forever). */
	void (*await_events)(__dtfl_allocator *a, uint64_t timeout_ns);
	void (*destroy)(__dtfl_allocator *a);
} __dtfl_allocator_ops;

/*
 * The common head of every allocator.  Pushers and pullers point into
 * `events`, so every channel endpoint must be destroyed before its
 * allocator (Rust keeps the events alive with an Rc; C makes the order
 * a contract).
 */
struct __dtfl_allocator {
	const __dtfl_allocator_ops *ops;
	size_t index;	/* this worker, 0 .. peers-1 */
	size_t peers;
	__dtfl_events events;
	/* Wakes this worker; shared with remote pushers (counters.rs). */
	__dtfl_parker *parker;
};

/* Fills the common head; takes ownership of one parker reference. */
void __dtfl_allocator_init(__dtfl_allocator *a, const __dtfl_allocator_ops *ops,
    size_t index, size_t peers, __dtfl_parker *parker);
/* Releases the common head (events, parker); for ops->destroy. */
void __dtfl_allocator_fini(__dtfl_allocator *a);
/*
 * thread.rs await_events, shared by the allocators whose events fill
 * from other threads: parks only if no events are pending.
 */
void __dtfl_allocator_park_if_idle(__dtfl_allocator *a, uint64_t timeout_ns);

void __dtfl_allocate(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pushers, __dtfl_pull **puller);
/*
 * Allocate::broadcast: one pusher that clones to every worker (the
 * first gets the original), and this worker's puller.
 */
void __dtfl_broadcast(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pusher, __dtfl_pull **puller);
/*
 * mod.rs Broadcaster: pushes a clone to pushers[1..n) and the original
 * to pushers[0].  Takes ownership of the pushers array (allocated with
 * __dtfl_alloc) and of every pusher in it.
 */
__dtfl_push *__dtfl_broadcaster_new(const __dtfl_msg_class *mc,
    __dtfl_push **pushers, size_t n);
/*
 * Allocator::pipeline: a worker-local channel (thread.rs new_from) that
 * never leaves the worker.  Messages are not encoded; `mc` frees any
 * still queued when the channel is destroyed (Rust's Drop for T).
 */
void __dtfl_pipeline(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pusher, __dtfl_pull **puller);
/*
 * Thread::new_from: a counting worker-local channel on `events`.
 */
void __dtfl_thread_new_from(size_t identifier, const __dtfl_msg_class *mc,
    __dtfl_events *events, __dtfl_push **pusher, __dtfl_pull **puller);
void __dtfl_allocator_receive(__dtfl_allocator *a);
void __dtfl_allocator_release(__dtfl_allocator *a);
void __dtfl_allocator_await_events(__dtfl_allocator *a, uint64_t timeout_ns);
void __dtfl_allocator_destroy(__dtfl_allocator *a);

/*
 * Builders.  A builder is created on the launching thread and built on
 * the worker thread (AllocateBuilder: Send), exactly once.
 */
typedef struct __dtfl_allocator_builder __dtfl_allocator_builder;

/* zero_copy/spill.rs SpillPolicyFn; defined in zc_int.h. */
typedef struct __dtfl_spill_fn __dtfl_spill_fn;

struct __dtfl_allocator_builder {
	__dtfl_allocator *(*build)(__dtfl_allocator_builder *b);
	/* Releases an unbuilt builder. */
	void (*destroy)(__dtfl_allocator_builder *b);
};

__dtfl_allocator *__dtfl_allocator_build(__dtfl_allocator_builder *b);

/* thread.rs: one worker. */
__dtfl_allocator_builder *__dtfl_thread_builder(void);

/*
 * process.rs (typed, moves pointers) and zero_copy/allocator_process.rs
 * (bytes, encodes): `peers` builders sharing one rendezvous.  The
 * caller hands builders[i] to worker thread i.
 */
void __dtfl_process_builders(size_t peers, __dtfl_allocator_builder **builders);
/*
 * `spill` (borrowed; NULL for none) supplies a policy pair for every
 * MergeQueue the builders create (Hooks::spill).
 */
void __dtfl_process_binary_builders(size_t peers, __dtfl_spill_fn *spill,
    __dtfl_allocator_builder **builders);

/*
 * Builders hold one reference to their shared state; an unbuilt builder
 * releases it in destroy.  Process allocators do not rendezvous at
 * build time: every worker's parker is created with the builders, so
 * a builder never blocks (upstream exchanges Buzzers in build, which
 * blocks until every peer has built; nothing depends on that wait).
 */

/*
 * zero_copy/allocator.rs: workers threads..threads*... of a cluster.
 * Requires the network to be initialized (initialize.c); see
 * __dtfl_cluster_builders.
 */

#endif /* DTFL_COMM_INT_H */
