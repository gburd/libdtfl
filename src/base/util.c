/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/util.c: __dtfl_vec and FNV-1a.  The monotonic clock is in
 * os_sync.c, behind the sync hooks.
 */
#include "base_int.h"

#include <limits.h>

void
__dtfl_vec_init(__dtfl_vec *v, size_t elem)
{
	DTFL_ASSERT(elem > 0 && elem <= UINT_MAX);
	v->data = NULL;
	v->len = 0;
	v->cap = 0;
	v->elem = (unsigned)elem;
	v->borrowed = false;
}

void
__dtfl_vec_init_borrowed(__dtfl_vec *v, size_t elem, void *data, size_t cap)
{
	__dtfl_vec_init(v, elem);
	if (data != NULL && cap != 0) {
		v->data = data;
		v->cap = cap;
		v->borrowed = true;
	}
}

/* A borrowed buffer is the owner's to free; only an owned one is ours. */
void
__dtfl_vec_free(__dtfl_vec *v)
{
	if (!v->borrowed)
		__dtfl_free(v->data);
	v->data = NULL;
	v->len = 0;
	v->cap = 0;
	v->borrowed = false;
}

void
__dtfl_vec_grow(__dtfl_vec *v, size_t additional)
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
	if (v->borrowed) {
		/* Copy out of the owner's memory once; from here on it is ours. */
		unsigned char *p = __dtfl_alloc(__dtfl_mul_size(ncap, v->elem));

		if (v->len != 0)
			memcpy(p, v->data, v->len * v->elem);
		v->data = p;
		v->borrowed = false;
	} else
		v->data = __dtfl_realloc(v->data,
		    __dtfl_mul_size(ncap, v->elem));
	v->cap = ncap;
}

void
__dtfl_vec_shrink(__dtfl_vec *v)
{
	if (v->borrowed) {
		/* The owner's memory cannot shrink; only forget an empty one. */
		if (v->len == 0)
			__dtfl_vec_free(v);
	} else if (v->len == 0) {
		__dtfl_free(v->data);
		v->data = NULL;
		v->cap = 0;
	} else if (v->cap > v->len) {
		v->data = __dtfl_realloc(v->data, v->len * v->elem);
		v->cap = v->len;
	}
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

#if defined(__SANITIZE_ADDRESS__)
#define DTFL_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DTFL_ASAN 1
#endif
#endif
#ifdef DTFL_ASAN
#include <sanitizer/asan_interface.h>
#define FL_POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define FL_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define FL_POISON(p, n) ((void)(p), (void)(n))
#define FL_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

void
__dtfl_freelist_init(__dtfl_freelist *f, size_t size, size_t max)
{
	DTFL_ASSERT(size >= sizeof(void *));
	f->head = NULL;
	f->len = 0;
	f->max = max;
	f->size = size;
}

void *
__dtfl_freelist_get(__dtfl_freelist *f)
{
	void *p = f->head;

	if (p == NULL)
		return __dtfl_alloc(f->size);
	FL_UNPOISON(p, f->size);
	memcpy(&f->head, p, sizeof(void *));
	f->len--;
	return p;
}

void
__dtfl_freelist_put(__dtfl_freelist *f, void *p)
{
	if (p == NULL)
		return;
	if (f->len >= f->max) {
		__dtfl_free(p);
		return;
	}
	memcpy(p, &f->head, sizeof(void *));
	FL_POISON(p, f->size);
	f->head = p;
	f->len++;
}

void
__dtfl_freelist_free(__dtfl_freelist *f)
{
	while (f->head != NULL) {
		void *p = f->head;

		FL_UNPOISON(p, f->size);
		memcpy(&f->head, p, sizeof(void *));
		__dtfl_free(p);
	}
	f->len = 0;
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
