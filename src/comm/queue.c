/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/queue.c: the FIFO (VecDeque) and the many-producer,
 * one-consumer queue (std::sync::mpsc::channel) that the typed
 * allocators are built from.  See comm_int.h for the contracts.
 */
#include "comm/comm_int.h"

#include <sched.h>
#include <stddef.h>

void
__dtfl_fifo_init(__dtfl_fifo *q, size_t elem)
{
	__dtfl_vec_init(&q->v, elem);
	q->head = 0;
}

void
__dtfl_fifo_push(__dtfl_fifo *q, const void *elem)
{
	/*
	 * Reclaim the consumed prefix once it is at least half the vector,
	 * so a queue that never quite empties stays O(live) in size.
	 */
	if (q->head > 0 && q->head >= q->v.len / 2) {
		memmove(q->v.data, q->v.data + q->head * q->v.elem,
		    (q->v.len - q->head) * q->v.elem);
		q->v.len -= q->head;
		q->head = 0;
	}
	__dtfl_vec_push(&q->v, elem);
}

bool
__dtfl_fifo_pop(__dtfl_fifo *q, void *out)
{
	if (q->head == q->v.len)
		return false;
	memcpy(out, q->v.data + q->head * q->v.elem, q->v.elem);
	if (++q->head == q->v.len) {
		q->head = 0;
		q->v.len = 0;
	}
	return true;
}

void
__dtfl_fifo_free(__dtfl_fifo *q)
{
	__dtfl_vec_free(&q->v);
	q->head = 0;
}

/* ------------------------------------------------------------------ */
/* The MPSC queue                                                      */
/* ------------------------------------------------------------------ */

/*
 * Ported from rust 1.99.1 (58cf8f9c3 2026-10-13)
 *   library/std/src/sync/mpmc/list.rs (the list flavor that backs
 *   std::sync::mpsc::channel), and Backoff and CachePadded from
 *   library/std/src/sync/mpmc/utils.rs
 * Copyright (c) 2019 The Crossbeam Project Developers.
 * Copyright (c) The Rust Project Developers.
 * Licensed MIT OR Apache-2.0; used here under the MIT license.
 *
 * The algorithm, constants and memory orders are list.rs's.  The C
 * differences:
 *
 * - T becomes `elem` bytes, moved in and out with memcpy.  A slot is
 *   its state word followed by the payload, so a block is one
 *   allocation of a header and BLOCK_CAP slots of q->slot_size bytes.
 *   Blocks come from __dtfl_calloc (zeroed, as Block::new's
 *   Box::new_zeroed) and go back through __dtfl_free.
 * - Dropping a message (drop_in_place) calls q->drop, if set.
 * - There is no SyncWaker: libdtfl never blocks in recv (only try_recv
 *   is ported), and callers wake the consumer through its parker.
 * - The Channel's Arc (counter.rs) is `refs`; the last release runs
 *   Drop for Channel.
 */

/*
 * Bits indicating the state of a slot:
 * - If a message has been written into the slot, `WRITE` is set.
 * - If a message has been read from the slot, `READ` is set.
 * - If the block is being destroyed, `DESTROY` is set.
 * (Upstream's WRITE, READ and DESTROY, prefixed here so they cannot
 * collide with a system header's macros.)
 */
#define SLOT_WRITE ((size_t)1)
#define SLOT_READ ((size_t)2)
#define SLOT_DESTROY ((size_t)4)

/* Each block covers one "lap" of indices. */
#define LAP ((size_t)32)
/* The maximum number of messages a block can hold. */
#define BLOCK_CAP (LAP - 1)
/* How many lower bits are reserved for metadata. */
#define SHIFT 1
/*
 * Has two different purposes:
 * - If set in head, indicates that the block is not the last one.
 * - If set in tail, indicates that the channel is disconnected.
 */
#define MARK_BIT ((size_t)1)

/*
 * CachePadded: 128 bytes on x86_64, aarch64 and powerpc64 (utils.rs),
 * used here on every target.  The queue comes from __dtfl_alloc, which
 * does not align to 128, so the padding keeps head and tail at least
 * 112 bytes apart instead: never on the same 64-byte line.
 */
#define CACHE_PAD 128

/* utils.rs SPIN_LIMIT. */
#define SPIN_LIMIT 6

/*
 * Backoff (utils.rs): spin_light spins step^2 times; spin_heavy spins
 * the same way until step passes SPIN_LIMIT, then yields the CPU with
 * sched_yield (thread::yield_now).  There is no parking: every wait
 * here is for another thread's few instructions in progress.
 */
typedef struct backoff {
	unsigned step;
} backoff;

static inline void
spin_loop(void)
{
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
	__builtin_ia32_pause();
#elif defined(__GNUC__) && defined(__aarch64__)
	__asm__ __volatile__("yield" ::: "memory");
#endif
}

static inline void
spin_light(backoff *b)
{
	unsigned step = b->step < SPIN_LIMIT ? b->step : SPIN_LIMIT;
	unsigned i;

	for (i = 0; i < step * step; i++)
		spin_loop();
	b->step++;
}

static inline void
spin_heavy(backoff *b)
{
	unsigned i;

	if (b->step <= SPIN_LIMIT)
		for (i = 0; i < b->step * b->step; i++)
			spin_loop();
	else
		sched_yield();
	b->step++;
}

/*
 * A slot in a block: the state word, then the message at q->msg_off.
 * The message is plain memory, written only by the sender that
 * reserved the slot, before its Release fetch_or of SLOT_WRITE, and read
 * only after an Acquire load sees SLOT_WRITE.
 */
typedef struct slot {
	atomic_size_t state;
} slot;

/*
 * A block in a linked list.  BLOCK_CAP slots follow the header, at
 * SLOTS_OFF.  Each block in the list can hold up to BLOCK_CAP messages.
 */
typedef struct block {
	/* The next block in the linked list. */
	_Atomic(struct block *) next;
} block;

#define SLOTS_OFF \
	((sizeof(block) + _Alignof(max_align_t) - 1) & \
	    ~(_Alignof(max_align_t) - 1))

/* A position in a channel. */
typedef struct position {
	/* The index in the channel. */
	atomic_size_t index;
	/* The block in the linked list. */
	_Atomic(block *) block;
} position;

typedef union padded_position {
	position v;
	char pad[CACHE_PAD];
} padded_position;

/* ListToken: the reserved slot, or block NULL if disconnected. */
typedef struct list_token {
	block *block;
	size_t offset;
} list_token;

/*
 * Unbounded channel implemented as a linked list.
 *
 * Each message sent into the channel is assigned a sequence number,
 * i.e. an index.  Indices are represented as numbers of type size_t and
 * wrap on overflow.
 *
 * Consecutive messages are grouped into blocks in order to put less
 * pressure on the allocator and improve cache efficiency.
 */
struct __dtfl_mpsc {
	/* The head of the channel.  Written by the one receiver. */
	padded_position head;
	/* The tail of the channel.  Written by the senders and close. */
	padded_position tail;

	/*
	 * Senders plus the receiver.  acq_rel on decrement: the thread
	 * that frees sees every other holder's accesses.
	 */
	atomic_size_t refs;
	/* Read-only after __dtfl_mpsc_new. */
	size_t elem;		/* message bytes */
	size_t msg_off;		/* message offset within a slot */
	size_t slot_size;	/* slot stride */
	size_t block_size;
	void (*drop)(void *elem, void *ctx);
	void *ctx;
};

static inline slot *
slot_at(const __dtfl_mpsc *q, block *b, size_t i)
{
	return (slot *)(void *)((unsigned char *)b + SLOTS_OFF +
	    i * q->slot_size);
}

static inline unsigned char *
slot_msg(const __dtfl_mpsc *q, slot *s)
{
	return (unsigned char *)s + q->msg_off;
}

/* Slot::wait_write: waits until a message is written into the slot. */
static void
wait_write(slot *s)
{
	backoff b = { 0 };

	while ((atomic_load_explicit(&s->state, memory_order_acquire) &
	    SLOT_WRITE) == 0)
		spin_heavy(&b);
}

/* Block::new: creates an empty block (all zeros: next NULL, states 0). */
static block *
block_new(const __dtfl_mpsc *q)
{
	return __dtfl_calloc(1, q->block_size);
}

/* Block::wait_next: waits until the next pointer is set. */
static block *
wait_next(block *b)
{
	backoff bo = { 0 };
	block *next;

	for (;;) {
		next = atomic_load_explicit(&b->next, memory_order_acquire);
		if (next != NULL)
			return next;
		spin_heavy(&bo);
	}
}

/*
 * Block::destroy: sets the SLOT_DESTROY bit in slots starting from `start`
 * and destroys the block.
 */
static void
block_destroy(const __dtfl_mpsc *q, block *this, size_t start)
{
	size_t i;

	/*
	 * It is not necessary to set the SLOT_DESTROY bit in the last slot
	 * because that slot has begun destruction of the block.
	 */
	for (i = start; i < BLOCK_CAP - 1; i++) {
		slot *s = slot_at(q, this, i);

		/* Mark the SLOT_DESTROY bit if a thread is still using the slot. */
		if ((atomic_load_explicit(&s->state, memory_order_acquire) &
		    SLOT_READ) == 0 &&
		    (atomic_fetch_or_explicit(&s->state, SLOT_DESTROY,
		    memory_order_acq_rel) & SLOT_READ) == 0) {
			/*
			 * If a thread is still using the slot, it will
			 * continue destruction of the block.
			 */
			return;
		}
	}

	/* No thread is using the block, now it is safe to destroy it. */
	__dtfl_free(this);
}

__dtfl_mpsc *
__dtfl_mpsc_new(size_t elem, void (*drop)(void *, void *), void *ctx)
{
	__dtfl_mpsc *q = __dtfl_alloc(sizeof *q);
	/*
	 * A type's alignment divides its size, so the largest power of two
	 * dividing `elem` (capped at max_align_t) aligns any `elem`-byte
	 * object.  The state word needs size_t alignment.
	 */
	size_t align = elem & -elem;

	if (align > _Alignof(max_align_t) || align == 0)
		align = _Alignof(max_align_t);
	if (align < _Alignof(atomic_size_t))
		align = _Alignof(atomic_size_t);
	/* Channel::new: both positions start at index 0, block NULL. */
	memset(q, 0, sizeof *q);
	atomic_init(&q->head.v.index, 0);
	atomic_init(&q->head.v.block, NULL);
	atomic_init(&q->tail.v.index, 0);
	atomic_init(&q->tail.v.block, NULL);
	atomic_init(&q->refs, 1);
	q->elem = elem;
	q->msg_off = (sizeof(slot) + align - 1) & ~(align - 1);
	q->slot_size = (q->msg_off + elem + align - 1) & ~(align - 1);
	q->block_size = SLOTS_OFF + __dtfl_mul_size(BLOCK_CAP, q->slot_size);
	q->drop = drop;
	q->ctx = ctx;
	return q;
}

__dtfl_mpsc *
__dtfl_mpsc_retain(__dtfl_mpsc *q)
{
	/* relaxed: the caller already holds a reference. */
	atomic_fetch_add_explicit(&q->refs, 1, memory_order_relaxed);
	return q;
}

/*
 * Drop for Channel.  The caller is the last holder, so no other thread
 * touches the queue and the Relaxed loads are list.rs's.
 */
static void
channel_drop(__dtfl_mpsc *q)
{
	size_t head = atomic_load_explicit(&q->head.v.index,
	    memory_order_relaxed);
	size_t tail = atomic_load_explicit(&q->tail.v.index,
	    memory_order_relaxed);
	block *b = atomic_load_explicit(&q->head.v.block,
	    memory_order_relaxed);

	/* Erase the lower bits. */
	head &= ~(((size_t)1 << SHIFT) - 1);
	tail &= ~(((size_t)1 << SHIFT) - 1);

	/*
	 * Drop all messages between head and tail and deallocate the
	 * heap-allocated blocks.
	 */
	while (head != tail) {
		size_t offset = (head >> SHIFT) % LAP;

		if (offset < BLOCK_CAP) {
			/* Drop the message in the slot. */
			if (q->drop != NULL)
				q->drop(slot_msg(q, slot_at(q, b, offset)),
				    q->ctx);
		} else {
			/* Deallocate the block and move to the next one. */
			block *next = atomic_load_explicit(&b->next,
			    memory_order_relaxed);

			__dtfl_free(b);
			b = next;
		}
		head += (size_t)1 << SHIFT;
	}

	/* Deallocate the last remaining block. */
	if (b != NULL)
		__dtfl_free(b);
}

void
__dtfl_mpsc_release(__dtfl_mpsc *q)
{
	if (q == NULL)
		return;
	if (atomic_fetch_sub_explicit(&q->refs, 1, memory_order_acq_rel) != 1)
		return;
	channel_drop(q);
	__dtfl_free(q);
}

/* Channel::start_send: attempts to reserve a slot for sending a message. */
static bool
start_send(__dtfl_mpsc *q, list_token *token)
{
	backoff bo = { 0 };
	size_t tail = atomic_load_explicit(&q->tail.v.index,
	    memory_order_acquire);
	block *b = atomic_load_explicit(&q->tail.v.block,
	    memory_order_acquire);
	block *next_block = NULL;	/* Option<Box<Block>>: freed if unused */

	for (;;) {
		size_t offset, new_tail, expected;

		/* Check if the channel is disconnected. */
		if ((tail & MARK_BIT) != 0) {
			token->block = NULL;
			__dtfl_free(next_block);
			return true;
		}

		/* Calculate the offset of the index into the block. */
		offset = (tail >> SHIFT) % LAP;

		/*
		 * If we reached the end of the block, wait until the next
		 * one is installed.
		 */
		if (offset == BLOCK_CAP) {
			spin_heavy(&bo);
			tail = atomic_load_explicit(&q->tail.v.index,
			    memory_order_acquire);
			b = atomic_load_explicit(&q->tail.v.block,
			    memory_order_acquire);
			continue;
		}

		/*
		 * If we're going to have to install the next block, allocate
		 * it in advance in order to make the wait for other threads
		 * as short as possible.
		 */
		if (offset + 1 == BLOCK_CAP && next_block == NULL)
			next_block = block_new(q);

		/*
		 * If this is the first message to be sent into the channel,
		 * we need to allocate the first block and install it.
		 */
		if (b == NULL) {
			block *new = block_new(q);
			block *cur = b;

			if (atomic_compare_exchange_strong_explicit(
			    &q->tail.v.block, &cur, new,
			    memory_order_release, memory_order_relaxed)) {
				atomic_store_explicit(&q->head.v.block, new,
				    memory_order_release);
				b = new;
			} else {
				__dtfl_free(next_block);
				next_block = new;
				tail = atomic_load_explicit(&q->tail.v.index,
				    memory_order_acquire);
				b = atomic_load_explicit(&q->tail.v.block,
				    memory_order_acquire);
				continue;
			}
		}

		new_tail = tail + ((size_t)1 << SHIFT);

		/* Try advancing the tail forward. */
		expected = tail;
		if (atomic_compare_exchange_weak_explicit(&q->tail.v.index,
		    &expected, new_tail, memory_order_seq_cst,
		    memory_order_acquire)) {
			/*
			 * If we've reached the end of the block, install the
			 * next one.
			 */
			if (offset + 1 == BLOCK_CAP) {
				atomic_store_explicit(&q->tail.v.block,
				    next_block, memory_order_release);
				atomic_fetch_add_explicit(&q->tail.v.index,
				    (size_t)1 << SHIFT, memory_order_release);
				atomic_store_explicit(&b->next, next_block,
				    memory_order_release);
				next_block = NULL;
			}
			__dtfl_free(next_block);
			token->block = b;
			token->offset = offset;
			return true;
		}
		spin_light(&bo);
		tail = atomic_load_explicit(&q->tail.v.index,
		    memory_order_acquire);
		b = atomic_load_explicit(&q->tail.v.block,
		    memory_order_acquire);
	}
}

/*
 * Channel::write: writes a message into the channel.  False (the
 * sender keeps the message) if the channel is disconnected.
 */
static bool
channel_write(__dtfl_mpsc *q, const list_token *token, const void *msg)
{
	slot *s;

	/* If there is no slot, the channel is disconnected. */
	if (token->block == NULL)
		return false;

	/* Write the message into the slot. */
	s = slot_at(q, token->block, token->offset);
	memcpy(slot_msg(q, s), msg, q->elem);
	/* Pairs with the Acquire load in wait_write. */
	atomic_fetch_or_explicit(&s->state, SLOT_WRITE, memory_order_release);

	/* receivers.notify(): no receiver blocks here (see above). */
	return true;
}

/*
 * Channel::start_recv: attempts to reserve a slot for receiving a
 * message.  False when the channel is empty.
 */
static bool
start_recv(__dtfl_mpsc *q, list_token *token)
{
	backoff bo = { 0 };
	size_t head = atomic_load_explicit(&q->head.v.index,
	    memory_order_acquire);
	block *b = atomic_load_explicit(&q->head.v.block,
	    memory_order_acquire);

	for (;;) {
		size_t offset, new_head, expected;

		/* Calculate the offset of the index into the block. */
		offset = (head >> SHIFT) % LAP;

		/*
		 * If we reached the end of the block, wait until the next
		 * one is installed.
		 */
		if (offset == BLOCK_CAP) {
			spin_heavy(&bo);
			head = atomic_load_explicit(&q->head.v.index,
			    memory_order_acquire);
			b = atomic_load_explicit(&q->head.v.block,
			    memory_order_acquire);
			continue;
		}

		new_head = head + ((size_t)1 << SHIFT);

		if ((new_head & MARK_BIT) == 0) {
			size_t tail;

#if defined(__SANITIZE_THREAD__) && !defined(__clang__) && __GNUC__ >= 12
/*
 * gcc's TSan does not model fences (-Wtsan).  Ignoring one can only
 * drop happens-before edges, so it could cause a false report, never
 * hide a race.  This fence orders the head and tail index loads; the
 * data a reader sees is published by the Release/Acquire pairs on
 * head.block and the slot state, which TSan does see.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtsan"
#endif
			atomic_thread_fence(memory_order_seq_cst);
#if defined(__SANITIZE_THREAD__) && !defined(__clang__) && __GNUC__ >= 12
#pragma GCC diagnostic pop
#endif
			tail = atomic_load_explicit(&q->tail.v.index,
			    memory_order_relaxed);

			/*
			 * If the tail equals the head, that means the channel
			 * is empty.
			 */
			if (head >> SHIFT == tail >> SHIFT) {
				/* If the channel is disconnected... */
				if ((tail & MARK_BIT) != 0) {
					/* ...then receive an error. */
					token->block = NULL;
					return true;
				}
				/*
				 * Otherwise, the receive operation is not
				 * ready.
				 */
				return false;
			}

			/*
			 * If head and tail are not in the same block, set
			 * MARK_BIT in head.
			 */
			if ((head >> SHIFT) / LAP != (tail >> SHIFT) / LAP)
				new_head |= MARK_BIT;
		}

		/*
		 * The block can be null here only if the first message is
		 * being sent into the channel.  In that case, just wait until
		 * it gets initialized.
		 */
		if (b == NULL) {
			spin_heavy(&bo);
			head = atomic_load_explicit(&q->head.v.index,
			    memory_order_acquire);
			b = atomic_load_explicit(&q->head.v.block,
			    memory_order_acquire);
			continue;
		}

		/*
		 * Try moving the head index forward.  With one receiver
		 * (which also runs close) nothing else writes head.index, so
		 * only a spurious failure retries; it stays list.rs's CAS.
		 */
		expected = head;
		if (atomic_compare_exchange_weak_explicit(&q->head.v.index,
		    &expected, new_head, memory_order_seq_cst,
		    memory_order_acquire)) {
			/*
			 * If we've reached the end of the block, move to the
			 * next one.
			 */
			if (offset + 1 == BLOCK_CAP) {
				block *next = wait_next(b);
				size_t next_index = (new_head & ~MARK_BIT) +
				    ((size_t)1 << SHIFT);

				if (atomic_load_explicit(&next->next,
				    memory_order_relaxed) != NULL)
					next_index |= MARK_BIT;

				atomic_store_explicit(&q->head.v.block, next,
				    memory_order_release);
				atomic_store_explicit(&q->head.v.index,
				    next_index, memory_order_release);
			}

			token->block = b;
			token->offset = offset;
			return true;
		}
		spin_light(&bo);
		head = atomic_load_explicit(&q->head.v.index,
		    memory_order_acquire);
		b = atomic_load_explicit(&q->head.v.block,
		    memory_order_acquire);
	}
}

/*
 * Channel::read: reads a message from the channel into *out.  False if
 * the channel is disconnected.
 */
static bool
channel_read(__dtfl_mpsc *q, const list_token *token, void *out)
{
	block *b = token->block;
	size_t offset = token->offset;
	slot *s;

	if (b == NULL) {
		/* The channel is disconnected. */
		return false;
	}

	/* Read the message. */
	s = slot_at(q, b, offset);
	wait_write(s);
	memcpy(out, slot_msg(q, s), q->elem);

	/*
	 * Destroy the block if we've reached the end, or if another thread
	 * wanted to destroy but couldn't because we were busy reading from
	 * the slot.
	 */
	if (offset + 1 == BLOCK_CAP)
		block_destroy(q, b, 0);
	else if ((atomic_fetch_or_explicit(&s->state, SLOT_READ,
	    memory_order_acq_rel) & SLOT_DESTROY) != 0)
		block_destroy(q, b, offset + 1);
	return true;
}

bool
__dtfl_mpsc_send(__dtfl_mpsc *q, const void *elem)
{
	list_token token;

	/* Channel::send: start_send always succeeds for the list flavor. */
	(void)start_send(q, &token);
	return channel_write(q, &token, elem);
}

bool
__dtfl_mpsc_recv(__dtfl_mpsc *q, void *out)
{
	list_token token;

	/* Channel::try_recv: Empty and Disconnected are both false. */
	if (start_recv(q, &token))
		return channel_read(q, &token, out);
	return false;
}

void
__dtfl_mpsc_drain(__dtfl_mpsc *q, __dtfl_vec *out)
{
	DTFL_ASSERT(out->elem == q->elem);
	/* process.rs receive: while let Ok(index) = try_recv() { push }. */
	for (;;) {
		__dtfl_vec_reserve(out, 1);
		if (!__dtfl_mpsc_recv(q, out->data + out->len * out->elem))
			break;
		out->len++;
	}
}

/*
 * Channel::discard_all_messages: discards all messages.  This should
 * only be called when all receivers are dropped.
 */
static void
discard_all_messages(__dtfl_mpsc *q)
{
	backoff bo = { 0 };
	size_t tail = atomic_load_explicit(&q->tail.v.index,
	    memory_order_acquire);
	size_t head;
	block *b;

	for (;;) {
		size_t offset = (tail >> SHIFT) % LAP;

		if (offset != BLOCK_CAP)
			break;

		/*
		 * New updates to tail will be rejected by MARK_BIT and
		 * aborted unless it's at boundary.  We need to wait for the
		 * updates take affect otherwise there can be memory leaks.
		 */
		spin_heavy(&bo);
		tail = atomic_load_explicit(&q->tail.v.index,
		    memory_order_acquire);
	}

	head = atomic_load_explicit(&q->head.v.index, memory_order_acquire);
	/*
	 * The channel may be uninitialized, so we have to swap to avoid
	 * overwriting any sender's attempts to initialize the first block
	 * before noticing that the receivers disconnected.  Late
	 * allocations will be deallocated by the sender in Drop.
	 */
	b = atomic_exchange_explicit(&q->head.v.block, NULL,
	    memory_order_acq_rel);

	/*
	 * If we're going to be dropping messages we need to synchronize
	 * with initialization.
	 */
	if (head >> SHIFT != tail >> SHIFT) {
		/*
		 * The block can be null here only if a sender is in the
		 * process of initializing the channel while another sender
		 * managed to send a message by inserting it into the
		 * semi-initialized channel and advanced the tail.  In that
		 * case, just wait until it gets initialized.
		 */
		while (b == NULL) {
			spin_heavy(&bo);
			b = atomic_exchange_explicit(&q->head.v.block, NULL,
			    memory_order_acq_rel);
		}
	}
	/*
	 * After this point head.block is not modified again and it will be
	 * deallocated if it's non-null.  The Drop code of the channel,
	 * which runs after this function, also attempts to deallocate
	 * head.block if it's non-null.  Therefore this function must
	 * maintain the invariant that if a deallocation of head.block is
	 * attempted then it must also be set to NULL.  Failing to do so
	 * will lead to the Drop code attempting a double free.  For this
	 * reason both reads above do an atomic swap instead of a simple
	 * atomic load.
	 */

	/*
	 * Drop all messages between head and tail and deallocate the
	 * heap-allocated blocks.  q->drop is the user's: no lock is held.
	 */
	while (head >> SHIFT != tail >> SHIFT) {
		size_t offset = (head >> SHIFT) % LAP;

		if (offset < BLOCK_CAP) {
			/* Drop the message in the slot. */
			slot *s = slot_at(q, b, offset);

			wait_write(s);
			if (q->drop != NULL)
				q->drop(slot_msg(q, s), q->ctx);
		} else {
			block *next;

			(void)wait_next(b);
			/* Deallocate the block and move to the next one. */
			next = atomic_load_explicit(&b->next,
			    memory_order_acquire);
			__dtfl_free(b);
			b = next;
		}
		head += (size_t)1 << SHIFT;
	}

	/* Deallocate the last remaining block. */
	if (b != NULL)
		__dtfl_free(b);

	head &= ~MARK_BIT;
	atomic_store_explicit(&q->head.v.index, head, memory_order_release);
}

void
__dtfl_mpsc_close(__dtfl_mpsc *q)
{
	/* Channel::disconnect_receivers. */
	size_t tail = atomic_fetch_or_explicit(&q->tail.v.index, MARK_BIT,
	    memory_order_seq_cst);

	if ((tail & MARK_BIT) == 0) {
		/*
		 * If receivers are dropped first, discard all messages to
		 * free memory eagerly.
		 */
		discard_all_messages(q);
	}
}
