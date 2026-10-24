/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/bytes_exchange.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/zc_bytes_exchange.c: MergeQueue and SendEndpoint.
 *
 * A MergeQueue is an unbounded queue of byte views for point-to-point
 * communication between two threads.  new_pair returns a writer handle
 * (owned by the producing thread) and a reader handle (owned by the
 * consuming thread) on one shared deque.  The writer merges adjacent
 * views of the same store as it appends, so a stream of small writes
 * into one slab reaches the reader as one large view.
 *
 * Synchronization:
 *   - `lock` guards `entries`.  Every entry move happens under it, so
 *     the unlock in extend synchronizes with the lock in drain_into,
 *     and the bytes written before extend are visible to the reader.
 *   - `handles` counts live handles (upstream Arc::strong_count).  A
 *     drop decrements it with release order before buzzing; the
 *     acquire load in is_complete pairs with it, so a reader that sees
 *     1 also sees every entry the dropped writer pushed.
 *   - `ref` keeps the shared state alive until the last handle has
 *     finished buzzing, and is released after the buzz.
 *   - `panic` poisons the pair (unclean shutdown).  Sequentially
 *     consistent, as upstream's SeqCst.
 */
#include "comm/zc_int.h"

/* ------------------------------------------------------------------ */
/* VecDeque<QueueEntry>                                                */
/* ------------------------------------------------------------------ */

/*
 * A sliding array, not a ring.  pop_front advances head and push_back
 * compacts when the tail is full, so each entry moves O(1) times
 * amortized; insert/remove in the middle are O(n), which only the
 * prefetch policy uses near the front.
 */
void
__dtfl_qe_init(__dtfl_queue_entries *q)
{
	q->buf = NULL;
	q->head = 0;
	q->len = 0;
	q->cap = 0;
}

/* Drops one entry's payload (a view or a spill handle). */
void
__dtfl_qe_entry_drop(__dtfl_queue_entry *e)
{
	if (e->kind == DTFL_QE_BYTES)
		__dtfl_bytes_release(&e->u.bytes);
	else if (e->u.paged != NULL)
		e->u.paged->ops->destroy(e->u.paged);
	e->u.paged = NULL;
}

/* Drops every entry and the storage. */
void
__dtfl_qe_free(__dtfl_queue_entries *q)
{
	size_t i;

	for (i = 0; i < q->len; i++)
		__dtfl_qe_entry_drop(__dtfl_qe_at(q, i));
	__dtfl_free(q->buf);
	__dtfl_qe_init(q);
}

/* Makes room for one more entry at buf[head + len]. */
static void
qe_reserve_one(__dtfl_queue_entries *q)
{
	if (q->head + q->len < q->cap)
		return;
	if (q->head > 0) {
		memmove(q->buf, q->buf + q->head, q->len * sizeof *q->buf);
		q->head = 0;
		if (q->len < q->cap)
			return;
	}
	q->cap = q->cap < 8 ? 8 : q->cap * 2;
	q->buf = __dtfl_realloc(q->buf,
	    __dtfl_mul_size(q->cap, sizeof *q->buf));
}

/* Appends e; the deque takes ownership. */
void
__dtfl_qe_push_back(__dtfl_queue_entries *q, __dtfl_queue_entry e)
{
	qe_reserve_one(q);
	q->buf[q->head + q->len++] = e;
}

/* Removes the last entry; the caller takes ownership. */
__dtfl_queue_entry
__dtfl_qe_pop_back(__dtfl_queue_entries *q)
{
	DTFL_ASSERT(q->len > 0);
	return q->buf[q->head + --q->len];
}

/* Removes the first entry; the caller takes ownership. */
__dtfl_queue_entry
__dtfl_qe_pop_front(__dtfl_queue_entries *q)
{
	__dtfl_queue_entry e;

	DTFL_ASSERT(q->len > 0);
	e = q->buf[q->head];
	q->head++;
	if (--q->len == 0)
		q->head = 0;
	return e;
}

/* Inserts e at index i (0 <= i <= len), shifting later entries back. */
void
__dtfl_qe_insert(__dtfl_queue_entries *q, size_t i, __dtfl_queue_entry e)
{
	__dtfl_queue_entry *at;

	DTFL_ASSERT(i <= q->len);
	qe_reserve_one(q);
	at = &q->buf[q->head + i];
	memmove(at + 1, at, (q->len - i) * sizeof *at);
	*at = e;
	q->len++;
}

/* Removes and returns the entry at index i. */
__dtfl_queue_entry
__dtfl_qe_remove(__dtfl_queue_entries *q, size_t i)
{
	__dtfl_queue_entry *at, e;

	DTFL_ASSERT(i < q->len);
	at = &q->buf[q->head + i];
	e = *at;
	memmove(at, at + 1, (q->len - i - 1) * sizeof *at);
	q->len--;
	return e;
}

/* Wraps *b in a Bytes entry, moving it (b is zeroed). */
static __dtfl_queue_entry
qe_bytes(__dtfl_bytes *b)
{
	__dtfl_queue_entry e;

	e.kind = DTFL_QE_BYTES;
	e.u.bytes = *b;
	memset(b, 0, sizeof *b);
	return e;
}

/* ------------------------------------------------------------------ */
/* MergeQueue                                                          */
/* ------------------------------------------------------------------ */

typedef struct mq_shared {
	__dtfl_arc ref;		/* struct lifetime; one per handle */
	atomic_size_t handles;	/* live handles (Arc::strong_count) */
	atomic_bool panic;	/* poisons the queue */
	__dtfl_mutex lock;
	__dtfl_queue_entries entries;	/* guarded by lock */
	void (*buzz)(void *ctx);	/* awakens the receiver thread */
	void *ctx;
	void (*ctx_drop)(void *ctx);
} mq_shared;

struct __dtfl_merge_queue {
	mq_shared *shared;
	__dtfl_spill_policy *policy;	/* owned; NULL for none */
};

/* Panics if either handle dropped while panicking. */
static void
check_poison(mq_shared *s)
{
	if (atomic_load_explicit(&s->panic, memory_order_seq_cst))
		__dtfl_panic("MergeQueue poisoned.");
}

/* Takes the entries lock. */
static void
mq_lock(mq_shared *s)
{
	/*
	 * Upstream spins on try_lock to avoid yielding; a plain lock is
	 * equivalent in behavior.
	 */
	__dtfl_mutex_lock(&s->lock);
}

/* Releases the entries lock. */
static void
mq_unlock(mq_shared *s)
{
	__dtfl_mutex_unlock(&s->lock);
}

/*
 * MergeQueue::new_pair: a writer and a reader handle on one queue, each
 * owning its spill policy.  The writer runs its policy after each
 * extend; the reader runs its policy before each drain_into.
 */
void
__dtfl_merge_queue_new_pair(void (*buzz)(void *ctx), void *ctx,
    void (*ctx_drop)(void *ctx), __dtfl_spill_policy *writer_policy,
    __dtfl_spill_policy *reader_policy, __dtfl_merge_queue **writer,
    __dtfl_merge_queue **reader)
{
	mq_shared *s = __dtfl_alloc(sizeof *s);

	DTFL_ASSERT(buzz != NULL);
	__dtfl_arc_init(&s->ref);
	__dtfl_arc_retain(&s->ref);
	atomic_init(&s->handles, 2);
	atomic_init(&s->panic, false);
	__dtfl_mutex_init(&s->lock);
	__dtfl_qe_init(&s->entries);
	s->buzz = buzz;
	s->ctx = ctx;
	s->ctx_drop = ctx_drop;

	*writer = __dtfl_alloc(sizeof **writer);
	(*writer)->shared = s;
	(*writer)->policy = writer_policy;
	*reader = __dtfl_alloc(sizeof **reader);
	(*reader)->shared = s;
	(*reader)->policy = reader_policy;
}

/* True when every other handle has dropped and the queue is empty. */
bool
__dtfl_merge_queue_is_complete(__dtfl_merge_queue *q)
{
	mq_shared *s = q->shared;
	bool empty;

	check_poison(s);
	/* Acquire: pairs with the release decrement in drop. */
	if (atomic_load_explicit(&s->handles, memory_order_acquire) != 1)
		return false;
	mq_lock(s);
	empty = s->entries.len == 0;
	mq_unlock(s);
	return empty;
}

/*
 * BytesPush::extend: moves items[0 .. n) into the queue (each is
 * zeroed), merging each into the tail when they are adjacent views of
 * one store.  Buzzes the receiver only on the empty to non-empty
 * transition, after releasing the lock.
 */
void
__dtfl_merge_queue_extend(__dtfl_merge_queue *q, __dtfl_bytes *items,
    size_t n)
{
	mq_shared *s = q->shared;
	__dtfl_queue_entries *queue = &s->entries;
	bool should_ping = false;
	__dtfl_bytes tail;
	size_t i;

	/* Only frozen views (Rust's Bytes) may cross threads. */
	for (i = 0; i < n; i++)
		DTFL_ASSERT(!items[i].writable);
	check_poison(s);
	mq_lock(s);
	if (n > 0) {
		if (queue->len == 0) {
			should_ping = true;
			tail = items[0];
		} else {
			__dtfl_queue_entry back = __dtfl_qe_pop_back(queue);

			if (back.kind == DTFL_QE_BYTES) {
				tail = back.u.bytes;
				if (!__dtfl_bytes_try_merge(&tail, &items[0])) {
					__dtfl_qe_push_back(queue,
					    qe_bytes(&tail));
					tail = items[0];
				}
			} else {
				__dtfl_qe_push_back(queue, back);
				tail = items[0];
			}
		}
		memset(&items[0], 0, sizeof items[0]);
		for (i = 1; i < n; i++) {
			if (!__dtfl_bytes_try_merge(&tail, &items[i])) {
				__dtfl_qe_push_back(queue, qe_bytes(&tail));
				tail = items[i];
			}
			memset(&items[i], 0, sizeof items[i]);
		}
		__dtfl_qe_push_back(queue, qe_bytes(&tail));
	}

	/* Dispatch the spill policy, if any, while the lock is still held. */
	if (q->policy != NULL)
		q->policy->ops->apply(q->policy, queue);

	/* Wake the corresponding thread after releasing the lock. */
	mq_unlock(s);
	if (should_ping)
		s->buzz(s->ctx);  /* only from empty to non-empty */
}

/*
 * BytesPull::drain_into: appends the resident entries at the front of
 * the queue to `out` (a vector of __dtfl_bytes; ownership moves),
 * stopping at the first Paged entry the reader policy did not bring
 * back in.
 */
void
__dtfl_merge_queue_drain_into(__dtfl_merge_queue *q, __dtfl_vec *out)
{
	mq_shared *s = q->shared;
	__dtfl_queue_entries *queue = &s->entries;
	bool stuck;

	check_poison(s);
	mq_lock(s);

	/*
	 * A reader-side policy may materialize Paged entries near the front
	 * of the queue, up to its own budget.
	 */
	if (q->policy != NULL)
		q->policy->ops->apply(q->policy, queue);

	while (queue->len > 0 &&
	    __dtfl_qe_at(queue, 0)->kind == DTFL_QE_BYTES) {
		__dtfl_queue_entry e = __dtfl_qe_pop_front(queue);

		__dtfl_vec_push(out, &e.u.bytes);
	}

	/*
	 * Nothing produced but the queue is not empty: something is stuck
	 * (failed fetch, no reader policy, budget exhausted).  Buzz so the
	 * consumer retries rather than parking.
	 */
	stuck = out->len == 0 && queue->len > 0;
	mq_unlock(s);
	if (stuck)
		s->buzz(s->ctx);
}

/*
 * Drop.  A channel closing can unblock a thread waiting on the next bit
 * of data, so the receiver is buzzed after this handle is gone.  The
 * last handle frees the remaining entries and calls ctx_drop.
 */
void
__dtfl_merge_queue_drop(__dtfl_merge_queue *q, bool panicking)
{
	mq_shared *s;

	if (q == NULL)
		return;
	s = q->shared;
	/* Propagate panic information: clean versus unclean shutdown. */
	if (panicking)
		atomic_store_explicit(&s->panic, true, memory_order_seq_cst);
	else
		check_poison(s);
	if (q->policy != NULL)
		q->policy->ops->destroy(q->policy);
	__dtfl_free(q);

	/* Release: pairs with the acquire load in is_complete. */
	(void)atomic_fetch_sub_explicit(&s->handles, 1, memory_order_release);
	s->buzz(s->ctx);
	if (!__dtfl_arc_release(&s->ref))
		return;
	/* Last handle: no other thread can reach s. */
	__dtfl_qe_free(&s->entries);
	__dtfl_mutex_destroy(&s->lock);
	if (s->ctx_drop != NULL)
		s->ctx_drop(s->ctx);
	__dtfl_free(s);
}

/* ------------------------------------------------------------------ */
/* SendEndpoint<MergeQueue>                                            */
/* ------------------------------------------------------------------ */

/* Moves the slab's valid bytes into the queue. */
static void
send_buffer(__dtfl_send_endpoint *e)
{
	size_t valid_len;
	__dtfl_bytes b;

	(void)__dtfl_slab_valid(&e->buffer, &valid_len);
	if (valid_len > 0) {
		__dtfl_slab_extract(&e->buffer, valid_len, &b);
		__dtfl_merge_queue_extend(e->send, &b, 1);
	}
}

/* SendEndpoint::new: takes ownership of the writer handle `queue`. */
__dtfl_send_endpoint *
__dtfl_send_endpoint_new(__dtfl_merge_queue *queue)
{
	__dtfl_send_endpoint *e = __dtfl_alloc(sizeof *e);

	e->rc = 1;
	e->send = queue;
	__dtfl_slab_init(&e->buffer, 20);
	return e;
}

/* Rc::clone: one more worker-local reference. */
__dtfl_send_endpoint *
__dtfl_send_endpoint_retain(__dtfl_send_endpoint *e)
{
	e->rc++;
	return e;
}

/* Drop of the last reference sends what is staged, then drops the queue. */
void
__dtfl_send_endpoint_release(__dtfl_send_endpoint *e)
{
	if (e == NULL || --e->rc > 0)
		return;
	send_buffer(e);
	__dtfl_merge_queue_drop(e->send, false);
	__dtfl_slab_free(&e->buffer);
	__dtfl_free(e);
}

/*
 * Makes the next `bytes` bytes valid.  This also sends them, to ensure
 * early visibility.
 */
void
__dtfl_send_endpoint_make_valid(__dtfl_send_endpoint *e, size_t bytes)
{
	__dtfl_slab_make_valid(&e->buffer, bytes);
	send_buffer(e);
}

/*
 * Returns the empty region (at least `capacity` bytes, *len set to its
 * size).  The pointer is valid until the next call on `e`.
 */
unsigned char *
__dtfl_send_endpoint_reserve(__dtfl_send_endpoint *e, size_t capacity,
    size_t *len)
{
	unsigned char *p = __dtfl_slab_empty(&e->buffer, len);

	if (*len < capacity) {
		send_buffer(e);
		__dtfl_slab_ensure_capacity(&e->buffer, capacity);
		p = __dtfl_slab_empty(&e->buffer, len);
	}
	DTFL_ASSERT(*len >= capacity);
	return p;
}

/* Marks all written data as valid and makes it visible. */
void
__dtfl_send_endpoint_publish(__dtfl_send_endpoint *e)
{
	send_buffer(e);
}
