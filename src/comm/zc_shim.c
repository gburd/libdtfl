/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/comm/zc_shim.c: TEMPORARY: removed when feat/p2-comm merges.
 * Minimal correct versions of the parker and the push/pull helpers
 * declared in comm_int.h, which the comm worker owns, so the zero-copy
 * layer links and tests on its own branch.  Nothing here is a port;
 * delete the file and its line in zc_local.mk at merge.
 */
#include "comm/comm_int.h"

/* ------------------------------------------------------------------ */
/* Parker: mutex + condition variable + token                          */
/* ------------------------------------------------------------------ */

struct __dtfl_parker {
	__dtfl_arc ref;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool token;	/* guarded by lock: an unpark not yet consumed */
};

/* A parker with no pending token and one reference. */
__dtfl_parker *
__dtfl_parker_new(void)
{
	__dtfl_parker *p = __dtfl_alloc(sizeof *p);

	__dtfl_arc_init(&p->ref);
	if (pthread_mutex_init(&p->lock, NULL) != 0 ||
	    pthread_cond_init(&p->cond, NULL) != 0)
		__dtfl_panic("parker init failed");
	p->token = false;
	return p;
}

/* One more reference, from any thread. */
__dtfl_parker *
__dtfl_parker_retain(__dtfl_parker *p)
{
	__dtfl_arc_retain(&p->ref);
	return p;
}

/* Drops a reference; the last frees.  NULL is a no-op. */
void
__dtfl_parker_release(__dtfl_parker *p)
{
	if (p == NULL || !__dtfl_arc_release(&p->ref))
		return;
	(void)pthread_cond_destroy(&p->cond);
	(void)pthread_mutex_destroy(&p->lock);
	__dtfl_free(p);
}

/* Sets the token and wakes the owner.  The mutex orders it with park. */
void
__dtfl_parker_unpark(__dtfl_parker *p)
{
	(void)pthread_mutex_lock(&p->lock);
	p->token = true;
	(void)pthread_cond_signal(&p->cond);
	(void)pthread_mutex_unlock(&p->lock);
}

/* Returns at the first unpark (consuming it) or after timeout_ns. */
void
__dtfl_parker_park(__dtfl_parker *p, uint64_t timeout_ns)
{
	struct timespec ts;
	uint64_t ns;

	(void)pthread_mutex_lock(&p->lock);
	if (timeout_ns == UINT64_MAX) {
		while (!p->token)
			(void)pthread_cond_wait(&p->cond, &p->lock);
	} else {
		(void)clock_gettime(CLOCK_REALTIME, &ts);
		ns = (uint64_t)ts.tv_nsec + timeout_ns % 1000000000u;
		ts.tv_sec += (time_t)(timeout_ns / 1000000000u +
		    ns / 1000000000u);
		ts.tv_nsec = (long)(ns % 1000000000u);
		while (!p->token &&
		    pthread_cond_timedwait(&p->cond, &p->lock, &ts) == 0)
			;
	}
	p->token = false;
	(void)pthread_mutex_unlock(&p->lock);
}

/* ------------------------------------------------------------------ */
/* Push / Pull helpers                                                 */
/* ------------------------------------------------------------------ */

/* Push::send: moves msg in; frees any recycled element. */
void
__dtfl_push_send(__dtfl_push *p, const __dtfl_msg_class *mc, void *msg)
{
	void *m = msg;

	p->ops->push(p, &m);
	if (m != NULL)
		mc->free(mc, m);
}

/* Push::done: a flush. */
void
__dtfl_push_done(__dtfl_push *p)
{
	p->ops->push(p, NULL);
}

/* Drops a pusher.  NULL is a no-op. */
void
__dtfl_push_destroy(__dtfl_push *p)
{
	if (p != NULL)
		p->ops->destroy(p);
}

/* Drops a puller.  NULL is a no-op. */
void
__dtfl_pull_destroy(__dtfl_pull *p)
{
	if (p != NULL)
		p->ops->destroy(p);
}
