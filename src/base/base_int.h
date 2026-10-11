/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/base_int.h: library-internal primitives shared by every
 * component.  Nothing here is installed or exported.
 *
 * Contents:
 *   - allocation wrappers (never return NULL; failure panics, D6)
 *   - panic and error-detail plumbing
 *   - __dtfl_vec, a growable array with explicit element size
 *   - atomic and worker-local reference counts
 *   - min/max/likely helpers
 */
#ifndef DTFL_BASE_INT_H
#define DTFL_BASE_INT_H

#include "config.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>

#include "dtfl/base.h"

#if defined(__GNUC__) || defined(__clang__)
#define DTFL_LIKELY(x) __builtin_expect(!!(x), 1)
#define DTFL_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define DTFL_UNUSED __attribute__((unused))
#define DTFL_HIDDEN __attribute__((visibility("hidden")))
#else
#define DTFL_LIKELY(x) (x)
#define DTFL_UNLIKELY(x) (x)
#define DTFL_UNUSED
#define DTFL_HIDDEN
#endif

#define DTFL_MIN(a, b) ((a) < (b) ? (a) : (b))
#define DTFL_MAX(a, b) ((a) > (b) ? (a) : (b))
#define DTFL_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/*
 * Allocation.  These never return NULL: on failure they call the panic
 * hook, as Rust aborts on allocation failure (AGENTS.md section 5).
 * Zero-size requests return a unique non-NULL pointer.
 */
void *__dtfl_alloc(size_t size);
void *__dtfl_calloc(size_t n, size_t size);
void *__dtfl_realloc(void *ptr, size_t size);
void __dtfl_free(void *ptr);
char *__dtfl_strdup(const char *s);
void *__dtfl_memdup(const void *p, size_t n);

/* Overflow-checked n * size; panics on overflow. */
size_t __dtfl_mul_size(size_t n, size_t size);

/*
 * Contract violation: format a message and call the panic hook.  Never
 * returns.  Use for the cases where upstream Rust panics or asserts.
 */
DTFL_NORETURN void __dtfl_panic(const char *fmt, ...) DTFL_PRINTF(1, 2);

#define DTFL_ASSERT(cond)						\
	do {								\
		if (DTFL_UNLIKELY(!(cond)))				\
			__dtfl_panic("assertion failed: %s (%s:%d)",	\
			    #cond, __FILE__, __LINE__);			\
	} while (0)

#ifdef DTFL_DEBUG
#define DTFL_DEBUG_ASSERT(cond) DTFL_ASSERT(cond)
#else
#define DTFL_DEBUG_ASSERT(cond) ((void)0)
#endif

/*
 * Record an error detail for dtfl_last_error_detail() and return err.
 * Usage: return __dtfl_err(DTFL_EINVAL, "bad port %zu", port);
 */
int __dtfl_err(int err, const char *fmt, ...) DTFL_PRINTF(2, 3);
void __dtfl_err_clear(void);

/*
 * __dtfl_vec: a growable array of fixed-size elements.  The element
 * size is fixed at init.  Storage comes from the allocation hooks.
 */
typedef struct __dtfl_vec {
	unsigned char *data;
	size_t len;	/* elements in use */
	size_t cap;	/* elements allocated */
	size_t elem;	/* element size in bytes */
} __dtfl_vec;

void __dtfl_vec_init(__dtfl_vec *v, size_t elem);
void __dtfl_vec_free(__dtfl_vec *v);
void __dtfl_vec_reserve(__dtfl_vec *v, size_t additional);
void __dtfl_vec_shrink(__dtfl_vec *v);
void *__dtfl_vec_push(__dtfl_vec *v, const void *elem);	/* returns slot */
void *__dtfl_vec_push_n(__dtfl_vec *v, const void *elems, size_t n);
void __dtfl_vec_truncate(__dtfl_vec *v, size_t len);
void __dtfl_vec_remove(__dtfl_vec *v, size_t index);	/* order preserving */
void __dtfl_vec_swap_remove(__dtfl_vec *v, size_t index);
void __dtfl_vec_clear(__dtfl_vec *v);
void __dtfl_vec_swap(__dtfl_vec *a, __dtfl_vec *b);
void __dtfl_vec_copy(__dtfl_vec *dst, const __dtfl_vec *src);

static inline void *
__dtfl_vec_at(const __dtfl_vec *v, size_t i)
{
	DTFL_DEBUG_ASSERT(i < v->len);
	return v->data + i * v->elem;
}

static inline void *
__dtfl_vec_last(const __dtfl_vec *v)
{
	return v->len == 0 ? NULL : v->data + (v->len - 1) * v->elem;
}

#define DTFL_VEC_AT(v, type, i) ((type *)__dtfl_vec_at((v), (i)))

/* Atomic reference count, for objects shared across threads. */
typedef struct __dtfl_arc {
	atomic_size_t count;
} __dtfl_arc;

static inline void
__dtfl_arc_init(__dtfl_arc *a)
{
	atomic_init(&a->count, 1);
}

static inline void
__dtfl_arc_retain(__dtfl_arc *a)
{
	atomic_fetch_add_explicit(&a->count, 1, memory_order_relaxed);
}

/* Returns true when the caller dropped the last reference. */
static inline bool
__dtfl_arc_release(__dtfl_arc *a)
{
	if (atomic_fetch_sub_explicit(&a->count, 1, memory_order_release) == 1) {
		atomic_thread_fence(memory_order_acquire);
		return true;
	}
	return false;
}

static inline size_t
__dtfl_arc_count(__dtfl_arc *a)
{
	return atomic_load_explicit(&a->count, memory_order_acquire);
}

/* FNV-1a, the default record hash (upstream hashable.rs uses FNV). */
uint64_t __dtfl_fnv1a(const void *p, size_t len);
uint64_t __dtfl_fnv1a_extend(uint64_t h, const void *p, size_t len);

/* Monotonic clock in nanoseconds. */
uint64_t __dtfl_clock_ns(void);

#endif /* DTFL_BASE_INT_H */
