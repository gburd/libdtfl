/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/spill.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * tests/unit/test_spill.c: the spill.rs unit tests, with its in-memory
 * MockStrategy and MockHandle.
 */
#include "dtfl_test.h"
#include "comm/zc_int.h"

/* dtfl_test.h's panic hook is unused here: no spill test expects a panic. */
static void (*const unused_hook)(const char *, void *) DTFL_UNUSED =
    dtfl_test_panic_hook;

/* bytes_of: a read-only view of a fresh copy of data. */
static void
bytes_of(__dtfl_bytes *out, const void *data, size_t n)
{
	__dtfl_bytes tmp;

	__dtfl_bytes_new(&tmp, n);
	memcpy(tmp.ptr, data, n);
	__dtfl_bytes_extract_to(&tmp, n, out);
	__dtfl_bytes_release(&tmp);
}

typedef struct mock_handle {
	__dtfl_bytes_fetch base;
	__dtfl_bytes data;
} mock_handle;

static bool
mock_fetch(__dtfl_bytes_fetch *h, __dtfl_vec *out)
{
	mock_handle *m = (mock_handle *)h;

	__dtfl_vec_push(out, &m->data);
	__dtfl_free(m);
	return true;
}

static void
mock_handle_destroy(__dtfl_bytes_fetch *h)
{
	mock_handle *m = (mock_handle *)h;

	__dtfl_bytes_release(&m->data);
	__dtfl_free(m);
}

static const __dtfl_bytes_fetch_ops mock_handle_ops = {
	mock_fetch, mock_handle_destroy
};

/* MockStrategy::spill: every chunk becomes a MockHandle. */
static void
mock_spill(__dtfl_bytes_spill *s, __dtfl_vec *chunks, __dtfl_vec *handles)
{
	size_t i;

	(void)s;
	for (i = 0; i < chunks->len; i++) {
		mock_handle *m = __dtfl_alloc(sizeof *m);

		m->base.ops = &mock_handle_ops;
		m->data = *DTFL_VEC_AT(chunks, __dtfl_bytes, i);
		__dtfl_vec_push(handles, &m);
	}
	__dtfl_vec_clear(chunks);
}

static void
mock_strategy_destroy(__dtfl_bytes_spill *s)
{
	__dtfl_free(s);
}

static const __dtfl_bytes_spill_ops mock_strategy_ops = {
	mock_spill, mock_strategy_destroy
};

static __dtfl_bytes_spill *
mock_strategy(void)
{
	__dtfl_bytes_spill *s = __dtfl_alloc(sizeof *s);

	s->ops = &mock_strategy_ops;
	return s;
}

/* EagerPolicy: spills every resident entry but the last. */
typedef struct eager_policy {
	__dtfl_spill_policy base;
	__dtfl_bytes_spill *strategy;
} eager_policy;

static void
eager_apply(__dtfl_spill_policy *pp, __dtfl_queue_entries *queue)
{
	eager_policy *p = (eager_policy *)pp;
	size_t last = queue->len > 0 ? queue->len - 1 : 0, i, n;
	__dtfl_vec indices, bytes, handles;

	__dtfl_vec_init(&indices, sizeof(size_t));
	__dtfl_vec_init(&bytes, sizeof(__dtfl_bytes));
	__dtfl_vec_init(&handles, sizeof(__dtfl_bytes_fetch *));
	for (i = 0; i < queue->len; i++) {
		__dtfl_queue_entry *e = __dtfl_qe_at(queue, i);

		if (i == last)
			break;
		if (e->kind == DTFL_QE_BYTES) {
			__dtfl_vec_push(&indices, &i);
			__dtfl_bytes_clone(&e->u.bytes,
			    __dtfl_vec_push(&bytes, NULL));
		}
	}
	if (bytes.len > 0) {
		p->strategy->ops->spill(p->strategy, &bytes, &handles);
		n = DTFL_MIN(indices.len, handles.len);
		for (i = 0; i < n; i++) {
			__dtfl_queue_entry *e = __dtfl_qe_at(queue,
			    *DTFL_VEC_AT(&indices, size_t, i));

			__dtfl_qe_entry_drop(e);
			e->kind = DTFL_QE_PAGED;
			e->u.paged = *DTFL_VEC_AT(&handles,
			    __dtfl_bytes_fetch *, i);
		}
	}
	__dtfl_vec_free(&indices);
	__dtfl_vec_free(&bytes);
	__dtfl_vec_free(&handles);
}

static void
eager_destroy(__dtfl_spill_policy *pp)
{
	eager_policy *p = (eager_policy *)pp;

	p->strategy->ops->destroy(p->strategy);
}

static const __dtfl_spill_policy_ops eager_ops = { eager_apply, eager_destroy };

DTFL_TEST(allocator_zero_copy_spill__eager_policy_moves_middle_entries)
{
	eager_policy p = { { &eager_ops }, mock_strategy() };
	__dtfl_queue_entries queue;
	unsigned char buf[8];
	__dtfl_queue_entry e;
	int i;

	(void)params; (void)data;
	__dtfl_qe_init(&queue);
	for (i = 0; i < 4; i++) {
		memset(buf, i, sizeof buf);
		e.kind = DTFL_QE_BYTES;
		bytes_of(&e.u.bytes, buf, sizeof buf);
		__dtfl_qe_push_back(&queue, e);
	}
	p.base.ops->apply(&p.base, &queue);
	assert_int(__dtfl_qe_at(&queue, 0)->kind, ==, DTFL_QE_PAGED);
	assert_int(__dtfl_qe_at(&queue, 1)->kind, ==, DTFL_QE_PAGED);
	assert_int(__dtfl_qe_at(&queue, 2)->kind, ==, DTFL_QE_PAGED);
	assert_int(__dtfl_qe_at(&queue, 3)->kind, ==, DTFL_QE_BYTES);
	__dtfl_qe_free(&queue);
	p.base.ops->destroy(&p.base);
	return MUNIT_OK;
}

static void
noop_buzz(void *ctx)
{
	(void)ctx;
}

DTFL_TEST(allocator_zero_copy_spill__merge_queue_spill_roundtrip_mock)
{
	const size_t head_reserve = 128;
	__dtfl_spill_threshold *tp =
	    __dtfl_spill_threshold_new(mock_strategy());
	__dtfl_spill_prefetch *rp = __dtfl_spill_prefetch_new(head_reserve);
	__dtfl_merge_queue *writer, *reader;
	unsigned char expected[100 * 64], got[100 * 64], d[64];
	__dtfl_vec received;
	size_t before, off = 0, i, paged = 0;
	__dtfl_bytes b;

	(void)params; (void)data;
	tp->threshold_bytes = 512;
	tp->head_reserve_bytes = head_reserve;
	__dtfl_merge_queue_new_pair(noop_buzz, NULL, NULL, &tp->base,
	    &rp->base, &writer, &reader);

	for (i = 0; i < 100; i++) {
		memset(d, (int)(i % 251), sizeof d);
		memcpy(expected + 64 * i, d, sizeof d);
		bytes_of(&b, d, sizeof d);
		__dtfl_merge_queue_extend(writer, &b, 1);
	}

	__dtfl_vec_init(&received, sizeof(__dtfl_bytes));
	for (;;) {
		before = received.len;
		__dtfl_merge_queue_drain_into(reader, &received);
		if (received.len == before)
			break;
		paged++;
	}

	for (i = 0; i < received.len; i++) {
		__dtfl_bytes *r = DTFL_VEC_AT(&received, __dtfl_bytes, i);

		assert_size(off + r->len, <=, sizeof got);
		memcpy(got + off, r->ptr, r->len);
		off += r->len;
		__dtfl_bytes_release(r);
	}
	assert_size(off, ==, sizeof expected);
	assert_memory_equal(sizeof expected, expected, got);
	/* The reader needed several rounds: the writer really spilled. */
	assert_size(paged, >, 1);
	__dtfl_vec_free(&received);
	__dtfl_merge_queue_drop(writer, false);
	__dtfl_merge_queue_drop(reader, false);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(allocator_zero_copy_spill__eager_policy_moves_middle_entries),
	DTFL_T(allocator_zero_copy_spill__merge_queue_spill_roundtrip_mock),
	DTFL_T_END
};

DTFL_TEST_MAIN("spill", tests)
