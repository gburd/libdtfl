/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_base.c: allocation and panic hooks, error detail,
 * __dtfl_vec, FNV-1a.
 */
#include "dtfl_test.h"
#include "base/base_int.h"

static size_t hook_mallocs;

static void *
counting_malloc(size_t n, void *ctx)
{
	(void)ctx;
	hook_mallocs++;
	return malloc(n);
}

static void *
counting_realloc(void *p, size_t n, void *ctx)
{
	(void)ctx;
	return realloc(p, n);
}

static void
counting_free(void *p, void *ctx)
{
	(void)ctx;
	free(p);
}

DTFL_TEST(alloc_hooks_route_allocation)
{
	dtfl_alloc_hooks h = { counting_malloc, counting_realloc, counting_free, NULL };
	void *p;

	(void)params; (void)data;
	assert_int(dtfl_set_alloc_hooks(&h), ==, DTFL_OK);
	p = __dtfl_alloc(16);
	assert_size(hook_mallocs, ==, 1);
	__dtfl_free(p);
	/* Hooks are latched after first use. */
	assert_int(dtfl_set_alloc_hooks(NULL), ==, DTFL_ESTATE);
	assert_string_not_equal(dtfl_last_error_detail(), "");
	return MUNIT_OK;
}

DTFL_TEST(alloc_hooks_reject_null_members)
{
	dtfl_alloc_hooks h = { NULL, counting_realloc, counting_free, NULL };

	(void)params; (void)data;
	assert_int(dtfl_set_alloc_hooks(&h), ==, DTFL_EINVAL);
	return MUNIT_OK;
}

DTFL_TEST(panic_reaches_hook)
{
	(void)params; (void)data;
	DTFL_EXPECT_PANIC(__dtfl_panic("boom %d", 7));
	assert_string_equal(dtfl_test_panic_msg, "boom 7");
	DTFL_EXPECT_PANIC(DTFL_ASSERT(1 == 2));
	return MUNIT_OK;
}

DTFL_TEST(mul_size_overflow_panics)
{
	(void)params; (void)data;
	DTFL_EXPECT_PANIC((void)__dtfl_mul_size(SIZE_MAX, 2));
	assert_size(__dtfl_mul_size(3, 4), ==, 12);
	return MUNIT_OK;
}

DTFL_TEST(vec_push_remove)
{
	__dtfl_vec v;
	uint32_t x, i;

	(void)params; (void)data;
	__dtfl_vec_init(&v, sizeof(uint32_t));
	for (x = 0; x < 100; x++)
		__dtfl_vec_push(&v, &x);
	assert_size(v.len, ==, 100);
	__dtfl_vec_remove(&v, 0);
	assert_uint32(*DTFL_VEC_AT(&v, uint32_t, 0), ==, 1);
	__dtfl_vec_swap_remove(&v, 0);
	assert_uint32(*DTFL_VEC_AT(&v, uint32_t, 0), ==, 99);
	assert_size(v.len, ==, 98);
	for (i = 1; i < v.len; i++)
		assert_uint32(*DTFL_VEC_AT(&v, uint32_t, i), ==, i + 1);
	__dtfl_vec_truncate(&v, 3);
	assert_size(v.len, ==, 3);
	__dtfl_vec_shrink(&v);
	assert_size(v.cap, ==, 3);
	__dtfl_vec_free(&v);
	return MUNIT_OK;
}

DTFL_TEST(fnv1a_known_vectors)
{
	(void)params; (void)data;
	/* Reference values for 64-bit FNV-1a. */
	assert_uint64(__dtfl_fnv1a("", 0), ==, UINT64_C(0xcbf29ce484222325));
	assert_uint64(__dtfl_fnv1a("a", 1), ==, UINT64_C(0xaf63dc4c8601ec8c));
	assert_uint64(__dtfl_fnv1a("foobar", 6), ==, UINT64_C(0x85944171f73967e8));
	return MUNIT_OK;
}

DTFL_TEST(strerror_covers_codes)
{
	int e;

	(void)params; (void)data;
	for (e = DTFL_OK; e <= DTFL_EOF; e++)
		assert_string_not_equal(dtfl_strerror(e), "unknown error");
	assert_string_equal(dtfl_strerror(-1), "unknown error");
	assert_string_equal(dtfl_version(), PACKAGE_VERSION);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(alloc_hooks_route_allocation),
	DTFL_T(alloc_hooks_reject_null_members),
	DTFL_T(panic_reaches_hook),
	DTFL_T(mul_size_overflow_panics),
	DTFL_T(vec_push_remove),
	DTFL_T(fnv1a_known_vectors),
	DTFL_T(strerror_covers_codes),
	DTFL_T_END
};

DTFL_TEST_MAIN("base", tests)
