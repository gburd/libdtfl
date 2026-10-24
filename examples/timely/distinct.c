/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/distinct.c: mirrors timely/examples/distinct.rs.
 *
 * Two workers each send a fixed array of values at round 0.  A unary
 * operator with an Exchange pact (route by value) keeps a per-time count
 * of each value and emits a value only the first time it is seen within
 * a time; inspect prints "worker I:\tvalue N".  Because the pact routes
 * by value, each distinct value is deduplicated globally within the time.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-time table of values seen so far (linear scan; few values here). */
typedef struct seen_slot {
	uint64_t time;
	uint64_t *vals;
	size_t n, cap;
} seen_slot;

typedef struct distinct_ctx {
	seen_slot *slots;
	size_t n, cap;
} distinct_ctx;

static seen_slot *
slot_for(distinct_ctx *c, uint64_t time)
{
	size_t i;
	seen_slot s;

	for (i = 0; i < c->n; i++)
		if (c->slots[i].time == time)
			return &c->slots[i];
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 4;
		c->slots = realloc(c->slots, c->cap * sizeof *c->slots);
	}
	memset(&s, 0, sizeof s);
	s.time = time;
	c->slots[c->n] = s;
	return &c->slots[c->n++];
}

/* True if this is the first time `v` is seen in the slot (and records it). */
static bool
first_seen(seen_slot *s, uint64_t v)
{
	size_t i;

	for (i = 0; i < s->n; i++)
		if (s->vals[i] == v)
			return false;
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 8;
		s->vals = realloc(s->vals, s->cap * sizeof *s->vals);
	}
	s->vals[s->n++] = v;
	return true;
}

static uint64_t
route_by_value(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);
}

static bool
distinct_logic(dtfl_op *op, void *vctx)
{
	distinct_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		seen_slot *s = slot_for(c, t[0]);
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *sess = dtfl_op_session(op, 0, cap);
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			uint64_t v = dtfl_get_u64(rec);

			if (first_seen(s, v)) {
				unsigned char buf[8];

				dtfl_put_u64(buf, v);
				dtfl_session_give(sess, buf, sizeof buf);
			}
		}
		dtfl_session_close(sess);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
distinct_free(void *vctx)
{
	distinct_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->n; i++)
		free(c->slots[i].vals);
	free(c->slots);
	free(c);
}

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
	size_t index;
} build_ctx;

static void
inspect_value(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	size_t index = *(size_t *)ctx;

	(void)time; (void)len;
	printf("worker %zu:\tvalue %llu\n", index,
	    (unsigned long long)dtfl_get_u64(rec));
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Distinct");
	distinct_ctx *dc = calloc(1, sizeof *dc);
	dtfl_stream *out;

	dtfl_op_builder_input(b, s, dtfl_pact_exchange(route_by_value, NULL));
	/*
	 * As Operator::unary (generic/operator.rs): frontier
	 * changes alone do not schedule.
	 */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	out = dtfl_op_builder_output(b, dtfl_type_u64());
	dtfl_op_builder_build(b, distinct_logic, dc, distinct_free);
	out = dtfl_stream_inspect(out, inspect_value, DTFL_CTX(&c->index, NULL));
	dtfl_stream_probe_with(out, c->probe);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	static const uint64_t w0[] = { 0, 1, 2, 2, 2, 3, 3, 4 };
	static const uint64_t w1[] = { 0, 0, 3, 4, 4, 5, 7, 7 };
	dtfl_time_class *tc = dtfl_time_u64();
	build_ctx c;
	uint64_t next = 1;

	(void)ctx;
	c.input = dtfl_input_new(dtfl_type_u64());
	c.probe = dtfl_probe_new();
	c.index = dtfl_worker_index(w);
	dtfl_worker_dataflow(w, "distinct", tc, build, &c);

	if (c.index == 0 || c.index == 1) {
		const uint64_t *src = c.index == 0 ? w0 : w1;
		size_t i;

		for (i = 0; i < 8; i++) {
			unsigned char buf[8];

			dtfl_put_u64(buf, src[i]);
			dtfl_input_send(c.input, buf, sizeof buf);
		}
	}
	dtfl_input_advance_to(c.input, &next);
	while (dtfl_probe_less_than(c.probe, dtfl_input_time(c.input)))
		dtfl_worker_step(w);
	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
