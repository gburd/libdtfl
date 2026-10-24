/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/push_tee.c: fan-out to several pushers.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/channels/pushers/tee.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A Tee shares one list of pushers between the producing stream (which
 * adds pushers) and the Tee pusher the operator pushes into.  Upstream
 * avoids a Clone bound by distinguishing a single-consumer PushOne from
 * a multi-consumer PushMany; here every batch is refcounted, so the Tee
 * always clones (shares the batch) for every pusher but the last and
 * moves the message into the last.  A flush (NULL) is forwarded to all.
 *
 * Ownership: the Tee owns its pushers.  It is
 * reference counted so the stream and the Tee pusher may both hold it;
 * the pushers are destroyed when the last reference goes.  Clone and
 * free go through the static message class (message.c).
 */
#include "timely/timely_int.h"

struct __dtfl_tee {
	__dtfl_arc ref;
	__dtfl_vec pushers;		/* __dtfl_push * */
};

__dtfl_tee *
__dtfl_tee_new(void)
{
	__dtfl_tee *t = __dtfl_alloc(sizeof *t);

	__dtfl_arc_init(&t->ref);
	__dtfl_vec_init(&t->pushers, sizeof(__dtfl_push *));
	return t;
}

__dtfl_tee *
__dtfl_tee_retain(__dtfl_tee *t)
{
	__dtfl_arc_retain(&t->ref);
	return t;
}

void
__dtfl_tee_release(__dtfl_tee *t)
{
	size_t i;

	if (t == NULL || !__dtfl_arc_release(&t->ref))
		return;
	for (i = 0; i < t->pushers.len; i++)
		__dtfl_push_destroy(*DTFL_VEC_AT(&t->pushers, __dtfl_push *, i));
	__dtfl_vec_free(&t->pushers);
	__dtfl_free(t);
}

void
__dtfl_tee_add_pusher(__dtfl_tee *t, __dtfl_push *p)
{
	__dtfl_vec_push(&t->pushers, &p);
}

/* ------------------------------------------------------------------ */
/* The Tee pusher                                                      */
/* ------------------------------------------------------------------ */

typedef struct tee_pusher {
	__dtfl_push base;
	__dtfl_tee *tee;	/* retained */
} tee_pusher;

static void
tee_push(__dtfl_push *p, void **msg)
{
	tee_pusher *tp = (tee_pusher *)p;
	__dtfl_tee *t = tp->tee;
	const __dtfl_msg_class *mc = __dtfl_message_class();
	size_t n = t->pushers.len, i;

	if (*msg != NULL) {
		/* Defensively clone for all but the last, who takes ownership. */
		for (i = 0; i + 1 < n; i++) {
			void *copy = mc->clone(mc, *msg);

			__dtfl_push_send(*DTFL_VEC_AT(&t->pushers, __dtfl_push *, i),
			    mc, copy);
		}
		if (n > 0) {
			__dtfl_push_send(*DTFL_VEC_AT(&t->pushers, __dtfl_push *, n - 1),
			    mc, *msg);
			*msg = NULL;
		} else {
			mc->free(mc, *msg);
			*msg = NULL;
		}
	} else {
		for (i = 0; i < n; i++)
			__dtfl_push_done(*DTFL_VEC_AT(&t->pushers, __dtfl_push *, i));
	}
}

static void
tee_destroy(__dtfl_push *p)
{
	tee_pusher *tp = (tee_pusher *)p;

	__dtfl_tee_release(tp->tee);
	__dtfl_free(tp);
}

static const __dtfl_push_ops tee_ops = { tee_push, tee_destroy };

__dtfl_push *
__dtfl_tee_pusher(__dtfl_tee *t)
{
	tee_pusher *tp = __dtfl_alloc(sizeof *tp);

	tp->base.ops = &tee_ops;
	tp->tee = __dtfl_tee_retain(t);
	return &tp->base;
}
