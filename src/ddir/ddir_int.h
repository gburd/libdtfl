/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/ir.rs, interactive/src/parse/mod.rs,
 *   interactive/src/scope_ir.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/ddir_int.h: the DDIR internal data model.
 *
 * The IR node types (the scalar Term, the Value data model, the LinearOp
 * steps), the AST the parsers produce (Expr/Stmt), and the scope-tree IR
 * lowering targets (Ref/Source/Import/Export/Var/Bind/Node/Item/Scope/
 * Program).  These are the C translations of the Rust enums; the derived
 * Ord on Value is implemented by dtfl_ddir_value_cmp (ir.c).
 */
#ifndef DTFL_DDIR_INT_H
#define DTFL_DDIR_INT_H

#include "dtfl/ddir.h"

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* A small growable pointer vector, used throughout the IR.            */
/* ------------------------------------------------------------------ */

typedef struct dd_vec {
	void **data;
	size_t len;
	size_t cap;
} dd_vec;

void dd_vec_push(dd_vec *v, void *p);
void dd_vec_free(dd_vec *v); /* frees the backing array only, not elements */

/* ------------------------------------------------------------------ */
/* Scalar language: Term (ir.rs / parse/mod.rs)                        */
/* ------------------------------------------------------------------ */

typedef enum dd_unop {
	DD_UN_NEG,
	DD_UN_NOT,
	DD_UN_ISTAG, /* carries a tag in u.istag */
	DD_UN_LEN
} dd_unop;

typedef enum dd_binop {
	DD_BIN_ADD,
	DD_BIN_SUB,
	DD_BIN_MUL,
	DD_BIN_EQ,
	DD_BIN_NE,
	DD_BIN_LT,
	DD_BIN_LE,
	DD_BIN_GT,
	DD_BIN_GE,
	DD_BIN_AND,
	DD_BIN_OR
} dd_binop;

typedef enum dd_term_kind {
	DD_T_VAR,
	DD_T_BOUND,
	DD_T_INT,
	DD_T_TUPLE,
	DD_T_LIST,
	DD_T_SPREAD,
	DD_T_PROJ,
	DD_T_INJECT,
	DD_T_CASE,
	DD_T_FOLD,
	DD_T_IF,
	DD_T_UNARY,
	DD_T_BINARY,
	DD_T_HASH
} dd_term_kind;

typedef struct dd_term dd_term;
struct dd_term {
	dd_term_kind kind;
	union {
		size_t var;	 /* Var: absolute env index */
		size_t bound;	 /* Bound: de Bruijn (0 = innermost) */
		int64_t int_lit; /* Int literal */
		dd_vec seq;	 /* Tuple/List: vec of dd_term* */
		dd_term *spread; /* Spread: inner term */
		struct {
			dd_term *base;
			size_t index;
		} proj;		 /* Proj(term, index) */
		struct {
			dd_term *tag;
			dd_term *payload;
		} inject;	 /* Inject(tag, payload) */
		struct {
			dd_term *scrutinee;
			dd_vec arms;	   /* vec of dd_term* */
			dd_term *dflt;	   /* NULL if none */
		} case_;
		struct {
			dd_term *list;
			dd_term *init;
			dd_term *step;
		} fold;
		struct {
			dd_term *cond;
			dd_term *then_;
			dd_term *els;
		} if_;
		struct {
			dd_unop op;
			uint32_t tag; /* for DD_UN_ISTAG */
			dd_term *arg;
		} unary;
		struct {
			dd_binop op;
			dd_term *l;
			dd_term *r;
		} binary;
		dd_vec hash;	 /* Hash: vec of dd_term* (args[0] = bound) */
	} u;
};

dd_term *dd_term_new(dd_term_kind kind);
dd_term *dd_term_clone(const dd_term *t);
void dd_term_free(dd_term *t);

/* ------------------------------------------------------------------ */
/* Projection and Reducer (parse/mod.rs)                               */
/* ------------------------------------------------------------------ */

typedef struct dd_projection {
	dd_term *key;
	dd_term *val;
} dd_projection;

dd_projection dd_projection_clone(const dd_projection *p);
void dd_projection_free_fields(dd_projection *p);

typedef enum dd_reducer {
	DD_RED_MIN,
	DD_RED_DISTINCT,
	DD_RED_COUNT,
	DD_RED_COLLECT
} dd_reducer;

/* ------------------------------------------------------------------ */
/* LinearOp (ir.rs)                                                    */
/* ------------------------------------------------------------------ */

typedef enum dd_linear_kind {
	DD_LIN_PROJECT, /* carries dd_projection */
	DD_LIN_FILTER,	/* carries dd_term* */
	DD_LIN_NEGATE,
	DD_LIN_ENTER_AT, /* carries dd_term* */
	DD_LIN_LIFT_ITER,
	DD_LIN_FLATMAP	 /* carries dd_term* */
} dd_linear_kind;

typedef struct dd_linear_op {
	dd_linear_kind kind;
	dd_projection proj; /* DD_LIN_PROJECT */
	dd_term *term;	    /* DD_LIN_FILTER / ENTER_AT / FLATMAP */
} dd_linear_op;

dd_linear_op dd_linear_op_clone(const dd_linear_op *op);
void dd_linear_op_free_fields(dd_linear_op *op);

/* ------------------------------------------------------------------ */
/* Collection language AST: Expr / Stmt (parse/mod.rs)                 */
/* ------------------------------------------------------------------ */

typedef enum dd_expr_kind {
	DD_E_INPUT,	/* u.input */
	DD_E_IMPORT,	/* u.name */
	DD_E_NAME,	/* u.name */
	DD_E_QUALIFIED, /* u.qualified */
	DD_E_MAP,	/* u.map */
	DD_E_JOIN,	/* u.join */
	DD_E_REDUCE,	/* u.reduce */
	DD_E_FILTER,	/* u.op_term */
	DD_E_NEGATE,	/* u.op */
	DD_E_ENTER_AT,	/* u.op_term */
	DD_E_LIFT_ITER, /* u.op */
	DD_E_FLATMAP,	/* u.op_term */
	DD_E_INSPECT,	/* u.inspect */
	DD_E_CONCAT,	/* u.concat */
	DD_E_ARRANGE	/* u.op */
} dd_expr_kind;

typedef struct dd_expr dd_expr;
struct dd_expr {
	dd_expr_kind kind;
	union {
		size_t input;
		char *name; /* Import or Name (owned) */
		struct {
			char *scope;
			char *field;
		} qualified;
		struct {
			dd_expr *e;
			dd_projection proj;
		} map;
		struct {
			dd_expr *l;
			dd_expr *r;
			dd_projection proj;
		} join;
		struct {
			dd_expr *e;
			dd_reducer reducer;
		} reduce;
		struct {
			dd_expr *e;
			dd_term *term;
		} op_term;	/* Filter / EnterAt / FlatMap */
		struct {
			dd_expr *e;
		} op;		/* Negate / LiftIter / Arrange */
		struct {
			dd_expr *e;
			char *label;
		} inspect;
		dd_vec concat;	/* vec of dd_expr* */
	} u;
};

dd_expr *dd_expr_new(dd_expr_kind kind);
void dd_expr_free(dd_expr *e);

typedef enum dd_stmt_kind {
	DD_S_LET,
	DD_S_VAR,
	DD_S_SCOPE,
	DD_S_EXPORT
} dd_stmt_kind;

typedef struct dd_stmt {
	dd_stmt_kind kind;
	char *name;	/* Let/Var/Scope/Export name (owned) */
	dd_expr *expr;	/* Let/Var/Export */
	dd_vec body;	/* Scope: vec of dd_stmt* */
} dd_stmt;

dd_stmt *dd_stmt_new(dd_stmt_kind kind);
void dd_stmt_free(dd_stmt *s);
void dd_stmts_free(dd_vec *stmts); /* frees each dd_stmt* and the vec */

/* ------------------------------------------------------------------ */
/* Scope-tree IR (scope_ir.rs)                                         */
/* ------------------------------------------------------------------ */

typedef enum dd_ref_kind {
	DD_REF_LOCAL,	    /* u.item */
	DD_REF_IMPORT,	    /* u.import */
	DD_REF_VAR,	    /* u.var */
	DD_REF_CHILD_EXPORT /* u.child_export */
} dd_ref_kind;

typedef struct dd_ref {
	dd_ref_kind kind;
	union {
		size_t item;
		size_t import;
		size_t var;
		struct {
			size_t item;
			size_t field;
		} child_export;
	} u;
} dd_ref;

typedef enum dd_source_kind {
	DD_SRC_PARENT, /* u.parent (a dd_ref) */
	DD_SRC_INPUT,  /* u.input */
	DD_SRC_TRACE   /* u.trace (owned name) */
} dd_source_kind;

typedef struct dd_source {
	dd_source_kind kind;
	union {
		dd_ref parent;
		size_t input;
		char *trace;
	} u;
} dd_source;

typedef struct dd_import {
	char *name;
	dd_source from;
} dd_import;

typedef struct dd_export {
	char *name;
	dd_ref value;
} dd_export;

typedef struct dd_var {
	char *name;
} dd_var;

typedef struct dd_bind {
	size_t var;
	dd_ref value;
} dd_bind;

typedef enum dd_node_kind {
	DD_N_LINEAR,  /* input ref + ops vec of dd_linear_op* */
	DD_N_CONCAT,  /* refs vec of dd_ref* */
	DD_N_ARRANGE, /* single ref */
	DD_N_JOIN,    /* left, right, projection */
	DD_N_REDUCE,  /* input ref + reducer */
	DD_N_INSPECT  /* input ref + label */
} dd_node_kind;

typedef struct dd_node {
	dd_node_kind kind;
	dd_ref input;	 /* Linear/Arrange/Reduce/Inspect primary input */
	dd_vec ops;	 /* Linear: vec of dd_linear_op* */
	dd_vec refs;	 /* Concat: vec of dd_ref* */
	dd_ref right;	 /* Join: right (input holds left) */
	dd_projection projection; /* Join */
	dd_reducer reducer;	  /* Reduce */
	char *label;		  /* Inspect */
} dd_node;

typedef enum dd_item_kind {
	DD_I_OP,
	DD_I_SUB
} dd_item_kind;

typedef struct dd_scope dd_scope;

typedef struct dd_item {
	dd_item_kind kind;
	dd_node *op;	/* DD_I_OP */
	dd_scope *sub;	/* DD_I_SUB */
} dd_item;

struct dd_scope {
	char *name;
	dd_vec imports; /* vec of dd_import* */
	dd_vec vars;	/* vec of dd_var* */
	dd_vec items;	/* vec of dd_item* */
	dd_vec binds;	/* vec of dd_bind* */
	dd_vec exports; /* vec of dd_export* */
};

struct dtfl_ddir_program {
	dd_scope root;
};

dd_scope *dd_scope_clone(const dd_scope *s);
void dd_scope_free_fields(dd_scope *s);
void dd_scope_free(dd_scope *s);

/* optimize (scope_ir.rs Scope::optimize) */
void dd_scope_optimize(dd_scope *s);
void dd_program_optimize(dtfl_ddir_program *p);

/* Lower a parsed AST (vec of dd_stmt*) into a Program (lower.rs). */
dtfl_ddir_program *dd_lower_tree(const dd_vec *stmts);
/* Free a program and its scope tree. */
void dd_program_free(dtfl_ddir_program *p);

/* ------------------------------------------------------------------ */
/* Value data model and the Term interpreter (ir.rs)                   */
/* ------------------------------------------------------------------ */

typedef enum dd_value_kind {
	DD_V_INT,
	DD_V_TUPLE,
	DD_V_VARIANT,
	DD_V_LIST
} dd_value_kind;

typedef struct dd_value dd_value;
struct dd_value {
	dd_value_kind kind;
	union {
		int64_t int_v;
		struct {
			dd_value **items; /* owned array of owned values */
			size_t len;
		} seq;		/* Tuple / List */
		struct {
			uint32_t tag;
			dd_value *payload;
		} variant;
	} u;
};

dd_value *dd_value_int(int64_t n);
dd_value *dd_value_unit(void); /* empty tuple */
dd_value *dd_value_tuple(dd_value **items, size_t len); /* takes ownership */
dd_value *dd_value_list(dd_value **items, size_t len);	/* takes ownership */
dd_value *dd_value_variant(uint32_t tag, dd_value *payload);
dd_value *dd_value_clone(const dd_value *v);
void dd_value_free(dd_value *v);
int dd_value_cmp(const dd_value *a, const dd_value *b); /* derived Ord */
bool dd_value_truthy(const dd_value *v);
int64_t dd_value_as_int(const dd_value *v); /* panics if not Int */

/* splitmix64 helpers (lib.rs hash_u64). */
uint64_t dd_hash_u64(uint64_t index);

/*
 * Evaluate a scalar Term against the environment stack `env` (an array of
 * owned dd_value* with `len` entries, capacity `cap`).  Returns a freshly
 * owned dd_value.  env is restored on return (Case/Fold push/pop).
 */
dd_value *dd_eval(const dd_term *t, dd_value ***env, size_t *len, size_t *cap);

/* ------------------------------------------------------------------ */
/* Value serialization: the on-wire record form (backend_vec.c).       */
/* ------------------------------------------------------------------ */

/*
 * Serialize a Value to a self-describing byte string, appended to *buf
 * (grown as needed; *cap tracks capacity, *len the used length).  The
 * encoding sorts, under dtfl_type_cmp of the ddir value type, in the same
 * order as the derived Ord on Value.
 */
void dd_value_encode(const dd_value *v, uint8_t **buf, size_t *len,
    size_t *cap);
/* Decode one Value from `p` (at least `avail` bytes); advances *p. */
dd_value *dd_value_decode(const uint8_t **p, const uint8_t *end);
/* The dtfl_type for a serialized Value (structural Ord). */
const dtfl_type *dd_value_type(void);
/* The dtfl_type for a (key, val) pair of serialized Values. */
const dtfl_type *dd_pair_type(void);

#endif /* DTFL_DDIR_INT_H */
