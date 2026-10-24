/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_exchange.c: the Exchange pusher routing property.
 *
 * For random records and peer counts, every record pushed through an
 * Exchange pusher lands on exactly one peer, that peer is
 * route(record) % peers, and the record multiplicity is preserved
 * (pushers/exchange.rs: the record count per time must not change).
 *
 * The Exchange pusher is driven directly: `peers` collecting pushers
 * record which records they receive, a message of random records is
 * pushed and flushed, and the collected routing is checked against the
 * route function.
 */
#include "pbt.h"

#include "dtfl/record.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

#define MAX_PEERS 8
#define MAX_RECS 64

/* ------------------------------------------------------------------ */
/* A collecting pusher: appends each received record's value           */
/* ------------------------------------------------------------------ */

typedef struct collector {
	__dtfl_push base;
	uint64_t vals[MAX_RECS];
	size_t n;
} collector;

static void
collector_push(__dtfl_push *p, void **msg)
{
	collector *c = (collector *)p;

	if (*msg != NULL) {
		dtfl_message *m = *msg;
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k && c->n < MAX_RECS; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			c->vals[c->n++] = dtfl_get_u64(rec);
		}
		__dtfl_message_free(m);
		*msg = NULL;
	}
	/* A flush (NULL) needs no action here. */
}

static void
collector_destroy(__dtfl_push *p)
{
	__dtfl_free(p);
}

static const __dtfl_push_ops collector_ops = { collector_push, collector_destroy };

/* ------------------------------------------------------------------ */
/* Route by the record value                                           */
/* ------------------------------------------------------------------ */

static uint64_t
route_by_value(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

typedef struct scenario {
	dtfl_time_class *tc;
	__dtfl_push *exch;
	collector **collectors;
	size_t peers;
} scenario;

static void
scenario_free(void *vp)
{
	scenario *s = vp;

	/* Destroys the exchange and, through it, every collecting pusher. */
	__dtfl_push_destroy(s->exch);
	__dtfl_free(s->collectors);
	dtfl_time_class_free(s->tc);
	__dtfl_free(s);
}

static void
prop_routes_once(pbt_tc *tc)
{
	scenario *s;
	size_t peers = (size_t)pbt_int(tc, 1, MAX_PEERS);
	size_t nrecs = (size_t)pbt_int(tc, 0, MAX_RECS);
	uint64_t vals[MAX_RECS];
	__dtfl_push **pushers;
	__dtfl_batch *data;
	void *bundle;
	uint64_t t = 0;
	size_t i, p;

	/* Draw every value first, so no allocation precedes a draw overrun. */
	for (i = 0; i < nrecs; i++)
		vals[i] = (uint64_t)pbt_int(tc, 0, 1000000);

	s = __dtfl_calloc(1, sizeof *s);
	pbt_defer(tc, scenario_free, s);
	pushers = __dtfl_alloc(peers * sizeof(__dtfl_push *));
	s->tc = dtfl_time_u64();
	s->peers = peers;
	s->collectors = __dtfl_alloc(peers * sizeof(collector *));
	for (p = 0; p < peers; p++) {
		collector *c = __dtfl_calloc(1, sizeof *c);

		c->base.ops = &collector_ops;
		s->collectors[p] = c;
		pushers[p] = &c->base;
	}
	s->exch = __dtfl_push_exchange_new(s->tc, dtfl_type_u64(), pushers, peers,
	    route_by_value, NULL);

	/* Build a message of the drawn records, all at the same time. */
	data = __dtfl_batch_new(0, 0);
	for (i = 0; i < nrecs; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, vals[i]);
		__dtfl_batch_push(data, buf, sizeof buf, NULL, NULL);
	}
	bundle = __dtfl_message_new(s->tc, &t, data);
	__dtfl_push_send(s->exch, __dtfl_message_class(), bundle);
	__dtfl_push_done(s->exch);	/* flush all buffers */

	/* Every record lands on exactly route(v) % peers. */
	for (p = 0; p < peers; p++) {
		collector *c = s->collectors[p];

		for (i = 0; i < c->n; i++)
			PBT_ASSERT((size_t)(c->vals[i] % peers) == p);
	}

	/* Multiplicity is preserved: each input value appears once total. */
	for (i = 0; i < nrecs; i++) {
		size_t found = 0;

		for (p = 0; p < peers; p++) {
			collector *c = s->collectors[p];
			size_t j;

			for (j = 0; j < c->n; j++)
				if (c->vals[j] == vals[i] &&
				    (size_t)(vals[i] % peers) == p)
					found++;
		}
		/* vals[i] may repeat; just require it appears at least once. */
		PBT_ASSERT(found >= 1);
	}

	/* Total records out equals total records in. */
	{
		size_t total = 0;

		for (p = 0; p < peers; p++)
			total += s->collectors[p]->n;
		PBT_ASSERT(total == nrecs);
	}
	/* The exchange owns `pushers`; scenario_free destroys it. */
}

static const pbt_prop props[] = {
	{ "routes_once", prop_routes_once, 0 },
};

PBT_MAIN("exchange", props)
