/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_antichain.c: Antichain, MutableAntichain and
 * ChangeBatch.  The progress_frontier__ tests port the #[test]s of
 * timely/src/progress/frontier.rs at timely-v0.31.0 (PLAN D6).
 */
#include "dtfl_test.h"
#include "progress/progress_int.h"

/*
 * frontier.rs tests use `Elem(char, usize)` ordered as a product; a
 * Product<u64, u64> class with the char as its code is the same order.
 */
static dtfl_time_class *
elem_class(void)
{
	dtfl_time_class *u = dtfl_time_u64();
	dtfl_time_class *p = dtfl_time_product(u, u);

	dtfl_time_class_free(u);
	return p;
}

/* Antichain::from(vec): inserts each element in order. */
static void
antichain_from(__dtfl_antichain *a, const dtfl_time_class *tc,
    const uint64_t *elems, size_t n)
{
	size_t i;

	__dtfl_antichain_init(a, tc);
	for (i = 0; i < n; i++)
		(void)__dtfl_antichain_insert(a, elems + 2 * i);
}

static bool
from_eq(const __dtfl_antichain *h, const dtfl_time_class *tc,
    const uint64_t *elems, size_t n)
{
	__dtfl_antichain b;
	bool eq;

	antichain_from(&b, tc, elems, n);
	eq = __dtfl_antichain_eq(h, &b);
	__dtfl_antichain_free(&b);
	return eq;
}

/*
 * frontier.rs antichain_hash.  Upstream puts an antichain in a HashSet
 * and probes it with antichains built in other insertion orders, which
 * tests that Hash and Eq ignore element order.  libdtfl has no
 * Antichain hash, so this tests the same property of the equality the
 * HashSet relies on, __dtfl_antichain_eq, with the same probes.
 */
DTFL_TEST(progress_frontier__antichain_hash)
{
	dtfl_time_class *tc = elem_class();
	__dtfl_antichain h;
	const uint64_t a2b1[4] = { 'a', 2, 'b', 1 }, b1a2[4] = { 'b', 1, 'a', 2 };

	(void)params; (void)data;
	antichain_from(&h, tc, a2b1, 2);
	assert_size(__dtfl_antichain_len(&h), ==, 2);
	assert_true(from_eq(&h, tc, a2b1, 2));
	assert_true(from_eq(&h, tc, b1a2, 2));

	assert_false(from_eq(&h, tc, (uint64_t[2]){ 'a', 2 }, 1));
	assert_false(from_eq(&h, tc, (uint64_t[2]){ 'a', 1 }, 1));
	assert_false(from_eq(&h, tc, (uint64_t[2]){ 'b', 2 }, 1));
	assert_false(from_eq(&h, tc, (uint64_t[4]){ 'a', 1, 'b', 2 }, 2));
	assert_false(from_eq(&h, tc, (uint64_t[2]){ 'c', 3 }, 1));
	assert_false(from_eq(&h, tc, NULL, 0));
	__dtfl_antichain_free(&h);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* frontier.rs mutable_compaction. */
DTFL_TEST(progress_frontier__mutable_compaction)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_mutable_antichain m;
	int i;

	(void)params; (void)data;
	__dtfl_mantichain_init(&m, tc);
	for (i = 0; i < 6; i++)
		__dtfl_mantichain_update(&m, &(uint64_t){ 7 }, 1);
	for (i = 0; i < 5; i++)
		__dtfl_mantichain_update(&m, &(uint64_t){ 8 }, 1);
	for (i = 0; i < 1000; i++) {
		__dtfl_mantichain_update(&m, &(uint64_t){ 9 }, 1);
		__dtfl_mantichain_update(&m, &(uint64_t){ 9 }, -1);
	}
	assert_size(m.updates.updates.len, <=, 32);
	__dtfl_mantichain_free(&m);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* frontier.rs doc examples for Antichain::insert, less_than, less_equal. */
DTFL_TEST(antichain_doc_examples)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_antichain a, b;

	(void)params; (void)data;
	__dtfl_antichain_init(&a, tc);
	assert_true(__dtfl_antichain_insert(&a, &(uint64_t){ 2 }));
	assert_false(__dtfl_antichain_insert(&a, &(uint64_t){ 3 }));
	assert_true(__dtfl_antichain_less_than(&a, &(uint64_t){ 3 }));
	assert_false(__dtfl_antichain_less_than(&a, &(uint64_t){ 2 }));
	assert_true(__dtfl_antichain_less_equal(&a, &(uint64_t){ 2 }));
	assert_false(__dtfl_antichain_less_equal(&a, &(uint64_t){ 1 }));
	assert_true(__dtfl_antichain_insert(&a, &(uint64_t){ 1 }));
	assert_size(__dtfl_antichain_len(&a), ==, 1);
	assert_uint64(__dtfl_antichain_at(&a, 0)[0], ==, 1);
	__dtfl_antichain_init_elem(&b, tc, &(uint64_t){ 4 });
	assert_true(__dtfl_antichain_dominates(&a, &b));
	assert_false(__dtfl_antichain_dominates(&b, &a));
	__dtfl_antichain_copy(&b, &a);
	assert_true(__dtfl_antichain_eq(&a, &b));
	__dtfl_antichain_clear(&a);
	assert_false(__dtfl_antichain_less_equal(&a, &(uint64_t){ 3 }));
	assert_true(__dtfl_antichain_dominates(&b, &a));	/* empty other */
	__dtfl_antichain_free(&a);
	__dtfl_antichain_free(&b);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(antichain_sort)
{
	dtfl_time_class *tc = elem_class();
	__dtfl_antichain a;
	const uint64_t e[6] = { 3, 0, 1, 2, 0, 5 };

	(void)params; (void)data;
	antichain_from(&a, tc, e, 3);
	assert_size(__dtfl_antichain_len(&a), ==, 3);
	__dtfl_antichain_sort(&a);
	assert_uint64(__dtfl_antichain_at(&a, 0)[0], ==, 0);
	assert_uint64(__dtfl_antichain_at(&a, 1)[0], ==, 1);
	assert_uint64(__dtfl_antichain_at(&a, 2)[0], ==, 3);
	/* (0, 0) dominates everything and evicts all three. */
	assert_true(__dtfl_antichain_insert(&a, (uint64_t[2]){ 0, 0 }));
	assert_size(__dtfl_antichain_len(&a), ==, 1);
	__dtfl_antichain_free(&a);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* frontier.rs update_iter doc example. */
DTFL_TEST(mutable_update_iter_example)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_mutable_antichain m;
	__dtfl_change_batch u;

	(void)params; (void)data;
	__dtfl_mantichain_init_elem(&m, tc, &(uint64_t){ 1 });
	assert_false(__dtfl_mantichain_less_than(&m, &(uint64_t){ 1 }));
	assert_true(__dtfl_mantichain_less_than(&m, &(uint64_t){ 2 }));
	assert_true(__dtfl_mantichain_less_equal(&m, &(uint64_t){ 1 }));
	assert_false(__dtfl_mantichain_less_equal(&m, &(uint64_t){ 0 }));
	__dtfl_cb_init(&u, 1);
	__dtfl_cb_update(&u, &(uint64_t){ 1 }, -1);
	__dtfl_cb_update(&u, &(uint64_t){ 2 }, 7);
	__dtfl_mantichain_update_batch(&m, &u);
	assert_true(__dtfl_cb_is_empty(&u));
	assert_size(__dtfl_mantichain_frontier_len(&m), ==, 1);
	assert_uint64(__dtfl_mantichain_frontier(&m)[0], ==, 2);
	assert_size(__dtfl_cb_iter(&m.changes), ==, 2);
	assert_uint64(__dtfl_cb_key(&m.changes, 0)[0], ==, 1);
	assert_int64(__dtfl_cb_delta(&m.changes, 0), ==, -1);
	assert_uint64(__dtfl_cb_key(&m.changes, 1)[0], ==, 2);
	assert_int64(__dtfl_cb_delta(&m.changes, 1), ==, 1);
	assert_int64(__dtfl_mantichain_count_for(&m, &(uint64_t){ 2 }), ==, 7);
	assert_int64(__dtfl_mantichain_count_for(&m, &(uint64_t){ 1 }), ==, 0);
	__dtfl_cb_clear(&m.changes);
	/* An update beyond the frontier changes nothing. */
	__dtfl_mantichain_update(&m, &(uint64_t){ 5 }, 1);
	assert_true(__dtfl_cb_is_empty(&m.changes));
	__dtfl_mantichain_update(&m, &(uint64_t){ 2 }, -7);
	assert_size(__dtfl_mantichain_frontier_len(&m), ==, 1);
	assert_uint64(__dtfl_mantichain_frontier(&m)[0], ==, 5);
	__dtfl_mantichain_clear(&m);
	assert_true(__dtfl_mantichain_is_empty(&m));
	__dtfl_cb_free(&u);
	__dtfl_mantichain_free(&m);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* MutableAntichain over the unit class: one-word storage for 0 words. */
DTFL_TEST(mutable_unit_class)
{
	dtfl_time_class *tc = dtfl_time_unit();
	__dtfl_mutable_antichain m;

	(void)params; (void)data;
	__dtfl_mantichain_init_elem(&m, tc, NULL);
	assert_false(__dtfl_mantichain_is_empty(&m));
	__dtfl_mantichain_update(&m, NULL, -1);
	assert_true(__dtfl_mantichain_is_empty(&m));
	__dtfl_mantichain_free(&m);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* change_batch.rs doc examples, and zero elimination. */
DTFL_TEST(change_batch_basics)
{
	__dtfl_change_batch cb;

	(void)params; (void)data;
	__dtfl_cb_init(&cb, 1);
	assert_true(__dtfl_cb_is_empty(&cb));
	__dtfl_cb_update(&cb, &(uint64_t){ 17 }, 1);
	assert_false(__dtfl_cb_is_empty(&cb));
	__dtfl_cb_update(&cb, &(uint64_t){ 17 }, -1);
	assert_true(__dtfl_cb_is_dirty(&cb));
	assert_true(__dtfl_cb_is_empty(&cb));
	assert_false(__dtfl_cb_is_dirty(&cb));
	__dtfl_cb_update(&cb, &(uint64_t){ 17 }, -1);
	__dtfl_cb_update(&cb, &(uint64_t){ 14 }, -1);
	__dtfl_cb_update(&cb, &(uint64_t){ 17 }, 1);
	assert_size(__dtfl_cb_len(&cb), ==, 1);
	assert_uint64(__dtfl_cb_key(&cb, 0)[0], ==, 14);
	assert_int64(__dtfl_cb_delta(&cb, 0), ==, -1);
	__dtfl_cb_clear(&cb);
	assert_true(__dtfl_cb_is_empty(&cb));
	__dtfl_cb_free(&cb);
	return MUNIT_OK;
}

/*
 * maintain_bounds: compaction happens once more than 32 entries exist
 * and at least half are dirty; 32 entries are left alone.
 */
DTFL_TEST(change_batch_threshold)
{
	__dtfl_change_batch cb;
	uint64_t k;

	(void)params; (void)data;
	__dtfl_cb_init(&cb, 2);
	for (k = 0; k < 32; k++)
		__dtfl_cb_update(&cb, (uint64_t[2]){ 0, k % 4 }, 1);
	assert_size(cb.updates.len, ==, 32);
	assert_size(cb.clean, ==, 0);
	__dtfl_cb_update(&cb, (uint64_t[2]){ 0, 0 }, 1);
	assert_size(cb.updates.len, ==, 4);
	assert_size(cb.clean, ==, 4);
	assert_int64(__dtfl_cb_delta(&cb, 0), ==, 9);
	/* 4 clean: 29 more distinct pushes make 33 with 16 >= 4, compact. */
	for (k = 0; k < 28; k++)
		__dtfl_cb_update(&cb, (uint64_t[2]){ 1, k }, 1);
	assert_size(cb.updates.len, ==, 32);
	__dtfl_cb_update(&cb, (uint64_t[2]){ 1, 28 }, 1);
	assert_size(cb.updates.len, ==, 33);
	assert_size(cb.clean, ==, 33);
	/* Two-word keys sort word-lexicographically. */
	assert_uint64(__dtfl_cb_key(&cb, 3)[1], ==, 3);
	assert_uint64(__dtfl_cb_key(&cb, 4)[0], ==, 1);
	__dtfl_cb_free(&cb);
	return MUNIT_OK;
}

/* drain_into swaps when the target is empty and extends otherwise. */
DTFL_TEST(change_batch_drain_into)
{
	__dtfl_change_batch a, b;
	unsigned char *a_data;

	(void)params; (void)data;
	__dtfl_cb_init(&a, 1);
	__dtfl_cb_init(&b, 1);
	__dtfl_cb_update(&a, &(uint64_t){ 17 }, 1);
	a_data = a.updates.data;
	__dtfl_cb_drain_into(&a, &b);
	assert_ptr_equal(b.updates.data, a_data);
	assert_true(__dtfl_cb_is_empty(&a));
	assert_false(__dtfl_cb_is_empty(&b));
	__dtfl_cb_update(&a, &(uint64_t){ 17 }, 2);
	__dtfl_cb_update(&a, &(uint64_t){ 3 }, 1);
	__dtfl_cb_drain_into(&a, &b);
	assert_size(a.updates.len, ==, 0);
	assert_size(a.clean, ==, 0);
	assert_size(__dtfl_cb_len(&b), ==, 2);
	assert_uint64(__dtfl_cb_key(&b, 0)[0], ==, 3);
	assert_int64(__dtfl_cb_delta(&b, 1), ==, 3);
	__dtfl_cb_extend(&a, &b);
	assert_size(__dtfl_cb_len(&a), ==, 2);
	assert_size(__dtfl_cb_len(&b), ==, 2);
	__dtfl_cb_free(&a);
	__dtfl_cb_free(&b);
	/* Batches of different key widths do not mix. */
	__dtfl_cb_init(&a, 1);
	__dtfl_cb_init(&b, 2);
	DTFL_EXPECT_PANIC(__dtfl_cb_drain_into(&a, &b));
	__dtfl_cb_free(&a);
	__dtfl_cb_free(&b);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(progress_frontier__antichain_hash),
	DTFL_T(progress_frontier__mutable_compaction),
	DTFL_T(antichain_doc_examples),
	DTFL_T(antichain_sort),
	DTFL_T(mutable_update_iter_example),
	DTFL_T(mutable_unit_class),
	DTFL_T(change_batch_basics),
	DTFL_T(change_batch_threshold),
	DTFL_T(change_batch_drain_into),
	DTFL_T_END
};

DTFL_TEST_MAIN("antichain", tests)
