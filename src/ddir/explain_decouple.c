/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/explain/decouple.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/explain_decouple.c: the reverse-tracing rules.
 *
 * Each rule reverses one forward operator: given the demand on the
 * operator's output (`dep`, a (K ; V, chain, q) envelope) and the host
 * (witness, forward) collections of the input `side`, it builds the
 * demand contributed to the input.  The rules are the bodies of
 * decouple.rs, written once over the dd_df Dataflow backend (project /
 * filter / join / concat); the Val row model (the dd_val_* projection and
 * predicate builders) is the one data model.  The real backend is the Sb
 * scope builder; the explain_decouple tests drive the same rules against an
 * in-memory evaluator.
 *
 * The demand envelope and the per-rule input/output shapes are documented
 * in decouple.rs; the dd_val_* contracts in explain_int.h restate them.
 */
#include "explain_int.h"

#include "../base/base_int.h"

/* The four Dataflow primitives, dispatched through the backend vtable. */
static void *
df_project(dd_df *df, void *c, dd_projection p)
{
	return df->project(df->ctx, c, p);
}

static void *
df_filter(dd_df *df, void *c, dd_term *cond)
{
	return df->filter(df->ctx, c, cond);
}

static void *
df_join(dd_df *df, void *l, void *r, dd_projection p)
{
	return df->join(df->ctx, l, r, p);
}

static void *
df_concat(dd_df *df, void **cs, size_t n)
{
	return df->concat(df->ctx, cs, n);
}

/* opt_filter: apply `cond` to `c`, or pass it through when cond is NULL. */
static void *
opt_filter(dd_df *df, void *c, dd_term *cond)
{
	if (cond == NULL)
		return c;
	return df_filter(df, c, cond);
}

/* The pair table of a side: witness concat forward. */
static void *
side_pair(dd_df *df, const dd_side *side)
{
	void *refs[2];

	refs[0] = side->witness;
	refs[1] = side->forward;
	return df_concat(df, refs, 2);
}

/* ------------------------------------------------------------------ */
/* Shape-preserving lookup (Concat / Leave).  keep == in_len.          */
/* ------------------------------------------------------------------ */

void *
dd_shape_preserving_lookup(dd_df *df, void *dep, const dd_side *side,
    size_t out_depth)
{
	size_t k = side->shape.k, v = side->shape.v, in_len = side->user_len;
	void *pair = side_pair(df, side);
	void *pair_keyed = df_project(df, pair, dd_val_pack(k, v, in_len, false));
	void *dep_keyed = df_project(df, dep, dd_val_pack(k, v, out_depth, true));
	void *joined = df_join(df, dep_keyed, pair_keyed,
	    dd_val_sp_reassemble(k, v, in_len, out_depth));
	void *filtered = opt_filter(df, joined,
	    dd_val_time_le(v, in_len, out_depth));

	return df_project(df, filtered,
	    dd_val_strip(k, v, in_len, out_depth, in_len));
}

/* ------------------------------------------------------------------ */
/* Keyed (Reduce) lookup; `min` enables value-narrowing.               */
/* ------------------------------------------------------------------ */

void *
dd_keyed_lookup(dd_df *df, void *dep, const dd_side *side,
    dd_shape output_shape, size_t out_len, bool reducer_min)
{
	size_t k = side->shape.k, v_in = side->shape.v, in_len = side->user_len;
	size_t v_out = output_shape.v;
	bool min = reducer_min && v_in == v_out && v_in > 0;
	void *pair = side_pair(df, side);
	void *joined = df_join(df, dep, pair,
	    dd_val_ky_join(k, v_in, v_out, in_len, out_len, min));
	void *after, *filtered;

	if (min) {
		void *f = opt_filter(df, joined, dd_val_ky_data_eq(v_in));

		after = df_project(df, f,
		    dd_val_ky_drop_vout(k, v_in, v_out, in_len, out_len));
	} else {
		after = joined;
	}
	filtered = opt_filter(df, after, dd_val_time_le(v_in, in_len, out_len));
	return df_project(df, filtered,
	    dd_val_strip(k, v_in, in_len, out_len, in_len));
}

/* ------------------------------------------------------------------ */
/* Lossy (Project) lookup: always the pair-table fallback (Val's        */
/* lossy_try_invert returns None, so the invert fast path never runs).  */
/* ------------------------------------------------------------------ */

void *
dd_lossy_lookup(dd_df *df, void *dep, const dd_side *side,
    dd_shape output_shape, size_t out_len, const dd_projection *proj)
{
	size_t k_in = side->shape.k, v_in = side->shape.v;
	size_t in_len = side->user_len;
	size_t v_out = output_shape.v;
	void *pair = side_pair(df, side);
	void *pair_keyed = df_project(df, pair,
	    dd_val_lossy_pair(proj, k_in, v_in, in_len));
	void *joined = df_join(df, dep, pair_keyed,
	    dd_val_lossy_reassemble(k_in, v_in, v_out, in_len, out_len));
	void *filtered = opt_filter(df, joined,
	    dd_val_time_le(v_in, in_len, out_len));

	return df_project(df, filtered,
	    dd_val_strip(k_in, v_in, in_len, out_len, in_len));
}

/* ------------------------------------------------------------------ */
/* Join lookup (#758): forward-join the pair tables, one combined       */
/* dep x pair join carrying both chains, one combined time+value        */
/* filter, then split per side.                                        */
/* ------------------------------------------------------------------ */

void
dd_join_lookup(dd_df *df, void *dep, const dd_side *left, const dd_side *right,
    dd_shape output_shape, size_t out_len, const dd_projection *proj,
    void **lc_out, void **rc_out)
{
	size_t k = left->shape.k, v_l = left->shape.v, v_r = right->shape.v;
	size_t v_out = output_shape.v;
	size_t l_len = left->user_len, r_len = right->user_len;
	void *lp = side_pair(df, left);
	void *rp = side_pair(df, right);
	void *pair = df_join(df, lp, rp,
	    dd_val_join_forward(proj, k, v_l, v_r, l_len, r_len));
	void *joined = df_join(df, dep, pair,
	    dd_val_join_combined(k, v_l, v_r, v_out, l_len, r_len, out_len));
	void *timed = opt_filter(df, joined,
	    dd_val_join_filter(v_l, v_r, v_out, l_len, r_len, out_len));

	*lc_out = df_project(df, timed,
	    dd_val_join_split(true, k, v_l, v_r, l_len, r_len, out_len));
	*rc_out = df_project(df, timed,
	    dd_val_join_split(false, k, v_l, v_r, l_len, r_len, out_len));
}
