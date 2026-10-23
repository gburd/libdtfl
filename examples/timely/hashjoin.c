/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/hashjoin.c: mirrors timely/examples/hashjoin.rs.
 *
 * Arguments: [keys vals batch] [timely flags].  Two inputs carry (key,
 * value) records; a binary operator joins them, keeping a map of values
 * per key for each side and, as records arrive on one side, emitting the
 * cross product against the other side's stored values.  Both inputs use
 * an exchange pact routed by key, so matching keys meet on one worker.
 * With no positional arguments a small fixed pair of relations is used so
 * the number of join results is exact and checkable; with keys/vals/batch
 * a random workload is generated as upstream.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed relations: left (key, v1), right (key, v2).  Join on key. */
static const uint64_t left_rel[][2] = {
	{ 1, 10 }, { 1, 11 }, { 2, 20 }, { 3, 30 },
};
static const uint64_t right_rel[][2] = {
	{ 1, 100 }, { 2, 200 }, { 2, 201 }, { 4, 400 },
};
#define LEFT_N (sizeof left_rel / sizeof left_rel[0])
#define RIGHT_N (sizeof right_rel / sizeof right_rel[0])
/*
 * Expected join results (key present on both sides, cross product):
 *   key 1: 2 left x 1 right = 2
 *   key 2: 1 left x 2 right = 2
 * total = 4.
 */

/* A multimap of key -> list of values (linear scan; small here). */
typedef struct kv { uint64_t key, val; } kv;
typedef struct map {
	kv *v;
	size_t n, cap;
} map;

static void
map_push(map *m, uint64_t key, uint64_t val)
{
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 16;
		m->v = realloc(m->v, m->cap * sizeof *m->v);
	}
	m->v[m->n].key = key;
	m->v[m->n].val = val;
	m->n++;
}

typedef struct join_ctx {
	map map1, map2;
	size_t *count;	/* join results emitted */
} join_ctx;

static bool
join_logic(dtfl_op *op, void *vctx)
{
	join_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	/* input 0: probe map2, then insert into map1. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k; i++) {
			size_t len, j;
			const void *rec = dtfl_message_record(m, i, &len);
			uint64_t key = dtfl_get_u64(rec);
			uint64_t v1 = dtfl_get_u64((const unsigned char *)rec + 8);

			for (j = 0; j < c->map2.n; j++)
				if (c->map2.v[j].key == key) {
					unsigned char buf[16];

					dtfl_put_u64(buf, v1);
					dtfl_put_u64(buf + 8, c->map2.v[j].val);
					dtfl_session_give(s, buf, sizeof buf);
				}
			map_push(&c->map1, key, v1);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}

	/* input 1: probe map1, then insert into map2. */
	while ((m = dtfl_op_input_next(op, 1)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 1, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k; i++) {
			size_t len, j;
			const void *rec = dtfl_message_record(m, i, &len);
			uint64_t key = dtfl_get_u64(rec);
			uint64_t v2 = dtfl_get_u64((const unsigned char *)rec + 8);

			for (j = 0; j < c->map1.n; j++)
				if (c->map1.v[j].key == key) {
					unsigned char buf[16];

					dtfl_put_u64(buf, c->map1.v[j].val);
					dtfl_put_u64(buf + 8, v2);
					dtfl_session_give(s, buf, sizeof buf);
				}
			map_push(&c->map2, key, v2);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
join_free(void *vctx)
{
	join_ctx *c = vctx;

	free(c->map1.v);
	free(c->map2.v);
	free(c);
}

static uint64_t
route_by_key(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

/* Count sink. */
static bool
count_logic(dtfl_op *op, void *vctx)
{
	size_t *n = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL)
		*n += dtfl_message_len(m);
	return false;
}

typedef struct build_ctx {
	dtfl_input *in1, *in2;
	dtfl_probe *probe;
	size_t *count;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	dtfl_stream *s1 = dtfl_input_to_stream(c->in1, scope);
	dtfl_stream *s2 = dtfl_input_to_stream(c->in2, scope);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "HashJoin");
	join_ctx *jc = calloc(1, sizeof *jc);
	dtfl_stream *out;
	dtfl_op_builder *cb;

	jc->count = c->count;
	dtfl_op_builder_input(b, s1, dtfl_pact_exchange(route_by_key, NULL));
	dtfl_op_builder_input(b, s2, dtfl_pact_exchange(route_by_key, NULL));
	/* As Operator::binary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	dtfl_op_builder_set_notify(b, 1, DTFL_NOTIFY_NEVER);
	out = dtfl_op_builder_output(b, pair);
	dtfl_op_builder_build(b, join_logic, jc, join_free);
	out = dtfl_stream_probe_with(out, c->probe);
	cb = dtfl_op_builder_new(scope, "Count");
	dtfl_op_builder_input(cb, out, dtfl_pact_pipeline());
	dtfl_op_builder_build(cb, count_logic, c->count, NULL);
	dtfl_type_free(pair);
}

typedef struct run_ctx {
	bool fixed;
	size_t keys, vals, batch;
	size_t total_count[16];
} run_ctx;

static void
worker(dtfl_worker *w, void *ctx)
{
	run_ctx *r = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	size_t index = dtfl_worker_index(w);
	size_t peers = dtfl_worker_peers(w);
	build_ctx c;
	uint64_t next = 1;
	size_t i;

	c.in1 = dtfl_input_new(pair);
	c.in2 = dtfl_input_new(pair);
	c.probe = dtfl_probe_new();
	c.count = &r->total_count[index];
	dtfl_worker_dataflow(w, "hashjoin", tc, build, &c);
	dtfl_type_free(pair);

	if (r->fixed) {
		if (index == 0) {
			for (i = 0; i < LEFT_N; i++) {
				unsigned char buf[16];

				dtfl_put_u64(buf, left_rel[i][0]);
				dtfl_put_u64(buf + 8, left_rel[i][1]);
				dtfl_input_send(c.in1, buf, sizeof buf);
			}
			for (i = 0; i < RIGHT_N; i++) {
				unsigned char buf[16];

				dtfl_put_u64(buf, right_rel[i][0]);
				dtfl_put_u64(buf + 8, right_rel[i][1]);
				dtfl_input_send(c.in2, buf, sizeof buf);
			}
		}
	} else {
		uint64_t seed = 0x9e3779b97f4a7c15ull * (index + 1);
		size_t per = r->vals / (peers ? peers : 1);

		for (i = 0; i < per; i++) {
			unsigned char b1[16], b2[16];
			uint64_t k;

			seed = seed * 6364136223846793005ull + 1442695040888963407ull;
			k = seed % (r->keys ? r->keys : 1);
			dtfl_put_u64(b1, k);
			dtfl_put_u64(b1 + 8, seed);
			dtfl_put_u64(b2, k);
			dtfl_put_u64(b2 + 8, seed >> 7);
			dtfl_input_send(c.in1, b1, sizeof b1);
			dtfl_input_send(c.in2, b2, sizeof b2);
		}
	}
	dtfl_input_advance_to(c.in1, &next);
	dtfl_input_advance_to(c.in2, &next);
	while (dtfl_probe_less_than(c.probe, dtfl_input_time(c.in1)))
		dtfl_worker_step(w);
	dtfl_input_close(c.in1);
	dtfl_input_close(c.in2);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	run_ctx r;
	dtfl_config *cfg;
	int rc, skip = 0;
	size_t i, total = 0;

	memset(&r, 0, sizeof r);
	r.fixed = argc < 4;
	if (!r.fixed) {
		r.keys = ex_size(argc, argv, 1, 100);
		r.vals = ex_size(argc, argv, 2, 1000);
		r.batch = ex_size(argc, argv, 3, 100);
		skip = 3;
	}
	cfg = ex_config(argc, argv, skip);
	rc = dtfl_execute(cfg, worker, &r, NULL);
	dtfl_config_free(cfg);

	for (i = 0; i < 16; i++)
		total += r.total_count[i];
	if (r.fixed)
		printf("join results: %zu\n", total);
	return rc;
}
