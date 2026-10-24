/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/identifiers.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/algo/identifiers.c: assign a unique u64 identifier to each element.
 *
 * The design develops a collection of (round, record) proposals: the
 * identifier of a record is the hash of (round, record).  Colliding
 * proposals pick a winner (the lower round, ties broken by record) and
 * tell the losers to increment their round and retry.  Losers are rare,
 * so the reduce emits only the additions and subtractions of losers
 * rather than reproducing the winners.  The final map pairs each record
 * with the hash of its surviving (round, record) proposal.
 *
 * Record buffers are sized for the small records these algorithms carry
 * (node identifiers and short tuples); a record longer than the buffer
 * is a usage error outside the supported envelope.
 */
#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"

#include "base/base_int.h"

#include <string.h>

/* Fixed record buffer: enough for a u64 round plus a short record. */
#define IDBUF 256

/*
 * The hash callbacks run during stepping, after the build function has
 * returned, so they own a heap ctx that holds a retained copy of the
 * (round, record) descriptor and releases it when the operator drops.
 */
typedef struct hash_ctx {
	const dtfl_type *pair;
} hash_ctx;

static hash_ctx *
hash_ctx_new(const dtfl_type *pair)
{
	hash_ctx *h = __dtfl_alloc(sizeof *h);

	h->pair = dtfl_type_retain(pair);
	return h;
}

static void
hash_ctx_free(void *vctx)
{
	hash_ctx *h = vctx;

	dtfl_type_free(h->pair);
	__dtfl_free(h);
}

typedef struct id_ctx {
	const dtfl_type *rec_type;	/* the element record type */
	const dtfl_type *pair;		/* (u64 round, record) */
	const dtfl_type *hashed;	/* (u64 hash, pair) */
	const dtfl_type *out;		/* (record, u64 id) */
	dtfl_collection *init;		/* round-0 proposals, outer scope */
} id_ctx;

/* map record -> (0u64, record): the initial round-0 proposal. */
static void
to_round0(void *vctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[IDBUF];

	(void)vctx;
	dtfl_put_u64(buf, 0);
	if (len != 0)
		memcpy(buf + 8, rec, len);
	dtfl_collection_emit_record(out, buf, len + 8);
}

/*
 * map pair=(round, record) -> (hash, pair): key each proposal by the
 * hash of (round, record).
 */
static void
to_hashed(void *vctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	hash_ctx *c = vctx;
	unsigned char buf[IDBUF];
	uint64_t h = dtfl_type_hash(c->pair, rec, len);

	dtfl_put_u64(buf, h);
	if (len != 0)
		memcpy(buf + 8, rec, len);
	dtfl_collection_emit_record(out, buf, len + 8);
}

/* map (hash, pair) -> pair: drop the hash key. */
static void
drop_hash(void *vctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)vctx;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + 8,
	    len - 8);
}

/* filter that drops every pair, to seed an empty iterate. */
static bool
drop_all_pairs(void *ctx, const void *rec, size_t len)
{
	(void)ctx; (void)rec; (void)len;
	return false;
}

/*
 * map pair=(round, record) -> (record, hash): the final output pairs each
 * record with the hash of its surviving (round, record) proposal.
 */
static void
to_output(void *vctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	hash_ctx *c = vctx;
	unsigned char buf[IDBUF];
	uint64_t h = dtfl_type_hash(c->pair, rec, len);
	size_t rl = len - 8;

	if (rl != 0)
		memcpy(buf, (const unsigned char *)rec + 8, rl);
	dtfl_put_u64(buf + rl, h);
	dtfl_collection_emit_record(out, buf, rl + 8);
}

/*
 * reduce logic over the proposals sharing a hash, sorted by pair order
 * (round first, then record).  The winner is input[0]: if its round is
 * positive, move it back to round 0 (retract the round-positive form,
 * assert the round-0 form).  Every loser (input[1..]) increments its
 * round: retract its round-0 form, assert its round+1 form.
 */
static void
resolve(void *vctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	const unsigned char *dp = diffs;
	size_t i;
	unsigned char buf[IDBUF];

	(void)vctx; (void)key; (void)klen;
	for (i = 0; i < n; i++) {
		const unsigned char *v = vals[i];
		uint64_t round = dtfl_get_u64(v);
		size_t rl = vlens[i] - 8;
		int64_t cnt;
		int64_t neg;

		memcpy(&cnt, dp + i * sizeof(int64_t), sizeof cnt);
		neg = -cnt;

		if (i == 0) {
			if (round == 0)
				continue;	/* winner already at round 0 */
			dtfl_put_u64(buf, 0);
			if (rl != 0)
				memcpy(buf + 8, v + 8, rl);
			dtfl_reduce_emit_pair(out, buf, rl + 8, &neg);
			dtfl_reduce_emit_pair(out, v, vlens[i], &cnt);
		} else {
			dtfl_put_u64(buf, 0);
			if (rl != 0)
				memcpy(buf + 8, v + 8, rl);
			dtfl_reduce_emit_pair(out, buf, rl + 8, &neg);
			dtfl_put_u64(buf, round + 1);
			dtfl_reduce_emit_pair(out, buf, rl + 8, &cnt);
		}
	}
}

static dtfl_collection *
id_logic(void *vctx, dtfl_scope *inner, dtfl_collection *diff)
{
	id_ctx *c = vctx;
	dtfl_collection *init = dtfl_collection_enter(c->init, inner);
	dtfl_collection *merged = dtfl_collection_concat(init, diff);
	dtfl_collection *keyed = dtfl_collection_map(merged, c->hashed,
	    to_hashed, DTFL_CTX(hash_ctx_new(c->pair), hash_ctx_free));
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *reduced = dtfl_collection_reduce(keyed,
	    dtfl_type_u64(), c->pair, c->pair, i64, resolve, DTFL_NOCTX);
	dtfl_collection *result = dtfl_collection_map(reduced, c->pair,
	    drop_hash, DTFL_NOCTX);

	dtfl_diff_free(i64);
	dtfl_collection_free(reduced);
	dtfl_collection_free(keyed);
	dtfl_collection_free(merged);
	dtfl_collection_free(init);
	return result;
}

dtfl_collection *
dtfl_algo_identifiers(dtfl_collection *c, const dtfl_type *rec_type)
{
	id_ctx ic;
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), rec_type);
	const dtfl_type *hashed = dtfl_type_pair(dtfl_type_u64(), pair);
	const dtfl_type *out = dtfl_type_pair(rec_type, dtfl_type_u64());
	dtfl_collection *init = dtfl_collection_map(c, pair, to_round0,
	    DTFL_NOCTX);
	dtfl_collection *empty;
	dtfl_collection *looped;
	dtfl_collection *all;
	dtfl_collection *result;

	ic.rec_type = rec_type;
	ic.pair = pair;
	ic.hashed = hashed;
	ic.out = out;
	ic.init = init;

	empty = dtfl_collection_filter(init, drop_all_pairs, DTFL_NOCTX);
	looped = dtfl_collection_iterate(empty, id_logic, DTFL_CTX(&ic, NULL));
	all = dtfl_collection_concat(looped, init);
	result = dtfl_collection_map(all, out, to_output,
	    DTFL_CTX(hash_ctx_new(pair), hash_ctx_free));

	dtfl_collection_free(all);
	dtfl_collection_free(looped);
	dtfl_collection_free(empty);
	dtfl_collection_free(init);
	dtfl_type_free(out);
	dtfl_type_free(hashed);
	dtfl_type_free(pair);
	return result;
}
