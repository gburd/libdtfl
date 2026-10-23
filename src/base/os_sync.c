/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/os_sync.c: locks, waits, the clock and the thread identity,
 * through the host's dtfl_sync_hooks or the pthread/futex defaults.  The
 * only file in src/ that may call pthread_mutex_*, pthread_cond_*,
 * futex(2), sched_yield, nanosleep, clock_gettime or pthread_self
 * (AGENTS.md section 5, ADR-0013).
 */
#if defined(__linux__)
#define _GNU_SOURCE	/* syscall(2) */
#else
#define _POSIX_C_SOURCE 200809L	/* pthread_condattr_setclock */
#endif

#include "base/os_sync.h"

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <time.h>

#if defined(__linux__) && defined(HAVE_LINUX_FUTEX_H) && \
    defined(HAVE_SYS_SYSCALL_H) && !defined(DTFL_PARKER_PTHREAD)
#define DTFL_HAVE_FUTEX 1
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/*
 * The hooks are process-wide and fixed before first use.  The setter
 * writes sync_hooks, then sync_hooks_set with release; every reader
 * loads sync_hooks_set with acquire before touching sync_hooks.  A
 * mutex or condition created on the hooked path carries a non-NULL
 * handle, and whoever uses it later reached it through some
 * synchronization after its creation, so its operations read
 * sync_hooks without a second acquire.  sync_used latches on the first
 * use of any primitive, after which the setter refuses.
 */
static dtfl_sync_hooks sync_hooks;
static atomic_bool sync_hooks_set;
static atomic_bool sync_used;

/* Latches first use; returns the hooks, or NULL for the defaults. */
static const dtfl_sync_hooks *
use_hooks(void)
{
	/* A relaxed load first: once set, the shared line stays clean. */
	if (!atomic_load_explicit(&sync_used, memory_order_relaxed))
		atomic_store_explicit(&sync_used, true, memory_order_relaxed);
	return atomic_load_explicit(&sync_hooks_set, memory_order_acquire) ?
	    &sync_hooks : NULL;
}

int
dtfl_set_sync_hooks(const dtfl_sync_hooks *h)
{
	bool mutex, cond, word;

	if (atomic_load(&sync_used))
		return __dtfl_err(DTFL_ESTATE,
		    "sync hooks must be set before libdtfl first locks or waits");
	if (h == NULL) {
		atomic_store_explicit(&sync_hooks_set, false,
		    memory_order_release);
		return DTFL_OK;
	}
	mutex = h->mutex_init != NULL || h->mutex_destroy != NULL ||
	    h->mutex_lock != NULL || h->mutex_unlock != NULL;
	cond = h->cond_init != NULL || h->cond_destroy != NULL ||
	    h->cond_wait != NULL || h->cond_signal != NULL ||
	    h->cond_broadcast != NULL;
	word = h->wait != NULL || h->wake != NULL;
	if (mutex && (h->mutex_init == NULL || h->mutex_destroy == NULL ||
	    h->mutex_lock == NULL || h->mutex_unlock == NULL))
		return __dtfl_err(DTFL_EINVAL,
		    "sync hooks: the mutex functions come as a set of four");
	if (cond && (h->cond_init == NULL || h->cond_destroy == NULL ||
	    h->cond_wait == NULL || h->cond_signal == NULL ||
	    h->cond_broadcast == NULL))
		return __dtfl_err(DTFL_EINVAL,
		    "sync hooks: the condition functions come as a set of five");
	if (mutex != cond)
		return __dtfl_err(DTFL_EINVAL,
		    "sync hooks: mutex and condition hooks come together");
	if (word && (h->wait == NULL || h->wake == NULL))
		return __dtfl_err(DTFL_EINVAL,
		    "sync hooks: wait and wake come together");
	sync_hooks = *h;
	atomic_store_explicit(&sync_hooks_set, true, memory_order_release);
	return DTFL_OK;
}

/* ------------------------------------------------------------------ */
/* Mutexes and conditions                                              */
/* ------------------------------------------------------------------ */

void
__dtfl_mutex_init(__dtfl_mutex *m)
{
	const dtfl_sync_hooks *h = use_hooks();

	m->h = NULL;
	if (h != NULL && h->mutex_init != NULL) {
		if (h->mutex_init(&m->h, h->ctx) != 0 || m->h == NULL)
			__dtfl_panic("sync hook mutex_init failed");
		return;
	}
	if (pthread_mutex_init(&m->pm, NULL) != 0)
		__dtfl_panic("pthread_mutex_init failed");
}

void
__dtfl_mutex_destroy(__dtfl_mutex *m)
{
	if (m->h != NULL)
		sync_hooks.mutex_destroy(m->h, sync_hooks.ctx);
	else
		(void)pthread_mutex_destroy(&m->pm);
}

void
__dtfl_mutex_lock(__dtfl_mutex *m)
{
	if (m->h != NULL)
		sync_hooks.mutex_lock(m->h, sync_hooks.ctx);
	else if (pthread_mutex_lock(&m->pm) != 0)
		__dtfl_panic("pthread_mutex_lock failed");
}

void
__dtfl_mutex_unlock(__dtfl_mutex *m)
{
	if (m->h != NULL)
		sync_hooks.mutex_unlock(m->h, sync_hooks.ctx);
	else
		(void)pthread_mutex_unlock(&m->pm);
}

void
__dtfl_cond_init(__dtfl_cond *c)
{
	const dtfl_sync_hooks *h = use_hooks();
	pthread_condattr_t ca;

	c->h = NULL;
	if (h != NULL && h->cond_init != NULL) {
		if (h->cond_init(&c->h, h->ctx) != 0 || c->h == NULL)
			__dtfl_panic("sync hook cond_init failed");
		return;
	}
	if (pthread_condattr_init(&ca) != 0 ||
	    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) != 0 ||
	    pthread_cond_init(&c->pc, &ca) != 0)
		__dtfl_panic("pthread_cond_init failed");
	(void)pthread_condattr_destroy(&ca);
}

void
__dtfl_cond_destroy(__dtfl_cond *c)
{
	if (c->h != NULL)
		sync_hooks.cond_destroy(c->h, sync_hooks.ctx);
	else
		(void)pthread_cond_destroy(&c->pc);
}

void
__dtfl_cond_wait(__dtfl_cond *c, __dtfl_mutex *m, uint64_t timeout_ns)
{
	struct timespec ts;
	uint64_t deadline;

	if (c->h != NULL) {
		DTFL_ASSERT(m->h != NULL);
		sync_hooks.cond_wait(c->h, m->h, timeout_ns, sync_hooks.ctx);
		return;
	}
	if (timeout_ns == UINT64_MAX) {
		(void)pthread_cond_wait(&c->pc, &m->pm);
		return;
	}
	/* The condition's own clock, not a host clock hook. */
	clock_gettime(CLOCK_MONOTONIC, &ts);
	deadline = (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
	    (uint64_t)ts.tv_nsec;
	deadline = timeout_ns > UINT64_MAX - deadline ? UINT64_MAX :
	    deadline + timeout_ns;
	ts.tv_sec = (time_t)(deadline / UINT64_C(1000000000));
	ts.tv_nsec = (long)(deadline % UINT64_C(1000000000));
	(void)pthread_cond_timedwait(&c->pc, &m->pm, &ts);
}

void
__dtfl_cond_signal(__dtfl_cond *c)
{
	if (c->h != NULL)
		sync_hooks.cond_signal(c->h, sync_hooks.ctx);
	else
		(void)pthread_cond_signal(&c->pc);
}

void
__dtfl_cond_broadcast(__dtfl_cond *c)
{
	if (c->h != NULL)
		sync_hooks.cond_broadcast(c->h, sync_hooks.ctx);
	else
		(void)pthread_cond_broadcast(&c->pc);
}

/* ------------------------------------------------------------------ */
/* Word wait and wake                                                  */
/* ------------------------------------------------------------------ */

bool
__dtfl_word_wait_available(void)
{
	const dtfl_sync_hooks *h = use_hooks();

	if (h != NULL && h->wait != NULL)
		return true;
	/* A host with its own conditions gets a parker built on them. */
	if (h != NULL && h->cond_wait != NULL)
		return false;
#ifdef DTFL_HAVE_FUTEX
	return true;
#else
	return false;
#endif
}

void
__dtfl_word_wait(_Atomic uint32_t *word, uint32_t expect,
    uint64_t timeout_ns)
{
	const dtfl_sync_hooks *h = use_hooks();

	if (h != NULL && h->wait != NULL) {
		h->wait((const uint32_t *)(const void *)word, expect,
		    timeout_ns, h->ctx);
		return;
	}
#ifdef DTFL_HAVE_FUTEX
	{
		struct timespec rel, *relp = NULL;

		if (timeout_ns != UINT64_MAX) {
			rel.tv_sec = (time_t)(timeout_ns / UINT64_C(1000000000));
			rel.tv_nsec = (long)(timeout_ns % UINT64_C(1000000000));
			relp = &rel;
		}
		/* EINTR, EAGAIN and ETIMEDOUT all mean "reread the word". */
		(void)syscall(SYS_futex, (uint32_t *)word, FUTEX_WAIT_PRIVATE,
		    expect, relp, NULL, 0);
	}
#else
	(void)word; (void)expect; (void)timeout_ns;
	__dtfl_panic("word wait without futex(2) or a sync hook");
#endif
}

void
__dtfl_word_wake(_Atomic uint32_t *word)
{
	const dtfl_sync_hooks *h = use_hooks();

	if (h != NULL && h->wake != NULL) {
		h->wake((const uint32_t *)(const void *)word, h->ctx);
		return;
	}
#ifdef DTFL_HAVE_FUTEX
	(void)syscall(SYS_futex, (uint32_t *)word, FUTEX_WAKE_PRIVATE, 1,
	    NULL, NULL, 0);
#else
	(void)word;
#endif
}

/* ------------------------------------------------------------------ */
/* Yield, sleep, clock, identity                                       */
/* ------------------------------------------------------------------ */

void
__dtfl_yield(void)
{
	const dtfl_sync_hooks *h = use_hooks();

	if (h != NULL && h->yield != NULL)
		h->yield(h->ctx);
	else
		(void)sched_yield();
}

void
__dtfl_sleep_ns(uint64_t ns)
{
	const dtfl_sync_hooks *h = use_hooks();
	struct timespec ts;

	if (h != NULL && h->sleep != NULL) {
		h->sleep(ns, h->ctx);
		return;
	}
	ts.tv_sec = (time_t)(ns / UINT64_C(1000000000));
	ts.tv_nsec = (long)(ns % UINT64_C(1000000000));
	(void)nanosleep(&ts, NULL);
}

uint64_t
__dtfl_clock_ns(void)
{
	const dtfl_sync_hooks *h = use_hooks();
	struct timespec ts;

	if (h != NULL && h->clock != NULL)
		return h->clock(h->ctx);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

uintptr_t
__dtfl_thread_self(void)
{
	const dtfl_sync_hooks *h = use_hooks();

	if (h != NULL && h->self != NULL)
		return h->self(h->ctx);
	return (uintptr_t)pthread_self();
}
