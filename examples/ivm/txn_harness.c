/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/ivm/txn_harness.c: the transition table -> delta -> apply loop
 * of an incrementally maintained view, with the PostgreSQL half modelled
 * in plain C so the dataflow half can be exercised without a server.
 *
 *   orders(id, region, amount)
 *   CREATE VIEW region_totals AS
 *     SELECT region, count(*), sum(amount) FROM orders GROUP BY region;
 *
 * The toy pieces and what they stand for:
 *  - table: the base heap, a row store keyed by id (the row identity a
 *    TID gives a real heap).  Only committed rows are in it.
 *  - txn: BEGIN takes a snapshot of the table; each statement runs against
 *    the snapshot and appends its transition tables (OLD TABLE / NEW
 *    TABLE, what an AFTER STATEMENT trigger declared REFERENCING OLD TABLE
 *    AS o NEW TABLE AS n receives) to the transaction's log.  COMMIT
 *    applies the log to the table, takes the next commit-order number,
 *    feeds the log to the dataflow at that time (OLD rows removed, NEW
 *    rows inserted), steps until the view is complete, and applies the
 *    view delta to the view's own row store.  ROLLBACK drops the log: the
 *    engine never sees an uncommitted change.
 *  - mv: the materialized view's heap, keyed by region.
 *
 * After every COMMIT and ROLLBACK the mv store is compared with the view
 * recomputed from scratch over the table; a mismatch exits nonzero.
 */
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* shortcut: fixed capacities and linear scans, sized for the scenario
 * below; a real store would index by key and grow. */
#define MAXROWS 32
#define MAXSTMTS 8

static const char *const region_name[] = { "?", "east", "west", "north" };
enum { EAST = 1, WEST, NORTH };

typedef struct row {
	uint64_t id, region;
	int64_t amount;
} row;

typedef struct table {
	row rows[MAXROWS];
	size_t n;
} table;

/* One statement's transition tables. */
typedef struct transition {
	table old, new_;
} transition;

typedef struct txn {
	const char *name;
	table snap;			/* the rows this transaction sees */
	transition log[MAXSTMTS];	/* one entry per write statement */
	size_t nstmts;
} txn;

/* A view row; diff is its multiplicity in a delta. */
typedef struct vrow {
	uint64_t region;
	int64_t count, sum, diff;
} vrow;

typedef struct vset {
	vrow rows[MAXROWS];
	size_t n;
} vset;

/*
 * The engine and the stores.  The worker, session and probe are
 * worker-owned and used only on this thread.  delta collects what the
 * view's inspect reports while commit `now` is being stepped.
 */
typedef struct db {
	dtfl_worker *w;
	dtfl_input_session *orders;
	dtfl_probe *probe;
	const dtfl_type *kv_t, *agg_t;
	table base;
	vset mv;
	vset delta;
	uint64_t now;		/* the next commit-order number */
	int failed;
} db;

/* ------------------------------------------------------------------ */
/* Row stores.                                                          */
/* ------------------------------------------------------------------ */

static row *
table_find(table *t, uint64_t id)
{
	size_t i;

	for (i = 0; i < t->n; i++)
		if (t->rows[i].id == id)
			return &t->rows[i];
	return NULL;
}

static void
table_add(table *t, row r)
{
	if (t->n == MAXROWS) {
		fprintf(stderr, "table full\n");
		exit(2);
	}
	t->rows[t->n++] = r;
}

static void
table_del(table *t, row *r)
{
	*r = t->rows[--t->n];
}

static void
print_rows(const char *label, const table *t)
{
	size_t i;

	printf(" %s [", label);
	for (i = 0; i < t->n; i++)
		printf("%s(%llu, %s, %lld)", i ? " " : "",
		    (unsigned long long)t->rows[i].id,
		    region_name[t->rows[i].region],
		    (long long)t->rows[i].amount);
	printf("]");
}

/* ------------------------------------------------------------------ */
/* Transactions and statements.                                         */
/* ------------------------------------------------------------------ */

static void
txn_begin(db *d, txn *t, const char *name)
{
	memset(t, 0, sizeof *t);
	t->name = name;
	t->snap = d->base;
	printf("%s BEGIN\n", name);
}

static transition *
stmt_start(txn *t)
{
	if (t->nstmts == MAXSTMTS) {
		fprintf(stderr, "too many statements\n");
		exit(2);
	}
	return &t->log[t->nstmts++];
}

static void
stmt_end(const txn *t, const char *sql)
{
	const transition *tr = &t->log[t->nstmts - 1];

	printf("%s %s ->", t->name, sql);
	print_rows("OLD", &tr->old);
	print_rows("NEW", &tr->new_);
	printf("\n");
}

static void
sql_insert(txn *t, uint64_t id, uint64_t region, int64_t amount)
{
	transition *tr = stmt_start(t);
	row r = { id, region, amount };

	table_add(&t->snap, r);
	table_add(&tr->new_, r);
	stmt_end(t, "INSERT");
}

static void
sql_delete(txn *t, uint64_t id)
{
	transition *tr = stmt_start(t);
	row *r = table_find(&t->snap, id);

	if (r != NULL) {
		table_add(&tr->old, *r);
		table_del(&t->snap, r);
	}
	stmt_end(t, "DELETE");
}

/* UPDATE orders SET region = to WHERE region = from: one statement, so one
 * transition table pair holding every row it touched. */
static void
sql_move_region(txn *t, uint64_t from, uint64_t to)
{
	transition *tr = stmt_start(t);
	size_t i;

	for (i = 0; i < t->snap.n; i++)
		if (t->snap.rows[i].region == from) {
			table_add(&tr->old, t->snap.rows[i]);
			t->snap.rows[i].region = to;
			table_add(&tr->new_, t->snap.rows[i]);
		}
	stmt_end(t, "UPDATE SET region");
}

static void
sql_set_amount(txn *t, uint64_t id, int64_t amount)
{
	transition *tr = stmt_start(t);
	row *r = table_find(&t->snap, id);

	if (r != NULL) {
		table_add(&tr->old, *r);
		r->amount = amount;
		table_add(&tr->new_, *r);
	}
	stmt_end(t, "UPDATE SET amount");
}

/* ------------------------------------------------------------------ */
/* The dataflow: the view's delta rule.                                 */
/* ------------------------------------------------------------------ */

static void
put_row(unsigned char *b, const row *r)
{
	dtfl_put_u64(b, r->id);
	dtfl_put_u64(b + 8, r->region);
	dtfl_put_i64(b + 16, r->amount);
}

/* Project a row image onto (region, amount). */
static void
project(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 8, 16);
}

/* count(*), sum(amount) of a non-empty group's accumulated multiset. */
static void
count_sum(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t count = 0, sum = 0, w, one = 1;
	unsigned char b[16];
	size_t i;

	(void)ctx; (void)key; (void)klen; (void)vlens;
	for (i = 0; i < n; i++) {
		memcpy(&w, (const unsigned char *)diffs + i * sizeof w,
		    sizeof w);
		count += w;
		sum += w * dtfl_get_i64(vals[i]);
	}
	if (count == 0)
		return;
	dtfl_put_i64(b, count);
	dtfl_put_i64(b + 8, sum);
	dtfl_reduce_emit_pair(out, b, sizeof b, &one);
}

static void
on_delta(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	db *d = ctx;
	const unsigned char *p = rec;
	vrow *v;

	(void)len;
	if (*time != d->now || d->delta.n == MAXROWS) {
		fprintf(stderr, "unexpected view update at %llu\n",
		    (unsigned long long)*time);
		d->failed = 1;
		return;
	}
	v = &d->delta.rows[d->delta.n++];
	v->region = dtfl_get_u64(p);
	v->count = dtfl_get_i64(p + 8);
	v->sum = dtfl_get_i64(p + 16);
	memcpy(&v->diff, diff, sizeof v->diff);
}

static void
build(dtfl_scope *scope, void *arg)
{
	db *d = arg;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *in = dtfl_input_session_to_collection(d->orders,
	    scope);
	dtfl_collection *kv = dtfl_collection_map(in, d->kv_t, project,
	    DTFL_NOCTX);
	dtfl_collection *agg = dtfl_collection_reduce(kv, dtfl_type_u64(),
	    dtfl_type_i64(), d->agg_t, i64, count_sum, DTFL_NOCTX);
	dtfl_collection *seen = dtfl_collection_inspect(agg, on_delta,
	    DTFL_CTX(d, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(seen, d->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(seen);
	dtfl_collection_free(agg);
	dtfl_collection_free(kv);
	dtfl_collection_free(in);
	dtfl_diff_free(i64);
}

/* ------------------------------------------------------------------ */
/* Applying and checking the view.                                      */
/* ------------------------------------------------------------------ */

static vrow *
mv_find(vset *s, uint64_t region)
{
	size_t i;

	for (i = 0; i < s->n; i++)
		if (s->rows[i].region == region)
			return &s->rows[i];
	return NULL;
}

static int
cmp_vrow(const void *a, const void *b)
{
	const vrow *x = a, *y = b;

	if (x->region != y->region)
		return x->region < y->region ? -1 : 1;
	return (x->diff > y->diff) - (x->diff < y->diff);
}

/*
 * Apply the view delta to the mv store: each - row deletes the stored row
 * it names, then each + row inserts.  Deletes go first because the store
 * is keyed by region and a changed group arrives as - old, + new.
 */
static void
mv_apply(db *d)
{
	size_t i;
	vrow *r;

	qsort(d->delta.rows, d->delta.n, sizeof d->delta.rows[0], cmp_vrow);
	for (i = 0; i < d->delta.n; i++) {
		const vrow *v = &d->delta.rows[i];

		if (v->diff >= 0)
			continue;
		r = mv_find(&d->mv, v->region);
		if (v->diff != -1 || r == NULL || r->count != v->count ||
		    r->sum != v->sum) {
			printf("  apply: no stored row (%s, %lld, %lld)\n",
			    region_name[v->region], (long long)v->count,
			    (long long)v->sum);
			d->failed = 1;
			continue;
		}
		printf("  mv DELETE (%s, %lld, %lld)\n", region_name[v->region],
		    (long long)v->count, (long long)v->sum);
		*r = d->mv.rows[--d->mv.n];
	}
	for (i = 0; i < d->delta.n; i++) {
		const vrow *v = &d->delta.rows[i];

		if (v->diff <= 0)
			continue;
		if (v->diff != 1 || mv_find(&d->mv, v->region) != NULL) {
			printf("  apply: duplicate group %s\n",
			    region_name[v->region]);
			d->failed = 1;
			continue;
		}
		printf("  mv INSERT (%s, %lld, %lld)\n", region_name[v->region],
		    (long long)v->count, (long long)v->sum);
		d->mv.rows[d->mv.n++] = *v;
	}
}

/* The oracle: SELECT region, count(*), sum(amount) ... GROUP BY region. */
static void
check(db *d)
{
	vset want = { .n = 0 };
	bool ok;
	size_t i;
	vrow *g;

	for (i = 0; i < d->base.n; i++) {
		const row *r = &d->base.rows[i];

		if ((g = mv_find(&want, r->region)) == NULL) {
			g = &want.rows[want.n++];
			g->region = r->region;
			g->count = g->sum = 0;
		}
		g->count++;
		g->sum += r->amount;
	}
	ok = d->mv.n == want.n;
	printf("  mv:");
	for (i = EAST; i <= NORTH; i++) {
		const vrow *have = mv_find(&d->mv, i), *exp = mv_find(&want, i);

		if (have != NULL)
			printf(" (%s, %lld, %lld)", region_name[i],
			    (long long)have->count, (long long)have->sum);
		if ((have == NULL) != (exp == NULL) || (have != NULL &&
		    (have->count != exp->count || have->sum != exp->sum)))
			ok = false;
	}
	printf(ok ? "  [= recomputed]\n" :
	    "  [MISMATCH with recomputed view]\n");
	if (!ok)
		d->failed = 1;
}

/* ------------------------------------------------------------------ */
/* COMMIT and ROLLBACK.                                                 */
/* ------------------------------------------------------------------ */

/*
 * Replay the log onto the committed table, statement by statement.  Each
 * OLD image must still be the committed row: in PostgreSQL a concurrent
 * writer of the same row waits on its row lock (or fails to serialize),
 * so the OLD image a committing statement replaced is always the current
 * committed version.  The toy has no locks, so a violation is fatal.
 */
static void
apply_log(db *d, const txn *t)
{
	size_t s, i;

	for (s = 0; s < t->nstmts; s++) {
		const transition *tr = &t->log[s];

		for (i = 0; i < tr->old.n; i++) {
			row *r = table_find(&d->base, tr->old.rows[i].id);

			if (r == NULL || memcmp(r, &tr->old.rows[i],
			    sizeof *r) != 0) {
				fprintf(stderr, "%s: write-write conflict on "
				    "row %llu\n", t->name, (unsigned long long)
				    tr->old.rows[i].id);
				exit(2);
			}
			table_del(&d->base, r);
		}
		for (i = 0; i < tr->new_.n; i++)
			table_add(&d->base, tr->new_.rows[i]);
	}
}

static void
txn_commit(db *d, txn *t)
{
	uint64_t next = d->now + 1;
	unsigned char b[24];
	size_t s, i;

	apply_log(d, t);
	printf("%s COMMIT -> commit %llu (%zu statement%s)\n", t->name,
	    (unsigned long long)d->now, t->nstmts, t->nstmts == 1 ? "" : "s");
	/*
	 * The delta enters at this transaction's commit-order time, so the
	 * engine's history is the serial order of commits.  In PostgreSQL the
	 * number is taken in a pre-commit callback under the lock that
	 * orders commit records, and each transaction's transition tables
	 * hold only its own changes, which is what makes feeding them one
	 * commit at a time equal to the serial history.
	 */
	for (s = 0; s < t->nstmts; s++) {
		const transition *tr = &t->log[s];

		for (i = 0; i < tr->old.n; i++) {
			put_row(b, &tr->old.rows[i]);
			dtfl_input_session_remove(d->orders, b, sizeof b);
		}
		for (i = 0; i < tr->new_.n; i++) {
			put_row(b, &tr->new_.rows[i]);
			dtfl_input_session_insert(d->orders, b, sizeof b);
		}
	}
	d->delta.n = 0;
	dtfl_input_session_advance_to(d->orders, &next);
	dtfl_input_session_flush(d->orders);
	while (dtfl_probe_less_than(d->probe, &next))
		dtfl_worker_step(d->w);
	mv_apply(d);
	d->now = next;
	check(d);
}

/* The engine is not told: the log is dropped and the snapshot with it. */
static void
txn_rollback(db *d, txn *t)
{
	printf("%s ROLLBACK (%zu statement%s discarded, nothing fed)\n",
	    t->name, t->nstmts, t->nstmts == 1 ? "" : "s");
	t->nstmts = 0;
	check(d);
}

int
main(void)
{
	const dtfl_type *u64 = dtfl_type_u64(), *i64t = dtfl_type_i64();
	const dtfl_type *row_fields[3] = { u64, u64, i64t };
	const dtfl_type *row_t = dtfl_type_tuple(row_fields, 3);
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	db d;
	txn t0, t1, t2;

	/* A local, not a static, so LeakSanitizer sees a leaked handle. */
	memset(&d, 0, sizeof d);
	d.kv_t = dtfl_type_pair(u64, i64t);
	d.agg_t = dtfl_type_pair(i64t, i64t);
	d.w = dtfl_worker_new_standalone();
	d.orders = dtfl_input_session_new(row_t, i64);
	d.probe = dtfl_probe_new();
	dtfl_worker_dataflow(d.w, "region_totals", tc, build, &d);
	d.now = 1;
	dtfl_input_session_advance_to(d.orders, &d.now);

	txn_begin(&d, &t0, "Tload");
	sql_insert(&t0, 1, EAST, 50);
	sql_insert(&t0, 2, EAST, 30);
	sql_insert(&t0, 3, WEST, 70);
	sql_insert(&t0, 4, WEST, 20);
	sql_insert(&t0, 5, NORTH, 10);
	txn_commit(&d, &t0);

	txn_begin(&d, &t0, "Ta");
	sql_set_amount(&t0, 2, 35);
	sql_delete(&t0, 5);		/* the last row in north */
	txn_commit(&d, &t0);

	/* Its changes must reach neither the table nor the view. */
	txn_begin(&d, &t0, "Tb");
	sql_insert(&t0, 6, NORTH, 99);
	sql_move_region(&t0, WEST, EAST);
	txn_rollback(&d, &t0);

	/*
	 * Two concurrent transactions: both write before either commits, and
	 * they commit T2 then T1.  Each delta is its own log, fed at its own
	 * commit time, so T2's lands at the earlier time although T1 began
	 * first.
	 */
	txn_begin(&d, &t1, "T1");
	txn_begin(&d, &t2, "T2");
	sql_insert(&t1, 7, WEST, 40);
	sql_move_region(&t2, EAST, WEST);	/* moves rows 1 and 2 */
	sql_set_amount(&t1, 3, 75);
	sql_delete(&t2, 2);			/* a row it moved itself */
	txn_commit(&d, &t2);
	txn_commit(&d, &t1);

	txn_begin(&d, &t0, "Tc");
	/* West empties and north, emptied at commit 2, comes back. */
	sql_move_region(&t0, WEST, NORTH);
	sql_insert(&t0, 8, EAST, 15);
	txn_commit(&d, &t0);

	printf(d.failed ? "FAILED: mv diverged from recomputation\n" :
	    "ok: mv matched recomputation after every commit and rollback\n");

	dtfl_input_session_close(d.orders);
	while (dtfl_worker_step(d.w))
		;
	dtfl_probe_free(d.probe);
	dtfl_worker_free(d.w);
	dtfl_type_free(d.agg_t);
	dtfl_type_free(d.kv_t);
	dtfl_type_free(row_t);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
	return d.failed ? 1 : 0;
}
