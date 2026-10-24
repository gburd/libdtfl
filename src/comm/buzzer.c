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
 * The token is a word, as in Rust's std (futex parker): EMPTY, NOTIFIED,
 * or PARKED, slept on with the sync layer's word wait (futex(2) on
 * Linux, or the host's wait/wake hook).  When no word wait is available
 * (no futex, or a host that supplied mutex and condition hooks but no
 * wait/wake) a mutex and a condition guard a boolean token instead.
 * The choice is made per parker, at creation (ADR-0013).
 *
 * A third kind sleeps through callbacks (__dtfl_parker_new_with): the
 * word machine stays, but the sleep is the callback's (a transport's
 * wait) and an unpark of a PARKED parker calls the wake callback.  Its
 * park returns after one sleep, even with no unpark: the sleep may end
 * for bytes on the transport, which the worker reads when it steps.
 */
#include "base/os_sync.h"
#include "base/probes_int.h"
#include "comm/comm_int.h"

/* uint32_t, not an enum: C11 enumerators are int and PARKED is out of range. */
#define EMPTY		((uint32_t)0)
#define NOTIFIED	((uint32_t)1)
#define PARKED		UINT32_MAX

struct __dtfl_parker {
	/*
	 * Shared by the owner and every buzzer.  Decremented with acq_rel
	 * so the freeing thread sees every other holder's accesses.
	 */
	atomic_size_t refs;
	bool word;	/* the word path; else mu/cv/token.  Immutable. */
	/* Callback sleep and wake (NULL: none); immutable. */
	void (*sleep)(void *ctx, uint64_t timeout_ns);
	void (*wake)(void *ctx);
	void *cb_ctx;
	/*
	 * Detach protocol for the callbacks: an unparker increments
	 * wakers, then calls wake only if detached is still false, then
	 * decrements.  __dtfl_parker_detach sets detached, then waits for
	 * wakers to reach zero.  All four accesses are seq_cst, so either
	 * the detacher sees the increment and waits, or the unparker sees
	 * detached and skips the call.
	 */
	atomic_bool detached;
	atomic_uint wakers;
	/*
	 * The token on the word path.  unpark's release swap synchronizes
	 * with park's acquire read of NOTIFIED, so whatever the unparking
	 * thread wrote before unpark is visible when park returns.
	 */
	_Atomic uint32_t state;
	__dtfl_mutex mu;
	__dtfl_cond cv;
	bool token;	/* under mu */
};

__dtfl_parker *
__dtfl_parker_new(void)
{
	__dtfl_parker *p = __dtfl_calloc(1, sizeof *p);

	atomic_init(&p->refs, 1);
	atomic_init(&p->state, EMPTY);
	p->word = __dtfl_word_wait_available();
	if (!p->word) {
		__dtfl_mutex_init(&p->mu);
		__dtfl_cond_init(&p->cv);
		p->token = false;
	}
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
	if (!p->word) {
		__dtfl_cond_destroy(&p->cv);
		__dtfl_mutex_destroy(&p->mu);
	}
	__dtfl_free(p);
}

static void
word_unpark(__dtfl_parker *p)
{
	/* release: pairs with the acquire in park (see the state comment). */
	if (atomic_exchange_explicit(&p->state, NOTIFIED,
	    memory_order_release) == PARKED)
		__dtfl_word_wake(&p->state);
}

static void
word_park(__dtfl_parker *p, uint64_t timeout_ns)
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
			__dtfl_word_wait(&p->state, PARKED, UINT64_MAX);
		} else {
			now = __dtfl_clock_ns();
			if (now < deadline)
				__dtfl_word_wait(&p->state, PARKED,
				    deadline - now);
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

__dtfl_parker *
__dtfl_parker_new_with(void (*sleep)(void *ctx, uint64_t timeout_ns),
    void (*wake)(void *ctx), void *ctx)
{
	__dtfl_parker *p = __dtfl_calloc(1, sizeof *p);

	atomic_init(&p->refs, 1);
	atomic_init(&p->state, EMPTY);
	atomic_init(&p->detached, false);
	atomic_init(&p->wakers, 0);
	p->word = true;
	p->sleep = sleep;
	p->wake = wake;
	p->cb_ctx = ctx;
	return p;
}

void
__dtfl_parker_detach(__dtfl_parker *p)
{
	atomic_store_explicit(&p->detached, true, memory_order_seq_cst);
	while (atomic_load_explicit(&p->wakers, memory_order_seq_cst) != 0)
		__dtfl_yield();
}

static void
cb_unpark(__dtfl_parker *p)
{
	/* release: pairs with the acquire in cb_park, as word_unpark. */
	if (atomic_exchange_explicit(&p->state, NOTIFIED,
	    memory_order_release) != PARKED)
		return;
	atomic_fetch_add_explicit(&p->wakers, 1, memory_order_seq_cst);
	if (!atomic_load_explicit(&p->detached, memory_order_seq_cst))
		p->wake(p->cb_ctx);
	atomic_fetch_sub_explicit(&p->wakers, 1, memory_order_seq_cst);
}

static void
cb_park(__dtfl_parker *p, uint64_t timeout_ns)
{
	/* NOTIFIED -> EMPTY and return, or EMPTY -> PARKED. */
	if (atomic_fetch_sub_explicit(&p->state, 1,
	    memory_order_acquire) == NOTIFIED)
		return;
	if (!atomic_load_explicit(&p->detached, memory_order_seq_cst))
		p->sleep(p->cb_ctx, timeout_ns);
	/* PARKED or NOTIFIED -> EMPTY; a NOTIFIED here was this wakeup. */
	(void)atomic_exchange_explicit(&p->state, EMPTY, memory_order_acquire);
}

static void
cond_unpark(__dtfl_parker *p)
{
	__dtfl_mutex_lock(&p->mu);
	p->token = true;
	__dtfl_cond_signal(&p->cv);
	__dtfl_mutex_unlock(&p->mu);
}

static void
cond_park(__dtfl_parker *p, uint64_t timeout_ns)
{
	uint64_t deadline = 0, now;

	__dtfl_mutex_lock(&p->mu);
	if (timeout_ns != UINT64_MAX)
		deadline = __dtfl_clock_ns() + timeout_ns;
	while (!p->token) {
		if (timeout_ns == UINT64_MAX) {
			__dtfl_cond_wait(&p->cv, &p->mu, UINT64_MAX);
			continue;
		}
		now = __dtfl_clock_ns();
		if (now >= deadline)
			break;
		__dtfl_cond_wait(&p->cv, &p->mu, deadline - now);
	}
	p->token = false;
	__dtfl_mutex_unlock(&p->mu);
}

void
__dtfl_parker_unpark(__dtfl_parker *p)
{
	DTFL_PROBE1(unpark, (const void *)p);
	if (p->sleep != NULL)
		cb_unpark(p);
	else if (p->word)
		word_unpark(p);
	else
		cond_unpark(p);
}

void
__dtfl_parker_park(__dtfl_parker *p, uint64_t timeout_ns)
{
	if (p->sleep != NULL)
		cb_park(p, timeout_ns);
	else if (p->word)
		word_park(p, timeout_ns);
	else
		cond_park(p, timeout_ns);
}
