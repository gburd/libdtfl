/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/base.h: errors, hooks and version for libdtfl.
 *
 * Every other public header includes this one.  It declares the error
 * codes returned by fallible functions, the four process-wide host
 * hooks (allocation, panic, logging and thread spawning), and the
 * library version.  See dtfl(7), dtfl_embedding(7) and
 * dtfl_set_alloc_hooks(3).
 */
#ifndef DTFL_BASE_H
#define DTFL_BASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define DTFL_PRINTF(f, a) __attribute__((format(printf, f, a)))
#define DTFL_NORETURN __attribute__((noreturn))
#define DTFL_API __attribute__((visibility("default")))
#else
#define DTFL_PRINTF(f, a)
#define DTFL_NORETURN
#define DTFL_API
#endif

/* Error codes.  Functions return 0 on success or one of these. */
enum {
	DTFL_OK = 0,
	DTFL_EINVAL = 1,	/* invalid argument */
	DTFL_ENOMEM = 2,	/* allocation failed (only from hooks that may fail) */
	DTFL_ERANGE = 3,	/* value outside a representable range */
	DTFL_ESTATE = 4,	/* operation not valid in the current state */
	DTFL_EALGEBRA = 5,	/* diff class lacks an operation the operator needs */
	DTFL_EIO = 6,		/* I/O or network failure; see dtfl_last_error_detail() */
	DTFL_EPROTO = 7,	/* malformed wire data */
	DTFL_ENOTSUP = 8,	/* not supported in this build */
	DTFL_EOF = 9		/* end of stream */
};

/* Returns a static description of an error code. */
DTFL_API const char *dtfl_strerror(int err);

/*
 * Returns context for the most recent error reported on the calling
 * thread, or "" if there is none.  The string is valid until the next
 * libdtfl call on the same thread.
 */
DTFL_API const char *dtfl_last_error_detail(void);

/* Allocation hooks.  All library allocation goes through these. */
typedef struct dtfl_alloc_hooks {
	void *(*malloc_fn)(size_t size, void *ctx);
	void *(*realloc_fn)(void *ptr, size_t size, void *ctx);
	void (*free_fn)(void *ptr, void *ctx);
	void *ctx;
} dtfl_alloc_hooks;

/*
 * Installs allocation hooks.  Must be called before any other libdtfl
 * function allocates, and at most once; returns DTFL_ESTATE otherwise.
 * NULL restores the libc defaults (only before first use).
 */
DTFL_API int dtfl_set_alloc_hooks(const dtfl_alloc_hooks *hooks);

/*
 * Panic hook.  Called on a contract violation (for example, a
 * capability used for the wrong output) and on allocation failure.
 * The hook must not return: it should abort, exit, or longjmp out.
 * If it returns, libdtfl calls abort().  The default prints the
 * message to stderr and aborts.
 */
typedef void (*dtfl_panic_fn)(const char *msg, void *ctx);

/* Installs the panic hook.  NULL restores the default. */
DTFL_API void dtfl_set_panic_hook(dtfl_panic_fn fn, void *ctx);

/*
 * Diagnostic log hook.  This is the human-readable diagnostic log, for
 * messages such as "cannot start worker thread".  It is separate from
 * the structured timely and differential event logging (the logging
 * registry and BatchLogger reached through dtfl/log.h), which carries
 * machine-readable events and is unaffected by this hook.
 *
 * A hook routes libdtfl's diagnostics into the host's logging system;
 * a host like PostgreSQL maps the levels to elog(DEBUG1), elog(LOG),
 * elog(WARNING) and elog(ERROR).
 */
typedef enum dtfl_log_level {
	DTFL_LOG_DEBUG,
	DTFL_LOG_INFO,
	DTFL_LOG_WARN,
	DTFL_LOG_ERROR
} dtfl_log_level;
typedef void (*dtfl_log_fn)(dtfl_log_level level, const char *msg, void *ctx);

/*
 * Installs the log hook.  With no hook set, libdtfl writes WARN and
 * ERROR messages to stderr and drops DEBUG and INFO, so a library is
 * quiet by default.  NULL restores that default.  Set it, like the
 * other hooks, before the first dtfl_execute().
 */
DTFL_API void dtfl_set_log_hook(dtfl_log_fn fn, void *ctx);

/*
 * Thread-spawn hooks.  A host that supplies its own threads (for
 * example a PostgreSQL background worker) routes worker, network and
 * connection thread creation through these.  With no hooks set,
 * libdtfl uses pthreads.
 *
 * spawn starts start(arg) on a new thread and stores an opaque handle
 * in *handle_out; it returns 0 on success or a DTFL_E* code.  join
 * waits for the handle's thread, stores what start returned in
 * *retval_out, and returns 0 or a DTFL_E* code.  A handle is joined
 * exactly once.  A host that cannot spawn threads at all instead drives
 * dtfl_worker_new_standalone() and dtfl_worker_step() by hand and never
 * calls dtfl_execute(); see dtfl_embedding(7).
 */
typedef int (*dtfl_thread_fn)(void *(*start)(void *), void *arg,
    void **handle_out, void *ctx);
typedef int (*dtfl_thread_join_fn)(void *handle, void **retval_out, void *ctx);
typedef struct dtfl_thread_hooks {
	dtfl_thread_fn spawn;
	dtfl_thread_join_fn join;
	void *ctx;
} dtfl_thread_hooks;

/*
 * Installs thread hooks.  Both spawn and join must be non-NULL.  NULL
 * hooks restore the pthread default.  Must be called before the first
 * dtfl_execute(); returns DTFL_EINVAL if only one of the two function
 * pointers is set.
 */
DTFL_API int dtfl_set_thread_hooks(const dtfl_thread_hooks *hooks);

/* Library version. */
#define DTFL_VERSION_MAJOR 0
#define DTFL_VERSION_MINOR 0
#define DTFL_VERSION_PATCH 1
DTFL_API const char *dtfl_version(void);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_BASE_H */
