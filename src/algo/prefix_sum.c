/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/prefix_sum.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/algo/prefix_sum.c: data-parallel prefix sum.
 *
 * aggregate folds data into every power-of-two interval that contains
 * it; broadcast reconstructs each location's prefix by requesting the
 * intervals its index's binary representation names, filling absent
 * intervals with zero, and iteratively combining them.  The sums run
 * independently per key.
 *
 * Records: a value element is ((u64 index, u64 key), u64 data); an
 * interval is ((u64 pos, u64 log, u64 key), u64 data); a query is
 * (u64 index, u64 key).  The combine closure is associative per key.
 */
#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"

#include "base/base_int.h"

#include <string.h>

/* The combine closure and its context, shared by step-time callbacks. */
typedef struct comb_ctx {
	dtfl_algo_combine_fn combine;
	void *user;
	uint64_t zero;
} comb_ctx;

static comb_ctx *
comb_ctx_new(const comb_ctx *src)
{
	comb_ctx *c = __dtfl_alloc(sizeof *c);

	*c = *src;
	return c;
}

/* ((pos, log, key), data) interval record type. */
static const dtfl_type *
range_type(void)
{
	const dtfl_type *plk = dtfl_type_tuple((const dtfl_type *[]){
	    dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64() }, 3);
	const dtfl_type *r = dtfl_type_pair(plk, dtfl_type_u64());

	dtfl_type_free(plk);
	return r;
}

/* (index, key) query record type. */
static const dtfl_type *
query_type(void)
{
	return dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
}

/*
 * map ((index, key), data) -> ((index, 0, key), data): initial unit
 * ranges at width 2^0.  The input layout is index|key|data (24 bytes).
 */
static void
unit_range(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[32];

	(void)ctx; (void)len;
	memcpy(buf, p, 8);		/* index -> pos */
	dtfl_put_u64(buf + 8, 0);	/* log 0 */
	memcpy(buf + 16, p + 8, 8);	/* key */
	memcpy(buf + 24, p + 16, 8);	/* data */
	dtfl_collection_emit_record(out, buf, 32);
}

/* filter intervals with log < 64 (room to double). */
static bool
log_lt_64(void *ctx, const void *rec, size_t len)
{
	const unsigned char *p = rec;

	(void)ctx; (void)len;
	return dtfl_get_u64(p + 8) < 64;
}

/*
 * map ((pos, log, key), data) -> ((pos>>1, log+1, key), (pos, data)):
 * advertise the doubled, aligned range, carrying (pos, data) as value.
 * Value becomes (u64 pos, u64 data).
 */
static void
advertise(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	uint64_t pos = dtfl_get_u64(p);
	uint64_t log = dtfl_get_u64(p + 8);
	unsigned char buf[40];

	(void)ctx; (void)len;
	dtfl_put_u64(buf, pos >> 1);		/* pos >> 1 */
	dtfl_put_u64(buf + 8, log + 1);		/* log + 1 */
	memcpy(buf + 16, p + 16, 8);		/* key */
	dtfl_put_u64(buf + 24, pos);		/* value: pos */
	memcpy(buf + 32, p + 24, 8);		/* value: data */
	dtfl_collection_emit_record(out, buf, 40);
}

/*
 * reduce over the (pos, data) values advertised for one (pos, log, key),
 * sorted by (pos, data).  At most two values fall in a doubled range:
 * combine the first with the second (if present).  Emit data, diff 1.
 */
static void
summarize(void *vctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	comb_ctx *c = vctx;
	const unsigned char *kp = key;	/* (pos, log, key) */
	uint64_t result = dtfl_get_u64((const unsigned char *)vals[0] + 8);
	int64_t one = 1;
	unsigned char buf[8];

	(void)klen; (void)vlens; (void)diffs;
	if (n > 1) {
		uint64_t other = dtfl_get_u64((const unsigned char *)vals[1] + 8);
		uint64_t combined;

		c->combine(c->user, kp + 16, 8, &result, &other, &combined);
		result = combined;
	}
	dtfl_put_u64(buf, result);
	dtfl_reduce_emit_pair(out, buf, 8, &one);
}

typedef struct agg_ctx {
	dtfl_collection *unit;	/* the unit ranges, outer scope */
	comb_ctx comb;
	const dtfl_type *rt;	/* ((pos, log, key), data) */
} agg_ctx;

static dtfl_collection *
agg_logic(void *vctx, dtfl_scope *inner, dtfl_collection *ranges)
{
	agg_ctx *c = vctx;
	const dtfl_type *advk = dtfl_type_tuple((const dtfl_type *[]){
	    dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64() }, 3);
	const dtfl_type *advv = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	const dtfl_type *advrec;
	dtfl_collection *small = dtfl_collection_filter(ranges, log_lt_64,
	    DTFL_NOCTX);
	dtfl_collection *adv;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *reduced;
	dtfl_collection *unit = dtfl_collection_enter(c->unit, inner);
	dtfl_collection *result;

	advrec = dtfl_type_pair(advk, advv);
	adv = dtfl_collection_map(small, advrec, advertise, DTFL_NOCTX);
	reduced = dtfl_collection_reduce(adv, advk, advv, dtfl_type_u64(), i64,
	    summarize, DTFL_CTX(comb_ctx_new(&c->comb), __dtfl_free));
	result = dtfl_collection_concat(reduced, unit);

	dtfl_collection_free(reduced);
	dtfl_collection_free(unit);
	dtfl_collection_free(adv);
	dtfl_collection_free(small);
	dtfl_diff_free(i64);
	dtfl_type_free(advrec);
	dtfl_type_free(advv);
	dtfl_type_free(advk);
	return result;
}

/* map ((index, key), data) -> (index, key): the query of an element. */
static void
elem_to_query(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 16);	/* index|key */
}

/* flat_map (index, key) -> one request per set bit of index.
 * Request ((index>>i)-1, i, key) for each set bit i: the width-2^i
 * interval that the index needs.  Value side is the request record
 * ((pos, log, key)). */
static void
requests(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	uint64_t idx = dtfl_get_u64(p);
	unsigned i;
	unsigned char buf[24];

	(void)ctx; (void)len;
	for (i = 0; i < 64; i++) {
		if ((idx & (UINT64_C(1) << i)) == 0)
			continue;
		dtfl_put_u64(buf, (idx >> i) - 1);	/* pos */
		dtfl_put_u64(buf + 8, i);		/* log */
		memcpy(buf + 16, p + 8, 8);		/* key */
		dtfl_collection_emit_record(out, buf, 24);
	}
}

/* map ((pos, log, key), data) -> ((pos, log, key), zero): a zero range. */
typedef struct zero_ctx { uint64_t zero; } zero_ctx;

/*
 * A heap copy of a zero_ctx, freed with __dtfl_free.  The map callbacks
 * run during worker stepping, after prefix_broadcast has returned, so
 * their context must outlive the stack frame that creates them.
 */
static zero_ctx *
zero_ctx_new(uint64_t zero)
{
	zero_ctx *z = __dtfl_alloc(sizeof *z);

	z->zero = zero;
	return z;
}

static void
to_zero_range(void *vctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	zero_ctx *z = vctx;
	unsigned char buf[32];

	(void)len;
	memcpy(buf, rec, 24);			/* (pos, log, key) */
	dtfl_put_u64(buf + 24, z->zero);	/* zero data */
	dtfl_collection_emit_record(out, buf, 32);
}

/* map ((pos, log, key)) request -> ((pos, log, key), zero): seed zeros. */
static void
req_to_zero_range(void *vctx, const void *rec, size_t len,
    dtfl_collection_emit *out)
{
	zero_ctx *z = vctx;
	unsigned char buf[32];

	(void)len;
	memcpy(buf, rec, 24);
	dtfl_put_u64(buf + 24, z->zero);
	dtfl_collection_emit_record(out, buf, 32);
}

/* map (index, key) -> ((0, key), zero): each query seeds a zero at 0. */
static void
query_init(void *vctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	zero_ctx *z = vctx;
	const unsigned char *p = rec;
	unsigned char buf[24];

	(void)len;
	dtfl_put_u64(buf, 0);		/* pos 0 */
	memcpy(buf + 8, p + 8, 8);	/* key */
	dtfl_put_u64(buf + 16, z->zero);	/* zero data */
	dtfl_collection_emit_record(out, buf, 24);
}

/*
 * map ((pos, log, key), data) -> ((pos<<log, key), (log, data)): a range
 * contributes its data to the position pos<<log.
 */
static void
range_contrib(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	uint64_t pos = dtfl_get_u64(p);
	uint64_t log = dtfl_get_u64(p + 8);
	unsigned char buf[32];

	(void)ctx; (void)len;
	dtfl_put_u64(buf, pos << log);	/* position */
	memcpy(buf + 8, p + 16, 8);	/* key */
	dtfl_put_u64(buf + 16, log);	/* value: log */
	memcpy(buf + 24, p + 24, 8);	/* value: data */
	dtfl_collection_emit_record(out, buf, 32);
}

/*
 * join_map: range contribution ((pos, key) -> (log, data)) with the
 * current state ((pos, key) -> state).  Emit ((pos + (1<<log), key),
 * combine(key, state, data)).
 */
static void
extend_state(void *vctx, const void *key, size_t klen, const void *v1,
    size_t v1l, const void *v2, size_t v2l, dtfl_join_emit *out)
{
	comb_ctx *c = vctx;
	const unsigned char *k = key;		/* (pos, key) */
	const unsigned char *rv = v1;		/* (log, data) */
	uint64_t pos = dtfl_get_u64(k);
	uint64_t log = dtfl_get_u64(rv);
	uint64_t data = dtfl_get_u64(rv + 8);
	uint64_t state = dtfl_get_u64(v2);
	uint64_t combined;
	unsigned char buf[24];

	(void)klen; (void)v1l; (void)v2l;
	c->combine(c->user, k + 8, 8, &state, &data, &combined);
	dtfl_put_u64(buf, pos + (UINT64_C(1) << log));	/* position */
	memcpy(buf + 8, k + 8, 8);			/* key */
	dtfl_put_u64(buf + 16, combined);		/* combined data */
	dtfl_join_emit_record(out, buf, 24);
}

/*
 * The broadcast iterate context: used ranges in the subscope and the
 * combine closure.
 */
typedef struct bcast_ctx {
	dtfl_collection *used;	/* ((pos, log, key), data) ranges */
	dtfl_collection *init;	/* ((0, key), zero) initial states */
	comb_ctx comb;
} bcast_ctx;

static dtfl_collection *
bcast_logic(void *vctx, dtfl_scope *inner, dtfl_collection *states)
{
	bcast_ctx *c = vctx;
	const dtfl_type *contribk = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());			/* (pos, key) */
	const dtfl_type *contribv = dtfl_type_pair(dtfl_type_u64(),
	    dtfl_type_u64());			/* (log, data) */
	const dtfl_type *statev = dtfl_type_u64();
	const dtfl_type *outrec = dtfl_type_pair(contribk, statev);
	dtfl_collection *used = dtfl_collection_enter(c->used, inner);
	dtfl_collection *contrib = dtfl_collection_map(used, dtfl_type_pair(
	    contribk, contribv), range_contrib, DTFL_NOCTX);
	dtfl_collection *joined = dtfl_collection_join_map(contrib, states,
	    contribk, contribv, statev, outrec,
	    extend_state, DTFL_CTX(comb_ctx_new(&c->comb), __dtfl_free));
	dtfl_collection *init = dtfl_collection_enter(c->init, inner);
	dtfl_collection *merged = dtfl_collection_concat(joined, init);
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *result = dtfl_collection_distinct(merged, i64);

	dtfl_diff_free(i64);
	dtfl_collection_free(merged);
	dtfl_collection_free(init);
	dtfl_collection_free(joined);
	dtfl_collection_free(contrib);
	dtfl_collection_free(used);
	dtfl_type_free(outrec);
	dtfl_type_free(contribv);
	dtfl_type_free(contribk);
	return result;
}

/*
 * broadcast: reconstruct each query's prefix from the aggregated ranges.
 * Each query requests the intervals its index names; the requests are
 * de-duplicated, matched against the real ranges (semijoin), and missing
 * ranges are supplied as zeros.  An iterate then combines the ranges
 * along each query's path, and the result is restricted to the queries.
 */
static dtfl_collection *
prefix_broadcast(dtfl_collection *ranges, dtfl_collection *values,
    uint64_t zero, const comb_ctx *comb)
{
	const dtfl_type *qt = query_type();
	const dtfl_type *reqrec = dtfl_type_tuple((const dtfl_type *[]){
	    dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64() }, 3);
	const dtfl_type *rangek = reqrec;	/* (pos, log, key) */
	const dtfl_type *rt = range_type();
	const dtfl_type *initrec = dtfl_type_pair(
	    dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64()), dtfl_type_u64());
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *queries;
	dtfl_collection *req_raw;
	dtfl_collection *reqs;
	dtfl_collection *full;
	dtfl_collection *neg_full;
	dtfl_collection *req_zero;
	dtfl_collection *zero_ranges;
	dtfl_collection *used;
	dtfl_collection *init_states;
	dtfl_collection *init_distinct;
	bcast_ctx bc;
	dtfl_collection *expanded;
	dtfl_collection *result;

	/* queries = values.map(|(x,_)| x): the (index, key) of each element. */
	queries = dtfl_collection_map(values, qt, elem_to_query, DTFL_NOCTX);
	req_raw = dtfl_collection_flat_map(queries, reqrec, requests, DTFL_NOCTX);
	reqs = dtfl_collection_distinct(req_raw, i64);

	/* full = ranges.semijoin(reqs) by (pos, log, key). */
	full = dtfl_collection_semijoin(ranges, reqs, rangek, dtfl_type_u64(),
	    rt);

	/* zero_ranges = -(full.map(to zero)) concat reqs.map(to zero). */
	neg_full = dtfl_collection_map(full, rt, to_zero_range,
	    DTFL_CTX(zero_ctx_new(zero), __dtfl_free));
	{
		dtfl_collection *neg = dtfl_collection_negate(neg_full);

		req_zero = dtfl_collection_map(reqs, rt, req_to_zero_range,
		    DTFL_CTX(zero_ctx_new(zero), __dtfl_free));
		zero_ranges = dtfl_collection_concat(neg, req_zero);
		dtfl_collection_free(neg);
	}
	used = dtfl_collection_concat(full, zero_ranges);

	/* init_states = queries.map(|(_, key)| ((0, key), zero)).distinct(). */
	init_states = dtfl_collection_map(queries, initrec, query_init,
	    DTFL_CTX(zero_ctx_new(zero), __dtfl_free));
	init_distinct = dtfl_collection_distinct(init_states, i64);

	bc.used = used;
	bc.init = init_distinct;
	bc.comb = *comb;
	expanded = dtfl_collection_iterate(init_distinct, bcast_logic,
	    DTFL_CTX(&bc, NULL));

	/* restrict to the queries. */
	result = dtfl_collection_semijoin(expanded, queries,
	    dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64()), dtfl_type_u64(),
	    initrec);

	dtfl_collection_free(expanded);
	dtfl_collection_free(init_distinct);
	dtfl_collection_free(init_states);
	dtfl_collection_free(used);
	dtfl_collection_free(zero_ranges);
	dtfl_collection_free(req_zero);
	dtfl_collection_free(neg_full);
	dtfl_collection_free(full);
	dtfl_collection_free(reqs);
	dtfl_collection_free(req_raw);
	dtfl_collection_free(queries);
	dtfl_diff_free(i64);
	dtfl_type_free(initrec);
	dtfl_type_free(rt);
	dtfl_type_free(reqrec);
	dtfl_type_free(qt);
	return result;
}

/* The surviving public entry: aggregate the collection then broadcast. */
dtfl_collection *
dtfl_algo_prefix_sum(dtfl_collection *c, uint64_t zero,
    dtfl_algo_combine_fn combine, dtfl_ctx ctx)
{
	const dtfl_type *rt = range_type();
	agg_ctx ac;
	dtfl_collection *unit;
	dtfl_collection *ranges;
	dtfl_collection *result;

	ac.comb.combine = combine;
	ac.comb.user = ctx.ptr;
	ac.comb.zero = zero;
	ac.rt = rt;

	unit = dtfl_collection_map(c, rt, unit_range, DTFL_NOCTX);
	ac.unit = unit;
	ranges = dtfl_collection_iterate(unit, agg_logic, DTFL_CTX(&ac, NULL));

	result = prefix_broadcast(ranges, c, zero, &ac.comb);

	dtfl_collection_free(ranges);
	dtfl_collection_free(unit);
	dtfl_type_free(rt);
	return result;
}
