/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_join.c: join equals a reference nested-loop join.
 *
 * Two random collections of key||val updates over a few keys, values,
 * diffs and epochs are joined.  The reference model is the nested-loop
 * equijoin: for every pair of updates (val1, t1, d1) and (val2, t2, d2)
 * sharing a key, it accumulates the product diff d1 * d2 at the joined
 * time max(t1, t2) for the output record key||val1||val2, then drops the
 * (record, time) pairs whose diffs sum to zero.  The property asserts the
 * operator's consolidated output multiset equals the model, over single
 * worker runs and a multi-worker run that exchanges by key and must
 * converge to the same result.  The multi-worker run draws its execution
 * (pbt_sim.h): two to four workers on real threads or on the simulator,
 * with the simulator's seed and faults drawn too.
 */
#include "pbt.h"
#include "pbt_sim.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAXOPS 20
#define NKEY 3
#define NVAL 3
#define NEPOCH 3

/* An input update: key, value (both small u64), epoch time, +/- 1 diff. */
typedef struct op {
	uint64_t key;
	uint64_t val;
	uint64_t time;
	int64_t diff;
} op;

/* An output update: record bytes (key||val1||val2), time, diff. */
typedef struct out_upd {
	unsigned char rec[24];
	size_t rlen;
	uint64_t time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[256];
	size_t n;
} collected;

static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	uint64_t t;

	if (c->n >= 256 || len > sizeof c->u[0].rec)
		return;
	memcpy(c->u[c->n].rec, rec, len);
	c->u[c->n].rlen = len;
	/* The embedded time may be unaligned after a variable-length record. */
	memcpy(&t, time, sizeof t);
	c->u[c->n].time = t;
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* Net output diff of a record at a time, summed over the collected output. */
static int64_t
out_net(const collected *c, const void *rec, size_t rlen, uint64_t time)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].rlen == rlen && memcmp(c->u[i].rec, rec, rlen) == 0 &&
		    c->u[i].time == time)
			s += c->u[i].diff;
	return s;
}

/*
 * The reference model's net diff for output record key||v1||v2 at `t`:
 * sum over every pair (op1 in a with key, time t1), (op2 in b with key,
 * time t2) with max(t1, t2) == t of op1.diff * op2.diff.
 */
static int64_t
model_net(const op *a, size_t na, const op *b, size_t nb, uint64_t key,
    uint64_t v1, uint64_t v2, uint64_t t)
{
	int64_t s = 0;
	size_t i, j;

	for (i = 0; i < na; i++) {
		if (a[i].key != key || a[i].val != v1)
			continue;
		for (j = 0; j < nb; j++) {
			uint64_t jt;

			if (b[j].key != key || b[j].val != v2)
				continue;
			jt = a[i].time > b[j].time ? a[i].time : b[j].time;
			if (jt == t)
				s += a[i].diff * b[j].diff;
		}
	}
	return s;
}

/* ------------------------------------------------------------------ */
/* Build: two input sessions joined with key||v1||v2 output.           */
/* ------------------------------------------------------------------ */

typedef struct build_ctx {
	dtfl_input_session *in1;
	dtfl_input_session *in2;
	dtfl_probe *probe;
	collected *out;
	const dtfl_type *out_type;
} build_ctx;

static void
build_join(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *c1 = dtfl_input_session_to_collection(c->in1, scope);
	dtfl_collection *c2 = dtfl_input_session_to_collection(c->in2, scope);
	dtfl_collection *j = dtfl_collection_join(c1, c2, dtfl_type_u64(),
	    dtfl_type_u64(), dtfl_type_u64(), c->out_type);
	dtfl_collection *ins = dtfl_collection_inspect(j, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(j);
	dtfl_collection_free(c1);
	dtfl_collection_free(c2);
}

static void
feed(dtfl_input_session *in, const op *ops, size_t n)
{
	size_t i;
	uint64_t cur = 0;

	for (i = 0; i < n; i++) {
		unsigned char rec[16];
		size_t off = 0;

		if (ops[i].time > cur) {
			cur = ops[i].time;
			dtfl_input_session_advance_to(in, &cur);
		}
		off += dtfl_put_u64(rec, ops[i].key);
		off += dtfl_put_u64(rec + off, ops[i].val);
		if (ops[i].diff > 0)
			dtfl_input_session_insert(in, rec, off);
		else
			dtfl_input_session_remove(in, rec, off);
	}
}

static void free_worker(void *w) { dtfl_worker_free(w); }
static void free_tc(void *tc) { dtfl_time_class_free(tc); }
static void free_diff(void *d) { dtfl_diff_free(d); }
static void free_probe(void *p) { dtfl_probe_free(p); }
static void free_type(void *t) { dtfl_type_free(t); }

/* Generates `n` random ops over the small key/value/epoch space. */
static size_t
gen_ops(pbt_tc *tc, op *ops)
{
	size_t n = (size_t)pbt_int(tc, 0, MAXOPS), i;
	uint64_t epoch = 0;

	for (i = 0; i < n; i++) {
		if (epoch + 1 < NEPOCH && pbt_bool(tc))
			epoch++;
		ops[i].key = (uint64_t)pbt_int(tc, 0, NKEY - 1);
		ops[i].val = (uint64_t)pbt_int(tc, 0, NVAL - 1);
		ops[i].time = epoch;
		ops[i].diff = pbt_bool(tc) ? 1 : -1;
	}
	return n;
}

/* Checks the collected output against the reference model. */
static void
check(pbt_tc *tc, const collected *out, const op *a, size_t na, const op *b,
    size_t nb)
{
	uint64_t k, v1, v2, t;
	size_t i;

	for (i = 0; i < out->n; i++)
		PBT_ASSERT(out->u[i].diff != 0);
	for (k = 0; k < NKEY; k++)
		for (v1 = 0; v1 < NVAL; v1++)
			for (v2 = 0; v2 < NVAL; v2++)
				for (t = 0; t < NEPOCH; t++) {
					unsigned char rec[24];
					size_t off = 0;

					off += dtfl_put_u64(rec, k);
					off += dtfl_put_u64(rec + off, v1);
					off += dtfl_put_u64(rec + off, v2);
					PBT_ASSERT(out_net(out, rec, off, t) ==
					    model_net(a, na, b, nb, k, v1, v2,
					    t));
				}
}

static void
prop_join_model(pbt_tc *tc)
{
	static op a[MAXOPS], b[MAXOPS];
	size_t na, nb;
	dtfl_worker *w;
	dtfl_time_class *tcl;
	dtfl_diff *r;
	const dtfl_type *t1, *out_type;
	build_ctx c;
	static collected out;

	na = gen_ops(tc, a);
	nb = gen_ops(tc, b);

	w = dtfl_worker_new_standalone();
	pbt_defer(tc, free_worker, w);
	tcl = dtfl_time_u64();
	pbt_defer(tc, free_tc, tcl);
	r = dtfl_diff_i64();
	pbt_defer(tc, free_diff, r);
	t1 = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	pbt_defer(tc, free_type, (void *)t1);
	{
		const dtfl_type *f[3] = { dtfl_type_u64(), dtfl_type_u64(),
		    dtfl_type_u64() };

		out_type = dtfl_type_tuple(f, 3);
	}
	pbt_defer(tc, free_type, (void *)out_type);
	memset(&out, 0, sizeof out);
	c.in1 = dtfl_input_session_new(t1, r);
	c.in2 = dtfl_input_session_new(t1, r);
	c.probe = dtfl_probe_new();
	pbt_defer(tc, free_probe, c.probe);
	c.out = &out;
	c.out_type = out_type;
	PBT_ASSERT(dtfl_worker_dataflow(w, "pj", tcl, build_join, &c) == 0);

	feed(c.in1, a, na);
	feed(c.in2, b, nb);
	dtfl_input_session_close(c.in1);
	dtfl_input_session_close(c.in2);
	while (dtfl_worker_step(w))
		;

	check(tc, &out, a, na, b, nb);
}

/* ------------------------------------------------------------------ */
/* Multi-worker join converges to the same result.                     */
/* ------------------------------------------------------------------ */

#define MAXW 4

typedef struct mw_env {
	const op *a;
	size_t na;
	const op *b;
	size_t nb;
	collected out[MAXW];
	const dtfl_type *out_type;
	const dtfl_time_class *tc;
} mw_env;

static void
mw_worker(dtfl_worker *w, void *arg)
{
	mw_env *e = arg;
	size_t idx = dtfl_worker_index(w);
	dtfl_diff *r = dtfl_diff_i64();
	const dtfl_type *t1 = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	build_ctx c;

	memset(&c, 0, sizeof c);
	c.in1 = dtfl_input_session_new(t1, r);
	c.in2 = dtfl_input_session_new(t1, r);
	c.probe = dtfl_probe_new();
	c.out = &e->out[idx];
	c.out_type = e->out_type;
	(void)dtfl_worker_dataflow(w, "pjmw", e->tc, build_join, &c);

	/*
	 * Worker 0 feeds all the data; the exchange routes by key.  The
	 * worker returns without stepping, so dtfl_execute runs the
	 * dataflow to completion (required under the simulator; pbt_sim.h).
	 */
	if (idx == 0) {
		feed(c.in1, e->a, e->na);
		feed(c.in2, e->b, e->nb);
	}
	dtfl_input_session_close(c.in1);
	dtfl_input_session_close(c.in2);
	dtfl_probe_free(c.probe);
	dtfl_diff_free(r);
	dtfl_type_free(t1);
}

static void
prop_join_multiworker(pbt_tc *tc)
{
	static op a[MAXOPS], b[MAXOPS];
	size_t na, nb, i;
	pbt_exec x;
	dtfl_time_class *tcl;
	const dtfl_type *out_type;
	static mw_env e;
	collected merged;
	uint64_t k, v1, v2, t;

	na = gen_ops(tc, a);
	nb = gen_ops(tc, b);
	pbt_exec_draw(tc, &x, 2, MAXW);
	memset(&e, 0, sizeof e);
	e.a = a; e.na = na; e.b = b; e.nb = nb;
	tcl = dtfl_time_u64();
	pbt_defer(tc, free_tc, tcl);
	{
		const dtfl_type *f[3] = { dtfl_type_u64(), dtfl_type_u64(),
		    dtfl_type_u64() };

		out_type = dtfl_type_tuple(f, 3);
	}
	pbt_defer(tc, free_type, (void *)out_type);
	e.out_type = out_type;
	e.tc = tcl;

	PBT_ASSERT(pbt_exec_run(tc, &x, mw_worker, &e) == 0);

	/* Merge the workers' outputs; each key is owned by one. */
	memset(&merged, 0, sizeof merged);
	for (i = 0; i < x.threads; i++) {
		size_t j;

		for (j = 0; j < e.out[i].n && merged.n < 256; j++)
			merged.u[merged.n++] = e.out[i].u[j];
	}
	for (k = 0; k < NKEY; k++)
		for (v1 = 0; v1 < NVAL; v1++)
			for (v2 = 0; v2 < NVAL; v2++)
				for (t = 0; t < NEPOCH; t++) {
					unsigned char rec[24];
					size_t off = 0;

					off += dtfl_put_u64(rec, k);
					off += dtfl_put_u64(rec + off, v1);
					off += dtfl_put_u64(rec + off, v2);
					PBT_ASSERT(out_net(&merged, rec, off, t)
					    == model_net(a, na, b, nb, k, v1,
					    v2, t));
				}
}

static const pbt_prop props[] = {
	{ "join_model", prop_join_model, 0 },
	{ "join_multiworker", prop_join_multiworker, 0 },
};

PBT_MAIN("join", props)
