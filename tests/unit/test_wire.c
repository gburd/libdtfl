/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_wire.c: MessageHeader framing, BytesSlab recycling,
 * MergeQueue merge and wakeup rules, and the ProcessBinary allocator
 * with 2 and 4 worker threads exchanging u64 messages all-to-all.
 *
 * Every wait is bounded: loops check a deadline and fail, and each
 * threaded test arms alarm(2) as a backstop so a hang kills the forked
 * test child instead of the suite.
 */
#include "dtfl_test.h"
#include "comm/zc_int.h"

#include <pthread.h>
#include <unistd.h>

#define DEADLINE_NS (30ull * 1000000000ull)

/*
 * The allocator entry points (__dtfl_allocate and friends) belong to
 * feat/p2-comm; until it merges the tests call the ops directly.
 */
#define A_BUILD(b) ((b)->build(b))
#define A_ALLOCATE(a, id, mc, push, pull) \
	((a)->ops->allocate((a), (id), (mc), (push), (pull)))
#define A_RECEIVE(a) ((a)->ops->receive(a))
#define A_RELEASE(a) ((a)->ops->release(a))
#define A_AWAIT(a, ns) ((a)->ops->await_events((a), (ns)))
#define A_DESTROY(a) ((a)->ops->destroy(a))

/* ------------------------------------------------------------------ */
/* A u64 message class                                                 */
/* ------------------------------------------------------------------ */

static size_t
u64_len(const __dtfl_msg_class *mc, const void *msg)
{
	(void)mc; (void)msg;
	return 8;
}

static void
u64_encode(const __dtfl_msg_class *mc, const void *msg, unsigned char *out)
{
	uint64_t v = *(const uint64_t *)msg;
	int i;

	(void)mc;
	for (i = 0; i < 8; i++)
		out[i] = (unsigned char)(v >> (8 * i));
}

static void *
u64_decode(const __dtfl_msg_class *mc, const __dtfl_bytes *b)
{
	uint64_t *m, v = 0;
	int i;

	(void)mc;
	if (b->len != 8)
		return NULL;
	for (i = 7; i >= 0; i--)
		v = (v << 8) | b->ptr[i];
	m = __dtfl_alloc(sizeof *m);
	*m = v;
	return m;
}

static void *
u64_clone(const __dtfl_msg_class *mc, const void *msg)
{
	(void)mc;
	return __dtfl_memdup(msg, sizeof(uint64_t));
}

static void
u64_free(const __dtfl_msg_class *mc, void *msg)
{
	(void)mc;
	__dtfl_free(msg);
}

static const __dtfl_msg_class u64_class = {
	"u64", u64_len, u64_encode, u64_decode, u64_clone, u64_free, NULL
};

static void *
u64_new(uint64_t v)
{
	uint64_t *m = __dtfl_alloc(sizeof *m);

	*m = v;
	return m;
}

/* ------------------------------------------------------------------ */
/* MessageHeader                                                       */
/* ------------------------------------------------------------------ */

DTFL_TEST(header_bytes_are_big_endian_in_field_order)
{
	__dtfl_message_header h = { 0x0102030405060708ull, 2, 3, 4, 5,
	    0xa1a2a3a4a5a6a7a8ull }, r;
	unsigned char out[DTFL_HEADER_BYTES + 5];
	static const unsigned char want[DTFL_HEADER_BYTES] = {
		1, 2, 3, 4, 5, 6, 7, 8,
		0, 0, 0, 0, 0, 0, 0, 2,
		0, 0, 0, 0, 0, 0, 0, 3,
		0, 0, 0, 0, 0, 0, 0, 4,
		0, 0, 0, 0, 0, 0, 0, 5,
		0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
	};

	(void)params; (void)data;
	assert_size(__dtfl_header_header_bytes(&h), ==, 48);
	assert_size(__dtfl_header_required_bytes(&h), ==, 53);
	memset(out, 0xee, sizeof out);
	__dtfl_header_write_to(&h, out);
	assert_memory_equal(sizeof want, out, want);
	assert_uint8(out[DTFL_HEADER_BYTES], ==, 0xee);
	assert_true(__dtfl_header_try_read(&r, out, sizeof out));
	assert_memory_equal(sizeof h, &h, &r);
	return MUNIT_OK;
}

DTFL_TEST(header_try_read_needs_header_and_payload)
{
	__dtfl_message_header h = { 7, 1, 0, 1, 10, 0 }, r;
	unsigned char buf[DTFL_HEADER_BYTES + 10];
	size_t n;

	(void)params; (void)data;
	__dtfl_header_write_to(&h, buf);
	for (n = 0; n < sizeof buf; n++)
		assert_false(__dtfl_header_try_read(&r, buf, n));
	assert_true(__dtfl_header_try_read(&r, buf, sizeof buf));
	assert_size(r.length, ==, 10);

	/* A zero-length payload needs only the header. */
	h.length = 0;
	__dtfl_header_write_to(&h, buf);
	assert_false(__dtfl_header_try_read(&r, buf, DTFL_HEADER_BYTES - 1));
	assert_true(__dtfl_header_try_read(&r, buf, DTFL_HEADER_BYTES));

	/* A length that would overflow header + length is rejected. */
	h.length = SIZE_MAX - 8;
	__dtfl_header_write_to(&h, buf);
	assert_false(__dtfl_header_try_read(&r, buf, sizeof buf));
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* BytesSlab                                                           */
/* ------------------------------------------------------------------ */

DTFL_TEST(slab_recycles_buffer_after_readers_release)
{
	__dtfl_bytes_slab s;
	__dtfl_bytes_store *first, *second;
	__dtfl_bytes v, w;
	unsigned char *p;
	size_t n;

	(void)params; (void)data;
	__dtfl_slab_init(&s, 4);
	first = s.buffer.store;
	p = __dtfl_slab_empty(&s, &n);
	assert_size(n, ==, 16);
	memset(p, 'a', 12);
	__dtfl_slab_make_valid(&s, 12);
	__dtfl_slab_extract(&s, 10, &v);
	assert_size(v.len, ==, 10);
	(void)__dtfl_slab_valid(&s, &n);
	assert_size(n, ==, 2);

	/* Retire the first buffer; the 2 valid bytes move along. */
	__dtfl_slab_ensure_capacity(&s, 8);
	assert_ptr_not_equal(s.buffer.store, first);
	second = s.buffer.store;
	p = __dtfl_slab_valid(&s, &n);
	assert_size(n, ==, 2);
	assert_memory_equal(2, p, "aa");
	assert_size(s.in_progress.len, ==, 1);

	/* While v is alive the first buffer cannot come back. */
	__dtfl_slab_make_valid(&s, 14);
	__dtfl_slab_extract(&s, 16, &w);
	__dtfl_slab_ensure_capacity(&s, 8);
	assert_size(s.in_progress.len, ==, 2);
	assert_ptr_not_equal(s.buffer.store, first);

	/* Once the readers release, the next retirement reuses them. */
	__dtfl_bytes_release(&v);
	__dtfl_bytes_release(&w);
	__dtfl_slab_make_valid(&s, 16);
	__dtfl_slab_extract(&s, 16, &w);
	__dtfl_bytes_release(&w);
	__dtfl_slab_ensure_capacity(&s, 8);
	assert_true(s.buffer.store == first || s.buffer.store == second);
	assert_size(s.stash.len, ==, 1);
	assert_size(s.in_progress.len, ==, 1);

	/* A request beyond the size grows the shift and drops old buffers. */
	__dtfl_slab_ensure_capacity(&s, 100);
	assert_size(s.shift, ==, 7);
	assert_size(s.stash.len, ==, 0);
	assert_size(s.in_progress.len, ==, 0);
	(void)__dtfl_slab_empty(&s, &n);
	assert_size(n, >=, 100);
	__dtfl_slab_free(&s);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* MergeQueue                                                          */
/* ------------------------------------------------------------------ */

static void
count_buzz(void *ctx)
{
	atomic_fetch_add_explicit((atomic_size_t *)ctx, 1,
	    memory_order_relaxed);
}

static void
drain_all(__dtfl_merge_queue *q, __dtfl_vec *out)
{
	__dtfl_vec_clear(out);
	__dtfl_merge_queue_drain_into(q, out);
}

static void
release_all(__dtfl_vec *v)
{
	size_t i;

	for (i = 0; i < v->len; i++)
		__dtfl_bytes_release(DTFL_VEC_AT(v, __dtfl_bytes, i));
	__dtfl_vec_clear(v);
}

DTFL_TEST(merge_queue_merges_adjacent_and_buzzes_on_empty)
{
	atomic_size_t buzzes;
	__dtfl_merge_queue *w, *r;
	__dtfl_bytes store, a, b, c, other, tmp, items[2];
	__dtfl_vec got;

	(void)params; (void)data;
	atomic_init(&buzzes, 0);
	__dtfl_vec_init(&got, sizeof(__dtfl_bytes));
	__dtfl_merge_queue_new_pair(count_buzz, &buzzes, NULL, NULL, NULL,
	    &w, &r);

	__dtfl_bytes_new(&store, 64);
	memset(store.ptr, 'x', 64);
	__dtfl_bytes_extract_to(&store, 8, &a);
	__dtfl_bytes_extract_to(&store, 8, &b);
	__dtfl_bytes_extract_to(&store, 8, &c);
	__dtfl_bytes_new(&tmp, 4);
	__dtfl_bytes_extract_to(&tmp, 4, &other);
	__dtfl_bytes_release(&tmp);

	/* Empty to non-empty buzzes once; further extends do not. */
	__dtfl_merge_queue_extend(w, &a, 1);
	assert_size(atomic_load(&buzzes), ==, 1);
	assert_null(a.store);
	__dtfl_merge_queue_extend(w, &b, 1);
	assert_size(atomic_load(&buzzes), ==, 1);

	/* Adjacent views of one store merged into one entry. */
	drain_all(r, &got);
	assert_size(got.len, ==, 1);
	assert_size(DTFL_VEC_AT(&got, __dtfl_bytes, 0)->len, ==, 16);
	release_all(&got);

	/* Not adjacent (other store), then adjacent within one call. */
	items[0] = other;
	items[1] = c;
	__dtfl_merge_queue_extend(w, items, 2);
	assert_size(atomic_load(&buzzes), ==, 2);
	__dtfl_bytes_extract_to(&store, 8, &c);
	__dtfl_merge_queue_extend(w, &c, 1);
	assert_size(atomic_load(&buzzes), ==, 2);
	drain_all(r, &got);
	assert_size(got.len, ==, 2);
	assert_size(DTFL_VEC_AT(&got, __dtfl_bytes, 0)->len, ==, 4);
	assert_size(DTFL_VEC_AT(&got, __dtfl_bytes, 1)->len, ==, 16);
	release_all(&got);

	/* An empty extend changes nothing and does not buzz. */
	__dtfl_merge_queue_extend(w, NULL, 0);
	assert_size(atomic_load(&buzzes), ==, 2);
	drain_all(r, &got);
	assert_size(got.len, ==, 0);

	/* Completion: only after the writer drops, and the drop buzzes. */
	assert_false(__dtfl_merge_queue_is_complete(r));
	__dtfl_merge_queue_drop(w, false);
	assert_size(atomic_load(&buzzes), ==, 3);
	assert_true(__dtfl_merge_queue_is_complete(r));
	__dtfl_merge_queue_drop(r, false);
	__dtfl_bytes_release(&store);
	__dtfl_vec_free(&got);
	return MUNIT_OK;
}

DTFL_TEST(merge_queue_poison_panics_peer)
{
	atomic_size_t buzzes;
	__dtfl_merge_queue *w, *r;

	(void)params; (void)data;
	atomic_init(&buzzes, 0);
	__dtfl_merge_queue_new_pair(count_buzz, &buzzes, NULL, NULL, NULL,
	    &w, &r);
	__dtfl_merge_queue_drop(w, true);
	DTFL_EXPECT_PANIC(__dtfl_merge_queue_is_complete(r));
	DTFL_EXPECT_PANIC(__dtfl_merge_queue_extend(r, NULL, 0));
	/* A panicking drop does not check the poison. */
	__dtfl_merge_queue_drop(r, true);
	assert_size(atomic_load(&buzzes), ==, 2);
	return MUNIT_OK;
}

typedef struct mq_thread {
	__dtfl_merge_queue *w;
	size_t count;
} mq_thread;

/* Writes `count` 8-byte views, from two stores in alternation. */
static void *
mq_writer(void *arg)
{
	mq_thread *t = arg;
	__dtfl_bytes s0, s1, v;
	size_t i;

	__dtfl_bytes_new(&s0, 8 * t->count);
	__dtfl_bytes_new(&s1, 8 * t->count);
	for (i = 0; i < t->count; i++) {
		__dtfl_bytes *s = (i & 1) ? &s1 : &s0;
		uint64_t x = i;

		memcpy(s->ptr, &x, 8);
		__dtfl_bytes_extract_to(s, 8, &v);
		__dtfl_merge_queue_extend(t->w, &v, 1);
	}
	__dtfl_bytes_release(&s0);
	__dtfl_bytes_release(&s1);
	__dtfl_merge_queue_drop(t->w, false);
	return NULL;
}

static void
unpark_buzz(void *ctx)
{
	__dtfl_parker_unpark(ctx);
}

static void
parker_drop(void *ctx)
{
	__dtfl_parker_release(ctx);
}

DTFL_TEST(merge_queue_two_threads_deliver_in_order)
{
	__dtfl_parker *park = __dtfl_parker_new();
	__dtfl_merge_queue *r;
	mq_thread t = { NULL, 20000 };
	uint64_t deadline = __dtfl_clock_ns() + DEADLINE_NS, next = 0, x;
	__dtfl_vec got;
	pthread_t th;
	size_t i, off, n;

	(void)params; (void)data;
	alarm(120);
	__dtfl_vec_init(&got, sizeof(__dtfl_bytes));
	__dtfl_merge_queue_new_pair(unpark_buzz, __dtfl_parker_retain(park),
	    parker_drop, NULL, NULL, &t.w, &r);
	assert_int(pthread_create(&th, NULL, mq_writer, &t), ==, 0);
	while (!__dtfl_merge_queue_is_complete(r)) {
		if (__dtfl_clock_ns() > deadline)
			break;
		drain_all(r, &got);
		n = got.len;
		for (i = 0; i < n; i++) {
			__dtfl_bytes *b = DTFL_VEC_AT(&got, __dtfl_bytes, i);

			assert_size(b->len % 8, ==, 0);
			for (off = 0; off < b->len; off += 8) {
				memcpy(&x, b->ptr + off, 8);
				assert_uint64(x, ==, next);
				next++;
			}
		}
		release_all(&got);
		if (n == 0)
			__dtfl_parker_park(park, 1000000);
	}
	assert_int(pthread_join(th, NULL), ==, 0);
	drain_all(r, &got);
	assert_size(got.len, ==, 0);
	assert_uint64(next, ==, t.count);
	alarm(0);
	__dtfl_merge_queue_drop(r, false);
	__dtfl_parker_release(park);
	__dtfl_vec_free(&got);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* ProcessBinary                                                       */
/* ------------------------------------------------------------------ */

#define NCHAN 3
#define NMSG 500

/* Message value: sender, channel and sequence number packed in a u64. */
#define PACK(src, ch, seq) \
	(((uint64_t)(src) << 48) | ((uint64_t)(ch) << 40) | (uint64_t)(seq))

typedef struct pb_worker {
	__dtfl_allocator_builder *builder;
	size_t peers;
	int failed;		/* written by this worker only */
	char why[128];
} pb_worker;

#define PB_FAIL(w, ...)							\
	do {								\
		snprintf((w)->why, sizeof (w)->why, __VA_ARGS__);	\
		(w)->failed = 1;					\
		goto out;						\
	} while (0)

/*
 * Each worker allocates channels 10, 20, 30 (strictly increasing),
 * sends NMSG messages to every worker (itself included) on each, and
 * pulls until it has every message.  Checks exactly-once delivery and
 * FIFO order per (sender, channel).
 */
static void *
pb_worker_main(void *arg)
{
	pb_worker *w = arg;
	__dtfl_allocator *a = A_BUILD(w->builder);
	size_t peers = a->peers, me = a->index, c, t, s, got = 0;
	size_t want = NCHAN * peers * NMSG;
	__dtfl_push *push[NCHAN][8];
	__dtfl_pull *pull[NCHAN];
	size_t next[NCHAN][8];
	uint64_t deadline = __dtfl_clock_ns() + DEADLINE_NS;

	memset(next, 0, sizeof next);
	memset(push, 0, sizeof push);
	memset(pull, 0, sizeof pull);
	if (peers != w->peers || peers > 8)
		PB_FAIL(w, "peers %zu", peers);
	for (c = 0; c < NCHAN; c++)
		A_ALLOCATE(a, 10 * (c + 1), &u64_class, push[c], &pull[c]);
	for (s = 0; s < NMSG; s++) {
		for (c = 0; c < NCHAN; c++) {
			for (t = 0; t < peers; t++)
				__dtfl_push_send(push[c][t], &u64_class,
				    u64_new(PACK(me, c, s)));
		}
		/* Interleave receiving so queues stay short. */
		if (s % 64 == 0)
			A_RELEASE(a);
	}
	for (c = 0; c < NCHAN; c++) {
		for (t = 0; t < peers; t++)
			__dtfl_push_done(push[c][t]);
	}
	A_RELEASE(a);

	while (got < want) {
		bool any = false;

		if (__dtfl_clock_ns() > deadline)
			PB_FAIL(w, "worker %zu: timeout, %zu of %zu", me, got,
			    want);
		A_RECEIVE(a);
		for (c = 0; c < NCHAN; c++) {
			uint64_t *m;

			while ((m = __dtfl_pull_recv(pull[c])) != NULL) {
				size_t src = (size_t)(*m >> 48);
				size_t ch = (size_t)((*m >> 40) & 0xff);
				size_t seq = (size_t)(*m & 0xffffffffffull);

				__dtfl_free(m);
				any = true;
				if (src >= peers || ch != c)
					PB_FAIL(w, "worker %zu: bad message "
					    "src %zu ch %zu on %zu", me, src,
					    ch, c);
				if (seq != next[c][src])
					PB_FAIL(w, "worker %zu: from %zu on %zu"
					    " got seq %zu want %zu", me, src, c,
					    seq, next[c][src]);
				next[c][src]++;
				got++;
			}
		}
		A_RELEASE(a);
		__dtfl_vec_clear(&a->events.ids);
		if (!any)
			A_AWAIT(a, 1000000);
	}
	/* Exactly once: nothing further arrives. */
	A_RECEIVE(a);
	for (c = 0; c < NCHAN; c++) {
		void *m = __dtfl_pull_recv(pull[c]);

		if (m != NULL) {
			__dtfl_free(m);
			PB_FAIL(w, "worker %zu: extra message on %zu", me, c);
		}
	}
out:
	for (c = 0; c < NCHAN; c++) {
		for (t = 0; t < 8; t++) {
			if (push[c][t] != NULL)
				__dtfl_push_destroy(push[c][t]);
		}
		if (pull[c] != NULL)
			__dtfl_pull_destroy(pull[c]);
	}
	/* The pullers' canaries retire their channels. */
	A_RECEIVE(a);
	A_DESTROY(a);
	return NULL;
}

static MunitResult
run_process_binary(size_t peers)
{
	__dtfl_allocator_builder *b[8];
	pb_worker w[8];
	pthread_t th[8];
	size_t i;

	alarm(120);
	__dtfl_process_binary_builders(peers, NULL, b);
	for (i = 0; i < peers; i++) {
		memset(&w[i], 0, sizeof w[i]);
		w[i].builder = b[i];
		w[i].peers = peers;
		assert_int(pthread_create(&th[i], NULL, pb_worker_main, &w[i]),
		    ==, 0);
	}
	for (i = 0; i < peers; i++)
		assert_int(pthread_join(th[i], NULL), ==, 0);
	alarm(0);
	for (i = 0; i < peers; i++) {
		if (w[i].failed)
			munit_errorf("%s", w[i].why);
	}
	return MUNIT_OK;
}

DTFL_TEST(process_binary_all_to_all_2)
{
	(void)params; (void)data;
	return run_process_binary(2);
}

DTFL_TEST(process_binary_all_to_all_4)
{
	(void)params; (void)data;
	return run_process_binary(4);
}

/* One worker: delivery, event reporting, allocation order. */
DTFL_TEST(process_binary_single_worker_events_and_bound)
{
	__dtfl_allocator_builder *b;
	__dtfl_allocator *a;
	__dtfl_push *push[1], *push2[1];
	__dtfl_pull *pull, *pull2;
	uint64_t *m;

	(void)params; (void)data;
	__dtfl_process_binary_builders(1, NULL, &b);
	a = A_BUILD(b);
	assert_size(a->peers, ==, 1);
	assert_size(a->index, ==, 0);
	A_ALLOCATE(a, 5, &u64_class, push, &pull);
	__dtfl_push_send(push[0], &u64_class, u64_new(42));
	A_RELEASE(a);
	A_RECEIVE(a);
	assert_size(a->events.ids.len, ==, 1);
	assert_size(*DTFL_VEC_AT(&a->events.ids, size_t, 0), ==, 5);
	m = __dtfl_pull_recv(pull);
	assert_not_null(m);
	assert_uint64(*m, ==, 42);
	__dtfl_free(m);
	assert_null(__dtfl_pull_recv(pull));
	/* The run ended: the counting puller reported the channel. */
	assert_size(a->events.ids.len, ==, 2);

	A_ALLOCATE(a, 9, &u64_class, push2, &pull2);
	DTFL_EXPECT_PANIC(A_ALLOCATE(a, 9, &u64_class, push2, &pull2));
	__dtfl_push_destroy(push[0]);
	__dtfl_pull_destroy(pull);
	__dtfl_push_destroy(push2[0]);
	__dtfl_pull_destroy(pull2);
	A_RECEIVE(a);
	A_DESTROY(a);
	return MUNIT_OK;
}

typedef struct build_slot {
	__dtfl_allocator_builder *b;
	__dtfl_allocator *a;
} build_slot;

static void *
build_thread(void *arg)
{
	build_slot *s = arg;

	s->a = A_BUILD(s->b);
	return NULL;
}

/*
 * Two allocators built on their own threads, then stepped by hand from
 * this one, so the schedule is exact: data that arrives before the
 * receiver allocates its channel waits for it, and data for a channel
 * the receiver already dropped is discarded.
 */
DTFL_TEST(process_binary_early_data_and_dropped_channel)
{
	__dtfl_allocator_builder *b[2];
	build_slot slot[2];
	__dtfl_allocator *a0, *a1;
	__dtfl_push *p0[2], *p1[2];
	__dtfl_pull *q0, *q1;
	pthread_t th[2];
	uint64_t *m;
	int i;

	(void)params; (void)data;
	alarm(60);
	__dtfl_process_binary_builders(2, NULL, b);
	for (i = 0; i < 2; i++)
		slot[i].b = b[i];
	for (i = 0; i < 2; i++)
		assert_int(pthread_create(&th[i], NULL, build_thread, &slot[i]),
		    ==, 0);
	for (i = 0; i < 2; i++)
		assert_int(pthread_join(th[i], NULL), ==, 0);
	alarm(0);
	a0 = slot[0].a;
	a1 = slot[1].a;
	assert_size(a0->index, ==, 0);
	assert_size(a1->index, ==, 1);

	/* Channel 3: worker 0 allocates it and drops its puller at once. */
	A_ALLOCATE(a0, 3, &u64_class, p0, &q0);
	__dtfl_pull_destroy(q0);
	A_RECEIVE(a0);

	/* Worker 1 sends on 3 and on 7 before worker 0 allocates 7. */
	A_ALLOCATE(a1, 3, &u64_class, p1, &q1);
	__dtfl_push_send(p1[0], &u64_class, u64_new(30));
	for (i = 0; i < 2; i++)
		__dtfl_push_destroy(p1[i]);
	__dtfl_pull_destroy(q1);
	A_ALLOCATE(a1, 7, &u64_class, p1, &q1);
	__dtfl_push_send(p1[0], &u64_class, u64_new(70));
	__dtfl_push_send(p1[0], &u64_class, u64_new(71));
	A_RELEASE(a1);

	A_RECEIVE(a0);
	/* Both frames counted, even the one for the dropped channel. */
	assert_size(a0->events.ids.len, ==, 3);
	for (i = 0; i < 2; i++)
		__dtfl_push_destroy(p0[i]);
	A_ALLOCATE(a0, 7, &u64_class, p0, &q0);
	m = __dtfl_pull_recv(q0);
	assert_not_null(m);
	assert_uint64(*m, ==, 70);
	__dtfl_free(m);
	m = __dtfl_pull_recv(q0);
	assert_not_null(m);
	assert_uint64(*m, ==, 71);
	__dtfl_free(m);
	assert_null(__dtfl_pull_recv(q0));

	for (i = 0; i < 2; i++) {
		__dtfl_push_destroy(p0[i]);
		__dtfl_push_destroy(p1[i]);
	}
	__dtfl_pull_destroy(q0);
	__dtfl_pull_destroy(q1);
	A_RECEIVE(a0);
	A_RECEIVE(a1);
	A_DESTROY(a0);
	A_DESTROY(a1);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(header_bytes_are_big_endian_in_field_order),
	DTFL_T(header_try_read_needs_header_and_payload),
	DTFL_T(slab_recycles_buffer_after_readers_release),
	DTFL_T(merge_queue_merges_adjacent_and_buzzes_on_empty),
	DTFL_T(merge_queue_poison_panics_peer),
	DTFL_T(merge_queue_two_threads_deliver_in_order),
	DTFL_T(process_binary_all_to_all_2),
	DTFL_T(process_binary_all_to_all_4),
	DTFL_T(process_binary_single_worker_events_and_bound),
	DTFL_T(process_binary_early_data_and_dropped_channel),
	DTFL_T_END
};

DTFL_TEST_MAIN("wire", tests)
