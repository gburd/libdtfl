/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/algorithms/graphs/sequential.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/algo/sequential.c: apply a logic to nodes sequentially, in order
 * of node identifier.
 *
 * A node fires once all of its lower-identifier neighbours have fired.
 * The iteration carries each node's state as an option(u64): None until
 * the node fires, then Some(value).  New state flows forward along edges
 * (src < dst) and the initial state flows backward along edges
 * (src > dst).  A node whose forward inputs are not all complete
 * (someone sent None) is suppressed with an antijoin and emits None;
 * otherwise the logic runs over the complete inputs and emits Some.
 */
#include "dtfl/algo.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"

#include "base/base_int.h"

#include <string.h>

/* (u64 node, option(u64) value) record type. */
static const dtfl_type *
node_opt_type(void)
{
	return dtfl_type_pair(dtfl_type_u64(), dtfl_type_option(dtfl_type_u64()));
}

/* (u64 node, u64 value) record type (the value side carries a bare u64). */
static const dtfl_type *
node_val_type(void)
{
	return dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
}

/* map (node, value) -> (node, None): seed every node with None. */
static void
seed_none(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[9];

	(void)ctx; (void)len;
	memcpy(buf, rec, 8);	/* node */
	buf[8] = 0;		/* option tag None */
	dtfl_collection_emit_record(out, buf, 9);
}

/* filter: keep edges with src < dst (forward). */
static bool
is_forward(void *ctx, const void *rec, size_t len)
{
	const unsigned char *p = rec;

	(void)ctx; (void)len;
	return memcmp(p, p + 8, 8) < 0;
}

/* filter: keep edges with src > dst (reverse). */
static bool
is_reverse(void *ctx, const void *rec, size_t len)
{
	const unsigned char *p = rec;

	(void)ctx; (void)len;
	return memcmp(p, p + 8, 8) > 0;
}

/*
 * join_map: state keyed by node joined with edges keyed by src.  val1 is
 * the state (option(u64)), val2 is the destination.  Emit (dst, state):
 * the state travels to the destination.
 */
static void
send_state(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)key; (void)klen; (void)v2l;
	memcpy(buf, v2, 8);		/* destination node */
	memcpy(buf + 8, v1, v1l);	/* state option(u64) */
	dtfl_join_emit_record(out, buf, 8 + v1l);
}

/* filter: keep (node, state) where state is None (tag byte 0). */
static bool
state_is_none(void *ctx, const void *rec, size_t len)
{
	const unsigned char *p = rec;

	(void)ctx; (void)len;
	return p[8] == 0;
}

/* filter: keep (node, state) where state is Some (tag byte 1). */
static bool
state_is_some(void *ctx, const void *rec, size_t len)
{
	const unsigned char *p = rec;

	(void)ctx; (void)len;
	return p[8] == 1;
}

/* map (node, state) -> node: project the key for a distinct. */
static void
project_node(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, rec, 8);
}

/* map (node, Some(v)) -> (node, v): unwrap the option to a bare value. */
static void
unwrap_some(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *p = rec;
	unsigned char buf[16];

	(void)ctx; (void)len;
	memcpy(buf, p, 8);		/* node */
	memcpy(buf + 8, p + 9, 8);	/* value (skip the Some tag) */
	dtfl_collection_emit_record(out, buf, 16);
}

/* map node -> (node, None): the incomplete nodes emit None. */
static void
node_to_none(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[9];

	(void)ctx; (void)len;
	memcpy(buf, rec, 8);
	buf[8] = 0;
	dtfl_collection_emit_record(out, buf, 9);
}

/*
 * join_map: old state keyed by node joined with reverse edges keyed by
 * src.  val1 is the bare u64 value, val2 is the destination.  Emit
 * (dst, value): the initial state travels backward along the edge.
 */
static void
send_bare_value(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1l, const void *v2, size_t v2l, dtfl_join_emit *out)
{
	unsigned char buf[16];

	(void)ctx; (void)key; (void)klen; (void)v1l; (void)v2l;
	memcpy(buf, v2, 8);		/* destination node */
	memcpy(buf + 8, v1, 8);		/* bare value */
	dtfl_join_emit_record(out, buf, 16);
}

typedef struct seq_ctx {
	dtfl_collection *state;	/* (node, value) initial state */
	dtfl_collection *edges;	/* (src, dst) edges */
	dtfl_algo_sequence_fn logic;
	void *user;
	const dtfl_type *nv;	/* (u64, u64) */
	const dtfl_type *no;	/* (u64, option(u64)) */
} seq_ctx;

/*
 * The reduce logic runs during stepping, after the build function has
 * returned, so it owns a heap ctx with the user logic and context.  The
 * user context lifetime is the caller's; this ctx only holds a copy of
 * the function pointer and the user pointer.
 */
typedef struct logic_ctx {
	dtfl_algo_sequence_fn logic;
	void *user;
} logic_ctx;

/*
 * reduce logic: run the user logic over the accumulated neighbour values
 * for one node, emit Some(result) with diff 1.  The input values are
 * bare u64 (the message side), sorted by value.
 */
static void
run_logic(void *vctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	logic_ctx *c = vctx;
	unsigned char buf[9];
	uint64_t result = 0;
	int64_t one = 1;

	(void)klen;
	c->logic(c->user, key, n, vals, vlens, diffs, &result);
	buf[0] = 1;			/* Some tag */
	dtfl_put_u64(buf + 1, result);
	dtfl_reduce_emit_pair(out, buf, 9, &one);
}

static logic_ctx *
seq_logic_ctx_new(const seq_ctx *c)
{
	logic_ctx *lc = __dtfl_alloc(sizeof *lc);

	lc->logic = c->logic;
	lc->user = c->user;
	return lc;
}

static dtfl_collection *
seq_logic(void *vctx, dtfl_scope *inner, dtfl_collection *new_state)
{
	seq_ctx *c = vctx;
	dtfl_diff *i64 = dtfl_diff_i64();
	const dtfl_type *optv = dtfl_type_option(dtfl_type_u64());
	dtfl_collection *edges = dtfl_collection_enter(c->edges, inner);
	dtfl_collection *old_state_raw = dtfl_collection_enter(c->state, inner);
	/* old state carries bare values; map to option(Some) via send path. */
	dtfl_collection *forward = dtfl_collection_filter(edges, is_forward,
	    DTFL_NOCTX);
	dtfl_collection *reverse = dtfl_collection_filter(edges, is_reverse,
	    DTFL_NOCTX);
	dtfl_collection *new_msgs = dtfl_collection_join_map(new_state, forward,
	    dtfl_type_u64(), optv, dtfl_type_u64(), c->no, send_state,
	    DTFL_NOCTX);
	dtfl_collection *none_msgs = dtfl_collection_filter(new_msgs,
	    state_is_none, DTFL_NOCTX);
	dtfl_collection *inc_raw = dtfl_collection_map(none_msgs, dtfl_type_u64(),
	    project_node, DTFL_NOCTX);
	dtfl_collection *incomplete = dtfl_collection_distinct(inc_raw, i64);
	dtfl_collection *some_msgs = dtfl_collection_filter(new_msgs,
	    state_is_some, DTFL_NOCTX);
	dtfl_collection *new_vals = dtfl_collection_map(some_msgs, c->nv,
	    unwrap_some, DTFL_NOCTX);
	/* old_state is (node, u64); send it (via Some) along reverse edges.
	 * We map the bare value to Some(value) so both sides agree, then
	 * send along reverse edges and unwrap. */
	dtfl_collection *old_vals = dtfl_collection_join_map(old_state_raw,
	    reverse, dtfl_type_u64(), dtfl_type_u64(), dtfl_type_u64(), c->nv,
	    send_bare_value, DTFL_NOCTX);
	dtfl_collection *messages = dtfl_collection_concat(new_vals, old_vals);
	dtfl_collection *survivors = dtfl_collection_antijoin(messages,
	    incomplete, dtfl_type_u64(), dtfl_type_u64(), c->nv);
	dtfl_collection *reduced = dtfl_collection_reduce(survivors,
	    dtfl_type_u64(), dtfl_type_u64(), optv, i64, run_logic,
	    DTFL_CTX(seq_logic_ctx_new(c), __dtfl_free));
	dtfl_collection *none_out = dtfl_collection_map(incomplete, c->no,
	    node_to_none, DTFL_NOCTX);
	dtfl_collection *result = dtfl_collection_concat(reduced, none_out);

	dtfl_collection_free(none_out);
	dtfl_collection_free(reduced);
	dtfl_collection_free(survivors);
	dtfl_collection_free(messages);
	dtfl_collection_free(old_vals);
	dtfl_collection_free(new_vals);
	dtfl_collection_free(some_msgs);
	dtfl_collection_free(incomplete);
	dtfl_collection_free(inc_raw);
	dtfl_collection_free(none_msgs);
	dtfl_collection_free(new_msgs);
	dtfl_collection_free(reverse);
	dtfl_collection_free(forward);
	dtfl_collection_free(old_state_raw);
	dtfl_collection_free(edges);
	dtfl_type_free(optv);
	dtfl_diff_free(i64);
	return result;
}

dtfl_collection *
dtfl_algo_sequence(dtfl_collection *state, dtfl_collection *edges,
    dtfl_algo_sequence_fn logic, dtfl_ctx ctx)
{
	const dtfl_type *nv = node_val_type();
	const dtfl_type *no = node_opt_type();
	seq_ctx c = { state, edges, logic, ctx.ptr, nv, no };
	dtfl_collection *seed = dtfl_collection_map(state, no, seed_none,
	    DTFL_NOCTX);
	dtfl_collection *result = dtfl_collection_iterate(seed, seq_logic,
	    DTFL_CTX(&c, NULL));

	/* The user context must outlive the dataflow: run_logic reads it at
	 * step time.  This function does not take ownership of ctx.ptr. */
	dtfl_collection_free(seed);
	dtfl_type_free(no);
	dtfl_type_free(nv);
	return result;
}
