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
 *   push(&msg): msg != NULL sends it (ownership moves to the channel)
 *               and sets *msg to a recycled element or NULL;
 *               msg == NULL (via __dtfl_push_done) signals a flush.
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

#include <pthread.h>

#include "base/base_int.h"
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
	/* Upstream Push::push.  `msg` may be NULL for done(). */
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
/* Push::done: flush. */
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
/* Buzzer (thread wakeup)                                              */
/* ------------------------------------------------------------------ */

/*
 * A parker owned by one worker thread.  Any thread may buzz it; only
 * the owner parks.  A buzz before park makes the next park return
 * immediately (the semantics of std::thread::park/unpark).
 * Shared by reference count across threads.
 */
typedef struct __dtfl_parker __dtfl_parker;

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
	/* Moves cross-thread or network arrivals into local queues. */
	void (*receive)(__dtfl_allocator *a);
	/* Publishes buffered outgoing data. */
	void (*release)(__dtfl_allocator *a);
	/* Parks until an event arrives or timeout (UINT64_MAX: forever). */
	void (*await_events)(__dtfl_allocator *a, uint64_t timeout_ns);
	void (*destroy)(__dtfl_allocator *a);
} __dtfl_allocator_ops;

struct __dtfl_allocator {
	const __dtfl_allocator_ops *ops;
	size_t index;	/* this worker, 0 .. peers-1 */
	size_t peers;
	__dtfl_events events;
	/* Wakes this worker; shared with remote pushers (counters.rs). */
	__dtfl_parker *parker;
};

void __dtfl_allocate(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pushers, __dtfl_pull **puller);
/*
 * Allocate::broadcast: one pusher that clones to every worker (the
 * first gets the original), and this worker's puller.
 */
void __dtfl_broadcast(__dtfl_allocator *a, size_t identifier,
    const __dtfl_msg_class *mc, __dtfl_push **pusher, __dtfl_pull **puller);
/*
 * Allocator::pipeline: a worker-local channel (thread.rs new_from) that
 * never leaves the worker.  Messages are not encoded.
 */
void __dtfl_pipeline(__dtfl_allocator *a, size_t identifier,
    __dtfl_push **pusher, __dtfl_pull **puller);
void __dtfl_allocator_receive(__dtfl_allocator *a);
void __dtfl_allocator_release(__dtfl_allocator *a);
void __dtfl_allocator_await_events(__dtfl_allocator *a, uint64_t timeout_ns);
void __dtfl_allocator_destroy(__dtfl_allocator *a);

/*
 * Builders.  A builder is created on the launching thread and built on
 * the worker thread (AllocateBuilder: Send), exactly once.
 */
typedef struct __dtfl_allocator_builder __dtfl_allocator_builder;

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
void __dtfl_process_binary_builders(size_t peers,
    __dtfl_allocator_builder **builders);

/*
 * zero_copy/allocator.rs: workers threads..threads*... of a cluster.
 * Requires the network to be initialized (initialize.c); see
 * __dtfl_cluster_builders.
 */

#endif /* DTFL_COMM_INT_H */
