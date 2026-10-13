/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/interpreted.c: mirrors differential-dataflow/examples/interpreted.rs.
 *
 * A tiny relational-query interpreter: given an edge relation and a list of
 * (src, dst) field pairs to join, it extends a running tuple field by field.
 * Upstream reads a GraphMMap file and represents each result tuple as a
 * variable-length Vec<Node> grown with map_in_place; a C runtime cannot
 * reproduce that file, so this port feeds a small fixed directed graph and
 * fixes the tuple width at three fields (the widest the relations below
 * reach), which is exact and checkable.
 *
 * The query is the upstream default, relations [(0,2), (1,2)]:
 *   start:   results = edges.map((a,b) -> [a,b,-]);  fields {0,1} bound
 *   (0,2):   field 0 bound, field 2 free -> arrange edges by key (forward)
 *            and propose field 2: [a,b,c] where (a,c) is an edge.  Join on
 *            results.map(vec -> (vec[0], vec)).join_core(forward).
 *   (1,2):   fields 1 and 2 both bound -> semijoin against the edge relation
 *            arranged by self: keep [a,b,c] where (vec[1],vec[2]) = (b,c) is
 *            an edge.  Join on results.map(vec -> ((vec[1],vec[2]), vec)).
 * The result is every (a,b,c) with edges (a,b), (a,c) and (b,c): the
 * directed triangles of the fixed graph.
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

/* A fixed directed graph.  Triangles (a,b,c) with (a,b),(a,c),(b,c) edges:
 * (0,1,2) (0,1,3) (0,2,3) (1,2,3). */
static const uint64_t fixed_edges[][2] = {
	{ 0, 1 }, { 0, 2 }, { 0, 3 }, { 1, 2 }, { 1, 3 }, { 2, 3 },
	{ 3, 0 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])

/* The running tuple is three u64 fields: vec[0], vec[1], vec[2]. */
#define VEC_W 24

/* map edge (a,b) -> the tuple [a, b, 0] (field 2 not yet bound). */
static void
edge_to_vec(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[VEC_W];

	(void)ctx; (void)len;
	memcpy(buf, rec, 16);		/* a, b */
	memset(buf + 16, 0, 8);		/* field 2 placeholder */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* map vec -> (vec[0], vec): key the tuple on field 0 for the forward join. */
static void
vec_key0(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8 + VEC_W];

	(void)ctx; (void)len;
	memcpy(buf, rec, 8);		/* key = vec[0] */
	memcpy(buf + 8, rec, VEC_W);	/* value = whole tuple */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* map vec -> ((vec[1], vec[2]), vec): key on fields 1 and 2 for semijoin. */
static void
vec_key12(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[16 + VEC_W];

	(void)ctx; (void)len;
	memcpy(buf, (const unsigned char *)rec + 8, 16);	/* (b, c) */
	memcpy(buf + 16, rec, VEC_W);				/* value */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* forward join result: set vec[2] = dst_val, emit [a,b,c]. */
static void
forward_result(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[VEC_W];

	(void)ctx; (void)key; (void)klen; (void)v1len; (void)v2len;
	memcpy(buf, v1, VEC_W);		/* the tuple [a,b,-] */
	memcpy(buf + 16, v2, 8);	/* field 2 := proposed dst */
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* semijoin result: the key (b,c) is present as an edge, keep the tuple. */
static void
semijoin_result(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	(void)ctx; (void)key; (void)klen; (void)v1len; (void)v2; (void)v2len;
	dtfl_join_emit_record(out, v1, VEC_W);	/* v1 is the tuple [a,b,c] */
}

/* inspect: print each surviving triangle. */
static void
inspect_triangle(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	if (d <= 0)
		return;
	printf("Triangle: (%llu, %llu, %llu)\n",
	    (unsigned long long)dtfl_get_u64(rec),
	    (unsigned long long)dtfl_get_u64((const unsigned char *)rec + 8),
	    (unsigned long long)dtfl_get_u64((const unsigned char *)rec + 16));
}

typedef struct build_ctx {
	dtfl_input_session *edges;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *fields3[3] = { u64, u64, u64 };
	const dtfl_type *vec_t = dtfl_type_tuple(fields3, 3);
	const dtfl_type *pair = dtfl_type_pair(u64, u64);
	const dtfl_type *unit = dtfl_type_unit();
	dtfl_collection *edges = dtfl_input_session_to_collection(c->edges,
	    scope);
	/* Arrange the edge relation two ways. */
	dtfl_arranged *as_self = dtfl_collection_arrange(edges, "AsSelf");
	dtfl_arranged *forward = dtfl_collection_arrange_by_key(edges, u64, u64,
	    "Forward");
	/* results = edges.map((a,b) -> [a,b,0]). */
	dtfl_collection *results = dtfl_collection_map(edges, vec_t,
	    edge_to_vec, DTFL_NOCTX);
	/* relation (0,2): forward-join to propose field 2. */
	const dtfl_type *key0_rec = dtfl_type_pair(u64, vec_t);
	dtfl_collection *keyed0 = dtfl_collection_map(results, key0_rec,
	    vec_key0, DTFL_NOCTX);
	dtfl_arranged *keyed0_arr = dtfl_collection_arrange_by_key(keyed0, u64,
	    vec_t, "Keyed0");
	dtfl_collection *step1 = dtfl_arranged_join_core(keyed0_arr, forward,
	    vec_t, forward_result, DTFL_NOCTX);
	/* relation (1,2): semijoin against the edge relation by (b,c). */
	const dtfl_type *key12_rec = dtfl_type_pair(pair, vec_t);
	dtfl_collection *keyed12 = dtfl_collection_map(step1, key12_rec,
	    vec_key12, DTFL_NOCTX);
	dtfl_arranged *keyed12_arr = dtfl_collection_arrange_by_key(keyed12,
	    pair, vec_t, "Keyed12");
	dtfl_collection *step2 = dtfl_arranged_join_core(keyed12_arr, as_self,
	    vec_t, semijoin_result, DTFL_NOCTX);
	dtfl_collection *con = dtfl_collection_consolidate(step2);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_triangle,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	(void)unit;
	dtfl_type_free(vec_t);
	dtfl_type_free(pair);
	dtfl_type_free(key0_rec);
	dtfl_type_free(key12_rec);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(step2);
	dtfl_arranged_free(keyed12_arr);
	dtfl_collection_free(keyed12);
	dtfl_collection_free(step1);
	dtfl_arranged_free(keyed0_arr);
	dtfl_collection_free(keyed0);
	dtfl_collection_free(results);
	dtfl_arranged_free(forward);
	dtfl_arranged_free(as_self);
	dtfl_collection_free(edges);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	build_ctx c;
	uint64_t one = 1;
	size_t i;

	(void)ctx;
	c.edges = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "interpreted", tc, build, &c);

	if (dtfl_worker_index(w) == 0)
		for (i = 0; i < FIXED_E; i++) {
			unsigned char buf[16];

			dtfl_put_u64(buf, fixed_edges[i][0]);
			dtfl_put_u64(buf + 8, fixed_edges[i][1]);
			dtfl_input_session_insert(c.edges, buf, sizeof buf);
		}
	dtfl_input_session_advance_to(c.edges, &one);
	dtfl_input_session_flush(c.edges);
	while (dtfl_probe_less_than(c.probe, dtfl_input_session_time(c.edges)))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.edges);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_type_free(pair);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	/* Upstream takes a filename and timely flags; keep the flags only. */
	int skip = argc > 1 ? 1 : 0;
	dtfl_config *cfg = ex_config(argc, argv, skip);
	int rc;

	/* Drive a single worker so the fixed graph is not split. */
	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
