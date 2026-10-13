/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/itembased_cf.c: mirrors differential-dataflow/examples/itembased_cf.rs.
 *
 * Item-based collaborative filtering by Jaccard similarity over item
 * interaction histories (Algorithm 1 of https://ssc.io/pdf/amnesia.pdf):
 *   - keep users with fewer than a cap of interactions (semijoin);
 *   - count interactions per item;
 *   - arrange the remaining (user, item) interactions by key and join the
 *     arrangement with itself to find item co-occurrences (item_a > item_b),
 *     then count each pair;
 *   - join the co-occurrence counts with the per-item counts twice to gather
 *     occ_a and occ_b, then compute the Jaccard similarity and threshold it.
 *
 * Upstream generates a seeded synthetic interaction stream and reports
 * timing; a C runtime cannot reproduce that stream, so this port feeds a
 * small fixed interaction set and prints the thresholded pairs, which is
 * exact.  Upstream computes the Jaccard ratio in f64; floats are not a
 * built-in record type here, so this port carries the similarity as a
 * fixed-point value scaled by 1000 (so 0.05 becomes 50) and thresholds on
 * that.  The ratio is num_cooc / (occ_a + occ_b - num_cooc).
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

/* A fixed set of (user, item) interactions. */
static const uint64_t fixed_interactions[][2] = {
	{ 0, 10 }, { 0, 11 }, { 0, 12 },
	{ 1, 10 }, { 1, 11 },
	{ 2, 11 }, { 2, 12 },
	{ 3, 10 }, { 3, 12 },
	{ 4, 10 },
};
#define FIXED_I (sizeof fixed_interactions / sizeof fixed_interactions[0])

/* Keep users with fewer than this many interactions (upstream uses 500). */
#define USER_CAP 500
/* Jaccard threshold, scaled by 1000 (upstream 0.05). */
#define JACCARD_MILLI_MIN 50

/* map (user, item) -> user. */
static void
inter_to_user(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 8);
}

/* map (user, item) -> item. */
static void
inter_to_item(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 8, 8);
}

/* filter: keep (user, count) with count < USER_CAP. */
static bool
few_enough(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)len;
	return dtfl_get_i64((const unsigned char *)rec + 8) < USER_CAP;
}

/* map (user, count) -> user. */
static void
usercount_to_user(void *ctx, const void *rec, size_t len,
    dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 8);
}

/*
 * cooccurrence join: both sides are (user -> item).  Emit (item_a, item_b)
 * only when item_a > item_b, so each unordered item pair is counted once.
 */
static void
cooc_result(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[16];
	uint64_t a, b;

	(void)ctx; (void)key; (void)klen; (void)v1len; (void)v2len;
	a = dtfl_get_u64(v1);
	b = dtfl_get_u64(v2);
	if (a > b) {
		memcpy(buf, v1, 8);
		memcpy(buf + 8, v2, 8);
		dtfl_join_emit_record(out, buf, sizeof buf);
	}
}

/*
 * map the counted co-occurrences ((item_a, item_b) || num_cooc) to
 * (item_a, (item_b, num_cooc)) for the first per-item join.
 */
static void
cooc_key_a(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[24];

	(void)ctx; (void)len;
	memcpy(buf, rec, 8);			/* key = item_a */
	memcpy(buf + 8, (const unsigned char *)rec + 8, 8);	/* item_b */
	memcpy(buf + 16, (const unsigned char *)rec + 16, 8);	/* num_cooc */
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/*
 * first per-item join: key item_a, val1 = (item_b, num_cooc), val2 = occ_a.
 * Emit (item_b, (item_a, num_cooc, occ_a)).
 */
static void
join_occa(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[32];
	uint64_t item_a = dtfl_get_u64(key);
	uint64_t item_b = dtfl_get_u64(v1);
	int64_t num_cooc = dtfl_get_i64((const unsigned char *)v1 + 8);
	int64_t occ_a = dtfl_get_i64(v2);

	(void)ctx; (void)klen; (void)v1len; (void)v2len;
	dtfl_put_u64(buf, item_b);		/* new key = item_b */
	dtfl_put_u64(buf + 8, item_a);
	dtfl_put_i64(buf + 16, num_cooc);
	dtfl_put_i64(buf + 24, occ_a);
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * second per-item join: key item_b, val1 = (item_a, num_cooc, occ_a),
 * val2 = occ_b.  Compute the scaled Jaccard and emit
 * ((item_a, item_b), jaccard_milli) only when above the threshold.
 */
static void
join_occb(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[24];
	uint64_t item_b = dtfl_get_u64(key);
	uint64_t item_a = dtfl_get_u64(v1);
	int64_t num_cooc = dtfl_get_i64((const unsigned char *)v1 + 8);
	int64_t occ_a = dtfl_get_i64((const unsigned char *)v1 + 16);
	int64_t occ_b = dtfl_get_i64(v2);
	int64_t denom = occ_a + occ_b - num_cooc;
	int64_t jmilli;

	(void)ctx; (void)klen; (void)v1len; (void)v2len;
	if (denom <= 0)
		return;
	jmilli = num_cooc * 1000 / denom;
	if (jmilli <= JACCARD_MILLI_MIN)
		return;
	dtfl_put_u64(buf, item_a);
	dtfl_put_u64(buf + 8, item_b);
	dtfl_put_i64(buf + 16, jmilli);
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* inspect: print the surviving (item_a, item_b, jaccard_milli). */
static void
inspect_similarity(void *ctx, const void *rec, size_t len,
    const uint64_t *time, const void *diff)
{
	int64_t d;

	(void)ctx; (void)len; (void)time;
	memcpy(&d, diff, sizeof d);
	if (d <= 0)
		return;
	printf("similarity: (%llu, %llu) = %lld/1000\n",
	    (unsigned long long)dtfl_get_u64(rec),
	    (unsigned long long)dtfl_get_u64((const unsigned char *)rec + 8),
	    (long long)dtfl_get_i64((const unsigned char *)rec + 16));
}

typedef struct build_ctx {
	dtfl_input_session *interactions;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *i64t = dtfl_type_i64();
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *item_pair = dtfl_type_pair(u64, u64);
	const dtfl_type *a_rec = dtfl_type_pair(u64, item_pair);	/* a||(b||cooc) */
	const dtfl_type *b_fields[4] = { u64, u64, i64t, i64t };
	const dtfl_type *b_rec = dtfl_type_tuple(b_fields, 4);	/* b||a||cooc||occa */
	const dtfl_type *sim_fields[3] = { u64, u64, i64t };
	const dtfl_type *sim_rec = dtfl_type_tuple(sim_fields, 3);
	const dtfl_type *occa_fields[3] = { u64, i64t, i64t };
	const dtfl_type *occa_val = dtfl_type_tuple(occa_fields, 3);
	const dtfl_type *useritem = dtfl_type_pair(u64, u64);

	dtfl_collection *interactions = dtfl_input_session_to_collection(
	    c->interactions, scope);
	/* Users with few enough interactions. */
	dtfl_collection *users = dtfl_collection_map(interactions, u64,
	    inter_to_user, DTFL_NOCTX);
	dtfl_collection *user_counts = dtfl_collection_count_total(users, i64);
	dtfl_collection *few = dtfl_collection_filter(user_counts, few_enough,
	    DTFL_NOCTX);
	dtfl_collection *keep_users = dtfl_collection_map(few, u64,
	    usercount_to_user, DTFL_NOCTX);
	/* remaining = interactions.semijoin(keep_users). */
	dtfl_collection *remaining = dtfl_collection_semijoin(interactions,
	    keep_users, u64, u64, useritem);
	/* num interactions per item (item -> count). */
	dtfl_collection *items = dtfl_collection_map(remaining, u64,
	    inter_to_item, DTFL_NOCTX);
	dtfl_collection *per_item = dtfl_collection_count_total(items, i64);
	/* arrange remaining by key = user; join with itself for co-occurrences. */
	dtfl_arranged *arr_remaining = dtfl_collection_arrange_by_key(remaining,
	    u64, u64, "Remaining");
	dtfl_collection *cooc = dtfl_arranged_join_core(arr_remaining,
	    arr_remaining, item_pair, cooc_result, DTFL_NOCTX);
	dtfl_collection *cooc_counts = dtfl_collection_count(cooc, i64);
	/* arrange per-item counts by key = item. */
	dtfl_arranged *arr_per_item = dtfl_collection_arrange_by_key(per_item,
	    u64, i64t, "PerItem");
	/* first join: gather occ_a keyed on item_a. */
	dtfl_collection *keyed_a = dtfl_collection_map(cooc_counts, a_rec,
	    cooc_key_a, DTFL_NOCTX);
	dtfl_arranged *arr_keyed_a = dtfl_collection_arrange_by_key(keyed_a,
	    u64, item_pair, "KeyedA");
	dtfl_collection *with_occa = dtfl_arranged_join_core(arr_keyed_a,
	    arr_per_item, b_rec, join_occa, DTFL_NOCTX);
	/* second join: gather occ_b keyed on item_b, compute Jaccard. */
	dtfl_arranged *arr_with_occa = dtfl_collection_arrange_by_key(with_occa,
	    u64, occa_val, "WithOccA");
	dtfl_collection *sims = dtfl_arranged_join_core(arr_with_occa,
	    arr_per_item, sim_rec, join_occb, DTFL_NOCTX);
	dtfl_collection *con = dtfl_collection_consolidate(sims);
	dtfl_collection *ins = dtfl_collection_inspect(con, inspect_similarity,
	    DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_diff_free(i64);
	dtfl_type_free(item_pair);
	dtfl_type_free(a_rec);
	dtfl_type_free(b_rec);
	dtfl_type_free(sim_rec);
	dtfl_type_free(occa_val);
	dtfl_type_free(useritem);
	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(con);
	dtfl_collection_free(sims);
	dtfl_arranged_free(arr_with_occa);
	dtfl_collection_free(with_occa);
	dtfl_arranged_free(arr_keyed_a);
	dtfl_collection_free(keyed_a);
	dtfl_arranged_free(arr_per_item);
	dtfl_collection_free(cooc_counts);
	dtfl_collection_free(cooc);
	dtfl_arranged_free(arr_remaining);
	dtfl_collection_free(per_item);
	dtfl_collection_free(items);
	dtfl_collection_free(remaining);
	dtfl_collection_free(keep_users);
	dtfl_collection_free(few);
	dtfl_collection_free(user_counts);
	dtfl_collection_free(users);
	dtfl_collection_free(interactions);
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
	c.interactions = dtfl_input_session_new(pair, i64);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "itembased_cf", tc, build, &c);

	if (dtfl_worker_index(w) == 0)
		for (i = 0; i < FIXED_I; i++) {
			unsigned char buf[16];

			dtfl_put_u64(buf, fixed_interactions[i][0]);
			dtfl_put_u64(buf + 8, fixed_interactions[i][1]);
			dtfl_input_session_insert(c.interactions, buf,
			    sizeof buf);
		}
	dtfl_input_session_advance_to(c.interactions, &one);
	dtfl_input_session_flush(c.interactions);
	while (dtfl_probe_less_than(c.probe,
	    dtfl_input_session_time(c.interactions)))
		dtfl_worker_step(w);
	dtfl_input_session_close(c.interactions);
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
	/* Upstream takes a count argument; keep timely flags only. */
	int skip = argc > 1 ? 1 : 0;
	dtfl_config *cfg = ex_config(argc, argv, skip);
	int rc;

	/* Drive a single worker so the fixed interactions are not split. */
	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, NULL, NULL);
	dtfl_config_free(cfg);
	return rc;
}
