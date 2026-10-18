/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/ivm/ivm_view.c: an incrementally maintained view over two base
 * relations, driven one commit at a time on one standalone worker.
 *
 *   orders(order_id, customer_id, amount)
 *   customers(customer_id, region)
 *   CREATE VIEW region_totals AS
 *     SELECT region, count(*), sum(amount)
 *     FROM orders JOIN customers USING (customer_id) GROUP BY region;
 *
 * Each commit feeds one transaction's delta at its commit-order time n
 * (INSERT is insert, DELETE is remove, UPDATE is remove(old) +
 * insert(new)), steps until the probe passes n, prints the view delta at
 * n and then the whole view as of n.  The view as of n is checked against
 * a from-scratch evaluation of the query over the base rows the example
 * keeps, and the view before n plus the delta at n must equal the view
 * as of n, so the program is its own oracle: it exits nonzero on mismatch.
 *
 * PostgreSQL mapping (dev/notes/pg-incremental-views.md):
 *  - delta: the OLD TABLE / NEW TABLE transition tables of AFTER STATEMENT
 *    triggers on orders and customers; OLD rows are removed, NEW rows are
 *    inserted.
 *  - time: a logical commit order assigned at commit (one counter for the
 *    whole database), not an xid and not a snapshot.
 *  - apply: the - rows of the view delta are deleted from, and the + rows
 *    inserted into, the view's heap inside the committing transaction
 *    (examples/ivm/txn_harness.c applies them to a toy table).
 */
#include "dtfl/arrange.h"
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

/* shortcut: fixed-size tables with linear scans, fine for a script of a
 * few dozen rows; a real store would index by key. */
#define MAXROWS 32

typedef struct order_row {
	uint64_t id, cust;
	int64_t amount;
	bool live;
} order_row;

typedef struct cust_row {
	uint64_t id, region;
	bool live;
} cust_row;

/* One view row (region, count, sum) with a multiplicity. */
typedef struct view_row {
	uint64_t region;
	int64_t count, sum, diff;
} view_row;

typedef struct view_set {
	view_row rows[MAXROWS];
	size_t n;
} view_set;

static const char *const region_name[] = { "?", "east", "west", "north" };

/*
 * Everything the example owns.  The worker, sessions, probe and trace are
 * worker-owned handles used only on this thread.  ord/cust are the base
 * relations the oracle reads; delta collects the view changes inspect
 * reports while commit `now` is being stepped.
 */
typedef struct ivm {
	dtfl_worker *w;
	dtfl_input_session *orders, *customers;
	dtfl_probe *probe;
	dtfl_trace *view;
	const dtfl_type *keyed_t, *agg_t;
	order_row ord[MAXROWS];
	size_t nord;
	cust_row cust[MAXROWS];
	size_t ncust;
	view_set delta, prev;
	uint64_t now;
	int failed;
} ivm;

/* ------------------------------------------------------------------ */
/* Base relations: every change goes to the oracle and to the session. */
/* ------------------------------------------------------------------ */

static void
put_order(unsigned char *b, const order_row *o)
{
	dtfl_put_u64(b, o->id);
	dtfl_put_u64(b + 8, o->cust);
	dtfl_put_i64(b + 16, o->amount);
}

static void
put_cust(unsigned char *b, const cust_row *c)
{
	dtfl_put_u64(b, c->id);
	dtfl_put_u64(b + 8, c->region);
}

static order_row *
find_order(ivm *v, uint64_t id)
{
	size_t i;

	for (i = 0; i < v->nord; i++)
		if (v->ord[i].live && v->ord[i].id == id)
			return &v->ord[i];
	fprintf(stderr, "no order %llu\n", (unsigned long long)id);
	exit(2);
}

static cust_row *
find_cust(ivm *v, uint64_t id)
{
	size_t i;

	for (i = 0; i < v->ncust; i++)
		if (v->cust[i].live && v->cust[i].id == id)
			return &v->cust[i];
	return NULL;
}

static void
room(size_t n)
{
	if (n == MAXROWS) {
		fprintf(stderr, "table full\n");
		exit(2);
	}
}

static void
order_insert(ivm *v, uint64_t id, uint64_t cust, int64_t amount)
{
	order_row *o;
	unsigned char b[24];

	room(v->nord);
	o = &v->ord[v->nord++];

	o->id = id;
	o->cust = cust;
	o->amount = amount;
	o->live = true;
	put_order(b, o);
	dtfl_input_session_insert(v->orders, b, sizeof b);
}

static void
order_delete(ivm *v, uint64_t id)
{
	order_row *o = find_order(v, id);
	unsigned char b[24];

	put_order(b, o);
	dtfl_input_session_remove(v->orders, b, sizeof b);
	o->live = false;
}

static void
order_update_amount(ivm *v, uint64_t id, int64_t amount)
{
	order_row *o = find_order(v, id);
	unsigned char b[24];

	put_order(b, o);
	dtfl_input_session_remove(v->orders, b, sizeof b);
	o->amount = amount;
	put_order(b, o);
	dtfl_input_session_insert(v->orders, b, sizeof b);
}

static void
cust_insert(ivm *v, uint64_t id, uint64_t region)
{
	cust_row *c;
	unsigned char b[16];

	room(v->ncust);
	c = &v->cust[v->ncust++];
	c->id = id;
	c->region = region;
	c->live = true;
	put_cust(b, c);
	dtfl_input_session_insert(v->customers, b, sizeof b);
}

/* UPDATE customers SET region: the old image leaves its group. */
static void
cust_update_region(ivm *v, uint64_t id, uint64_t region)
{
	cust_row *c = find_cust(v, id);
	unsigned char b[16];

	put_cust(b, c);
	dtfl_input_session_remove(v->customers, b, sizeof b);
	c->region = region;
	put_cust(b, c);
	dtfl_input_session_insert(v->customers, b, sizeof b);
}

/* ------------------------------------------------------------------ */
/* The dataflow.                                                        */
/* ------------------------------------------------------------------ */

/* SELECT customer_id, amount FROM orders: key each order by the join
 * column.  Both fields are contiguous at offset 8. */
static void
order_by_cust(void *ctx, const void *rec, size_t len,
    dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 8, 16);
}

/* orders JOIN customers USING (customer_id) -> (region, amount). */
static void
join_region(void *ctx, const void *key, size_t klen, const void *amount,
    size_t alen, const void *region, size_t rlen, dtfl_join_emit *out)
{
	unsigned char b[16];

	(void)ctx; (void)key; (void)klen; (void)alen; (void)rlen;
	memcpy(b, region, 8);
	memcpy(b + 8, amount, 8);
	dtfl_join_emit_record(out, b, sizeof b);
}

/*
 * GROUP BY region: count(*) and sum(amount) over the group's accumulated
 * multiset.  An empty group is never passed in, so a group whose last row
 * goes away produces no output and its old row is retracted.
 */
static void
count_sum(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t count = 0, sum = 0, d, one = 1;
	unsigned char b[16];
	size_t i;

	(void)ctx; (void)key; (void)klen; (void)vlens;
	for (i = 0; i < n; i++) {
		memcpy(&d, (const unsigned char *)diffs + i * sizeof d,
		    sizeof d);
		count += d;
		sum += d * dtfl_get_i64(vals[i]);
	}
	if (count == 0)
		return;
	dtfl_put_i64(b, count);
	dtfl_put_i64(b + 8, sum);
	dtfl_reduce_emit_pair(out, b, sizeof b, &one);
}

static view_row
decode_view(const void *rec, int64_t diff)
{
	const unsigned char *p = rec;
	view_row r;

	r.region = dtfl_get_u64(p);
	r.count = dtfl_get_i64(p + 8);
	r.sum = dtfl_get_i64(p + 16);
	r.diff = diff;
	return r;
}

/* Adds r to the set, merging an equal (region, count, sum) row. */
static void
set_add(view_set *s, view_row r)
{
	size_t i;

	for (i = 0; i < s->n; i++)
		if (s->rows[i].region == r.region &&
		    s->rows[i].count == r.count && s->rows[i].sum == r.sum) {
			s->rows[i].diff += r.diff;
			return;
		}
	room(s->n);
	s->rows[s->n++] = r;
}

/* The view's change stream: every update must be at the open commit. */
static void
on_delta(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	ivm *v = ctx;
	int64_t d;

	(void)len;
	memcpy(&d, diff, sizeof d);
	if (*time != v->now) {
		fprintf(stderr, "view update at time %llu during commit %llu\n",
		    (unsigned long long)*time, (unsigned long long)v->now);
		v->failed = 1;
		return;
	}
	set_add(&v->delta, decode_view(rec, d));
}

static void
build(dtfl_scope *scope, void *arg)
{
	ivm *v = arg;
	const dtfl_type *u64 = dtfl_type_u64(), *i64t = dtfl_type_i64();
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *orders = dtfl_input_session_to_collection(v->orders,
	    scope);
	dtfl_collection *custs = dtfl_input_session_to_collection(
	    v->customers, scope);
	dtfl_collection *keyed = dtfl_collection_map(orders, v->keyed_t,
	    order_by_cust, DTFL_NOCTX);
	dtfl_collection *joined = dtfl_collection_join_map(keyed, custs, u64,
	    i64t, u64, v->keyed_t, join_region, DTFL_NOCTX);
	dtfl_collection *totals = dtfl_collection_reduce(joined, u64, i64t,
	    v->agg_t, i64, count_sum, DTFL_NOCTX);
	dtfl_collection *seen = dtfl_collection_inspect(totals, on_delta,
	    DTFL_CTX(v, NULL));
	/* The arranged view: the maintained index point-in-time reads use. */
	dtfl_arranged *arr = dtfl_collection_arrange(seen, "region_totals");
	/* Probe downstream of the arrangement, so passing n means the trace
	 * holds every view update at times <= n. */
	dtfl_collection *back = dtfl_arranged_as_collection(arr,
	    dtfl_arranged_type(arr), NULL, DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(back, v->probe);

	v->view = dtfl_arranged_trace(arr);

	dtfl_collection_free(probed);
	dtfl_collection_free(back);
	dtfl_arranged_free(arr);
	dtfl_collection_free(seen);
	dtfl_collection_free(totals);
	dtfl_collection_free(joined);
	dtfl_collection_free(keyed);
	dtfl_collection_free(custs);
	dtfl_collection_free(orders);
	dtfl_diff_free(i64);
}

/* ------------------------------------------------------------------ */
/* Reading the view and checking it.                                    */
/* ------------------------------------------------------------------ */

static int
cmp_view(const void *a, const void *b)
{
	const view_row *x = a, *y = b;

	if (x->region != y->region)
		return x->region < y->region ? -1 : 1;
	if (x->diff != y->diff)		/* removals before additions */
		return x->diff < y->diff ? -1 : 1;
	return 0;
}

/* Drops zero multiplicities and sorts by region. */
static void
set_finish(view_set *s)
{
	size_t i, k = 0;

	for (i = 0; i < s->n; i++)
		if (s->rows[i].diff != 0)
			s->rows[k++] = s->rows[i];
	s->n = k;
	qsort(s->rows, s->n, sizeof s->rows[0], cmp_view);
}

static void
snap_each(void *ctx, const void *key, size_t klen, const void *val,
    size_t vlen, const uint64_t *time, const void *diff)
{
	int64_t d;

	(void)klen; (void)val; (void)vlen; (void)time;
	memcpy(&d, diff, sizeof d);
	/* The view as of n is the sum of its updates at times <= n. */
	set_add(ctx, decode_view(key, d));
}

/* SELECT ... FROM orders JOIN customers ... GROUP BY region, from scratch. */
static void
recompute(ivm *v, view_set *out)
{
	size_t i, j;

	out->n = 0;
	for (i = 0; i < v->nord; i++) {
		const order_row *o = &v->ord[i];
		const cust_row *c;

		if (!o->live || (c = find_cust(v, o->cust)) == NULL)
			continue;
		for (j = 0; j < out->n; j++)
			if (out->rows[j].region == c->region)
				break;
		if (j == out->n) {
			out->rows[j].region = c->region;
			out->rows[j].count = out->rows[j].sum = 0;
			out->rows[j].diff = 1;
			out->n++;
		}
		out->rows[j].count++;
		out->rows[j].sum += o->amount;
	}
	set_finish(out);
}

static bool
set_equal(const view_set *a, const view_set *b)
{
	size_t i;

	if (a->n != b->n)
		return false;
	for (i = 0; i < a->n; i++)
		if (a->rows[i].region != b->rows[i].region ||
		    a->rows[i].count != b->rows[i].count ||
		    a->rows[i].sum != b->rows[i].sum ||
		    a->rows[i].diff != b->rows[i].diff)
			return false;
	return true;
}

static void
print_row(const char *prefix, const view_row *r)
{
	printf("    %s(%s, %lld, %lld)\n", prefix, region_name[r->region],
	    (long long)r->count, (long long)r->sum);
}

/*
 * COMMIT: seal time n on both inputs, run the dataflow until the view is
 * complete through n, report the delta at n and the view as of n.
 */
static void
commit(ivm *v, const char *what)
{
	uint64_t next = v->now + 1;
	view_set snap, want;
	size_t i;

	printf("commit %llu: %s\n", (unsigned long long)v->now, what);
	v->delta.n = 0;
	dtfl_input_session_advance_to(v->orders, &next);
	dtfl_input_session_advance_to(v->customers, &next);
	dtfl_input_session_flush(v->orders);
	dtfl_input_session_flush(v->customers);
	while (dtfl_probe_less_than(v->probe, &next))
		dtfl_worker_step(v->w);

	set_finish(&v->delta);
	printf("  delta @%llu:\n", (unsigned long long)v->now);
	if (v->delta.n == 0)
		printf("    (no change)\n");
	for (i = 0; i < v->delta.n; i++)
		print_row(v->delta.rows[i].diff > 0 ? "+ " : "- ",
		    &v->delta.rows[i]);

	snap.n = 0;
	if (!dtfl_trace_to_vec_through(v->view, &next, 1, snap_each,
	    DTFL_CTX(&snap, NULL))) {
		fprintf(stderr, "no clean cut at %llu\n",
		    (unsigned long long)next);
		v->failed = 1;
	}
	set_finish(&snap);
	printf("  view @%llu:\n", (unsigned long long)v->now);
	for (i = 0; i < snap.n; i++)
		print_row("", &snap.rows[i]);

	recompute(v, &want);
	if (!set_equal(&snap, &want)) {
		printf("  MISMATCH: recomputed view:\n");
		for (i = 0; i < want.n; i++)
			print_row("", &want.rows[i]);
		v->failed = 1;
	}
	/* Applying the delta to the previous view must give the new one. */
	for (i = 0; i < v->delta.n; i++)
		set_add(&v->prev, v->delta.rows[i]);
	set_finish(&v->prev);
	if (!set_equal(&v->prev, &snap)) {
		printf("  MISMATCH: previous view + delta != view\n");
		v->failed = 1;
	}
	v->prev = snap;
	v->now = next;
}

int
main(void)
{
	const dtfl_type *u64 = dtfl_type_u64(), *i64t = dtfl_type_i64();
	const dtfl_type *order_fields[3] = { u64, u64, i64t };
	const dtfl_type *order_t = dtfl_type_tuple(order_fields, 3);
	const dtfl_type *cust_t = dtfl_type_pair(u64, u64);
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	ivm v;
	uint64_t first = 1;

	/* A local, not a static, so LeakSanitizer sees a leaked handle. */
	memset(&v, 0, sizeof v);
	v.keyed_t = dtfl_type_pair(u64, i64t);
	v.agg_t = dtfl_type_pair(i64t, i64t);
	v.w = dtfl_worker_new_standalone();
	v.orders = dtfl_input_session_new(order_t, i64);
	v.customers = dtfl_input_session_new(cust_t, i64);
	v.probe = dtfl_probe_new();
	dtfl_worker_dataflow(v.w, "region_totals", tc, build, &v);

	/* Commit numbers start at 1; time 0 stays empty. */
	v.now = first;
	dtfl_input_session_advance_to(v.orders, &first);
	dtfl_input_session_advance_to(v.customers, &first);

	/* The initial load, as CREATE ... WITH DATA would seed it. */
	cust_insert(&v, 1, 1);
	cust_insert(&v, 2, 1);
	cust_insert(&v, 3, 2);
	cust_insert(&v, 4, 3);
	order_insert(&v, 101, 1, 50);
	order_insert(&v, 102, 2, 30);
	order_insert(&v, 103, 3, 70);
	order_insert(&v, 104, 3, 20);
	order_insert(&v, 105, 4, 10);
	commit(&v, "load 4 customers, 5 orders");

	order_insert(&v, 106, 2, 40);
	order_delete(&v, 105);
	commit(&v, "INSERT order 106; DELETE order 105 (last in north)");

	cust_update_region(&v, 1, 2);
	commit(&v, "UPDATE customers SET region = west WHERE id = 1");

	order_update_amount(&v, 103, 75);
	cust_insert(&v, 5, 3);
	order_insert(&v, 107, 5, 5);
	commit(&v, "UPDATE order 103 amount 70 -> 75; INSERT customer 5 "
	    "(north), order 107");

	cust_insert(&v, 6, 1);
	commit(&v, "INSERT customer 6 with no orders");

	printf(v.failed ? "FAILED: view diverged from recomputation\n" :
	    "ok: view matched recomputation at every commit\n");

	dtfl_trace_free(v.view);
	dtfl_input_session_close(v.orders);
	dtfl_input_session_close(v.customers);
	while (dtfl_worker_step(v.w))
		;
	dtfl_probe_free(v.probe);
	dtfl_worker_free(v.w);
	dtfl_type_free(v.agg_t);
	dtfl_type_free(v.keyed_t);
	dtfl_type_free(cust_t);
	dtfl_type_free(order_t);
	dtfl_diff_free(i64);
	dtfl_time_class_free(tc);
	return v.failed ? 1 : 0;
}
