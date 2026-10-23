/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_op.c: the generic operator builder (P4).
 *
 * Builds dataflows with dtfl_op_builder by hand: a feeder source (no
 * inputs, one output, which sends a run of records at a few times using
 * its initial capability, then drops it), a map-like operator (one
 * pipeline input, one output, which reads each input message and gives
 * each record + 1 at the input capability's time), and a collecting
 * sink.  Also a two-output Tee fan-out, an Exchange pact across two
 * process workers (records land on route(record) % peers), and the
 * capability counting rules (delayed/downgrade/clone/drop move the
 * output frontier; a backwards downgrade panics).
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"
#include "timely/timely_int.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Shared result sink                                                  */
/* ------------------------------------------------------------------ */

#define MAX_OUT 256
#define MAX_WORKERS 4

typedef struct collected {
	uint64_t vals[MAX_OUT];
	uint64_t times[MAX_OUT];
	size_t n;
} collected;

/* ------------------------------------------------------------------ */
/* Feeder: no inputs, one output                                       */
/* ------------------------------------------------------------------ */

typedef struct feeder_ctx {
	const uint64_t *vals;	/* value per record */
	const uint64_t *times;	/* time per record */
	size_t n;
} feeder_ctx;

static bool
feeder_logic(dtfl_op *op, void *vctx)
{
	feeder_ctx *f = vctx;
	dtfl_cap *cap = dtfl_op_initial_cap(op, 0);
	size_t i;

	if (cap == NULL)
		return false;	/* already fed on the first call */
	for (i = 0; i < f->n; i++) {
		dtfl_cap *at = dtfl_cap_delayed(cap, &f->times[i]);
		dtfl_session *s = dtfl_op_session(op, 0, at);
		unsigned char buf[8];

		dtfl_put_u64(buf, f->vals[i]);
		dtfl_session_give(s, buf, sizeof buf);
		dtfl_session_close(s);
		dtfl_cap_drop(at);
	}
	dtfl_cap_drop(cap);	/* release: lets the output frontier advance */
	return false;
}

static dtfl_stream *
make_feeder(dtfl_scope *scope, feeder_ctx *f)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Feeder");
	dtfl_stream *out = dtfl_op_builder_output(b, dtfl_type_u64());

	dtfl_op_builder_build(b, feeder_logic, f, NULL);
	return out;
}

/* ------------------------------------------------------------------ */
/* Map: one input, one output, record + 1                              */
/* ------------------------------------------------------------------ */

static bool
map_logic(dtfl_op *op, void *vctx)
{
	dtfl_message *m;

	(void)vctx;
	/* Drop the unused initial capability on the first call. */
	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			unsigned char buf[8];

			dtfl_put_u64(buf, dtfl_get_u64(rec) + 1);
			dtfl_session_give(s, buf, sizeof buf);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static dtfl_stream *
make_map(dtfl_stream *in)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(in), "Map");
	dtfl_stream *out;

	dtfl_op_builder_input(b, in, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, map_logic, NULL, NULL);
	return out;
}

/* ------------------------------------------------------------------ */
/* Sink: one input, collects records                                   */
/* ------------------------------------------------------------------ */

static bool
sink_logic(dtfl_op *op, void *vctx)
{
	collected *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t i, n = dtfl_message_len(m);
		const uint64_t *t = dtfl_message_time(m);

		for (i = 0; i < n && c->n < MAX_OUT; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);

			c->vals[c->n] = dtfl_get_u64(rec);
			c->times[c->n] = t[0];
			c->n++;
		}
	}
	return false;
}

static void
make_sink(dtfl_stream *in, dtfl_pact pact, collected *c)
{
	dtfl_op_builder *b = dtfl_op_builder_new(dtfl_stream_scope(in), "Sink");

	dtfl_op_builder_input(b, in, pact);
	dtfl_op_builder_build(b, sink_logic, c, NULL);
}

/* ------------------------------------------------------------------ */
/* Running a dataflow on a standalone worker                           */
/* ------------------------------------------------------------------ */

static void
run_standalone(dtfl_build_fn build, void *ctx)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();

	assert_int(dtfl_worker_dataflow(w, "test", tc, build, ctx), ==, 0);
	while (dtfl_worker_step(w))
		;
	dtfl_time_class_free(tc);
	dtfl_worker_free(w);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* ------------------------------------------------------------------ */
/* Test: feeder -> map -> sink                                         */
/* ------------------------------------------------------------------ */

typedef struct map_build_ctx {
	feeder_ctx feeder;
	collected *out;
} map_build_ctx;

static void
build_map_pipeline(dtfl_scope *scope, void *arg)
{
	map_build_ctx *c = arg;
	dtfl_stream *src = make_feeder(scope, &c->feeder);
	dtfl_stream *mapped = make_map(src);

	make_sink(mapped, dtfl_pact_pipeline(), c->out);
}

DTFL_TEST(builder__map_pipeline)
{
	static const uint64_t vals[] = { 10, 20, 30, 40 };
	static const uint64_t times[] = { 0, 0, 1, 2 };
	collected out;
	map_build_ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&out, 0, sizeof out);
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 4;
	c.out = &out;
	run_standalone(build_map_pipeline, &c);

	assert_size(out.n, ==, 4);
	qsort(out.vals, out.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 4; i++)
		assert_uint64(out.vals[i], ==, vals[i] + 1);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Test: a two-output Tee fan-out                                      */
/* ------------------------------------------------------------------ */

typedef struct tee_build_ctx {
	feeder_ctx feeder;
	collected *a, *b;
} tee_build_ctx;

static void
build_tee(dtfl_scope *scope, void *arg)
{
	tee_build_ctx *c = arg;
	dtfl_stream *src = make_feeder(scope, &c->feeder);

	/* One stream feeds two sinks: the Tee must deliver to both. */
	make_sink(src, dtfl_pact_pipeline(), c->a);
	make_sink(src, dtfl_pact_pipeline(), c->b);
}

DTFL_TEST(tee__fan_out)
{
	static const uint64_t vals[] = { 5, 6, 7 };
	static const uint64_t times[] = { 0, 0, 0 };
	collected a, b;
	tee_build_ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&a, 0, sizeof a);
	memset(&b, 0, sizeof b);
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 3;
	c.a = &a;
	c.b = &b;
	run_standalone(build_tee, &c);

	assert_size(a.n, ==, 3);
	assert_size(b.n, ==, 3);
	qsort(a.vals, a.n, sizeof(uint64_t), cmp_u64);
	qsort(b.vals, b.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 3; i++) {
		assert_uint64(a.vals[i], ==, vals[i]);
		assert_uint64(b.vals[i], ==, vals[i]);
	}
	return MUNIT_OK;
}

/*
 * Pass-through operators (concat, inspect, exchange's single-worker
 * path, probe) forward a received message by sharing its batch rather
 * than copying its records.  Two chains of them hang off one source, and
 * a map (which builds new records) runs on one chain, so the shared
 * batches reach several consumers and must stay unchanged in all.
 */
static uint64_t
route_by_value_fwd(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static void
add_one(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *e)
{
	unsigned char out[8];

	(void)ctx;
	(void)time;
	(void)len;
	dtfl_put_u64(out, dtfl_get_u64(rec) + 100);
	dtfl_emit_record(e, out, sizeof out);
}

static void
count_records(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	(void)time;
	(void)rec;
	(void)len;
	(*(size_t *)ctx)++;
}

typedef struct share_ctx {
	feeder_ctx feeder;
	collected a, b, c;
	size_t inspected;
} share_ctx;

static void
build_share(dtfl_scope *scope, void *arg)
{
	share_ctx *c = arg;
	dtfl_stream *src = make_feeder(scope, &c->feeder);
	dtfl_stream *ins = dtfl_stream_inspect(src, count_records,
	    DTFL_CTX(&c->inspected, NULL));
	dtfl_stream *cat = dtfl_stream_concat(ins, src);
	dtfl_stream *mapped = dtfl_stream_flat_map(cat, dtfl_type_u64(), add_one,
	    DTFL_CTX(NULL, NULL));

	make_sink(cat, dtfl_pact_pipeline(), &c->a);
	make_sink(mapped, dtfl_pact_pipeline(), &c->b);
	make_sink(src, dtfl_pact_exchange(route_by_value_fwd, NULL), &c->c);
}

DTFL_TEST(session__shared_forwarding_keeps_records)
{
	static const uint64_t vals[] = { 5, 6, 7, 8 };
	static const uint64_t times[] = { 0, 0, 1, 1 };
	share_ctx c;
	size_t i;

	(void)params; (void)data;
	memset(&c, 0, sizeof c);
	c.feeder.vals = vals;
	c.feeder.times = times;
	c.feeder.n = 4;
	run_standalone(build_share, &c);

	/* cat = inspect(src) ++ src: every value twice. */
	assert_size(c.inspected, ==, 4);
	assert_size(c.a.n, ==, 8);
	assert_size(c.b.n, ==, 8);
	assert_size(c.c.n, ==, 4);
	qsort(c.a.vals, c.a.n, sizeof(uint64_t), cmp_u64);
	qsort(c.b.vals, c.b.n, sizeof(uint64_t), cmp_u64);
	qsort(c.c.vals, c.c.n, sizeof(uint64_t), cmp_u64);
	for (i = 0; i < 8; i++) {
		assert_uint64(c.a.vals[i], ==, vals[i / 2]);
		assert_uint64(c.b.vals[i], ==, vals[i / 2] + 100);
	}
	for (i = 0; i < 4; i++)
		assert_uint64(c.c.vals[i], ==, vals[i]);
	return MUNIT_OK;
}

/*
 * Whether a forwarded message is shared depends on the element hooks.
 * A feeder emits records with (drop, retain) hooks of its choosing, a
 * pass-through operator forwards each message with
 * dtfl_session_give_message under hooks of its own, and a sink records
 * the hooks of the batches it receives.  A hookless forwarder, or one
 * with the batch's own hooks, shares the batch: the sink sees the
 * feeder's hooks and each record is dropped once and never retained
 * again.  A forwarder with other hooks copies into its own buffer (the
 * sink sees the forwarder's hooks).  Each hook counts its calls.
 */
static size_t n_drop_a, n_retain_a, n_retain_b;

static void hook_drop_a(void *rec) { (void)rec; n_drop_a++; }
static void hook_retain_a(void *rec) { (void)rec; n_retain_a++; }
static void hook_retain_b(void *rec) { (void)rec; n_retain_b++; }

typedef struct hook_pair {
	void (*drop)(void *rec);
	void (*retain)(void *rec);
} hook_pair;

typedef struct hooks_ctx {
	hook_pair feed, fwd;
	hook_pair seen[8];
	size_t nseen, nrecs;
} hooks_ctx;

static bool
hook_feeder_logic(dtfl_op *op, void *vctx)
{
	hooks_ctx *c = vctx;
	dtfl_cap *cap = dtfl_op_initial_cap(op, 0);
	dtfl_session *s;
	unsigned char buf[8];
	uint64_t v;

	if (cap == NULL)
		return false;
	s = dtfl_op_session(op, 0, cap);
	__dtfl_session_set_batch_ptr_hooks(s, c->feed.drop, c->feed.retain);
	for (v = 0; v < 3; v++) {
		dtfl_put_u64(buf, v);
		dtfl_session_give(s, buf, sizeof buf);
	}
	dtfl_session_close(s);
	dtfl_cap_drop(cap);
	return false;
}

static bool
hook_forward_logic(dtfl_op *op, void *vctx)
{
	hooks_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);

		if (c->fwd.drop != NULL || c->fwd.retain != NULL)
			__dtfl_session_set_batch_ptr_hooks(s, c->fwd.drop,
			    c->fwd.retain);
		dtfl_session_give_message(s, m);
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static bool
hook_sink_logic(dtfl_op *op, void *vctx)
{
	hooks_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		if (c->nseen < 8) {
			c->seen[c->nseen].drop = m->data->elem_drop;
			c->seen[c->nseen].retain = m->data->elem_retain;
			c->nseen++;
		}
		c->nrecs += dtfl_message_len(m);
	}
	return false;
}

static void
build_hooks(dtfl_scope *scope, void *arg)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "HookFeeder");
	dtfl_stream *src = dtfl_op_builder_output(b, dtfl_type_u64()), *fwd;

	dtfl_op_builder_build(b, hook_feeder_logic, arg, NULL);
	b = dtfl_op_builder_new(scope, "HookForward");
	dtfl_op_builder_input(b, src, dtfl_pact_pipeline());
	fwd = dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, hook_forward_logic, arg, NULL);
	b = dtfl_op_builder_new(scope, "HookSink");
	dtfl_op_builder_input(b, fwd, dtfl_pact_pipeline());
	dtfl_op_builder_build(b, hook_sink_logic, arg, NULL);
}

DTFL_TEST(session__forward_shares_only_with_equal_hooks)
{
	hooks_ctx c;

	(void)params; (void)data;

	/* Owning-pointer batch through a hookless forwarder: shared. */
	memset(&c, 0, sizeof c);
	n_drop_a = n_retain_a = n_retain_b = 0;
	c.feed.drop = hook_drop_a;
	c.feed.retain = hook_retain_a;
	run_standalone(build_hooks, &c);
	assert_size(c.nrecs, ==, 3);
	assert_size(c.nseen, >=, 1);
	assert_ptr_equal(c.seen[0].drop, hook_drop_a);
	assert_size(n_retain_a, ==, 0);
	assert_size(n_drop_a, ==, 3);

	/* The same hooks on both: shared, so each record dropped once. */
	memset(&c, 0, sizeof c);
	n_drop_a = n_retain_a = n_retain_b = 0;
	c.feed.drop = c.fwd.drop = hook_drop_a;
	c.feed.retain = c.fwd.retain = hook_retain_a;
	run_standalone(build_hooks, &c);
	assert_size(c.nrecs, ==, 3);
	assert_ptr_equal(c.seen[0].drop, hook_drop_a);
	assert_ptr_equal(c.seen[0].retain, hook_retain_a);
	assert_size(n_retain_a, ==, 0);
	assert_size(n_drop_a, ==, 3);

	/* Retain-only hooks on the forwarder, none on the feed: copied. */
	memset(&c, 0, sizeof c);
	n_drop_a = n_retain_a = n_retain_b = 0;
	c.fwd.retain = hook_retain_b;
	run_standalone(build_hooks, &c);
	assert_size(c.nrecs, ==, 3);
	assert_ptr_equal(c.seen[0].retain, hook_retain_b);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Test: Exchange pact across two process workers                      */
/* ------------------------------------------------------------------ */

/* Route by the record value, so the landing worker is value % peers. */
static uint64_t
route_by_value(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

typedef struct exch_ctx {
	collected per_worker[MAX_WORKERS];
} exch_ctx;

/* A u64 class shared by the worker threads; built once per process run. */
static dtfl_time_class *g_shared_tc;

static void
build_exchange(dtfl_scope *scope, void *arg)
{
	exch_ctx *e = arg;
	size_t idx = dtfl_scope_index(scope);
	/* Every worker feeds the same values; exchange routes them. */
	static const uint64_t vals[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	static const uint64_t times[] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	static feeder_ctx feeders[MAX_WORKERS];
	dtfl_stream *src;

	feeders[idx].vals = vals;
	feeders[idx].times = times;
	feeders[idx].n = 8;
	src = make_feeder(scope, &feeders[idx]);
	make_sink(src, dtfl_pact_exchange(route_by_value, NULL), &e->per_worker[idx]);
}

static void
exch_worker(dtfl_worker *w, void *arg)
{
	exch_ctx *e = arg;

	assert_int(dtfl_worker_dataflow(w, "exch", g_shared_tc, build_exchange, e),
	    ==, 0);
}

DTFL_TEST(exchange__two_process_workers)
{
	dtfl_config *cfg = dtfl_config_new();
	exch_ctx e;
	size_t peers = 2, w, i;

	(void)params; (void)data;
	memset(&e, 0, sizeof e);
	g_shared_tc = dtfl_time_u64();
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, peers), ==, 0);
	assert_int(dtfl_execute(cfg, exch_worker, &e, NULL), ==, 0);

	/*
	 * Each of the 2 workers feeds values 0..7; after exchange, worker k
	 * receives exactly the values v with v % 2 == k, from every worker.
	 * So each value's multiplicity is peers (one copy per feeder).
	 */
	for (w = 0; w < peers; w++) {
		size_t evens = 0, odds = 0;

		for (i = 0; i < e.per_worker[w].n; i++) {
			uint64_t v = e.per_worker[w].vals[i];

			assert_size((size_t)(v % peers), ==, w);
			if (v % 2 == 0)
				evens++;
			else
				odds++;
		}
		/* 4 even (or odd) values, each sent by peers feeders. */
		if (w == 0)
			assert_size(evens, ==, 4 * peers);
		else
			assert_size(odds, ==, 4 * peers);
	}
	dtfl_time_class_free(g_shared_tc);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

/*
 * Exchange flushes a peer's buffer once it holds a container's worth of
 * records, as upstream's CapacityContainerBuilder does, rather than
 * holding a whole epoch until the time changes.  Every worker sends
 * NBIG records at time 0 to worker 1; no message reaching worker 1 may
 * carry much more than DTFL_BUFFER_SIZE_BYTES (8 KiB) of records.
 */
#define NBIG 20000

typedef struct big_sink {
	size_t msgs;
	size_t recs;
	size_t max_len;
} big_sink;

typedef struct big_ctx {
	big_sink per_worker[MAX_WORKERS];
} big_ctx;

static uint64_t
route_to_one(const void *rec, size_t len, void *ctx)
{
	(void)rec; (void)len; (void)ctx;
	return 1;
}

static bool
big_feeder_logic(dtfl_op *op, void *vctx)
{
	dtfl_cap *cap = dtfl_op_initial_cap(op, 0);
	dtfl_session *s;
	size_t i;

	(void)vctx;
	if (cap == NULL)
		return false;
	s = dtfl_op_session(op, 0, cap);
	for (i = 0; i < NBIG; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, i);
		dtfl_session_give(s, buf, sizeof buf);
	}
	dtfl_session_close(s);
	dtfl_cap_drop(cap);
	return false;
}

static bool
big_sink_logic(dtfl_op *op, void *vctx)
{
	big_sink *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		size_t n = dtfl_message_len(m);

		c->msgs++;
		c->recs += n;
		if (n > c->max_len)
			c->max_len = n;
	}
	return false;
}

static void
build_big_exchange(dtfl_scope *scope, void *arg)
{
	big_ctx *e = arg;
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Feeder");
	dtfl_stream *src = dtfl_op_builder_output(b, dtfl_type_u64());

	dtfl_op_builder_build(b, big_feeder_logic, NULL, NULL);
	b = dtfl_op_builder_new(scope, "Sink");
	dtfl_op_builder_input(b, src, dtfl_pact_exchange(route_to_one, NULL));
	dtfl_op_builder_build(b, big_sink_logic,
	    &e->per_worker[dtfl_scope_index(scope)], NULL);
}

static void
big_worker(dtfl_worker *w, void *arg)
{
	assert_int(dtfl_worker_dataflow(w, "big", g_shared_tc,
	    build_big_exchange, arg), ==, 0);
}

DTFL_TEST(exchange__flushes_at_capacity)
{
	dtfl_config *cfg = dtfl_config_new();
	big_ctx e;

	(void)params; (void)data;
	memset(&e, 0, sizeof e);
	g_shared_tc = dtfl_time_u64();
	assert_int(dtfl_config_set_mode(cfg, DTFL_COMM_PROCESS), ==, 0);
	assert_int(dtfl_config_set_threads(cfg, 2), ==, 0);
	assert_int(dtfl_execute(cfg, big_worker, &e, NULL), ==, 0);
	assert_size(e.per_worker[0].recs, ==, 0);
	assert_size(e.per_worker[1].recs, ==, 2 * NBIG);
	/* 8-byte records, 8 KiB containers: about 1024 per message. */
	assert_size(e.per_worker[1].max_len, <=, 2048);
	assert_size(e.per_worker[1].msgs, >=, 2 * NBIG / 2048);
	dtfl_time_class_free(g_shared_tc);
	dtfl_config_free(cfg);
	return MUNIT_OK;
}

/* ------------------------------------------------------------------ */
/* Test: capability counting                                           */
/* ------------------------------------------------------------------ */

typedef struct cap_probe {
	bool checked;
} cap_probe;

/*
 * The capability operator holds its initial capability, then on the
 * second schedule inspects the output frontier through a downstream sink
 * and exercises delayed/clone/downgrade/drop.  Simpler: use a dedicated
 * logic that performs the moves and records the resulting internal batch
 * via a sink's observed frontier.
 */
static bool
cap_feeder_logic(dtfl_op *op, void *vctx)
{
	cap_probe *p = vctx;
	dtfl_cap *cap, *a, *b;
	uint64_t t1 = 1, t2 = 2, t0 = 0;

	if (p->checked)
		return false;
	p->checked = true;
	cap = dtfl_op_initial_cap(op, 0);
	assert_uint64(dtfl_cap_time(cap)[0], ==, 0);
	assert_size(dtfl_cap_output(cap), ==, 0);

	/* delayed does not move the source; try_delayed backwards is NULL. */
	a = dtfl_cap_delayed(cap, &t1);
	assert_uint64(dtfl_cap_time(a)[0], ==, 1);
	assert_true(dtfl_cap_try_delayed(a, &t0) == NULL);

	/* clone shares the time. */
	b = dtfl_cap_clone(a);
	assert_uint64(dtfl_cap_time(b)[0], ==, 1);

	/* downgrade forward moves it; backward returns DTFL_EINVAL. */
	assert_int(dtfl_cap_try_downgrade(b, &t2), ==, 0);
	assert_uint64(dtfl_cap_time(b)[0], ==, 2);
	assert_int(dtfl_cap_try_downgrade(b, &t0), ==, DTFL_EINVAL);

	dtfl_cap_drop(a);
	dtfl_cap_drop(b);
	dtfl_cap_drop(cap);	/* release everything so the dataflow completes */
	return false;
}

static void
build_cap(dtfl_scope *scope, void *arg)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Caps");

	(void)dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, cap_feeder_logic, arg, NULL);
}

DTFL_TEST(capability__counting)
{
	cap_probe p;

	(void)params; (void)data;
	memset(&p, 0, sizeof p);
	run_standalone(build_cap, &p);
	assert_true(p.checked);
	return MUNIT_OK;
}

/* A backwards downgrade panics (capability.rs downgrade). */
static dtfl_cap *g_panic_cap;

static bool
cap_panic_logic(dtfl_op *op, void *vctx)
{
	cap_probe *p = vctx;
	uint64_t t1 = 1, t0 = 0;

	if (p->checked)
		return false;
	p->checked = true;
	/* Stash the capability so the harness can drop it after the panic. */
	g_panic_cap = dtfl_op_initial_cap(op, 0);
	dtfl_cap_downgrade(g_panic_cap, &t1);
	/* This panics: 0 is not greater or equal to 1. */
	dtfl_cap_downgrade(g_panic_cap, &t0);
	return false;
}

static void
build_cap_panic(dtfl_scope *scope, void *arg)
{
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "CapsPanic");

	(void)dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, cap_panic_logic, arg, NULL);
}

/* The worker outlives the panic so the harness can free it (no leak). */
static dtfl_worker *g_panic_worker;
static dtfl_time_class *g_panic_tc;

static void
run_panic(void)
{
	cap_probe p;

	memset(&p, 0, sizeof p);
	g_panic_worker = dtfl_worker_new_standalone();
	g_panic_tc = dtfl_time_u64();
	assert_int(dtfl_worker_dataflow(g_panic_worker, "test", g_panic_tc,
	    build_cap_panic, &p), ==, 0);
	while (dtfl_worker_step(g_panic_worker))
		;
}

DTFL_TEST(capability__downgrade_backwards_panics)
{
	(void)params; (void)data;
	g_panic_worker = NULL;
	g_panic_tc = NULL;
	g_panic_cap = NULL;
	DTFL_EXPECT_PANIC(run_panic());
	/* Release what the panic left behind, so ASan stays clean. */
	dtfl_cap_drop(g_panic_cap);
	dtfl_worker_free(g_panic_worker);
	dtfl_time_class_free(g_panic_tc);
	return MUNIT_OK;
}

/*
 * A pooled message: freed on its own worker it goes back on that
 * worker's list, comes back with every field reset, and freed on
 * another worker (another list) it goes to the hooks instead of that
 * worker's list.  A clone is never a pooled block.
 */
DTFL_TEST(message_pool_stays_with_its_worker)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_freelist mine, theirs;
	dtfl_message *m, *m2, *c;
	const __dtfl_msg_class *mc = __dtfl_message_class();
	uint64_t t = 5;

	(void)params; (void)data;
	__dtfl_freelist_init(&mine, sizeof(dtfl_message), 4);
	__dtfl_freelist_init(&theirs, sizeof(dtfl_message), 4);
	m = __dtfl_message_new_pooled(&mine, tc, &t, __dtfl_batch_new(0, 0));
	m->from = 7;
	m->seq = 9;
	c = mc->clone(mc, m);
	assert_true(c->pool == 0);
	__dtfl_message_free_local(c, &mine);	/* not mine: to the hooks */
	assert_size(mine.len, ==, 0);
	__dtfl_message_free_local(m, &mine);
	assert_size(mine.len, ==, 1);
	t = 6;
	m2 = __dtfl_message_new_pooled(&mine, tc, &t, __dtfl_batch_new(0, 0));
	assert_ptr_equal(m2, m);
	assert_size(m2->from, ==, 0);
	assert_size(m2->seq, ==, 0);
	assert_uint64(m2->time[0], ==, 6);
	assert_size(__dtfl_batch_len(m2->data), ==, 0);
	__dtfl_message_free_local(m2, &theirs);
	assert_size(theirs.len, ==, 0);
	assert_size(mine.len, ==, 0);
	__dtfl_freelist_free(&mine);
	__dtfl_freelist_free(&theirs);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(message_pool_stays_with_its_worker),
	DTFL_T(builder__map_pipeline),
	DTFL_T(tee__fan_out),
	DTFL_T(session__shared_forwarding_keeps_records),
	DTFL_T(session__forward_shares_only_with_equal_hooks),
	DTFL_T(exchange__two_process_workers),
	DTFL_T(exchange__flushes_at_capacity),
	DTFL_T(capability__counting),
	DTFL_T(capability__downgrade_backwards_panics),
	DTFL_T_END
};

DTFL_TEST_MAIN("op", tests)
