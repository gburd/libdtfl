/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/base.h: errors, hooks and version for libdtfl.
 *
 * Every other public header includes this one.  It declares the error
 * codes returned by fallible functions, the five process-wide host
 * hooks (allocation, panic, logging, thread spawning and
 * synchronization), and the library version.  See dtfl(7),
 * dtfl_embedding(7) and dtfl_set_alloc_hooks(3).
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
 * elog(WARNING) and elog(LOG).  Not elog(ERROR): the hook runs inside
 * libdtfl, and an error that longjmps out leaves the worker unusable
 * (dtfl_embedding(7)).
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
 * Thread-spawn hooks.  A host that manages its own threads routes
 * worker, network and connection thread creation through these; each
 * must start a thread in this process, since the workers of one
 * execution share memory.  With no hooks set, libdtfl uses pthreads.
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

/*
 * Synchronization hooks (ADR-0013).  Every lock and wait libdtfl makes
 * goes through these, so a host whose threads are fibers or tasks (a
 * runtime that must not block its carrier threads in the kernel) can
 * supply versions that yield to its scheduler.  Atomics are not hooked;
 * they never block.  Each group is all or nothing, and a NULL group
 * keeps the default (pthreads, and futex(2) on Linux):
 *
 *   mutex_*      init stores an opaque handle in *mutex and returns 0
 *                or a DTFL_E* code.  Not recursive.
 *   cond_*       needs the mutex group.  cond_wait releases `mutex`,
 *                waits for a signal or timeout_ns (UINT64_MAX: no
 *                limit) and reacquires it; spurious returns are allowed.
 *   wait, wake   wait sleeps while *word == expect, for at most
 *                timeout_ns; it may return early.  wake wakes a
 *                thread waiting on word.  The worker parker sleeps on
 *                these; without them it uses the condition group if
 *                set, else futex(2) or a pthread condition.
 *   yield        a spin's back-off (default sched_yield).
 *   sleep        sleeps about ns nanoseconds (default nanosleep).
 *   clock        monotonic nanoseconds (default CLOCK_MONOTONIC); drives
 *                activate_after, timed parks and log timestamps.
 *   self         an identifier of the calling thread of execution,
 *                stable for its life, for the debug check that a
 *                worker is used only by its owner (default
 *                pthread_self).
 *
 * Every callback may run on any libdtfl thread and must be thread-safe.
 */
typedef struct dtfl_sync_hooks {
	int (*mutex_init)(void **mutex, void *ctx);
	void (*mutex_destroy)(void *mutex, void *ctx);
	void (*mutex_lock)(void *mutex, void *ctx);
	void (*mutex_unlock)(void *mutex, void *ctx);
	int (*cond_init)(void **cond, void *ctx);
	void (*cond_destroy)(void *cond, void *ctx);
	void (*cond_wait)(void *cond, void *mutex, uint64_t timeout_ns,
	    void *ctx);
	void (*cond_signal)(void *cond, void *ctx);
	void (*cond_broadcast)(void *cond, void *ctx);
	void (*wait)(const uint32_t *word, uint32_t expect, uint64_t timeout_ns,
	    void *ctx);
	void (*wake)(const uint32_t *word, void *ctx);
	void (*yield)(void *ctx);
	void (*sleep)(uint64_t ns, void *ctx);
	uint64_t (*clock)(void *ctx);
	uintptr_t (*self)(void *ctx);
	void *ctx;
} dtfl_sync_hooks;

/*
 * Installs synchronization hooks; NULL restores the defaults.  Must be
 * called before libdtfl first locks, waits or reads the clock (in
 * practice, before any other libdtfl call but the hook setters);
 * returns DTFL_ESTATE after that, and DTFL_EINVAL for a partial group.
 */
DTFL_API int dtfl_set_sync_hooks(const dtfl_sync_hooks *hooks);

/* Library version. */
#define DTFL_VERSION_MAJOR 0
#define DTFL_VERSION_MINOR 0
#define DTFL_VERSION_PATCH 1
DTFL_API const char *dtfl_version(void);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_BASE_H */
