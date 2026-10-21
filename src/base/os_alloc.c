/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/os_alloc.c: allocation hooks, panic hook, error detail.
 *
 * This is the only file in src/ allowed to call malloc/realloc/free
 * directly (AGENTS.md section 5); the API discipline gate enforces it.
 */
#include "base_int.h"

#include <stdio.h>
#include <stdlib.h>

static void *
default_malloc(size_t size, void *ctx)
{
	(void)ctx;
	return malloc(size);	/* DTFL_RAW_OK: default hook */
}

static void *
default_realloc(void *ptr, size_t size, void *ctx)
{
	(void)ctx;
	return realloc(ptr, size);	/* DTFL_RAW_OK: default hook */
}

static void
default_free(void *ptr, void *ctx)
{
	(void)ctx;
	free(ptr);	/* DTFL_RAW_OK: default hook */
}

/*
 * The hooks are written once, before the first allocation, and read
 * thereafter.  `hooks_used` latches on first use so a late
 * dtfl_set_alloc_hooks() is refused instead of mixing allocators.
 */
static dtfl_alloc_hooks hooks = {
	default_malloc, default_realloc, default_free, NULL
};
static atomic_bool hooks_used;

/*
 * Latch hooks_used.  A relaxed load first: once set, the shared line
 * stays read-only, so workers on many cores allocating at once do not
 * write one cache line on every allocation.  Relaxed is enough: the
 * latch only refuses a late dtfl_set_alloc_hooks(), which the API
 * requires to happen before any other call, so nothing is published
 * through it.
 */
static inline void
mark_hooks_used(void)
{
	if (!atomic_load_explicit(&hooks_used, memory_order_relaxed))
		atomic_store_explicit(&hooks_used, true, memory_order_relaxed);
}

static void default_panic(const char *msg, void *ctx);
static dtfl_panic_fn _Atomic panic_fn = default_panic;
static void *_Atomic panic_ctx;

static dtfl_log_fn _Atomic log_fn;
static void *_Atomic log_ctx;

/* Per-thread error detail (PLAN section 5, rule 5). */
static _Thread_local char err_detail[512];

int
dtfl_set_alloc_hooks(const dtfl_alloc_hooks *h)
{
	if (atomic_load(&hooks_used))
		return __dtfl_err(DTFL_ESTATE,
		    "allocation hooks must be set before first use");
	if (h == NULL) {
		hooks.malloc_fn = default_malloc;
		hooks.realloc_fn = default_realloc;
		hooks.free_fn = default_free;
		hooks.ctx = NULL;
		return DTFL_OK;
	}
	if (h->malloc_fn == NULL || h->realloc_fn == NULL || h->free_fn == NULL)
		return __dtfl_err(DTFL_EINVAL, "allocation hooks must be non-NULL");
	hooks = *h;
	return DTFL_OK;
}

void *
__dtfl_alloc(size_t size)
{
	void *p;

	mark_hooks_used();
	p = hooks.malloc_fn(size == 0 ? 1 : size, hooks.ctx);
	if (DTFL_UNLIKELY(p == NULL))
		__dtfl_panic("allocation of %zu bytes failed", size);
	return p;
}

void *
__dtfl_calloc(size_t n, size_t size)
{
	size_t total = __dtfl_mul_size(n, size);
	void *p = __dtfl_alloc(total);

	memset(p, 0, total);
	return p;
}

void *
__dtfl_realloc(void *ptr, size_t size)
{
	void *p;

	mark_hooks_used();
	p = hooks.realloc_fn(ptr, size == 0 ? 1 : size, hooks.ctx);
	if (DTFL_UNLIKELY(p == NULL))
		__dtfl_panic("reallocation to %zu bytes failed", size);
	return p;
}

void
__dtfl_free(void *ptr)
{
	if (ptr != NULL)
		hooks.free_fn(ptr, hooks.ctx);
}

char *
__dtfl_strdup(const char *s)
{
	return __dtfl_memdup(s, strlen(s) + 1);
}

void *
__dtfl_memdup(const void *p, size_t n)
{
	void *q = __dtfl_alloc(n);

	if (n != 0)
		memcpy(q, p, n);
	return q;
}

size_t
__dtfl_mul_size(size_t n, size_t size)
{
	if (size != 0 && n > SIZE_MAX / size)
		__dtfl_panic("size overflow: %zu * %zu", n, size);
	return n * size;
}

static void
default_panic(const char *msg, void *ctx)
{
	(void)ctx;
	fprintf(stderr, "libdtfl panic: %s\n", msg);
	fflush(stderr);
	abort();
}

void
dtfl_set_panic_hook(dtfl_panic_fn fn, void *ctx)
{
	atomic_store(&panic_ctx, ctx);
	atomic_store(&panic_fn, fn != NULL ? fn : default_panic);
}

void
__dtfl_panic(const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	dtfl_panic_fn fn;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	fn = atomic_load(&panic_fn);
	fn(msg, atomic_load(&panic_ctx));
	/* A hook that returns breaks the contract; do not continue. */
	abort();
}

void
dtfl_set_log_hook(dtfl_log_fn fn, void *ctx)
{
	atomic_store(&log_ctx, ctx);
	atomic_store(&log_fn, fn);
}

void
__dtfl_log(dtfl_log_level level, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	dtfl_log_fn fn;

	fn = atomic_load(&log_fn);
	if (fn == NULL && level < DTFL_LOG_WARN)
		return;	/* quiet default: drop DEBUG and INFO */
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (fn != NULL) {
		fn(level, msg, atomic_load(&log_ctx));
		return;
	}
	fprintf(stderr, "libdtfl: %s\n", msg);
	fflush(stderr);
}

int
__dtfl_err(int err, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err_detail, sizeof err_detail, fmt, ap);
	va_end(ap);
	return err;
}

void
__dtfl_err_clear(void)
{
	err_detail[0] = '\0';
}

const char *
dtfl_last_error_detail(void)
{
	return err_detail;
}

const char *
dtfl_strerror(int err)
{
	switch (err) {
	case DTFL_OK: return "success";
	case DTFL_EINVAL: return "invalid argument";
	case DTFL_ENOMEM: return "out of memory";
	case DTFL_ERANGE: return "value out of range";
	case DTFL_ESTATE: return "operation not valid in current state";
	case DTFL_EALGEBRA: return "diff class lacks a required operation";
	case DTFL_EIO: return "I/O error";
	case DTFL_EPROTO: return "protocol error";
	case DTFL_ENOTSUP: return "not supported";
	case DTFL_EOF: return "end of stream";
	default: return "unknown error";
	}
}

const char *
dtfl_version(void)
{
	return PACKAGE_VERSION;
}
