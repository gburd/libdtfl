/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_reduce.c: reduce, count and distinct equal a per-key
 * reference, across epochs, and a multi-worker count converges.
 *
 * The model is a per-key accumulation: for each (key, value) the summed i64
 * diff over all updates at times <= q.  count's output at a key is the sum
 * of its values' diffs; distinct's is the sign (1 if any value accumulates
 * non-zero); reduce's is a user fold (here min value with diff 1).  reduce is
 * incremental, so re-running at a new epoch retracts the stale output and
 * asserts the new one; the property checks the NET output accumulated at each
 * epoch against the reference, which catches a reduce that re-emits without
 * retracting (it would double).  A final property runs count on several
 * workers that exchange by key and checks the summed output converges to the
 * same per-key counts as the single-worker reference.  It draws its
 * execution (pbt_sim.h): two to four workers on real threads or on the
 * simulator, with the simulator's seed and faults drawn too.
 */
#include "pbt.h"
#include "pbt_sim.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <string.h>

#define MAXOPS 32
#define NKEY 4
#define NVAL 4
#define NEPOCH 4

typedef struct op {
	uint64_t key;
	uint64_t val;
	uint64_t time;
	int64_t diff;
} op;

typedef struct out_upd {
	uint64_t key;
	uint64_t uval;		/* field 2 as u64 (reduce value) */
	int64_t ival;		/* field 2 as i64 (count value) */
	uint64_t time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAXOPS * 8];
	size_t n;
} collected;

static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;

	if (c->n >= MAXOPS * 8)
		return;
	c->u[c->n].key = dtfl_get_u64(rec);
	c->u[c->n].uval = len >= 16 ?
	    dtfl_get_u64((const unsigned char *)rec + 8) : 0;
	c->u[c->n].ival = len >= 16 ?
	    dtfl_get_i64((const unsigned char *)rec + 8) : 0;
	c->u[c->n].time = time[0];
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/* Reference: net input diff of (key, val) at times <= q. */
static int64_t
ref_kv(const op *ops, size_t nops, uint64_t key, uint64_t val, uint64_t q)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < nops; i++)
		if (ops[i].key == key && ops[i].val == val && ops[i].time <= q)
			s += ops[i].diff;
	return s;
}

/* Reference: count of key (sum of its values' diffs) at <= q. */
static int64_t
ref_count(const op *ops, size_t nops, uint64_t key, uint64_t q)
{
	int64_t s = 0;
	uint64_t v;

	for (v = 0; v < NVAL; v++)
		s += ref_kv(ops, nops, key, v, q);
	return s;
}

/* Reference: the minimum value of key with non-zero accumulation at <= q. */
static int64_t
ref_min(const op *ops, size_t nops, uint64_t key, uint64_t q, bool *has)
{
	uint64_t v;

	*has = false;
	for (v = 0; v < NVAL; v++)
		if (ref_kv(ops, nops, key, v, q) != 0) {
			*has = true;
			return (int64_t)v;
		}
	return 0;
}

/* Net output diff of (key, uval) accumulated at times <= q. */
static int64_t
out_kv(const collected *c, uint64_t key, uint64_t uval, uint64_t q)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].key == key && c->u[i].uval == uval &&
		    c->u[i].time <= q)
			s += c->u[i].diff;
	return s;
}

/* Net output diff of (key, count-value) accumulated at <= q, by i64 value. */
static int64_t
out_count(const collected *c, uint64_t key, int64_t cnt, uint64_t q)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].key == key && c->u[i].ival == cnt &&
		    c->u[i].time <= q)
			s += c->u[i].diff;
	return s;
}

/* Net output diff of key (whole record is key, for distinct) at <= q. */
static int64_t
out_key(const collected *c, uint64_t key, uint64_t q)
{
	size_t i;
	int64_t s = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].key == key && c->u[i].time <= q)
			s += c->u[i].diff;
	return s;
}

typedef enum which { W_REDUCE, W_COUNT, W_DISTINCT } which;

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
	which w;
} build_ctx;

static void
min_logic(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t one = 1;
	unsigned char vbuf[8];

	(void)ctx; (void)key; (void)klen; (void)n; (void)vlens; (void)diffs;
	dtfl_put_u64(vbuf, dtfl_get_u64(vals[0]));
	dtfl_reduce_emit_pair(out, vbuf, sizeof vbuf, &one);
}

static void
build_fn(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_diff *od = dtfl_diff_i64();
	dtfl_collection *red, *ins, *probed;

	if (c->w == W_REDUCE)
		red = dtfl_collection_reduce(base, dtfl_type_u64(),
		    dtfl_type_u64(), dtfl_type_u64(), od, min_logic,
		    DTFL_NOCTX);
	else if (c->w == W_COUNT)
		red = dtfl_collection_count(base, od);
	else
		red = dtfl_collection_distinct(base, od);
	ins = dtfl_collection_inspect(red, sink, DTFL_CTX(c->out, NULL));
	probed = dtfl_collection_probe_with(ins, c->probe);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(red);
	dtfl_diff_free(od);
	dtfl_collection_free(base);
}

static void
free_worker(void *w) { dtfl_worker_free(w); }
static void
free_tc(void *tc) { dtfl_time_class_free(tc); }
static void
free_diff(void *d) { dtfl_diff_free(d); }
static void
free_probe(void *p) { dtfl_probe_free(p); }

typedef struct probe_cond {
	dtfl_probe *probe;
	uint64_t target;
} probe_cond;

static bool
probe_behind(void *vctx)
{
	probe_cond *pc = vctx;

	return dtfl_probe_less_than(pc->probe, &pc->target);
}

/* Generate a random op sequence over keys, values, epochs. */
static size_t
gen_ops(pbt_tc *tc, op *ops)
{
	size_t nops = (size_t)pbt_int(tc, 0, MAXOPS), i;
	uint64_t epoch = 0;

	for (i = 0; i < nops; i++) {
		if (epoch + 1 < NEPOCH && pbt_bool(tc))
			epoch++;
		ops[i].key = (uint64_t)pbt_int(tc, 0, NKEY - 1);
		ops[i].val = (uint64_t)pbt_int(tc, 0, NVAL - 1);
		ops[i].time = epoch;
		ops[i].diff = pbt_bool(tc) ? 1 : -1;
	}
	return nops;
}

/* Drive a reduce-family op sequence through a single standalone worker. */
static void
run_single(pbt_tc *tc, which w, const op *ops, size_t nops, collected *out)
{
	dtfl_worker *wk = dtfl_worker_new_standalone();
	dtfl_time_class *tcl = dtfl_time_u64();
	dtfl_diff *r = dtfl_diff_i64();
	build_ctx c;
	const dtfl_type *rt;
	uint64_t cur = 0;
	size_t i;

	pbt_defer(tc, free_worker, wk);
	pbt_defer(tc, free_tc, tcl);
	pbt_defer(tc, free_diff, r);
	memset(out, 0, sizeof *out);
	if (w == W_REDUCE) {
		const dtfl_type *f[2] = { dtfl_type_u64(), dtfl_type_u64() };

		rt = dtfl_type_tuple(f, 2);
	} else {
		rt = dtfl_type_u64();
	}
	c.in = dtfl_input_session_new(rt, r);
	c.probe = dtfl_probe_new();
	pbt_defer(tc, free_probe, c.probe);
	c.out = out;
	c.w = w;
	PBT_ASSERT(dtfl_worker_dataflow(wk, "p", tcl, build_fn, &c) == 0);

	for (i = 0; i < nops; i++) {
		unsigned char rec[16];

		if (ops[i].time > cur) {
			cur = ops[i].time;
			dtfl_input_session_advance_to(c.in, &cur);
			dtfl_input_session_flush(c.in);
			{ probe_cond pc = { c.probe, cur };
			  dtfl_worker_step_while(wk, probe_behind, &pc); }
		}
		dtfl_put_u64(rec, ops[i].key);
		if (w == W_REDUCE) {
			dtfl_put_u64(rec + 8, ops[i].val);
			if (ops[i].diff > 0)
				dtfl_input_session_insert(c.in, rec, 16);
			else
				dtfl_input_session_remove(c.in, rec, 16);
		} else {
			if (ops[i].diff > 0)
				dtfl_input_session_insert(c.in, rec, 8);
			else
				dtfl_input_session_remove(c.in, rec, 8);
		}
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(wk))
		;
	if (w == W_REDUCE)
		dtfl_type_free(rt);
}

/* reduce (min) equals the reference, with the retract-old/assert-new delta. */
static void
prop_reduce_min(pbt_tc *tc)
{
	static op ops[MAXOPS];
	static collected out;
	size_t nops = gen_ops(tc, ops);
	uint64_t k, q;

	run_single(tc, W_REDUCE, ops, nops, &out);
	for (k = 0; k < NKEY; k++)
		for (q = 0; q < NEPOCH; q++) {
			bool has;
			int64_t m = ref_min(ops, nops, k, q, &has);
			uint64_t v;

			/* The emitted min value accumulates to exactly 1, and
			 * every other value to 0 (retract-old/assert-new). */
			for (v = 0; v < NVAL; v++) {
				int64_t got = out_kv(&out, k, v, q);

				if (has && v == (uint64_t)m)
					PBT_ASSERT(got == 1);
				else
					PBT_ASSERT(got == 0);
			}
		}
}

/* count equals the per-key sum of diffs. */
static void
prop_count(pbt_tc *tc)
{
	static op ops[MAXOPS];
	static collected out;
	size_t nops = gen_ops(tc, ops);
	uint64_t k, q;

	run_single(tc, W_COUNT, ops, nops, &out);
	for (k = 0; k < NKEY; k++)
		for (q = 0; q < NEPOCH; q++) {
			int64_t cnt = ref_count(ops, nops, k, q);
			int64_t v;

			/* The output value equal to the count is +1; all other
			 * count-values accumulate to 0 (stale retracted). */
			for (v = -(int64_t)(MAXOPS); v <= (int64_t)MAXOPS; v++) {
				int64_t got = out_count(&out, k, v, q);

				if (cnt != 0 && v == cnt)
					PBT_ASSERT(got == 1);
				else
					PBT_ASSERT(got == 0);
			}
		}
}

/* distinct equals the sign of the per-key accumulation. */
static void
prop_distinct(pbt_tc *tc)
{
	static op ops[MAXOPS];
	static collected out;
	size_t nops = gen_ops(tc, ops);
	uint64_t k, q;

	run_single(tc, W_DISTINCT, ops, nops, &out);
	for (k = 0; k < NKEY; k++)
		for (q = 0; q < NEPOCH; q++) {
			int64_t cnt = ref_count(ops, nops, k, q);

			PBT_ASSERT(out_key(&out, k, q) == (cnt != 0 ? 1 : 0));
		}
}

/* ------------------------------------------------------------------ */
/* Multi-worker count: workers exchanging by key.                       */
/* ------------------------------------------------------------------ */

#define MAXW 4

typedef struct mw_ctx {
	op *ops;
	size_t nops;
	dtfl_time_class *tc;
	bool wait;		/* step to each epoch while feeding */
	uint8_t steps[MAXW][NEPOCH];	/* own steps at each epoch change */
	collected out[MAXW];	/* per-worker captured output */
} mw_ctx;

static void
mw_worker(dtfl_worker *w, void *arg)
{
	mw_ctx *e = arg;
	size_t idx = dtfl_worker_index(w);
	size_t peers = dtfl_worker_peers(w);
	build_ctx c;
	dtfl_diff *r = dtfl_diff_i64();
	uint64_t cur = 0;
	size_t i;

	memset(&e->out[idx], 0, sizeof e->out[idx]);
	c.in = dtfl_input_session_new(dtfl_type_u64(), r);
	c.probe = dtfl_probe_new();
	c.out = &e->out[idx];
	c.w = W_COUNT;
	dtfl_worker_dataflow(w, "mwc", e->tc, build_fn, &c);

	/* Each worker feeds the ops whose index modulo peers is its own, so
	 * every op is fed exactly once across the workers; count exchanges by
	 * key, so each key accumulates on one worker regardless.  On real
	 * threads a worker waits for each epoch to complete before feeding
	 * the next; the simulator runs worker functions one after another,
	 * so there a worker only takes a drawn number of its own steps at
	 * each epoch change (pbt_sim.h). */
	for (i = 0; i < e->nops; i++) {
		unsigned char k[8];

		if (e->ops[i].time > cur) {
			unsigned n = e->steps[idx][cur];

			cur = e->ops[i].time;
			dtfl_input_session_advance_to(c.in, &cur);
			dtfl_input_session_flush(c.in);
			if (e->wait) {
				probe_cond pc = { c.probe, cur };

				dtfl_worker_step_while(w, probe_behind, &pc);
			}
			while (n-- > 0)
				(void)dtfl_worker_step(w);
		}
		if ((i % peers) != idx)
			continue;
		dtfl_put_u64(k, e->ops[i].key);
		if (e->ops[i].diff > 0)
			dtfl_input_session_insert(c.in, k, 8);
		else
			dtfl_input_session_remove(c.in, k, 8);
	}
	dtfl_input_session_close(c.in);
	dtfl_probe_free(c.probe);
	dtfl_diff_free(r);
}

static void
free_tc_ctx(void *e)
{
	dtfl_time_class_free(((mw_ctx *)e)->tc);
}

static void
prop_count_multiworker(pbt_tc *tc)
{
	static op ops[MAXOPS];
	static mw_ctx e;
	size_t nops = gen_ops(tc, ops);
	pbt_exec x;
	uint64_t k, q;
	size_t wi;

	pbt_exec_draw(tc, &x, 2, MAXW);
	memset(&e, 0, sizeof e);
	for (wi = 0; wi < x.threads; wi++)
		for (q = 0; q < NEPOCH; q++)
			e.steps[wi][q] = (uint8_t)pbt_int(tc, 0, 3);
	e.ops = ops;
	e.nops = nops;
	e.tc = dtfl_time_u64();
	pbt_defer(tc, free_tc_ctx, &e);
	e.wait = x.mode != DTFL_COMM_SIM;
	PBT_ASSERT(pbt_exec_run(tc, &x, mw_worker, &e) == 0);

	/* The summed output across workers converges to the reference count
	 * per key: for each key the count-value equal to the reference sum is
	 * +1 at each epoch (one worker owns each key, so no double counting). */
	for (k = 0; k < NKEY; k++)
		for (q = 0; q < NEPOCH; q++) {
			int64_t cnt = ref_count(ops, nops, k, q);
			int64_t v;

			for (v = -(int64_t)MAXOPS; v <= (int64_t)MAXOPS; v++) {
				int64_t got = 0;

				for (wi = 0; wi < x.threads; wi++)
					got += out_count(&e.out[wi], k, v, q);
				if (cnt != 0 && v == cnt)
					PBT_ASSERT(got == 1);
				else
					PBT_ASSERT(got == 0);
			}
		}
}

static const pbt_prop props[] = {
	{ "reduce_min", prop_reduce_min, 80 },
	{ "count", prop_count, 80 },
	{ "distinct", prop_distinct, 80 },
	{ "count_multiworker", prop_count_multiworker, 20 },
};

PBT_MAIN("reduce", props)
