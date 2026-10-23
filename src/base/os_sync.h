/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/os_sync.h: the one layer every lock and wait goes through.
 *
 * Mutexes, condition variables, the futex-style wait/wake the parker
 * sleeps on, yield, sleep, the monotonic clock and the thread identity
 * all come from here, so a host's dtfl_sync_hooks (ADR-0013) reach every
 * blocking point and tests/dist/test_api_discipline.sh can keep the raw
 * calls in os_sync.c.  C11 atomics are not routed: they never block.
 *
 * The hooks latch on the first mutex, condition or word wait created, so
 * every primitive of one process belongs to the same family.
 */
#ifndef DTFL_OS_SYNC_H
#define DTFL_OS_SYNC_H

#include <pthread.h>

#include "base/base_int.h"

/*
 * A mutex: the pthread default in `pm`, or the host's handle in `h` (a
 * mutex created after the hooks latched).  Not copyable once initialized.
 */
typedef struct __dtfl_mutex {
	pthread_mutex_t pm;
	void *h;
} __dtfl_mutex;

/* A condition variable; waits use the monotonic clock.  As the mutex. */
typedef struct __dtfl_cond {
	pthread_cond_t pc;
	void *h;
} __dtfl_cond;

/* Each panics if the OS or the host hook refuses. */
void __dtfl_mutex_init(__dtfl_mutex *m);
void __dtfl_mutex_destroy(__dtfl_mutex *m);
void __dtfl_mutex_lock(__dtfl_mutex *m);
void __dtfl_mutex_unlock(__dtfl_mutex *m);

void __dtfl_cond_init(__dtfl_cond *c);
void __dtfl_cond_destroy(__dtfl_cond *c);
/*
 * Waits on c, which m (held) guards, for at most timeout_ns (UINT64_MAX:
 * no limit).  May return early or spuriously; callers loop on their
 * predicate.
 */
void __dtfl_cond_wait(__dtfl_cond *c, __dtfl_mutex *m, uint64_t timeout_ns);
void __dtfl_cond_signal(__dtfl_cond *c);
void __dtfl_cond_broadcast(__dtfl_cond *c);

/*
 * True if __dtfl_word_wait/__dtfl_word_wake work in this process: the
 * host's wait/wake hooks, or futex(2) when the host supplied no mutex
 * and condition hooks.  False means the parker must use a mutex and a
 * condition.  Latches the hooks.
 */
bool __dtfl_word_wait_available(void);
/*
 * Sleeps while *word == expect, for at most timeout_ns (UINT64_MAX: no
 * limit).  Returns on a wake, a timeout, a signal or spuriously; the
 * caller rereads the word.
 */
void __dtfl_word_wait(_Atomic uint32_t *word, uint32_t expect,
    uint64_t timeout_ns);
/* Wakes a thread sleeping in __dtfl_word_wait on word. */
void __dtfl_word_wake(_Atomic uint32_t *word);

/* Lets another thread run (a spin's back-off). */
void __dtfl_yield(void);
/* Sleeps for about ns nanoseconds. */
void __dtfl_sleep_ns(uint64_t ns);
/* The calling thread of execution; stable for its life. */
uintptr_t __dtfl_thread_self(void);

#endif /* DTFL_OS_SYNC_H */
