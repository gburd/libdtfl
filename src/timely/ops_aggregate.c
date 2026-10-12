/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_aggregate.c: intra-timestamp keyed aggregation.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/vec/aggregation/aggregate.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * aggregate folds the values of (key, value) records within each time,
 * keyed by key, then emits one result per key once the time is complete.
 * The input uses an Exchange pact that routes by the key's hash, so all
 * records with the same key land on the same worker.  Each time keeps a
 * key -> aggregate map; a notification is requested for the time, and
 * when the input frontier passes it the per-key aggregates are emitted
 * through the user emit callback and the time's map is dropped.
 *
 * Built on dtfl_stream_unary_notify (operator.c).  The record is a pair
 * whose first field is the key; the operator splits it with the type's
 * tuple layout (record.h).  Public at the pin, removed upstream later
 * (#818).
 */
#include "timely/ops_int.h"
#include "timely/ops_notify_int.h"
#include "progress/time_int.h"

#include "dtfl/record.h"

#include <string.h>

/* One key's running aggregate within a time: key bytes then agg bytes. */
typedef struct agg_entry {
	void *key;		/* owned */
	size_t klen;
	void *agg;		/* owned, agg_size bytes */
} agg_entry;

/* All aggregates open at one time. */
typedef struct agg_slot {
	uint64_t time[DTFL_TIME_MAX];
	__dtfl_vec entries;	/* agg_entry; linear scan (few keys per time) */
} agg_slot;

typedef struct agg_ctx {
	const dtfl_time_class *tc;
	const dtfl_type *in_type;	/* pair(key, value); not retained */
	size_t agg_size;
	dtfl_agg_fold_fn fold;
	dtfl_agg_emit_fn emit;
	void *user;
	void (*user_free)(void *);
	__dtfl_vec slots;		/* agg_slot */
} agg_ctx;

/* Routes a (key, value) record by the key field's hash. */
static uint64_t
agg_route(const void *rec, size_t len, void *vctx)
{
	agg_ctx *c = vctx;
	const dtfl_type *kt = dtfl_type_tuple_field(c->in_type, 0);
	size_t klen, koff = dtfl_type_tuple_offset(c->in_type, rec, len, 0, &klen);

	if (koff == SIZE_MAX)
		return 0;
	return dtfl_type_hash(kt, (const unsigned char *)rec + koff, klen);
}

static agg_slot *
agg_slot_for(agg_ctx *c, dtfl_op *op, dtfl_notificator *note,
    const uint64_t *time)
{
	size_t i;
	agg_slot s;
	agg_slot *slot;

	for (i = 0; i < c->slots.len; i++) {
		slot = DTFL_VEC_AT(&c->slots, agg_slot, i);
		if (dtfl_time_cmp(c->tc, slot->time, time) == 0)
			return slot;
	}
	__dtfl_time_copy(c->tc, s.time, time);
	__dtfl_vec_init(&s.entries, sizeof(agg_entry));
	slot = __dtfl_vec_push(&c->slots, &s);
	dtfl_notificator_notify_at(note,
	    dtfl_op_input_cap_delayed(op, 0, 0, time));
	return slot;
}

/* The aggregate for `key` in `slot`, zero-initialised on first sight. */
static agg_entry *
agg_entry_for(agg_ctx *c, agg_slot *slot, const void *key, size_t klen)
{
	size_t i;
	agg_entry e;
	const dtfl_type *kt = dtfl_type_tuple_field(c->in_type, 0);

	for (i = 0; i < slot->entries.len; i++) {
		agg_entry *ent = DTFL_VEC_AT(&slot->entries, agg_entry, i);

		if (dtfl_type_cmp(kt, ent->key, ent->klen, key, klen) == 0)
			return ent;
	}
	e.key = __dtfl_memdup(key, klen);
	e.klen = klen;
	e.agg = __dtfl_calloc(1, c->agg_size);	/* Default::default */
	return __dtfl_vec_push(&slot->entries, &e);
}

static void
agg_entry_free(agg_entry *e)
{
	__dtfl_free(e->key);
	__dtfl_free(e->agg);
}

static void
agg_run(void *vctx, dtfl_op *op, dtfl_notificator *note)
{
	agg_ctx *c = vctx;
	dtfl_message *m;
	dtfl_cap *cap;
	uint64_t count;

	/* Fold each (key, value) into the aggregate for its time and key. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		agg_slot *slot = agg_slot_for(c, op, note, t);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len, koff, klen, voff, vlen;
			const void *rec = dtfl_message_record(m, i, &len);
			agg_entry *ent;

			koff = dtfl_type_tuple_offset(c->in_type, rec, len, 0, &klen);
			voff = dtfl_type_tuple_offset(c->in_type, rec, len, 1, &vlen);
			if (koff == SIZE_MAX || voff == SIZE_MAX)
				continue;
			ent = agg_entry_for(c, slot,
			    (const unsigned char *)rec + koff, klen);
			c->fold(c->user, (const unsigned char *)rec + koff, klen,
			    (const unsigned char *)rec + voff, vlen, ent->agg);
		}
	}

	/* Emit the aggregates for each completed time, then drop them. */
	while (dtfl_notificator_next(note, &cap, &count)) {
		const uint64_t *t = dtfl_cap_time(cap);
		size_t i;

		for (i = 0; i < c->slots.len; i++) {
			agg_slot *slot = DTFL_VEC_AT(&c->slots, agg_slot, i);
			size_t j;

			if (dtfl_time_cmp(c->tc, slot->time, t) != 0)
				continue;
			{
				dtfl_session *s = dtfl_op_session(op, 0, cap);
				dtfl_emit emit = { s };

				for (j = 0; j < slot->entries.len; j++) {
					agg_entry *ent =
					    DTFL_VEC_AT(&slot->entries, agg_entry, j);

					c->emit(c->user, ent->key, ent->klen, ent->agg,
					    &emit);
					agg_entry_free(ent);
				}
				dtfl_session_close(s);
			}
			__dtfl_vec_free(&slot->entries);
			__dtfl_vec_remove(&c->slots, i);
			break;
		}
		dtfl_cap_drop(cap);
	}
}

static void
agg_free(void *vctx)
{
	agg_ctx *c = vctx;
	size_t i, j;

	if (c->user_free != NULL)
		c->user_free(c->user);
	for (i = 0; i < c->slots.len; i++) {
		agg_slot *slot = DTFL_VEC_AT(&c->slots, agg_slot, i);

		for (j = 0; j < slot->entries.len; j++)
			agg_entry_free(DTFL_VEC_AT(&slot->entries, agg_entry, j));
		__dtfl_vec_free(&slot->entries);
	}
	__dtfl_vec_free(&c->slots);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_aggregate(dtfl_stream *st, size_t agg_size, dtfl_agg_fold_fn fold,
    dtfl_agg_emit_fn emit, const dtfl_type *out_type, dtfl_ctx ctx)
{
	agg_ctx *c = __dtfl_calloc(1, sizeof *c);
	dtfl_pact pact;

	c->tc = dtfl_scope_time_class(dtfl_stream_scope(st));
	c->in_type = dtfl_stream_type(st);
	c->agg_size = agg_size;
	c->fold = fold;
	c->emit = emit;
	c->user = ctx.ptr;
	c->user_free = ctx.free;
	__dtfl_vec_init(&c->slots, sizeof(agg_slot));

	pact = dtfl_pact_exchange(agg_route, c);
	return dtfl_stream_unary_notify(st, pact, "Aggregate", NULL, 0, out_type,
	    agg_run, DTFL_CTX(c, agg_free));
}
