/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_ops_notify.c: multiset properties of delay and aggregate.
 *
 * For random inputs on a standalone worker:
 *   delay      shifts every record's time by a function of its value and
 *              preserves the multiset of (new_time, value) pairs: each
 *              input record v (at time 0) comes out once at time f(v).
 *   aggregate  sums the values per key within a time; the per-key sums
 *              must match a reference map computed directly.
 * Heap allocations are released with pbt_defer on case teardown.
 *
 * notify_multiworker is a progress-tracking property over a drawn
 * execution (pbt_sim.h): every worker feeds records at non-decreasing
 * epochs into an exchange followed by a unary_notify that requests a
 * notification at each time it sees.  A notification at t may fire only
 * once no record at t can still arrive, so for every worker and time:
 *   - the records counted when t was notified are all the records at t
 *     that worker ever receives (none arrives late);
 *   - t is notified exactly once if any record at t arrived, else never;
 *   - notifications come in increasing time order;
 *   - each worker receives exactly the records routed to it.
 * A progress update lost, duplicated or applied out of order shows up as
 * a late record or a missing notification (a livelock ends at the
 * simulator's step cap).  Between epochs a worker flushes, takes a drawn
 * number of steps of its own, advances, and steps again.  The production
 * of messages at a time and the release of that time then travel in
 * different progress batches; applying a release before an earlier
 * production lets a frontier pass a time with a message still in
 * flight, which the late check sees.
 *
 * Each worker also flushes after a drawn subset of records and steps
 * before closing.  That reached a hang inherited from the pin, which
 * libdtfl fixes (dev/upstream-notes.md, "Counter drops a flush";
 * tests/corpus/seeds.txt).  DTFL_PBT_FLUSH_STEPS=off skips those flushes.
 * The draw is made either way, so a reproduce blob means the same case
 * with or without it.
 */
#include "pbt.h"
#include "pbt_sim.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdlib.h>

#define MAX_RECS 128

/* ------------------------------------------------------------------ */
/* Feeder and sink (standalone worker)                                 */
/* ------------------------------------------------------------------ */

typedef struct feeder_ctx {
	uint64_t keys[MAX_RECS];
	uint64_t vals[MAX_RECS];
	size_t n;
	bool pair;
} feeder_ctx;

typedef struct collected {
	uint64_t *keys, *vals, *times;	/* heap; freed by the scenario */
	size_t n, cap;
} collected;

static void
collect_push(collected *c, uint64_t key, uint64_t val, uint64_t time)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 16;
		c->keys = realloc(c->keys, c->cap * sizeof(uint64_t));
		c->vals = realloc(c->vals, c->cap * sizeof(uint64_t));
		c->times = realloc(c->times, c->cap * sizeof(uint64_t));
	}
	c->keys[c->n] = key;
	c->vals[c->n] = val;
	c->times[c->n] = time;
	c->n++;
}

static bool
feeder_logic(dtfl_op *op, void *vctx)
{
	feeder_ctx *f = vctx;
	dtfl_cap *cap = dtfl_op_initial_cap(op, 0);
	size_t i;
	uint64_t t = 0;

	if (cap == NULL)
		return false;
	for (i = 0; i < f->n; i++) {
		dtfl_cap *at = dtfl_cap_delayed(cap, &t);
		dtfl_session *s = dtfl_op_session(op, 0, at);
		unsigned char buf[16];

		if (f->pair) {
			dtfl_put_u64(buf, f->keys[i]);
			dtfl_put_u64(buf + 8, f->vals[i]);
			dtfl_session_give(s, buf, 16);
		} else {
			dtfl_put_u64(buf, f->vals[i]);
			dtfl_session_give(s, buf, 8);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(at);
	}
	dtfl_cap_drop(cap);
	return false;
}

static dtfl_stream *
make_feeder(dtfl_scope *scope, feeder_ctx *f, const dtfl_type *type)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Feeder");
	dtfl_stream *out = dtfl_op_builder_output(b, type);

	dtfl_op_builder_build(b, feeder_logic, f, NULL);
	return out;
}

typedef struct sink_ctx {
	collected *c;
	bool pair;
} sink_ctx;

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	sink_ctx *sc = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t i, n = dtfl_message_len(m);
		const uint64_t *t = dtfl_message_time(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			if (sc->pair)
				collect_push(sc->c, dtfl_get_u64(rec),
				    dtfl_get_u64((const unsigned char *)rec + 8), t[0]);
			else
				collect_push(sc->c, 0, dtfl_get_u64(rec), t[0]);
		}
	}
	return false;
}

static void
make_sink(dtfl_stream *in, sink_ctx *sc)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(in), "Sink");

	dtfl_op_builder_input(b, in, dtfl_pact_pipeline());
	dtfl_op_builder_build(b, sink_logic, sc, NULL);
}

/* ------------------------------------------------------------------ */
/* Scenario                                                            */
/* ------------------------------------------------------------------ */

typedef struct scenario {
	dtfl_worker *w;
	dtfl_time_class *tc;
	collected out;
	sink_ctx sink;
	const dtfl_type *pair;	/* retained when used */
} scenario;

static void
scenario_free(void *vp)
{
	scenario *s = vp;

	dtfl_worker_free(s->w);
	dtfl_time_class_free(s->tc);
	if (s->pair != NULL)
		dtfl_type_free(s->pair);
	free(s->out.keys);
	free(s->out.vals);
	free(s->out.times);
	free(s);
}

static scenario *
scenario_new(pbt_tc *tc)
{
	scenario *s = calloc(1, sizeof *s);

	pbt_defer(tc, scenario_free, s);
	s->w = dtfl_worker_new_standalone();
	s->tc = dtfl_time_u64();
	return s;
}

static int
cmp_pair(const void *a, const void *b)
{
	const uint64_t *x = a, *y = b;

	if (x[0] != y[0])
		return x[0] < y[0] ? -1 : 1;
	if (x[1] != y[1])
		return x[1] < y[1] ? -1 : 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* delay: f(v) = v + k shifts the time, preserving (time, value) pairs */
/* ------------------------------------------------------------------ */

static uint64_t g_shift;	/* read-only during a case */

static void
delay_fn(void *ctx, const void *rec, size_t len, const uint64_t *time,
    uint64_t *new_time)
{
	(void)ctx; (void)len; (void)time;
	new_time[0] = dtfl_get_u64(rec) + g_shift;
}

typedef struct delay_build {
	feeder_ctx *f;
	sink_ctx *sink;
} delay_build;

static void
build_delay(dtfl_scope *scope, void *arg)
{
	delay_build *c = arg;
	dtfl_stream *s = make_feeder(scope, c->f, dtfl_type_u64());

	s = dtfl_stream_delay(s, delay_fn, DTFL_NOCTX);
	make_sink(s, c->sink);
}

static void
prop_delay_shift(pbt_tc *tc)
{
	scenario *s = scenario_new(tc);
	feeder_ctx f;
	size_t n = (size_t)pbt_int(tc, 0, 40);
	size_t i;
	uint64_t *want;	/* (new_time, value) pairs */
	delay_build c;

	g_shift = (uint64_t)pbt_int(tc, 0, 1000);
	f.n = n;
	f.pair = false;
	for (i = 0; i < n; i++)
		f.vals[i] = (uint64_t)pbt_int(tc, 0, 1000);

	s->sink.c = &s->out;
	s->sink.pair = false;
	c.f = &f;
	c.sink = &s->sink;
	PBT_ASSERT(dtfl_worker_dataflow(s->w, "delay", s->tc, build_delay, &c) == 0);
	while (dtfl_worker_step(s->w))
		;

	PBT_ASSERT(s->out.n == n);
	/* Every record v comes out exactly once at time v + shift. */
	want = malloc((n ? n : 1) * 2 * sizeof(uint64_t));
	pbt_defer(tc, free, want);
	for (i = 0; i < n; i++) {
		want[2 * i] = f.vals[i] + g_shift;	/* new time */
		want[2 * i + 1] = f.vals[i];		/* value */
	}
	{
		uint64_t *got = malloc((n ? n : 1) * 2 * sizeof(uint64_t));

		pbt_defer(tc, free, got);
		for (i = 0; i < n; i++) {
			got[2 * i] = s->out.times[i];
			got[2 * i + 1] = s->out.vals[i];
			PBT_ASSERT(s->out.times[i] == s->out.vals[i] + g_shift);
		}
		if (n > 1) {
			qsort(want, n, 2 * sizeof(uint64_t), cmp_pair);
			qsort(got, n, 2 * sizeof(uint64_t), cmp_pair);
		}
		for (i = 0; i < 2 * n; i++)
			PBT_ASSERT(got[i] == want[i]);
	}
}

/* ------------------------------------------------------------------ */
/* aggregate: per-key sums match a reference map                       */
/* ------------------------------------------------------------------ */

static void
agg_sum(void *ctx, const void *key, size_t klen, const void *val, size_t vlen,
    void *agg)
{
	(void)ctx; (void)key; (void)klen; (void)vlen;
	*(uint64_t *)agg += dtfl_get_u64(val);
}

static void
agg_emit(void *ctx, const void *key, size_t klen, const void *agg,
    dtfl_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)klen;
	dtfl_put_u64(buf, dtfl_get_u64(key));
	dtfl_put_u64(buf + 8, *(const uint64_t *)agg);
	dtfl_emit_record(out, buf, 16);
}

typedef struct agg_build {
	feeder_ctx *f;
	sink_ctx *sink;
	const dtfl_type *pair;
} agg_build;

static void
build_aggregate(dtfl_scope *scope, void *arg)
{
	agg_build *c = arg;
	dtfl_stream *s = make_feeder(scope, c->f, c->pair);

	s = dtfl_stream_aggregate(s, sizeof(uint64_t), agg_sum, agg_emit, c->pair,
	    DTFL_NOCTX);
	make_sink(s, c->sink);
}

#define NKEYS 8

static void
prop_aggregate_sums(pbt_tc *tc)
{
	scenario *s = scenario_new(tc);
	feeder_ctx f;
	size_t n = (size_t)pbt_int(tc, 0, 60);
	size_t i;
	uint64_t ref[NKEYS] = { 0 };	/* reference per-key sums */
	bool seen[NKEYS] = { false };
	agg_build c;

	s->pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	f.n = n;
	f.pair = true;
	for (i = 0; i < n; i++) {
		f.keys[i] = (uint64_t)pbt_int(tc, 0, NKEYS - 1);
		f.vals[i] = (uint64_t)pbt_int(tc, 0, 1000);
		ref[f.keys[i]] += f.vals[i];
		seen[f.keys[i]] = true;
	}

	s->sink.c = &s->out;
	s->sink.pair = true;
	c.f = &f;
	c.sink = &s->sink;
	c.pair = s->pair;
	PBT_ASSERT(dtfl_worker_dataflow(s->w, "agg", s->tc, build_aggregate, &c)
	    == 0);
	while (dtfl_worker_step(s->w))
		;

	/* One output per distinct key, each the reference sum. */
	{
		size_t distinct = 0;
		bool got[NKEYS] = { false };

		for (i = 0; i < NKEYS; i++)
			if (seen[i])
				distinct++;
		PBT_ASSERT(s->out.n == distinct);
		for (i = 0; i < s->out.n; i++) {
			uint64_t k = s->out.keys[i];

			PBT_ASSERT(k < NKEYS);
			PBT_ASSERT(!got[k]);	/* each key once */
			got[k] = true;
			PBT_ASSERT(s->out.vals[i] == ref[k]);
		}
	}
}

/* ------------------------------------------------------------------ */
/* notify_multiworker: notifications follow progress across workers    */
/* ------------------------------------------------------------------ */

#define NMW_MAXW 4
#define NMW_EPOCHS 6
#define NMW_MAXRECS 48
#define NMW_KEYS 8

typedef struct nmw_out {
	uint64_t got[NMW_EPOCHS];	/* records received at each time */
	uint64_t at_note[NMW_EPOCHS];	/* got[t] when t was notified */
	uint32_t notes[NMW_EPOCHS];	/* notifications at t */
	bool requested[NMW_EPOCHS];
	bool noted_any;
	uint64_t last_note;		/* the latest time notified */
	bool late;			/* a record at a notified time */
	bool order_bad;			/* a notification out of order */
	bool bad_time;			/* a time outside [0, NMW_EPOCHS) */
} nmw_out;

typedef struct nmw_env {
	uint64_t keys[NMW_MAXRECS], epochs[NMW_MAXRECS];
	size_t n;
	/* Own steps after the flush and after the advance at each epoch. */
	uint8_t steps[NMW_MAXW][NMW_EPOCHS][2];
	/* Unless DTFL_PBT_FLUSH_STEPS=off: flush after record i, then take
	 * this many own steps, less one; 0 = no flush. */
	uint8_t flush_after[NMW_MAXRECS];
	bool flush_steps;
	const dtfl_type *type;		/* pair(u64, u64): key, index */
	dtfl_time_class *tc;
	nmw_out out[NMW_MAXW];
} nmw_env;

static uint64_t
nmw_route(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static void
nmw_logic(void *ctx, dtfl_op *op, dtfl_notificator *note)
{
	nmw_out *o = ctx;
	dtfl_message *m;
	dtfl_cap *cap;
	uint64_t count;

	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		uint64_t t = dtfl_message_time(m)[0];

		if (t >= NMW_EPOCHS) {
			o->bad_time = true;
			continue;
		}
		if (o->notes[t] > 0 || (o->noted_any && t <= o->last_note))
			o->late = true;
		o->got[t] += dtfl_message_len(m);
		if (!o->requested[t]) {
			o->requested[t] = true;
			dtfl_notificator_notify_at(note,
			    dtfl_op_input_cap(op, 0, 0));
		}
	}
	while (dtfl_notificator_next(note, &cap, &count)) {
		uint64_t t = dtfl_cap_time(cap)[0];

		if (t >= NMW_EPOCHS) {
			o->bad_time = true;
		} else {
			if (o->noted_any && t <= o->last_note)
				o->order_bad = true;
			o->notes[t]++;
			o->at_note[t] = o->got[t];
			o->last_note = t;
			o->noted_any = true;
		}
		dtfl_cap_drop(cap);
	}
}

typedef struct nmw_build {
	nmw_env *e;
	dtfl_input *in;
} nmw_build;

static void
build_nmw(dtfl_scope *scope, void *arg)
{
	nmw_build *b = arg;
	dtfl_stream *s = dtfl_input_to_stream(b->in, scope);

	(void)dtfl_stream_unary_notify(s, dtfl_pact_exchange(nmw_route, NULL),
	    "NotifyCheck", NULL, 0, dtfl_type_unit(), nmw_logic,
	    DTFL_CTX(&b->e->out[dtfl_scope_index(scope)], NULL));
}

static void
nmw_steps(dtfl_worker *w, unsigned n)
{
	while (n-- > 0)
		(void)dtfl_worker_step(w);
}

/*
 * Each worker feeds the records whose index modulo peers is its own, in
 * epoch order, stepping only itself between epochs (pbt_sim.h), then
 * closes its input and returns; dtfl_execute steps the dataflow to
 * completion.
 */
static void
nmw_worker(dtfl_worker *w, void *arg)
{
	nmw_env *e = arg;
	size_t idx = dtfl_worker_index(w), peers = dtfl_worker_peers(w), i;
	nmw_build b;

	b.e = e;
	b.in = dtfl_input_new(e->type);
	(void)dtfl_worker_dataflow(w, "notify_mw", e->tc, build_nmw, &b);
	for (i = idx; i < e->n; i += peers) {
		unsigned char rec[16];
		uint64_t now = dtfl_input_time(b.in)[0];

		if (e->epochs[i] > now) {
			dtfl_input_flush(b.in);
			nmw_steps(w, e->steps[idx][now][0]);
			dtfl_input_advance_to(b.in, &e->epochs[i]);
			nmw_steps(w, e->steps[idx][now][1]);
		}
		dtfl_put_u64(rec, e->keys[i]);
		dtfl_put_u64(rec + 8, i);
		dtfl_input_send(b.in, rec, sizeof rec);
		if (e->flush_steps && e->flush_after[i] > 0) {
			dtfl_input_flush(b.in);
			nmw_steps(w, e->flush_after[i] - 1u);
		}
	}
	dtfl_input_close(b.in);
}

static void
nmw_free(void *arg)
{
	nmw_env *e = arg;

	dtfl_type_free(e->type);
	dtfl_time_class_free(e->tc);
}

static void
prop_notify_multiworker(pbt_tc *tc)
{
	static nmw_env e;
	pbt_exec x;
	uint64_t epoch = 0;
	size_t i, w, t;

	memset(&e, 0, sizeof e);
	e.n = (size_t)pbt_int(tc, 0, NMW_MAXRECS);
	for (i = 0; i < e.n; i++) {
		if (epoch + 1 < NMW_EPOCHS && pbt_int(tc, 0, 3) == 0)
			epoch++;
		e.keys[i] = (uint64_t)pbt_int(tc, 0, NMW_KEYS - 1);
		e.epochs[i] = epoch;
		e.flush_after[i] = (uint8_t)pbt_int(tc, 0, 3);
	}
	e.flush_steps = getenv("DTFL_PBT_FLUSH_STEPS") == NULL ||
	    strcmp(getenv("DTFL_PBT_FLUSH_STEPS"), "off") != 0;
	pbt_exec_draw(tc, &x, 2, NMW_MAXW);
	for (w = 0; w < x.threads; w++)
		for (t = 0; t < NMW_EPOCHS; t++) {
			e.steps[w][t][0] = (uint8_t)pbt_int(tc, 0, 3);
			e.steps[w][t][1] = (uint8_t)pbt_int(tc, 0, 3);
		}
	{
		char buf[400];
		size_t off = 0;

		for (i = 0; i < e.n && off + 24 < sizeof buf; i++)
			off += (size_t)snprintf(buf + off, sizeof buf - off,
			    " k%llu@%llu/f%u", (unsigned long long)e.keys[i],
			    (unsigned long long)e.epochs[i],
			    e.flush_steps ? e.flush_after[i] : 0u);
		buf[off] = '\0';
		pbt_note(tc, "records (key@epoch/flush):%s", buf);
		off = 0;
		for (w = 0; w < x.threads; w++)
			for (t = 0; t < NMW_EPOCHS && off + 8 < sizeof buf; t++)
				if (e.steps[w][t][0] + e.steps[w][t][1] > 0)
					off += (size_t)snprintf(buf + off,
					    sizeof buf - off, " w%zu.e%zu:%u+%u",
					    w, t, e.steps[w][t][0],
					    e.steps[w][t][1]);
		buf[off] = '\0';
		pbt_note(tc, "epoch steps (flush+advance):%s", buf);
	}
	e.type = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	e.tc = dtfl_time_u64();
	pbt_defer(tc, nmw_free, &e);
	PBT_ASSERT(pbt_exec_run(tc, &x, nmw_worker, &e) == 0);

	for (w = 0; w < x.threads; w++) {
		const nmw_out *o = &e.out[w];

		PBT_ASSERT(!o->bad_time);
		PBT_ASSERT(!o->late);
		PBT_ASSERT(!o->order_bad);
		for (t = 0; t < NMW_EPOCHS; t++) {
			uint64_t want = 0;

			for (i = 0; i < e.n; i++)
				if (e.epochs[i] == t &&
				    e.keys[i] % x.threads == w)
					want++;
			PBT_ASSERT(o->got[t] == want);
			PBT_ASSERT(o->notes[t] == (want > 0 ? 1u : 0u));
			PBT_ASSERT(o->at_note[t] == want);
		}
	}
}

static const pbt_prop props[] = {
	{ "delay_shift", prop_delay_shift, 0 },
	{ "aggregate_sums", prop_aggregate_sums, 0 },
	{ "notify_multiworker", prop_notify_multiworker, 100 },
};

PBT_MAIN("ops_notify", props)
