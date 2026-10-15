/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/util.c: __dtfl_vec, FNV-1a, monotonic clock.
 */
#include "base_int.h"

#include <time.h>

void
__dtfl_vec_init(__dtfl_vec *v, size_t elem)
{
	DTFL_ASSERT(elem > 0);
	v->data = NULL;
	v->len = 0;
	v->cap = 0;
	v->elem = elem;
}

void
__dtfl_vec_free(__dtfl_vec *v)
{
	__dtfl_free(v->data);
	v->data = NULL;
	v->len = 0;
	v->cap = 0;
}

void
__dtfl_vec_reserve(__dtfl_vec *v, size_t additional)
{
	size_t need, ncap;

	if (v->cap - v->len >= additional)
		return;
	need = v->len + additional;
	if (need < v->len)
		__dtfl_panic("vector length overflow");
	ncap = v->cap < 4 ? 4 : v->cap;
	while (ncap < need)
		ncap = ncap > SIZE_MAX / 2 ? need : ncap * 2;
	v->data = __dtfl_realloc(v->data, __dtfl_mul_size(ncap, v->elem));
	v->cap = ncap;
}

void
__dtfl_vec_shrink(__dtfl_vec *v)
{
	if (v->len == 0) {
		__dtfl_free(v->data);
		v->data = NULL;
		v->cap = 0;
	} else if (v->cap > v->len) {
		v->data = __dtfl_realloc(v->data, v->len * v->elem);
		v->cap = v->len;
	}
}

void *
__dtfl_vec_push(__dtfl_vec *v, const void *elem)
{
	void *slot;

	__dtfl_vec_reserve(v, 1);
	slot = v->data + v->len * v->elem;
	if (elem != NULL)
		memcpy(slot, elem, v->elem);
	else
		memset(slot, 0, v->elem);
	v->len++;
	return slot;
}

void *
__dtfl_vec_push_n(__dtfl_vec *v, const void *elems, size_t n)
{
	void *slot;

	if (n == 0)
		return v->data + v->len * v->elem;
	__dtfl_vec_reserve(v, n);
	slot = v->data + v->len * v->elem;
	if (elems != NULL)
		memcpy(slot, elems, n * v->elem);
	else
		memset(slot, 0, n * v->elem);
	v->len += n;
	return slot;
}

void *
__dtfl_vec_push_n_uninit(__dtfl_vec *v, size_t n)
{
	void *slot;

	if (n == 0)
		return v->data + v->len * v->elem;
	__dtfl_vec_reserve(v, n);
	slot = v->data + v->len * v->elem;
	v->len += n;
	return slot;
}

void
__dtfl_vec_truncate(__dtfl_vec *v, size_t len)
{
	if (len < v->len)
		v->len = len;
}

void
__dtfl_vec_remove(__dtfl_vec *v, size_t index)
{
	DTFL_ASSERT(index < v->len);
	memmove(v->data + index * v->elem, v->data + (index + 1) * v->elem,
	    (v->len - index - 1) * v->elem);
	v->len--;
}

void
__dtfl_vec_swap_remove(__dtfl_vec *v, size_t index)
{
	DTFL_ASSERT(index < v->len);
	if (index != v->len - 1)
		memcpy(v->data + index * v->elem,
		    v->data + (v->len - 1) * v->elem, v->elem);
	v->len--;
}

void
__dtfl_vec_clear(__dtfl_vec *v)
{
	v->len = 0;
}

void
__dtfl_vec_swap(__dtfl_vec *a, __dtfl_vec *b)
{
	__dtfl_vec t = *a;

	*a = *b;
	*b = t;
}

void
__dtfl_vec_copy(__dtfl_vec *dst, const __dtfl_vec *src)
{
	DTFL_ASSERT(dst->elem == src->elem);
	__dtfl_vec_clear(dst);
	__dtfl_vec_push_n(dst, src->data, src->len);
}

uint64_t
__dtfl_fnv1a_extend(uint64_t h, const void *p, size_t len)
{
	const unsigned char *b = p;
	size_t i;

	for (i = 0; i < len; i++) {
		h ^= b[i];
		h *= UINT64_C(0x100000001b3);
	}
	return h;
}

uint64_t
__dtfl_fnv1a(const void *p, size_t len)
{
	return __dtfl_fnv1a_extend(UINT64_C(0xcbf29ce484222325), p, len);
}

uint64_t
__dtfl_clock_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
