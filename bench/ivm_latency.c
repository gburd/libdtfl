/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * bench/ivm_latency.c: commit-to-delta latency of an incrementally
 * maintained view (PLAN P16.10).
 *
 * The view is examples/ivm/ivm_view.c's
 *
 *   SELECT region, count(*), sum(amount)
 *   FROM orders JOIN customers USING (customer_id) GROUP BY region
 *
 * held by one standalone worker, as the persistent maintenance worker of
 * dev/notes/pg-incremental-views.md (placement 2) holds it.  The base
 * tables start with `customers` customers over 16 regions and `orders`
 * orders.  Then `commits` transactions arrive on a fixed schedule at
 * `rate` commits per second (open loop, so a slow commit delays the ones
 * behind it, as a queue of committing backends would see).  Each
 * transaction inserts `ins` orders, deletes `del` and updates `upd`
 * (remove the old row, insert the new), then COMMIT feeds its delta at
 * the next time, steps until the probe passes it, and advances the view
 * trace's compaction holds.  Latency is from the scheduled arrival to the
 * view delta being complete.
 *
 *   bench/ivm_latency customers orders commits rate ins del upd [agg]
 *
 * agg is `reduce` (the default, as examples/ivm/ivm_view.c: count and sum
 * by a reduce over each touched group's whole multiset, so a commit costs
 * the size of the groups it touches) or `diff`, differential's way to
 * keep a sum O(change): explode each (region, amount) into (region) with
 * the amount moved into the difference, then count, which sums it.  The
 * diff form maintains sum(amount) only; count(*) would be a second count
 * over the unexploded rows.
 *
 * Prints one line: agg commits rate ins del upd p50_us p99_us p999_us
 * max_us achieved_rate.  Not a port; numbers come only from a metal host.
 */
#define _POSIX_C_SOURCE 200809L	/* clock_gettime, nanosleep */

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef REGIONS
#define REGIONS 16
#endif

typedef struct bench {
	int diff_agg;		/* agg == diff */
	dtfl_input_session *orders, *customers;
	dtfl_probe *probe;
	dtfl_trace *view;
	const dtfl_type *keyed_t, *agg_t;
} bench;

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* xorshift64*: deterministic workload, independent of the build. */
static uint64_t
rnd(uint64_t *s)
{
	*s ^= *s >> 12;
	*s ^= *s << 25;
	*s ^= *s >> 27;
	return *s * 0x2545f4914f6cdd1dULL;
}

/* order (id, cust, amount) -> (cust, amount), as examples/ivm/ivm_view.c */
static void
order_by_cust(void *ctx, const void *rec, size_t len,
    dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 8, 16);
}

/* (cust, amount) join (cust, region) -> (region, amount) */
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

/* region -> (count, sum) */
static void
count_sum(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	int64_t cnt = 0, sum = 0, d, one = 1;
	unsigned char b[16];
	size_t i;

	(void)ctx; (void)key; (void)klen; (void)vlens;
	for (i = 0; i < n; i++) {
		memcpy(&d, (const unsigned char *)diffs + i * sizeof d,
		    sizeof d);
		cnt += d;
		sum += d * dtfl_get_i64(vals[i]);
	}
	if (cnt == 0)
		return;
	dtfl_put_i64(b, cnt);
	dtfl_put_i64(b + 8, sum);
	dtfl_reduce_emit_pair(out, b, 16, &one);
}

/* (region, amount) -> (region) with the amount as the factor. */
static void
amount_into_diff(void *ctx, const void *rec, size_t len,
    dtfl_collection_emit *out)
{
	int64_t amount = dtfl_get_i64((const unsigned char *)rec + 8);

	(void)ctx; (void)len;
	dtfl_collection_emit_pair(out, rec, 8, &amount);
}

static void
build(dtfl_scope *scope, void *arg)
{
	bench *v = arg;
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
	dtfl_collection *exploded = NULL, *totals;
	dtfl_arranged *arr;

	if (v->diff_agg) {
		exploded = dtfl_collection_explode(joined, u64, i64,
		    amount_into_diff, DTFL_NOCTX);
		totals = dtfl_collection_count(exploded, i64);
	} else
		totals = dtfl_collection_reduce(joined, u64, i64t, v->agg_t,
		    i64, count_sum, DTFL_NOCTX);
	arr = dtfl_collection_arrange(totals, "region_totals");
	dtfl_collection *back = dtfl_arranged_as_collection(arr,
	    dtfl_arranged_type(arr), NULL, DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(back, v->probe);

	v->view = dtfl_arranged_trace(arr);
	dtfl_collection_free(probed);
	dtfl_collection_free(back);
	dtfl_arranged_free(arr);
	dtfl_collection_free(totals);
	if (exploded != NULL)
		dtfl_collection_free(exploded);
	dtfl_collection_free(joined);
	dtfl_collection_free(keyed);
	dtfl_collection_free(custs);
	dtfl_collection_free(orders);
	dtfl_diff_free(i64);
}

static void
put_order(unsigned char *b, uint64_t id, uint64_t cust, int64_t amount)
{
	dtfl_put_u64(b, id);
	dtfl_put_u64(b + 8, cust);
	dtfl_put_i64(b + 16, amount);
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* One sealed time: advance both inputs, step to the probe, move holds. */
static void
commit(dtfl_worker *w, bench *v, uint64_t *now)
{
	uint64_t next = *now + 1;

	dtfl_input_session_advance_to(v->orders, &next);
	dtfl_input_session_advance_to(v->customers, &next);
	dtfl_input_session_flush(v->orders);
	dtfl_input_session_flush(v->customers);
	while (dtfl_probe_less_than(v->probe, &next))
		dtfl_worker_step(w);
	dtfl_trace_set_logical_compaction(v->view, now, 1);
	dtfl_trace_set_physical_compaction(v->view, now, 1);
	*now = next;
}

int
main(int argc, char **argv)
{
	bench v;
	dtfl_worker *w;
	dtfl_time_class *tc;
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *u64 = dtfl_type_u64(), *i64t = dtfl_type_i64();
	const dtfl_type *f3[3] = { u64, u64, i64t };
	const dtfl_type *order_t = dtfl_type_tuple(f3, 3);
	const dtfl_type *cust_t = dtfl_type_pair(u64, u64);
	uint64_t ncust, nord, ncommit, rate, ins, del, upd;
	uint64_t seed = 88172645463325252ULL;
	uint64_t *lat, *ord_cust, nlive, next_id, now = 0, i, k, t0, period;
	int64_t *ord_amt;
	unsigned char b[24];

	if (argc != 8 && argc != 9) {
		fprintf(stderr,
		    "usage: %s customers orders commits rate ins del upd [agg]\n",
		    argv[0]);
		return 2;
	}
	ncust = strtoull(argv[1], NULL, 10);
	nord = strtoull(argv[2], NULL, 10);
	ncommit = strtoull(argv[3], NULL, 10);
	rate = strtoull(argv[4], NULL, 10);
	ins = strtoull(argv[5], NULL, 10);
	del = strtoull(argv[6], NULL, 10);
	upd = strtoull(argv[7], NULL, 10);
	if (ncust == 0 || ncommit == 0 || rate == 0 || del + upd > nord) {
		fprintf(stderr, "%s: bad arguments\n", argv[0]);
		return 2;
	}
	memset(&v, 0, sizeof v);
	v.diff_agg = argc == 9 && strcmp(argv[8], "diff") == 0;
	if (argc == 9 && !v.diff_agg && strcmp(argv[8], "reduce") != 0) {
		fprintf(stderr, "%s: agg is reduce or diff\n", argv[0]);
		return 2;
	}
	v.keyed_t = dtfl_type_pair(u64, i64t);
	v.agg_t = dtfl_type_pair(i64t, i64t);
	v.orders = dtfl_input_session_new(order_t, i64);
	v.customers = dtfl_input_session_new(cust_t, i64);
	v.probe = dtfl_probe_new();
	tc = dtfl_time_u64();
	w = dtfl_worker_new_standalone();
	dtfl_worker_dataflow(w, "ivm_latency", tc, build, &v);

	/* Load: customers and orders at time 0. */
	for (i = 0; i < ncust; i++) {
		dtfl_put_u64(b, i);
		dtfl_put_u64(b + 8, i % REGIONS);
		dtfl_input_session_insert(v.customers, b, 16);
	}
	ord_cust = malloc((nord + ncommit * ins + 1) * sizeof *ord_cust);
	ord_amt = malloc((nord + ncommit * ins + 1) * sizeof *ord_amt);
	for (i = 0; i < nord; i++) {
		ord_cust[i] = rnd(&seed) % ncust;
		ord_amt[i] = (int64_t)(rnd(&seed) % 1000);
		put_order(b, i, ord_cust[i], ord_amt[i]);
		dtfl_input_session_insert(v.orders, b, 24);
	}
	nlive = next_id = nord;
	commit(w, &v, &now);

	/*
	 * The commits.  Live orders are ids [next_id - nlive, next_id): a
	 * delete takes the oldest, an update rewrites a random live one.
	 */
	lat = malloc(ncommit * sizeof *lat);
	period = 1000000000u / rate;
	t0 = now_ns();
	for (k = 0; k < ncommit; k++) {
		uint64_t due = t0 + k * period, s;

		while ((s = now_ns()) < due)
			;
		for (i = 0; i < del && nlive > 0; i++) {
			uint64_t id = next_id - nlive--;

			put_order(b, id, ord_cust[id], ord_amt[id]);
			dtfl_input_session_remove(v.orders, b, 24);
		}
		for (i = 0; i < upd && nlive > 0; i++) {
			uint64_t id = next_id - nlive + rnd(&seed) % nlive;

			put_order(b, id, ord_cust[id], ord_amt[id]);
			dtfl_input_session_remove(v.orders, b, 24);
			ord_amt[id] = (int64_t)(rnd(&seed) % 1000);
			put_order(b, id, ord_cust[id], ord_amt[id]);
			dtfl_input_session_insert(v.orders, b, 24);
		}
		for (i = 0; i < ins; i++) {
			uint64_t id = next_id++;

			ord_cust[id] = rnd(&seed) % ncust;
			ord_amt[id] = (int64_t)(rnd(&seed) % 1000);
			put_order(b, id, ord_cust[id], ord_amt[id]);
			dtfl_input_session_insert(v.orders, b, 24);
			nlive++;
		}
		commit(w, &v, &now);
		lat[k] = now_ns() - due;
	}
	{
		double secs = (double)(now_ns() - t0) / 1e9;

		qsort(lat, ncommit, sizeof *lat, cmp_u64);
		printf("%s %llu %llu %llu %llu %llu %.1f %.1f %.1f %.1f %.0f\n",
		    v.diff_agg ? "diff" : "reduce", (unsigned long long)ncommit, (unsigned long long)rate,
		    (unsigned long long)ins, (unsigned long long)del,
		    (unsigned long long)upd, lat[ncommit / 2] / 1e3,
		    lat[ncommit * 99 / 100] / 1e3,
		    lat[ncommit * 999 / 1000] / 1e3, lat[ncommit - 1] / 1e3,
		    (double)ncommit / secs);
	}
	free(lat);
	free(ord_cust);
	free(ord_amt);
	dtfl_trace_free(v.view);
	dtfl_input_session_close(v.orders);
	dtfl_input_session_close(v.customers);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(v.probe);
	dtfl_worker_free(w);
	dtfl_time_class_free(tc);
	dtfl_diff_free(i64);
	dtfl_type_free(v.keyed_t);
	dtfl_type_free(v.agg_t);
	dtfl_type_free(order_t);
	dtfl_type_free(cust_t);
	return 0;
}
