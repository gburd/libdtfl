/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/projekt.c: mirrors differential-dataflow/examples/projekt.rs.
 *
 * Validates a candidate set of (x,y,z) placements against two projections
 * (x,y) and (x,z) given as goals, over three dataflows on fixed inputs (no
 * randomness, so the output is exact):
 *   1. report unmet goals and met non-goals: distinct/negate/concat the
 *      projections of the candidate against the goals, print "Not done";
 *   2. the maximum solution size: join the x-values of the two goals and
 *      sum the products of their multiplicities;
 *   3. the minimum solution size: count each goal's x-values, join, and
 *      explode by max(count_xy, count_xz) per x.
 * The candidate is chosen to meet both projections exactly, so dataflow 1
 * prints nothing; dataflow 2 prints 40 and dataflow 3 prints 16.
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

/* The XY projection goals. */
static const uint64_t xy_goal[][2] = {
	{ 0, 0 }, { 0, 1 }, { 0, 3 }, { 0, 4 }, { 1, 1 }, { 1, 3 },
	{ 2, 1 }, { 2, 2 }, { 3, 2 }, { 3, 3 }, { 3, 4 }, { 4, 0 },
	{ 4, 1 }, { 4, 2 },
};
#define N_XY (sizeof xy_goal / sizeof xy_goal[0])

/* The XZ projection goals. */
static const uint64_t xz_goal[][2] = {
	{ 0, 2 }, { 0, 3 }, { 0, 4 }, { 1, 2 }, { 1, 4 }, { 2, 1 },
	{ 2, 2 }, { 2, 3 }, { 3, 0 }, { 3, 1 }, { 3, 3 }, { 3, 4 },
	{ 4, 1 }, { 4, 4 },
};
#define N_XZ (sizeof xz_goal / sizeof xz_goal[0])

/* The candidate (x,y,z) placements. */
static const uint64_t xyzs[][3] = {
	{ 0, 0, 2 }, { 0, 1, 3 }, { 0, 3, 4 }, { 0, 4, 4 }, { 1, 1, 2 },
	{ 1, 3, 4 }, { 2, 1, 1 }, { 2, 2, 2 }, { 2, 2, 3 }, { 3, 2, 0 },
	{ 3, 3, 1 }, { 3, 4, 3 }, { 3, 4, 4 }, { 4, 0, 1 }, { 4, 1, 4 },
	{ 4, 2, 4 },
};
#define N_XYZ (sizeof xyzs / sizeof xyzs[0])

/* ------------------------------------------------------------------ */
/* Record callbacks.                                                   */
/* ------------------------------------------------------------------ */

/* map (x,y,z) -> (x,y). */
static void
xyz_to_xy(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 16);
}

/* map (x,y,z) -> (x,z) (fields 0 and 2). */
static void
xyz_to_xz(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, rec, 8);
	memcpy(buf + 8, (const unsigned char *)rec + 16, 8);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* map any pair -> () (the unit record). */
static void
pair_to_unit(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)rec; (void)len;
	dtfl_collection_emit_record(out, NULL, 0);
}

/* map (x,y) -> x (the key, first field). */
static void
pair_to_x(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 8);
}

/* join_map result for the min dataflow: emit (ys, zs) as a pair of i64. */
static void
min_join_result(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)key; (void)klen; (void)v1len; (void)v2len;
	memcpy(buf, v1, 8);
	memcpy(buf + 8, v2, 8);
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* explode (ys, zs) -> ((), max(ys, zs)). */
static void
min_explode(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	int64_t ys, zs, m;

	(void)ctx; (void)len;
	ys = dtfl_get_i64(rec);
	zs = dtfl_get_i64((const unsigned char *)rec + 8);
	m = ys > zs ? ys : zs;
	dtfl_collection_emit_pair(out, NULL, 0, &m);
}

typedef struct label { const char *fmt; } label;

static void
inspect_count(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	label *l = ctx;
	int64_t d;

	(void)rec; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	printf(l->fmt, (long long)d);
}

static label not_done = { "Not done: ((), %lld)\n" };
static label max_size = { "Maximum solution size: %lld\n" };
static label min_size = { "Minimum solution size: %lld\n" };

/* ------------------------------------------------------------------ */
/* Dataflow 1: validate.                                               */
/* ------------------------------------------------------------------ */

typedef struct build_ctx {
	dtfl_input_session *xyzs;
	dtfl_input_session *xy_goal;
	dtfl_input_session *xz_goal;
	dtfl_probe *probe;
} build_ctx;

/*
 * errors = goal_proj.distinct().negate().concat(goal).consolidate(), the
 * symmetric difference between the candidate's projection and the goal.
 */
static dtfl_collection *
projection_errors(dtfl_collection *proj, dtfl_collection *goal, dtfl_diff *i64)
{
	dtfl_collection *dist = dtfl_collection_distinct(proj, i64);
	dtfl_collection *neg = dtfl_collection_negate(dist);
	dtfl_collection *cat = dtfl_collection_concat(neg, goal);
	dtfl_collection *con = dtfl_collection_consolidate(cat);

	dtfl_collection_free(dist);
	dtfl_collection_free(neg);
	dtfl_collection_free(cat);
	return con;
}

/*
 * Upstream builds three separate dataflows that each register the shared
 * xy_goal and xz_goal inputs.  The C input session registers into one scope
 * only, so this port runs all three pipelines in one dataflow, each reading
 * the inputs' single collection.  The operators and output are identical.
 */
static void
build_validate(dtfl_scope *scope, dtfl_probe *probe, dtfl_collection *cxyz,
    dtfl_collection *cxyg, dtfl_collection *cxzg)
{
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *unit = dtfl_type_unit();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	dtfl_collection *xy = dtfl_collection_map(cxyz, pair, xyz_to_xy,
	    DTFL_NOCTX);
	dtfl_collection *xz = dtfl_collection_map(cxyz, pair, xyz_to_xz,
	    DTFL_NOCTX);
	dtfl_collection *xy_err = projection_errors(xy, cxyg, i64);
	dtfl_collection *xz_err = projection_errors(xz, cxzg, i64);
	dtfl_collection *xy_d = dtfl_collection_distinct(xy_err, i64);
	dtfl_collection *xz_d = dtfl_collection_distinct(xz_err, i64);
	dtfl_collection *xy_u = dtfl_collection_map(xy_d, unit, pair_to_unit,
	    DTFL_NOCTX);
	dtfl_collection *xz_u = dtfl_collection_map(xz_d, unit, pair_to_unit,
	    DTFL_NOCTX);
	dtfl_collection *cat = dtfl_collection_concat(xy_u, xz_u);
	dtfl_collection *tot = dtfl_collection_distinct(cat, i64);
	dtfl_collection *ins = dtfl_collection_inspect(tot, inspect_count,
	    DTFL_CTX(&not_done, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, probe);

	(void)scope;
	dtfl_diff_free(i64);
	dtfl_type_free(pair);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(tot);
	dtfl_collection_free(cat);
	dtfl_collection_free(xy_u);
	dtfl_collection_free(xz_u);
	dtfl_collection_free(xy_d);
	dtfl_collection_free(xz_d);
	dtfl_collection_free(xy_err);
	dtfl_collection_free(xz_err);
	dtfl_collection_free(xy);
	dtfl_collection_free(xz);
}

/* Dataflow 2: maximum solution size. */
static void
build_max(dtfl_scope *scope, dtfl_probe *probe, dtfl_collection *cxyg,
    dtfl_collection *cxzg)
{
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *unit = dtfl_type_unit();
	const dtfl_type *x_unit = dtfl_type_pair(dtfl_type_u64(), unit);
	/* xy_xs = xy_goal.map(x -> (x,())); likewise xz_xs. */
	dtfl_collection *xy_xs = dtfl_collection_map(cxyg, x_unit,
	    pair_to_x, DTFL_NOCTX);
	dtfl_collection *xz_xs = dtfl_collection_map(cxzg, x_unit,
	    pair_to_x, DTFL_NOCTX);
	/* join on x: key||val1||val2 = u64||()||() = just the u64 key. */
	dtfl_collection *joined = dtfl_collection_join(xy_xs, xz_xs,
	    dtfl_type_u64(), unit, unit, dtfl_type_u64());
	dtfl_collection *to_unit = dtfl_collection_map(joined, unit,
	    pair_to_unit, DTFL_NOCTX);
	dtfl_collection *con = dtfl_collection_consolidate(to_unit);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_count,
	    DTFL_CTX(&max_size, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, probe);

	(void)scope;
	dtfl_diff_free(i64);
	dtfl_type_free(x_unit);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(to_unit);
	dtfl_collection_free(joined);
	dtfl_collection_free(xy_xs);
	dtfl_collection_free(xz_xs);
}

/* Dataflow 3: minimum solution size. */
static void
build_min(dtfl_scope *scope, dtfl_probe *probe, dtfl_collection *cxyg,
    dtfl_collection *cxzg)
{
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *unit = dtfl_type_unit();
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *i64t = dtfl_type_i64();
	const dtfl_type *yszs = dtfl_type_pair(i64t, i64t);
	/* xy_xs = xy_goal.map(x).count(): records are (x, count:i64). */
	dtfl_collection *xy_x = dtfl_collection_map(cxyg, u64, pair_to_x,
	    DTFL_NOCTX);
	dtfl_collection *xz_x = dtfl_collection_map(cxzg, u64, pair_to_x,
	    DTFL_NOCTX);
	dtfl_collection *xy_cnt = dtfl_collection_count(xy_x, i64);
	dtfl_collection *xz_cnt = dtfl_collection_count(xz_x, i64);
	/* join (x, ys) with (x, zs); the counts are the values. */
	dtfl_collection *joined = dtfl_collection_join_map(xy_cnt, xz_cnt,
	    u64, i64t, i64t, yszs, min_join_result, DTFL_NOCTX);
	/* explode (ys, zs) -> ((), max(ys, zs)). */
	dtfl_collection *exploded = dtfl_collection_explode(joined, unit,
	    i64, min_explode, DTFL_NOCTX);
	dtfl_collection *con = dtfl_collection_consolidate(exploded);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_count,
	    DTFL_CTX(&min_size, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, probe);

	(void)scope;
	dtfl_diff_free(i64);
	dtfl_type_free(yszs);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(exploded);
	dtfl_collection_free(joined);
	dtfl_collection_free(xy_cnt);
	dtfl_collection_free(xz_cnt);
	dtfl_collection_free(xy_x);
	dtfl_collection_free(xz_x);
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_collection *cxyz = dtfl_input_session_to_collection(c->xyzs, scope);
	dtfl_collection *cxyg = dtfl_input_session_to_collection(c->xy_goal,
	    scope);
	dtfl_collection *cxzg = dtfl_input_session_to_collection(c->xz_goal,
	    scope);

	build_validate(scope, c->probe, cxyz, cxyg, cxzg);
	build_max(scope, c->probe, cxyg, cxzg);
	build_min(scope, c->probe, cxyg, cxzg);

	dtfl_collection_free(cxyz);
	dtfl_collection_free(cxyg);
	dtfl_collection_free(cxzg);
}

static void
worker(dtfl_worker *w, void *ctx)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());
	const dtfl_type *trip;
	const dtfl_type *fields[3];
	build_ctx c;
	uint64_t t;
	size_t i;

	(void)ctx;
	fields[0] = dtfl_type_u64();
	fields[1] = dtfl_type_u64();
	fields[2] = dtfl_type_u64();
	trip = dtfl_type_tuple(fields, 3);

	c.xyzs = dtfl_input_session_new(trip, i64);
	c.xy_goal = dtfl_input_session_new(pair, i64);
	c.xz_goal = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "projekt", tc, build, &c);

	/* Introduce the projection goals. */
	for (i = 0; i < N_XY; i++) {
		unsigned char buf[16];

		dtfl_put_u64(buf, xy_goal[i][0]);
		dtfl_put_u64(buf + 8, xy_goal[i][1]);
		dtfl_input_session_insert(c.xy_goal, buf, sizeof buf);
	}
	for (i = 0; i < N_XZ; i++) {
		unsigned char buf[16];

		dtfl_put_u64(buf, xz_goal[i][0]);
		dtfl_put_u64(buf + 8, xz_goal[i][1]);
		dtfl_input_session_insert(c.xz_goal, buf, sizeof buf);
	}

	/* Advance one round. */
	t = 1;
	dtfl_input_session_advance_to(c.xyzs, &t);
	dtfl_input_session_advance_to(c.xy_goal, &t);
	dtfl_input_session_advance_to(c.xz_goal, &t);
	dtfl_input_session_flush(c.xyzs);
	dtfl_input_session_flush(c.xy_goal);
	dtfl_input_session_flush(c.xz_goal);
	while (dtfl_probe_less_than(c.probe, &t))
		dtfl_worker_step(w);

	/* Introduce the candidate solution. */
	for (i = 0; i < N_XYZ; i++) {
		unsigned char buf[24];

		dtfl_put_u64(buf, xyzs[i][0]);
		dtfl_put_u64(buf + 8, xyzs[i][1]);
		dtfl_put_u64(buf + 16, xyzs[i][2]);
		dtfl_input_session_insert(c.xyzs, buf, sizeof buf);
	}

	/* Advance another round. */
	t = 2;
	dtfl_input_session_advance_to(c.xyzs, &t);
	dtfl_input_session_advance_to(c.xy_goal, &t);
	dtfl_input_session_advance_to(c.xz_goal, &t);
	dtfl_input_session_flush(c.xyzs);
	dtfl_input_session_flush(c.xy_goal);
	dtfl_input_session_flush(c.xz_goal);
	while (dtfl_probe_less_than(c.probe, &t))
		dtfl_worker_step(w);

	dtfl_input_session_close(c.xyzs);
	dtfl_input_session_close(c.xy_goal);
	dtfl_input_session_close(c.xz_goal);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_type_free(trip);
	dtfl_type_free(pair);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc;

	/* The candidate exactly meets both projections, so dataflow 1 is quiet;
	 * drive a single worker so the fixed inputs are not split. */
	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
