/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/buzzer.c: the per-worker parker (std::thread::park/unpark)
 * that upstream's Buzzer unparks.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/buzzer.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Upstream's Buzzer holds a std::thread::Thread and calls unpark().  In
 * C a worker gets an explicit parker object instead, and a Buzzer is a
 * reference to it.  The semantics are std's: one token; unpark sets it
 * (waking a parked owner); park consumes it, returning at once if it was
 * already set.  Spurious returns are allowed but never happen early on
 * a timed park here: park loops until it is unparked or the deadline
 * passes.
 *
 * On Linux the token is a futex word, as in Rust's std (futex parker):
 * EMPTY, NOTIFIED, or PARKED.  Elsewhere, or when DTFL_PARKER_PTHREAD
 * is defined, a mutex and a CLOCK_MONOTONIC condition variable guard a
 * boolean token.
 */
#if defined(__linux__)
#define _GNU_SOURCE	/* syscall(2) */
#else
#define _POSIX_C_SOURCE 200809L	/* pthread_condattr_setclock */
#endif

#include "comm/comm_int.h"

#include <errno.h>
#include <time.h>

#if defined(__linux__) && defined(HAVE_LINUX_FUTEX_H) && \
    defined(HAVE_SYS_SYSCALL_H) && !defined(DTFL_PARKER_PTHREAD)
#define DTFL_PARKER_FUTEX 1
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef DTFL_PARKER_FUTEX
/* uint32_t, not an enum: C11 enumerators are int and PARKED is out of range. */
#define EMPTY		((uint32_t)0)
#define NOTIFIED	((uint32_t)1)
#define PARKED		UINT32_MAX
#endif

struct __dtfl_parker {
	/*
	 * Shared by the owner and every buzzer.  Decremented with acq_rel
	 * so the freeing thread sees every other holder's accesses.
	 */
	atomic_size_t refs;
#ifdef DTFL_PARKER_FUTEX
	/*
	 * The token.  unpark's release swap synchronizes with park's
	 * acquire read of NOTIFIED, so whatever the unparking thread
	 * wrote before unpark is visible when park returns.
	 */
	_Atomic uint32_t state;
#else
	pthread_mutex_t mu;
	pthread_cond_t cv;
	bool token;	/* under mu */
#endif
};

__dtfl_parker *
__dtfl_parker_new(void)
{
	__dtfl_parker *p = __dtfl_alloc(sizeof *p);

	atomic_init(&p->refs, 1);
#ifdef DTFL_PARKER_FUTEX
	atomic_init(&p->state, EMPTY);
#else
	{
		pthread_condattr_t ca;

		if (pthread_mutex_init(&p->mu, NULL) != 0 ||
		    pthread_condattr_init(&ca) != 0 ||
		    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) != 0 ||
		    pthread_cond_init(&p->cv, &ca) != 0)
			__dtfl_panic("parker: cannot initialize mutex or condvar");
		pthread_condattr_destroy(&ca);
		p->token = false;
	}
#endif
	return p;
}

__dtfl_parker *
__dtfl_parker_retain(__dtfl_parker *p)
{
	/* relaxed: the caller already holds a reference. */
	atomic_fetch_add_explicit(&p->refs, 1, memory_order_relaxed);
	return p;
}

void
__dtfl_parker_release(__dtfl_parker *p)
{
	if (p == NULL)
		return;
	if (atomic_fetch_sub_explicit(&p->refs, 1, memory_order_acq_rel) != 1)
		return;
#ifndef DTFL_PARKER_FUTEX
	pthread_cond_destroy(&p->cv);
	pthread_mutex_destroy(&p->mu);
#endif
	__dtfl_free(p);
}

#ifdef DTFL_PARKER_FUTEX

/* Waits while *word == PARKED, for at most rel (NULL: no limit). */
static void
futex_wait(_Atomic uint32_t *word, const struct timespec *rel)
{
	/* EINTR, EAGAIN and ETIMEDOUT all mean "look at the word again". */
	(void)syscall(SYS_futex, (uint32_t *)word, FUTEX_WAIT_PRIVATE,
	    (uint32_t)PARKED, rel, NULL, 0);
}

void
__dtfl_parker_unpark(__dtfl_parker *p)
{
	/* release: pairs with the acquire in park (see the state comment). */
	if (atomic_exchange_explicit(&p->state, NOTIFIED,
	    memory_order_release) == PARKED)
		(void)syscall(SYS_futex, (uint32_t *)&p->state,
		    FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

void
__dtfl_parker_park(__dtfl_parker *p, uint64_t timeout_ns)
{
	uint64_t deadline = 0, now;
	uint32_t expect;

	/* NOTIFIED -> EMPTY and return, or EMPTY -> PARKED. */
	if (atomic_fetch_sub_explicit(&p->state, 1,
	    memory_order_acquire) == NOTIFIED)
		return;
	if (timeout_ns != UINT64_MAX)
		deadline = __dtfl_clock_ns() + timeout_ns;
	for (;;) {
		if (timeout_ns == UINT64_MAX) {
			futex_wait(&p->state, NULL);
		} else {
			struct timespec rel;

			now = __dtfl_clock_ns();
			if (now < deadline) {
				rel.tv_sec = (time_t)((deadline - now) /
				    UINT64_C(1000000000));
				rel.tv_nsec = (long)((deadline - now) %
				    UINT64_C(1000000000));
				futex_wait(&p->state, &rel);
			}
		}
		expect = NOTIFIED;
		if (atomic_compare_exchange_strong_explicit(&p->state, &expect,
		    EMPTY, memory_order_acquire, memory_order_relaxed))
			return;
		if (timeout_ns != UINT64_MAX && __dtfl_clock_ns() >= deadline)
			break;
	}
	/*
	 * Timed out.  Leave PARKED for EMPTY; an unpark racing with us may
	 * have set NOTIFIED, which this consumes (the wakeup arrived).
	 */
	(void)atomic_exchange_explicit(&p->state, EMPTY, memory_order_acquire);
}

#else /* !DTFL_PARKER_FUTEX */

void
__dtfl_parker_unpark(__dtfl_parker *p)
{
	pthread_mutex_lock(&p->mu);
	p->token = true;
	pthread_cond_signal(&p->cv);
	pthread_mutex_unlock(&p->mu);
}

void
__dtfl_parker_park(__dtfl_parker *p, uint64_t timeout_ns)
{
	struct timespec ts;
	uint64_t deadline;

	pthread_mutex_lock(&p->mu);
	if (timeout_ns == UINT64_MAX) {
		while (!p->token)
			pthread_cond_wait(&p->cv, &p->mu);
	} else {
		deadline = __dtfl_clock_ns() + timeout_ns;
		ts.tv_sec = (time_t)(deadline / UINT64_C(1000000000));
		ts.tv_nsec = (long)(deadline % UINT64_C(1000000000));
		while (!p->token) {
			if (pthread_cond_timedwait(&p->cv, &p->mu, &ts) ==
			    ETIMEDOUT)
				break;
		}
	}
	p->token = false;
	pthread_mutex_unlock(&p->mu);
}

#endif /* DTFL_PARKER_FUTEX */
