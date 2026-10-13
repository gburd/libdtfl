/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/explain/mod.rs, interactive/src/explain/decouple.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/explain_int.h: the explanation rewrite internals.
 *
 * explain(p, shapes) transforms a DDIR program into one whose execution
 * produces per-source demand-set explanations for queries against p's
 * first export.  clone_identity(p) copies a program (used by the identity
 * tests).  Both return a fresh program the caller frees with
 * dd_program_free.
 *
 * The one data model is Val: a demand row is (K ; Tuple([V..., chain...,
 * q])) -- a flat value tuple of V's fields, then the loop-iteration chain
 * (innermost-first), then the trailing query id.  The reverse rules
 * (explain_decouple.c) are written against the Sb scope builder and the
 * Val projection/predicate builders directly; upstream's RowModel/Dataflow
 * traits had a second (test-only) data model, which does not port.
 */
#ifndef DTFL_DDIR_EXPLAIN_INT_H
#define DTFL_DDIR_EXPLAIN_INT_H

#include "ddir_int.h"

/* ------------------------------------------------------------------ */
/* Addr: a value-producing site in an original tree                    */
/* ------------------------------------------------------------------ */

typedef enum dd_site_kind {
	DD_SITE_OP,
	DD_SITE_VAR
} dd_site_kind;

/* A site: path of Sub item indices from the root, then the site within. */
typedef struct dd_addr {
	size_t *path;	/* owned */
	size_t path_len;
	dd_site_kind site_kind;
	size_t site;
} dd_addr;

dd_addr dd_addr_make(const size_t *path, size_t path_len, dd_site_kind kind,
    size_t site);
dd_addr dd_addr_clone(const dd_addr *a);
void dd_addr_free_fields(dd_addr *a);
int dd_addr_cmp(const dd_addr *a, const dd_addr *b);

/* ------------------------------------------------------------------ */
/* An Addr -> dd_ref map (sorted; op counts are small)                 */
/* ------------------------------------------------------------------ */

typedef struct dd_addr_entry {
	dd_addr key;
	dd_ref val;
} dd_addr_entry;

typedef struct dd_addr_map {
	dd_addr_entry *data;
	size_t len, cap;
} dd_addr_map;

void dd_addr_map_free(dd_addr_map *m);
/* Insert or overwrite; takes ownership of key's path (clones nothing). */
void dd_addr_map_set(dd_addr_map *m, dd_addr key, dd_ref val);
/* Borrowed lookup, NULL if absent. */
const dd_ref *dd_addr_map_get(const dd_addr_map *m, const dd_addr *key);

/* ------------------------------------------------------------------ */
/* Sb: a minimal scope-under-construction builder                      */
/* ------------------------------------------------------------------ */

typedef struct dd_sb {
	dd_scope s;	/* the scope being built (owns everything) */
	bool debug;	/* insert Inspect taps on demand collections */
	/* Boxed dd_ref handles for the dd_df backend; freed by dd_sb_free_boxes. */
	dd_ref **boxes;
	size_t n_boxes, cap_boxes;
} dd_sb;

void dd_sb_init(dd_sb *b, const char *name);
void dd_sb_free_boxes(dd_sb *b);
dd_ref dd_sb_op(dd_sb *b, dd_node *n);		   /* push an Op, return Local */
dd_ref dd_sb_linear(dd_sb *b, dd_ref input, dd_vec ops); /* owns ops elems */
dd_ref dd_sb_project(dd_sb *b, dd_ref input, dd_projection p);
dd_ref dd_sb_filter(dd_sb *b, dd_ref input, dd_term *cond);
dd_ref dd_sb_concat(dd_sb *b, dd_ref *refs, size_t n);
dd_ref dd_sb_join(dd_sb *b, dd_ref l, dd_ref r, dd_projection p);
dd_ref dd_sb_reduce(dd_sb *b, dd_ref input, dd_reducer reducer);
dd_ref dd_sb_variable(dd_sb *b, char *name);		  /* owns name */
void dd_sb_bind(dd_sb *b, dd_ref var, dd_ref value);
dd_ref dd_sb_import(dd_sb *b, char *name, dd_source from); /* owns name */
size_t dd_sb_export(dd_sb *b, char *name, dd_ref value);   /* owns name */
void dd_sb_debug_inspect(dd_sb *b, dd_ref input, char *label); /* owns label */

/* ------------------------------------------------------------------ */
/* clone_into / clone_identity (explain/mod.rs)                        */
/* ------------------------------------------------------------------ */

/*
 * Clone `orig`'s contents into `out`, splicing (no extra scope).
 * import_map[k] is the ref in `out` for orig's import k.  Writes the
 * host-visible ref at the out level for every subtree site into `visible`
 * (caller frees with dd_addr_map_free).
 */
void dd_clone_into(const dd_scope *orig, dd_scope *out, const dd_ref *import_map,
    size_t n_imports, dd_addr_map *visible);

dtfl_ddir_program *dd_clone_identity(const dtfl_ddir_program *p);

/* Total operator (Op) count across all scopes (scope_ir.rs op_count). */
size_t dd_op_count(const dtfl_ddir_program *p);

/* The explanation transform (explain/mod.rs). */
dtfl_ddir_program *dd_explain(const dtfl_ddir_program *p,
    const size_t (*source_shapes)[2], size_t n_shapes);

/* ------------------------------------------------------------------ */
/* Shapes and resolution (explain/mod.rs)                              */
/* ------------------------------------------------------------------ */

/* A (k, v) column-count shape. */
typedef struct dd_shape {
	size_t k, v;
} dd_shape;

/* A resolved target: a site, or a root source (import k). */
typedef enum dd_target_kind {
	DD_TGT_SITE,
	DD_TGT_SOURCE
} dd_target_kind;

typedef struct dd_target {
	dd_target_kind kind;
	dd_addr site;	/* DD_TGT_SITE: owned */
	size_t source;	/* DD_TGT_SOURCE */
} dd_target;

void dd_target_free_fields(dd_target *t);
int dd_target_cmp(const dd_target *a, const dd_target *b);

/* ------------------------------------------------------------------ */
/* Val row-model term builders (explain/mod.rs Val)                    */
/* ------------------------------------------------------------------ */

/* Field selectors over the demand envelope's slots. */
dd_term *dd_val_var(size_t slot);		 /* $slot */
dd_term *dd_val_f(size_t slot, size_t i);	 /* $slot[i] */
dd_term *dd_val_tup(dd_term **xs, size_t n);	 /* tuple(xs); copies ptrs */
/* A vector of f(slot, lo..hi). */
dd_vec dd_val_fidx(size_t slot, size_t lo, size_t hi);

/* The RowModel projections/predicates (explain/mod.rs impl RowModel for Val).
 * Each returns a fresh dd_projection / dd_term the caller owns. */
dd_projection dd_val_pack(size_t k, size_t v, size_t chain_len, bool has_q);
dd_projection dd_val_distinct_unpack(size_t k, size_t v);
dd_projection dd_val_project_kv(size_t k, size_t v);
dd_projection dd_val_sp_reassemble(size_t k, size_t v, size_t in_len,
    size_t out_len);
dd_projection dd_val_ky_join(size_t k, size_t v_in, size_t v_out, size_t in_len,
    size_t out_len, bool min);
dd_term *dd_val_ky_data_eq(size_t v_in);	/* NULL if none */
dd_projection dd_val_ky_drop_vout(size_t k, size_t v_in, size_t v_out,
    size_t in_len, size_t out_len);
dd_projection dd_val_lossy_pair(const dd_projection *proj, size_t k_in,
    size_t v_in, size_t in_len);
dd_projection dd_val_lossy_reassemble(size_t k_in, size_t v_in, size_t v_out,
    size_t in_len, size_t out_len);
dd_projection dd_val_join_forward(const dd_projection *proj, size_t k,
    size_t v_l, size_t v_r, size_t l_len, size_t r_len);
dd_projection dd_val_join_combined(size_t k, size_t v_l, size_t v_r,
    size_t v_out, size_t l_len, size_t r_len, size_t out_len);
dd_term *dd_val_join_filter(size_t v_l, size_t v_r, size_t v_out, size_t l_len,
    size_t r_len, size_t out_len);		/* NULL if none */
dd_projection dd_val_join_split(bool left, size_t k, size_t v_l, size_t v_r,
    size_t l_len, size_t r_len, size_t out_len);
dd_term *dd_val_time_le(size_t v, size_t in_len, size_t out_len); /* NULL */
dd_projection dd_val_strip(size_t k, size_t v, size_t in_len, size_t out_len,
    size_t keep);
dd_term *dd_val_bind_filter(size_t v);		/* NULL if none */
dd_projection dd_val_bind_decrement(size_t k, size_t v, size_t user_len);

/* ------------------------------------------------------------------ */
/* Dataflow backend: the four primitive ops the reverse rules call      */
/* (decouple.rs Dataflow trait).  A handle is an opaque pointer; the     */
/* real backend is the Sb scope builder (dd_df_sb), the test backend an  */
/* in-memory (key, val) evaluator.  Projections/predicates are owned by  */
/* the callee.                                                           */
/* ------------------------------------------------------------------ */

typedef struct dd_df {
	void *ctx;
	void *(*project)(void *ctx, void *c, dd_projection p);
	void *(*filter)(void *ctx, void *c, dd_term *cond);
	void *(*join)(void *ctx, void *l, void *r, dd_projection p);
	void *(*concat)(void *ctx, void **cs, size_t n);
} dd_df;

/* A Dataflow over the Sb scope builder (handles are boxed dd_ref). */
dd_df dd_df_sb(dd_sb *b);

/* ------------------------------------------------------------------ */
/* A reverse-rule side: a target's host (witness, forward) handles      */
/* ------------------------------------------------------------------ */

typedef struct dd_side {
	void *witness;
	void *forward;
	dd_shape shape;
	size_t user_len;
} dd_side;

/* The reverse rules (explain_decouple.c).  Each builds the contrib into
 * `df` and returns the contrib handle. */
void *dd_shape_preserving_lookup(dd_df *df, void *dep, const dd_side *side,
    size_t out_depth);
void *dd_keyed_lookup(dd_df *df, void *dep, const dd_side *side,
    dd_shape output_shape, size_t out_len, bool reducer_min);
void *dd_lossy_lookup(dd_df *df, void *dep, const dd_side *side,
    dd_shape output_shape, size_t out_len, const dd_projection *proj);
void dd_join_lookup(dd_df *df, void *dep, const dd_side *left,
    const dd_side *right, dd_shape output_shape, size_t out_len,
    const dd_projection *proj, void **lc_out, void **rc_out);

#endif /* DTFL_DDIR_EXPLAIN_INT_H */
