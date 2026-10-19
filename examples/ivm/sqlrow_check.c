/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/ivm/sqlrow_check.c: checks every encoding rule in sqlrow.h
 * against a reference comparator that states PostgreSQL's order
 * directly, and checks that the encoded records group and sort through a
 * real libdtfl dataflow the way SQL GROUP BY does.  Exits nonzero on any
 * failure.
 */
#include "sqlrow.h"

#include "dtfl/collection.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...)						\
	do {								\
		if (!(cond)) {						\
			printf("FAIL: ");				\
			printf(__VA_ARGS__);				\
			printf("\n");					\
			failures++;					\
		}							\
	} while (0)

/*
 * PostgreSQL's float8 order (float8_cmp_internal): NaN equals NaN and is
 * greater than everything else; otherwise numeric order, -0.0 == 0.0.
 */
static int
pg_float8_cmp(double a, double b)
{
	bool an = a != a, bn = b != b;

	if (an || bn)
		return an && bn ? 0 : an ? 1 : -1;
	return a < b ? -1 : a > b ? 1 : 0;
}

static int
sign(int x)
{
	return (x > 0) - (x < 0);
}

static void
check_float8(void)
{
	const double inf = HUGE_VAL;
	double v[] = { -inf, -1e300, -2.5, -1.0, -1e-300, -0.0, 0.0, 1e-300,
	    1.0, 2.5, 1e300, inf, 0.0 / 0.0, -(0.0 / 0.0) };
	const dtfl_type *t = sqlrow_float8_type();
	size_t n = sizeof v / sizeof v[0], i, j;

	for (i = 0; i < n; i++) {
		unsigned char a[8];
		double back;

		sqlrow_put_float8(a, v[i]);
		back = sqlrow_get_float8(a);
		CHECK(pg_float8_cmp(back, v[i]) == 0,
		    "float8 round trip of %g gave %g", v[i], back);
		for (j = 0; j < n; j++) {
			unsigned char b[8];
			int enc, ref;

			sqlrow_put_float8(b, v[j]);
			enc = sign(dtfl_type_cmp(t, a, 8, b, 8));
			ref = pg_float8_cmp(v[i], v[j]);
			CHECK(enc == ref, "float8 order: cmp(%g, %g) encoded %d, "
			    "PostgreSQL %d", v[i], v[j], enc, ref);
			if (ref == 0)
				CHECK(dtfl_type_hash(t, a, 8) ==
				    dtfl_type_hash(t, b, 8),
				    "float8 equal values %g and %g hash apart",
				    v[i], v[j]);
		}
	}
}

/* NULLS FIRST, then the values in integer order, through option(i64). */
static void
check_nullable_int8(void)
{
	const dtfl_type *t = sqlrow_nullable(dtfl_type_i64());
	int64_t vals[] = { INT64_MIN, -7, 0, 7, INT64_MAX };
	unsigned char rec[6][9];
	size_t len[6], i, j;

	len[0] = sqlrow_put_null(rec[0]);
	for (i = 0; i < 5; i++) {
		len[i + 1] = sqlrow_put_present(rec[i + 1]);
		len[i + 1] += dtfl_put_i64(rec[i + 1] + 1, vals[i]);
	}
	for (i = 0; i < 6; i++)
		for (j = 0; j < 6; j++) {
			int got = sign(dtfl_type_cmp(t, rec[i], len[i], rec[j],
			    len[j]));
			int want = i < j ? -1 : i > j;

			CHECK(got == want, "nullable int8 order: %zu vs %zu "
			    "gave %d, want %d", i, j, got, want);
		}
	CHECK(sqlrow_is_null(rec[0]) && !sqlrow_is_null(rec[1]),
	    "sqlrow_is_null");
	dtfl_type_free(t);
}

/*
 * GROUP BY a nullable float8 column through a dataflow: one group for all
 * NULLs, one for 0.0 and -0.0 together, one for every NaN, and the rest
 * distinct.  count_total gives each group's size.
 */
typedef struct grp {
	unsigned char key[9];
	size_t klen;
	int64_t count;
} grp;

typedef struct grp_out {
	grp g[16];
	size_t n;
} grp_out;

static void
grp_sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	grp_out *o = ctx;
	int64_t d;
	size_t klen = len - 8;

	(void)time;
	memcpy(&d, diff, sizeof d);
	if (d != 1 || o->n >= 16 || klen > 9) {
		CHECK(false, "group by: unexpected update diff %lld",
		    (long long)d);
		return;
	}
	memcpy(o->g[o->n].key, rec, klen);
	o->g[o->n].klen = klen;
	o->g[o->n].count = dtfl_get_i64((const unsigned char *)rec + klen);
	o->n++;
}

typedef struct grp_build {
	dtfl_input_session *in;
	dtfl_probe *probe;
	grp_out *out;
} grp_build;

static void
grp_dataflow(dtfl_scope *scope, void *arg)
{
	grp_build *b = arg;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *in = dtfl_input_session_to_collection(b->in, scope);
	dtfl_collection *ct = dtfl_collection_count_total(in, i64);
	dtfl_collection *ins = dtfl_collection_inspect(ct, grp_sink,
	    DTFL_CTX(b->out, NULL));
	dtfl_collection *pr = dtfl_collection_probe_with(ins, b->probe);

	dtfl_collection_free(pr);
	dtfl_collection_free(ins);
	dtfl_collection_free(ct);
	dtfl_collection_free(in);
	dtfl_diff_free(i64);
}

static void
check_group_by(void)
{
	const double nan = 0.0 / 0.0;
	/* SQL: NULL x2, -0.0 and 0.0 one group, NaN x2 one group, 1.5. */
	struct { bool null; double v; } rows[] = {
		{ true, 0 }, { false, 0.0 }, { false, nan }, { false, 1.5 },
		{ true, 0 }, { false, -0.0 }, { false, -nan },
	};
	const dtfl_type *t = sqlrow_nullable(sqlrow_float8_type());
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	grp_out out;
	grp_build b;
	uint64_t next = 1;
	size_t i;

	memset(&out, 0, sizeof out);
	b.in = dtfl_input_session_new(t, i64);
	b.probe = dtfl_probe_new();
	b.out = &out;
	dtfl_worker_dataflow(w, "group_by", tc, grp_dataflow, &b);
	for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
		unsigned char rec[9];
		size_t len;

		if (rows[i].null)
			len = sqlrow_put_null(rec);
		else {
			len = sqlrow_put_present(rec);
			len += sqlrow_put_float8(rec + 1, rows[i].v);
		}
		dtfl_input_session_insert(b.in, rec, len);
	}
	dtfl_input_session_advance_to(b.in, &next);
	dtfl_input_session_close(b.in);
	while (dtfl_worker_step(w))
		;

	/* Groups come out in key order: NULL, 0.0, 1.5, NaN. */
	CHECK(out.n == 4, "group by: %zu groups, want 4", out.n);
	if (out.n == 4) {
		CHECK(sqlrow_is_null(out.g[0].key) && out.g[0].count == 2,
		    "group by: NULL group");
		CHECK(!sqlrow_is_null(out.g[1].key) &&
		    sqlrow_get_float8(out.g[1].key + 1) == 0.0 &&
		    out.g[1].count == 2, "group by: zero group (0.0, -0.0)");
		CHECK(sqlrow_get_float8(out.g[2].key + 1) == 1.5 &&
		    out.g[2].count == 1, "group by: 1.5 group");
		CHECK(isnan(sqlrow_get_float8(out.g[3].key + 1)) &&
		    out.g[3].count == 2, "group by: NaN group");
	}
	for (i = 0; i < out.n; i++) {
		printf("group ");
		if (sqlrow_is_null(out.g[i].key))
			printf("NULL");
		else if (isnan(sqlrow_get_float8(out.g[i].key + 1)))
			printf("NaN");	/* printf's NaN spelling varies by libc */
		else
			printf("%g", sqlrow_get_float8(out.g[i].key + 1));
		printf(": %lld\n", (long long)out.g[i].count);
	}

	dtfl_probe_free(b.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(i64);
	dtfl_worker_free(w);
	dtfl_type_free(t);
}

int
main(void)
{
	check_float8();
	check_nullable_int8();
	check_group_by();
	if (failures != 0) {
		printf("%d failure%s\n", failures, failures == 1 ? "" : "s");
		return 1;
	}
	printf("sqlrow: all checks passed\n");
	return 0;
}
