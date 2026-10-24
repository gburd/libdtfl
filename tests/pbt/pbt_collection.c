/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_collection.c: a consolidated Collection equals a naive
 * reference.
 *
 * Over a random sequence of insert and remove operations spread across a
 * few records and epochs, the model is the map (record, time) -> summed
 * i64 diff, with zero sums dropped.  The property builds a one-worker
 * dataflow that feeds the operations through an input session, applies
 * dtfl_collection_consolidate and records the output with an inspect
 * sink, then steps to termination and checks the output multiset against
 * the model: the same keys, the same diffs, and no zero diffs.
 */
#include "pbt.h"

#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAXOPS 48
#define NREC 4
#define NEPOCH 3

typedef struct op {
	uint64_t rec;
	uint64_t time;
	int64_t diff;
} op;

/* Collected consolidated output. */
typedef struct out_upd {
	uint64_t rec;
	uint64_t time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAXOPS * 4];
	size_t n;
} collected;

static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;

	(void)len;
	c->u[c->n].rec = dtfl_get_u64(rec);
	c->u[c->n].time = time[0];
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* The model: sum of diffs for (rec, time). */
static int64_t
model_sum(const op *ops, size_t nops, uint64_t rec, uint64_t time)
{
	uint64_t s = 0;
	size_t i;

	for (i = 0; i < nops; i++)
		if (ops[i].rec == rec && ops[i].time == time)
			s += (uint64_t)ops[i].diff;
	return (int64_t)s;
}

/* Distinct (rec, time) keys with a non-zero sum. */
static size_t
model_count(const op *ops, size_t nops)
{
	size_t i, j, n = 0;

	for (i = 0; i < nops; i++) {
		for (j = 0; j < i; j++)
			if (ops[j].rec == ops[i].rec &&
			    ops[j].time == ops[i].time)
				break;
		if (j == i && model_sum(ops, nops, ops[i].rec, ops[i].time) != 0)
			n++;
	}
	return n;
}

static int64_t
out_diff(const collected *c, uint64_t rec, uint64_t time)
{
	size_t i;

	for (i = 0; i < c->n; i++)
		if (c->u[i].rec == rec && c->u[i].time == time)
			return c->u[i].diff;
	return 0;
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
} build_ctx;

static void
build_dataflow(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *con = dtfl_collection_consolidate(base);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(base);
}

static void
free_worker(void *w)
{
	dtfl_worker_free(w);
}

static void
free_tc(void *tc)
{
	dtfl_time_class_free(tc);
}

static void
free_diff(void *d)
{
	dtfl_diff_free(d);
}

static void
free_probe(void *p)
{
	dtfl_probe_free(p);
}

static void
prop_consolidate_model(pbt_tc *tc)
{
	static op ops[MAXOPS];
	size_t nops, i, epoch;
	dtfl_worker *w;
	dtfl_time_class *tcl;
	dtfl_diff *r;
	build_ctx c;
	static collected out;

	nops = (size_t)pbt_int(tc, 0, MAXOPS);
	/* Draw operations in non-decreasing epoch order. */
	epoch = 0;
	for (i = 0; i < nops; i++) {
		if (epoch + 1 < NEPOCH && pbt_bool(tc))
			epoch++;
		ops[i].rec = (uint64_t)pbt_int(tc, 0, NREC - 1);
		ops[i].time = epoch;
		ops[i].diff = pbt_bool(tc) ? 1 : -1;
	}

	w = dtfl_worker_new_standalone();
	pbt_defer(tc, free_worker, w);
	tcl = dtfl_time_u64();
	pbt_defer(tc, free_tc, tcl);
	r = dtfl_diff_i64();
	pbt_defer(tc, free_diff, r);
	memset(&out, 0, sizeof out);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	pbt_defer(tc, free_probe, c.probe);
	c.out = &out;
	PBT_ASSERT(dtfl_worker_dataflow(w, "p", tcl, build_dataflow, &c) == 0);

	/* Feed the operations, advancing the epoch as it increases. */
	{
		uint64_t cur = 0;

		for (i = 0; i < nops; i++) {
			unsigned char key[8];

			if (ops[i].time > cur) {
				cur = ops[i].time;
				dtfl_input_session_advance_to(c.in, &cur);
			}
			dtfl_put_u64(key, ops[i].rec);
			if (ops[i].diff > 0)
				dtfl_input_session_insert(c.in, key, sizeof key);
			else
				dtfl_input_session_remove(c.in, key, sizeof key);
		}
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;
	PBT_ASSERT(dtfl_probe_done(c.probe));

	/* Output holds exactly the model's non-zero sums, no zeros. */
	PBT_ASSERT(out.n == model_count(ops, nops));
	for (i = 0; i < out.n; i++) {
		PBT_ASSERT(out.u[i].diff != 0);
		PBT_ASSERT(out.u[i].diff ==
		    model_sum(ops, nops, out.u[i].rec, out.u[i].time));
	}
	/* Every model key appears in the output with the summed diff. */
	{
		uint64_t rec, t;

		for (rec = 0; rec < NREC; rec++)
			for (t = 0; t < NEPOCH; t++)
				PBT_ASSERT(out_diff(&out, rec, t) ==
				    model_sum(ops, nops, rec, t));
	}
}

static const pbt_prop props[] = {
	{ "consolidate_model", prop_consolidate_model, 0 },
};

PBT_MAIN("collection", props)
