/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_state_machine.c: keyed state-transition operator.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/vec/aggregation/state_machine.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * state_machine keeps a state per key and applies a transition to each
 * (key, value) record in time order.  Each transition may emit output
 * and may ask that the key's state be deregistered.  The input uses an
 * Exchange pact that routes by the key's hash.  Records whose time the
 * input frontier has not yet reached are stashed and a notification is
 * requested; records at a time the frontier has already passed are
 * applied immediately (state_machine.rs).  Pending stashes are flushed
 * first on each schedule, as upstream calls notificator.for_each before
 * reading the input.
 *
 * Built on dtfl_stream_unary_notify (operator.c).  The record is a pair
 * whose first field is the key.  Public at the pin, removed upstream
 * later (#818).
 */
#include "timely/ops_int.h"
#include "timely/ops_notify_int.h"
#include "progress/time_int.h"

#include "dtfl/record.h"

#include <string.h>

/* A stashed (key, value) record: key bytes then value bytes, packed. */
typedef struct sm_pending {
	uint64_t time[DTFL_TIME_MAX];
	__dtfl_vec recs;	/* each: klen, vlen, key bytes, value bytes */
} sm_pending;

/* One key's state. */
typedef struct sm_state {
	void *key;		/* owned */
	size_t klen;
	void *state;		/* owned, state_size bytes */
} sm_state;

typedef struct sm_ctx {
	const dtfl_time_class *tc;
	const dtfl_type *in_type;	/* pair(key, value); not retained */
	size_t state_size;
	dtfl_state_fn step;
	void *user;
	void (*user_free)(void *);
	__dtfl_vec pending;		/* sm_pending, times with stashed data */
	__dtfl_vec states;		/* sm_state, key -> state */
} sm_ctx;

static uint64_t
sm_route(const void *rec, size_t len, void *vctx)
{
	sm_ctx *c = vctx;
	const dtfl_type *kt = dtfl_type_tuple_field(c->in_type, 0);
	size_t klen, koff = dtfl_type_tuple_offset(c->in_type, rec, len, 0, &klen);

	if (koff == SIZE_MAX)
		return 0;
	return dtfl_type_hash(kt, (const unsigned char *)rec + koff, klen);
}

/* The state slot for `key`, zero-initialised on first sight. */
static sm_state *
sm_state_for(sm_ctx *c, const void *key, size_t klen)
{
	size_t i;
	sm_state s;
	const dtfl_type *kt = dtfl_type_tuple_field(c->in_type, 0);

	for (i = 0; i < c->states.len; i++) {
		sm_state *st = DTFL_VEC_AT(&c->states, sm_state, i);

		if (dtfl_type_cmp(kt, st->key, st->klen, key, klen) == 0)
			return st;
	}
	s.key = __dtfl_memdup(key, klen);
	s.klen = klen;
	s.state = __dtfl_calloc(1, c->state_size);	/* Default::default */
	return __dtfl_vec_push(&c->states, &s);
}

static void
sm_state_remove(sm_ctx *c, const void *key, size_t klen)
{
	size_t i;
	const dtfl_type *kt = dtfl_type_tuple_field(c->in_type, 0);

	for (i = 0; i < c->states.len; i++) {
		sm_state *st = DTFL_VEC_AT(&c->states, sm_state, i);

		if (dtfl_type_cmp(kt, st->key, st->klen, key, klen) == 0) {
			__dtfl_free(st->key);
			__dtfl_free(st->state);
			__dtfl_vec_remove(&c->states, i);
			return;
		}
	}
}

/* Applies one transition; emits through the session; may remove the key. */
static void
sm_apply(sm_ctx *c, dtfl_session *s, const void *key, size_t klen,
    const void *val, size_t vlen)
{
	sm_state *st = sm_state_for(c, key, klen);
	dtfl_emit emit = { s };
	bool remove = c->step(c->user, key, klen, val, vlen, st->state, &emit);

	if (remove)
		sm_state_remove(c, key, klen);
}

/* Stashes a (key, value) under `time`, opening a slot if needed. */
static sm_pending *
sm_pending_for(sm_ctx *c, const uint64_t *time)
{
	size_t i;
	sm_pending p;

	for (i = 0; i < c->pending.len; i++) {
		sm_pending *pp = DTFL_VEC_AT(&c->pending, sm_pending, i);

		if (dtfl_time_cmp(c->tc, pp->time, time) == 0)
			return pp;
	}
	__dtfl_time_copy(c->tc, p.time, time);
	__dtfl_vec_init(&p.recs, 1);
	return __dtfl_vec_push(&c->pending, &p);
}

static void
sm_pending_push(sm_pending *p, size_t klen, size_t vlen, const void *key,
    const void *val)
{
	__dtfl_vec_push_n(&p->recs, &klen, sizeof klen);
	__dtfl_vec_push_n(&p->recs, &vlen, sizeof vlen);
	if (klen > 0)
		__dtfl_vec_push_n(&p->recs, key, klen);
	if (vlen > 0)
		__dtfl_vec_push_n(&p->recs, val, vlen);
}

static void
sm_run(void *vctx, dtfl_op *op, dtfl_notificator *note)
{
	sm_ctx *c = vctx;
	dtfl_message *m;
	dtfl_cap *cap;
	uint64_t count;

	/* Flush stashed data for each completed time (for_each first). */
	while (dtfl_notificator_next(note, &cap, &count)) {
		const uint64_t *t = dtfl_cap_time(cap);
		size_t i;

		for (i = 0; i < c->pending.len; i++) {
			sm_pending *p = DTFL_VEC_AT(&c->pending, sm_pending, i);
			size_t off = 0;

			if (dtfl_time_cmp(c->tc, p->time, t) != 0)
				continue;
			{
				dtfl_session *s = dtfl_op_session(op, 0, cap);

				while (off < p->recs.len) {
					size_t klen, vlen;

					memcpy(&klen, p->recs.data + off, sizeof klen);
					off += sizeof klen;
					memcpy(&vlen, p->recs.data + off, sizeof vlen);
					off += sizeof vlen;
					sm_apply(c, s, p->recs.data + off, klen,
					    p->recs.data + off + klen, vlen);
					off += klen + vlen;
				}
				dtfl_session_close(s);
			}
			__dtfl_vec_free(&p->recs);
			__dtfl_vec_remove(&c->pending, i);
			break;
		}
		dtfl_cap_drop(cap);
	}

	/* Process new input: stash if too early, else apply immediately. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		size_t i, n = dtfl_message_len(m);
		bool early = dtfl_op_frontier_less_than(op, 0, t);
		dtfl_session *s = NULL;
		dtfl_cap *scap = NULL;

		if (early) {
			dtfl_notificator_notify_at(note, dtfl_op_input_cap(op, 0, 0));
		} else {
			scap = dtfl_op_input_cap(op, 0, 0);
			s = dtfl_op_session(op, 0, scap);
		}

		for (i = 0; i < n; i++) {
			size_t len, koff, klen, voff, vlen;
			const void *rec = dtfl_message_record(m, i, &len);

			koff = dtfl_type_tuple_offset(c->in_type, rec, len, 0, &klen);
			voff = dtfl_type_tuple_offset(c->in_type, rec, len, 1, &vlen);
			if (koff == SIZE_MAX || voff == SIZE_MAX)
				continue;
			if (early) {
				sm_pending *p = sm_pending_for(c, t);

				sm_pending_push(p, klen, vlen,
				    (const unsigned char *)rec + koff,
				    (const unsigned char *)rec + voff);
			} else {
				sm_apply(c, s, (const unsigned char *)rec + koff, klen,
				    (const unsigned char *)rec + voff, vlen);
			}
		}
		if (s != NULL) {
			dtfl_session_close(s);
			dtfl_cap_drop(scap);
		}
	}
}

static void
sm_free(void *vctx)
{
	sm_ctx *c = vctx;
	size_t i;

	if (c->user_free != NULL)
		c->user_free(c->user);
	for (i = 0; i < c->pending.len; i++)
		__dtfl_vec_free(&DTFL_VEC_AT(&c->pending, sm_pending, i)->recs);
	for (i = 0; i < c->states.len; i++) {
		sm_state *st = DTFL_VEC_AT(&c->states, sm_state, i);

		__dtfl_free(st->key);
		__dtfl_free(st->state);
	}
	__dtfl_vec_free(&c->pending);
	__dtfl_vec_free(&c->states);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_state_machine(dtfl_stream *st, size_t state_size,
    dtfl_state_fn step, const dtfl_type *out_type, dtfl_ctx ctx)
{
	sm_ctx *c = __dtfl_calloc(1, sizeof *c);
	dtfl_pact pact;

	c->tc = dtfl_scope_time_class(dtfl_stream_scope(st));
	c->in_type = dtfl_stream_type(st);
	c->state_size = state_size;
	c->step = step;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	__dtfl_vec_init(&c->pending, sizeof(sm_pending));
	__dtfl_vec_init(&c->states, sizeof(sm_state));

	pact = dtfl_pact_exchange(sm_route, c);
	return dtfl_stream_unary_notify(st, pact, "StateMachine", NULL, 0,
	    out_type, sm_run, DTFL_CTX(c, sm_free));
}
