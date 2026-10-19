/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * bench/longrun.c: per-commit cost over a long run of tiny commits.  A
 * standalone worker feeds one insert or one remove per epoch into a base
 * relation of a handful of rows and steps until the probe passes the
 * epoch.  Differential's work is proportional to the change, so the time
 * per block of epochs must stay flat as the run grows.
 *
 * Run as: bench/longrun shape epochs [block]
 *   shape: input | arrange | reduce | inspect | count | join
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime */

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum shape { S_INPUT, S_ARRANGE, S_REDUCE, S_INSPECT, S_COUNT, S_JOIN };

typedef struct lr {
	dtfl_input_session *in1, *in2;
	dtfl_probe *probe;
	enum shape shape;
	uint64_t seen;
} lr;

static void
ident(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx;
	dtfl_collection_emit_record(out, rec, len);
}

static void
arr_kv(void *ctx, const void *key, size_t klen, const void *val, size_t vlen,
    dtfl_arranged_emit *out)
{
	unsigned char b[16];

	(void)ctx;
	(void)klen;
	(void)vlen;
	memcpy(b, key, 8);
	memcpy(b + 8, val, 8);
	dtfl_arranged_emit_record(out, b, 16);
}

static void
cnt(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	const int64_t *d = diffs;
	int64_t c = 0, one = 1;
	unsigned char b[8];
	size_t i;

	(void)ctx; (void)key; (void)klen; (void)vals; (void)vlens;
	for (i = 0; i < n; i++)
		c += d[i];
	dtfl_put_i64(b, c);
	dtfl_reduce_emit_pair(out, b, 8, &one);
}

static void
on_rec(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	(void)rec; (void)len; (void)time; (void)diff;
	((lr *)ctx)->seen++;
}

static void
join_res(void *ctx, const void *key, size_t klen, const void *v1, size_t l1,
    const void *v2, size_t l2, dtfl_join_emit *out)
{
	unsigned char b[24];

	(void)ctx; (void)klen; (void)l1; (void)l2;
	memcpy(b, key, 8);
	memcpy(b + 8, v1, 8);
	memcpy(b + 16, v2, 8);
	dtfl_join_emit_record(out, b, 24);
}

static void
build(dtfl_scope *scope, void *arg)
{
	lr *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *kv = dtfl_type_pair(u64, u64);
	const dtfl_type *ki = dtfl_type_pair(u64, dtfl_type_i64());
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *in = dtfl_input_session_to_collection(c->in1, scope);
	dtfl_collection *m = dtfl_collection_map(in, kv, ident, DTFL_NOCTX);
	dtfl_collection *out = NULL, *p;

	switch (c->shape) {
	case S_INPUT:
		out = m;
		m = NULL;
		break;
	case S_ARRANGE: {
		dtfl_arranged *a = dtfl_collection_arrange_by_key(m, u64, u64,
		    "arr");

		out = dtfl_arranged_as_collection(a, kv, arr_kv, DTFL_NOCTX);
		dtfl_arranged_free(a);
		break;
	}
	case S_REDUCE:
	case S_INSPECT:
		out = dtfl_collection_reduce(m, u64, u64, ki, i64, cnt,
		    DTFL_NOCTX);
		if (c->shape == S_INSPECT) {
			dtfl_collection *s = dtfl_collection_inspect(out,
			    on_rec, DTFL_CTX(c, NULL));

			dtfl_collection_free(out);
			out = s;
		}
		break;
	case S_COUNT:
		out = dtfl_collection_count(m, i64);
		break;
	case S_JOIN: {
		dtfl_collection *in2 = dtfl_input_session_to_collection(c->in2,
		    scope);
		const dtfl_type *f3[3] = { u64, u64, u64 };

		out = dtfl_collection_join_map(m, in2, u64, u64, u64,
		    dtfl_type_tuple(f3, 3), join_res, DTFL_NOCTX);
		dtfl_collection_free(in2);
		break;
	}
	}
	p = dtfl_collection_probe_with(out, c->probe);
	dtfl_collection_free(p);
	dtfl_collection_free(out);
	if (m != NULL)
		dtfl_collection_free(m);
	dtfl_collection_free(in);
	dtfl_diff_free(i64);
}

static double
now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int
main(int argc, char **argv)
{
	static const char *const names[] = { "input", "arrange", "reduce",
		"inspect", "count", "join" };
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *kv = dtfl_type_pair(u64, u64);
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_worker *w;
	lr c;
	uint64_t e, epochs, block, t;
	double t0;
	size_t i;

	memset(&c, 0, sizeof c);
	if (argc < 3) {
		fprintf(stderr, "usage: %s shape epochs [block]\n", argv[0]);
		return 2;
	}
	for (i = 0; i < sizeof names / sizeof names[0]; i++)
		if (strcmp(argv[1], names[i]) == 0)
			c.shape = (enum shape)i;
	epochs = strtoull(argv[2], NULL, 10);
	block = argc > 3 ? strtoull(argv[3], NULL, 10) : 500;

	w = dtfl_worker_new_standalone();
	c.in1 = dtfl_input_session_new(kv, i64);
	c.in2 = dtfl_input_session_new(kv, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "longrun", tc, build, &c);

	/* A base of four rows on both sides, then one change per epoch. */
	for (i = 0; i < 4; i++) {
		unsigned char b[16];

		dtfl_put_u64(b, i);
		dtfl_put_u64(b + 8, 100 + i);
		dtfl_input_session_insert(c.in1, b, 16);
		if (c.shape == S_JOIN)
			dtfl_input_session_insert(c.in2, b, 16);
	}
	t0 = now();
	for (e = 0; e < epochs; e++) {
		unsigned char b[16];

		/* Odd epochs remove what the even epoch before inserted. */
		dtfl_put_u64(b, (e / 2) % 4);
		dtfl_put_u64(b + 8, 1000 + (e / 2) % 7);
		if (e % 2 == 0)
			dtfl_input_session_insert(c.in1, b, 16);
		else
			dtfl_input_session_remove(c.in1, b, 16);
		t = e + 1;
		dtfl_input_session_advance_to(c.in1, &t);
		dtfl_input_session_flush(c.in1);
		if (c.shape == S_JOIN) {
			dtfl_input_session_advance_to(c.in2, &t);
			dtfl_input_session_flush(c.in2);
		}
		while (dtfl_probe_less_than(c.probe, &t))
			dtfl_worker_step(w);
		if ((e + 1) % block == 0) {
			double t1 = now();

			printf("%s %llu-%llu %.4f s (%.3f ms/epoch)\n",
			    names[c.shape],
			    (unsigned long long)(e + 1 - block),
			    (unsigned long long)e, t1 - t0,
			    (t1 - t0) * 1e3 / block);
			fflush(stdout);
			t0 = t1;
		}
	}
	dtfl_input_session_close(c.in1);
	if (c.shape == S_JOIN)
		dtfl_input_session_close(c.in2);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_worker_free(w);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
	return 0;
}
