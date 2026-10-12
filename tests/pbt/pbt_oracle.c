/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_oracle.c: a differential fuzz oracle for count and
 * distinct (PLAN section 6 tiers 3 and 4).
 *
 * Hegel generates a small random update stream (<= 8 keys, <= 16 updates,
 * <= 3 epochs, |diff| <= 3) and the property runs it through libdtfl's
 * count and distinct operators across those epochs.  The reference answer
 * is a from-scratch C model: per key, per time, the accumulated input diff
 * (count) or whether it is non-zero (distinct), with zeros dropped.  The
 * incremental operator output must reconstruct that model collection at
 * every epoch frontier (the Adequate square of tier 3, applied to count
 * and distinct).
 *
 * This is the in-C oracle.  tests/interop/test_fuzz_oracle.c anchors the
 * same operators to REAL differential-dataflow on a fixed committed seed
 * set, so the broad random search here (C against C) is backed by a sample
 * cross-checked against Rust.
 *
 * Cases are tiny on purpose: each finishes well under a second.  Run under
 * `timeout 20` standalone.
 */
#include "pbt.h"

#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdlib.h>

#define NKEYS 8
#define MAX_UPDATES 16
#define MAX_EPOCHS 3		/* epochs 0 .. MAX_EPOCHS-1 */
#define MAX_OUT 512
#define MAX_REC 16

/* One generated update. */
typedef struct gen_upd {
	uint64_t key;
	uint64_t epoch;
	int64_t diff;
} gen_upd;

/* Collected operator output (record bytes, time, diff). */
typedef struct out_upd {
	unsigned char rec[MAX_REC];
	size_t rlen;
	uint64_t time;
	int64_t diff;
} out_upd;

typedef struct collected {
	out_upd u[MAX_OUT];
	size_t n;
	bool overflow;
} collected;

static void
inspect_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	collected *c = ctx;
	uint64_t t;

	if (c->n >= MAX_OUT || len > MAX_REC) {
		c->overflow = true;
		return;
	}
	memcpy(c->u[c->n].rec, rec, len);
	c->u[c->n].rlen = len;
	memcpy(&t, time, sizeof t);
	c->u[c->n].time = t;
	memcpy(&c->u[c->n].diff, diff, sizeof(int64_t));
	c->n++;
}

/*
 * The collection libdtfl defines for a record at time t: the sum of every
 * output diff for that record at a time <= t.  A faithful incremental
 * operator makes this 0 or 1 for a count/distinct output (the record is
 * either present once or absent) and equal to the model.
 */
static int64_t
coll_at(const collected *c, const unsigned char *rec, size_t rlen, uint64_t t)
{
	size_t i;
	int64_t sum = 0;

	for (i = 0; i < c->n; i++)
		if (c->u[i].rlen == rlen && c->u[i].time <= t &&
		    memcmp(c->u[i].rec, rec, rlen) == 0)
			sum += c->u[i].diff;
	return sum;
}

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

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	collected *out;
	bool distinct;
} build_ctx;

static void
build_op(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *base = dtfl_input_session_to_collection(c->in, scope);
	dtfl_diff *od = dtfl_diff_i64();
	dtfl_collection *op = c->distinct ? dtfl_collection_distinct(base, od)
	    : dtfl_collection_count(base, od);
	dtfl_collection *con = dtfl_collection_consolidate(op);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_diff_free(od);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(op);
	dtfl_collection_free(base);
}

/* Teardown for one operator run. */
typedef struct run_state {
	dtfl_worker *w;
	dtfl_time_class *tc;
	dtfl_diff *r;
	dtfl_probe *probe;
} run_state;

static void
run_state_free(void *vp)
{
	run_state *s = vp;

	if (s->probe != NULL)
		dtfl_probe_free(s->probe);
	if (s->tc != NULL)
		dtfl_time_class_free(s->tc);
	if (s->r != NULL)
		dtfl_diff_free(s->r);
	if (s->w != NULL)
		dtfl_worker_free(s->w);
	free(s);
}

/*
 * Run count (or distinct) over the generated stream and collect the
 * consolidated output into `out`.  The worker, time class, diff and probe
 * live in a deferred run_state so a PBT_ASSERT unwind still frees them.
 */
static void
run_operator(pbt_tc *tc, const gen_upd *stream, size_t n, uint64_t max_epoch,
    bool distinct, collected *out)
{
	run_state *s = calloc(1, sizeof *s);
	build_ctx c;
	uint64_t cur = 0, e;
	size_t i;

	pbt_defer(tc, run_state_free, s);
	s->w = dtfl_worker_new_standalone();
	s->tc = dtfl_time_u64();
	s->r = dtfl_diff_i64();
	s->probe = dtfl_probe_new();
	c.in = dtfl_input_session_new(dtfl_type_u64(), s->r);
	c.probe = s->probe;
	c.out = out;
	c.distinct = distinct;
	PBT_ASSERT(dtfl_worker_dataflow(s->w, "op", s->tc, build_op, &c) == 0);

	/* Feed the stream epoch by epoch. */
	for (i = 0; i < n; i++) {
		unsigned char k[8];

		while (cur < stream[i].epoch) {
			uint64_t next = cur + 1;
			probe_cond pc = { s->probe, next };

			dtfl_input_session_advance_to(c.in, &next);
			dtfl_input_session_flush(c.in);
			dtfl_worker_step_while(s->w, probe_behind, &pc);
			cur = next;
		}
		dtfl_put_u64(k, stream[i].key);
		dtfl_input_session_update(c.in, k, 8, &stream[i].diff);
	}
	/* Drive to the final epoch frontier so every time has settled. */
	for (e = cur; e <= max_epoch; e++) {
		uint64_t next = e + 1;
		probe_cond pc = { s->probe, next };

		dtfl_input_session_advance_to(c.in, &next);
		dtfl_input_session_flush(c.in);
		dtfl_worker_step_while(s->w, probe_behind, &pc);
	}
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(s->w))
		;
	PBT_ASSERT(!out->overflow);
}

/* ------------------------------------------------------------------ */
/* The property                                                         */
/* ------------------------------------------------------------------ */
static void
prop_count_distinct(pbt_tc *tc)
{
	gen_upd stream[MAX_UPDATES];
	size_t n = (size_t)pbt_int(tc, 0, MAX_UPDATES);
	uint64_t max_epoch = 0;
	static collected count_out, dist_out;
	size_t i;
	uint64_t t, key;

	for (i = 0; i < n; i++) {
		int64_t mag;

		stream[i].key = (uint64_t)pbt_int(tc, 0, NKEYS - 1);
		/* Epochs are non-decreasing: an update either stays at the
		 * current epoch or advances by one (capped at MAX_EPOCHS - 1).
		 * The driver only moves the input frontier forward, so a
		 * non-monotonic epoch would land at the wrong time; keeping the
		 * sequence monotonic makes the drawn epoch the actual epoch. */
		if (i > 0)
			stream[i].epoch = stream[i - 1].epoch;
		else
			stream[i].epoch = 0;
		if (pbt_bool(tc) && stream[i].epoch + 1 < MAX_EPOCHS)
			stream[i].epoch += 1;
		mag = pbt_int(tc, 1, 3);
		stream[i].diff = pbt_bool(tc) ? mag : -mag;
		if (stream[i].epoch > max_epoch)
			max_epoch = stream[i].epoch;
	}

	memset(&count_out, 0, sizeof count_out);
	memset(&dist_out, 0, sizeof dist_out);
	run_operator(tc, stream, n, max_epoch, false, &count_out);
	run_operator(tc, stream, n, max_epoch, true, &dist_out);

	/*
	 * The in-C model: for each (key, time), the accumulated input diff is
	 * the sum of stream diffs with epoch <= time.  count's collection must
	 * then hold exactly {key || acc} with multiplicity 1 when acc != 0;
	 * distinct's must hold {key} with multiplicity 1 when acc != 0.  No
	 * other output record for that key may be present.
	 */
	for (t = 0; t <= max_epoch; t++) {
		for (key = 0; key < NKEYS; key++) {
			int64_t acc = 0;
			unsigned char cr[MAX_REC], dr[8];
			size_t crl;

			for (i = 0; i < n; i++)
				if (stream[i].key == key && stream[i].epoch <= t)
					acc += stream[i].diff;

			/* count output record: key || acc (i64). */
			dtfl_put_u64(cr, key);
			dtfl_put_i64(cr + 8, acc);
			crl = 16;
			/* distinct output record: just the key. */
			dtfl_put_u64(dr, key);

			if (acc != 0) {
				PBT_ASSERT(coll_at(&count_out, cr, crl, t) == 1);
				PBT_ASSERT(coll_at(&dist_out, dr, 8, t) == 1);
			} else {
				/* No count record for this key is present (any
				 * value): check the zero accumulation yields
				 * nothing for the key with the acc value, and
				 * distinct is absent. */
				PBT_ASSERT(coll_at(&count_out, cr, crl, t) == 0);
				PBT_ASSERT(coll_at(&dist_out, dr, 8, t) == 0);
			}
		}
	}

	/*
	 * Cross-check that count never emits a stale value: for every key, at
	 * most one count record is present at time t, and it is the one the
	 * model names.  Scan output records and confirm any present record
	 * (coll_at == 1) matches the model accumulation for its key.
	 */
	for (t = 0; t <= max_epoch; t++) {
		for (i = 0; i < count_out.n; i++) {
			const out_upd *o = &count_out.u[i];
			uint64_t k;
			int64_t present, acc = 0, stored;
			size_t j;

			if (o->time > t || o->rlen != 16)
				continue;
			present = coll_at(&count_out, o->rec, o->rlen, t);
			if (present == 0)
				continue;
			PBT_ASSERT(present == 1);
			k = dtfl_get_u64(o->rec);
			stored = dtfl_get_i64(o->rec + 8);
			for (j = 0; j < n; j++)
				if (stream[j].key == k && stream[j].epoch <= t)
					acc += stream[j].diff;
			PBT_ASSERT(stored == acc);
		}
	}
}

static const pbt_prop props[] = {
	{ "count_distinct", prop_count_distinct, 200 },
};

PBT_MAIN("oracle", props)
