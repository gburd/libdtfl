/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/ddir.h: the Differential Dataflow Intermediate Representation.
 *
 * DDIR is a small language for building differential dataflows at
 * runtime.  A program names positional input collections, defines derived
 * collections by expressions (map/filter, equijoin, reduce, concat, and
 * iterate for recursion), and declares named outputs.  Two surface
 * syntaxes parse to the same program: the applicative (.ddir) syntax
 * (MAP(e, proj), JOIN(e1, e2, proj), REDUCE(e, MIN)) and the pipe (.ddp)
 * syntax (expr | op | op).
 *
 * Pipeline: dtfl_ddir_parse turns program text into a lowered,
 * optimized scope-tree program (the IR); dtfl_ddir_build lowers that IR
 * into a live differential dataflow inside a scope, binding the program's
 * positional inputs to collections and surrendering its named exports;
 * dtfl_ddir_evaluate is the data-in/data-out shortcut that feeds explicit
 * input rows, runs to quiescence, and returns each export's consolidated
 * final contents.
 *
 * DDIR's time is Product<u64, PointStamp<u64>> (dtfl_time_product of a u64
 * outer and a dtfl_time_pointstamp inner), so iterate's dynamic scopes
 * push an iteration coordinate without changing the timestamp type.
 *
 * A DDIR value is an algebraic data type over a single scalar: an integer,
 * a tuple, a tagged variant, or a list.  A collection element is a
 * (key, value) pair of such values.  dtfl_ddir_value_* construct and
 * inspect values; dtfl_ddir_value_parse parses one from a closed scalar
 * term string.
 */
#ifndef DTFL_DDIR_H
#define DTFL_DDIR_H

#include "dtfl/base.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/timely.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

/*
 * A DDIR runtime value: an Int, a Tuple, a tagged Variant, or a List.
 * Reference-free: a dtfl_ddir_value is owned by its creator and released
 * with dtfl_ddir_value_free, which frees it and any children.
 */
typedef struct dd_value dtfl_ddir_value;

DTFL_API dtfl_ddir_value *dtfl_ddir_value_int(int64_t n);
DTFL_API dtfl_ddir_value *dtfl_ddir_value_unit(void); /* the empty tuple */
/* Tuple/List take ownership of `items` elements (not the array). */
DTFL_API dtfl_ddir_value *dtfl_ddir_value_tuple(dtfl_ddir_value **items,
    size_t len);
DTFL_API dtfl_ddir_value *dtfl_ddir_value_list(dtfl_ddir_value **items,
    size_t len);
DTFL_API dtfl_ddir_value *dtfl_ddir_value_variant(uint32_t tag,
    dtfl_ddir_value *payload);
DTFL_API dtfl_ddir_value *dtfl_ddir_value_clone(const dtfl_ddir_value *v);
DTFL_API void dtfl_ddir_value_free(dtfl_ddir_value *v);
/* Structural three-way compare (the derived Ord on Value). */
DTFL_API int dtfl_ddir_value_cmp(const dtfl_ddir_value *a,
    const dtfl_ddir_value *b);

/*
 * Parse a single closed scalar term (no $n input references) into a
 * constant value, e.g. "tuple(1, 2)" or "inject(0, 7)".  Returns NULL and
 * reports DTFL_EINVAL on a malformed term.
 */
DTFL_API dtfl_ddir_value *dtfl_ddir_value_parse(const char *term);

/* ------------------------------------------------------------------ */
/* Programs                                                            */
/* ------------------------------------------------------------------ */

/*
 * A parsed, lowered, optimized DDIR program: the scope-tree IR.  Owned by
 * the caller, released with dtfl_ddir_program_free.
 */
typedef struct dtfl_ddir_program dtfl_ddir_program;

/* The surface syntax of a program source. */
typedef enum dtfl_ddir_syntax {
	DTFL_DDIR_PIPE,	       /* .ddp: expr | op | op */
	DTFL_DDIR_APPLICATIVE  /* .ddir: MAP(e, proj), JOIN(...) */
} dtfl_ddir_syntax;

/*
 * Parse `program` in the given surface syntax, lower it to the scope-tree
 * IR, and optimize it.  Returns the program through `out` and 0 on
 * success; on a parse or lowering error returns a DTFL_E* code, reports a
 * message through dtfl_last_error_detail, and leaves *out NULL.
 */
DTFL_API int dtfl_ddir_parse(const char *program, dtfl_ddir_syntax syntax,
    dtfl_ddir_program **out);

DTFL_API void dtfl_ddir_program_free(dtfl_ddir_program *p);

/* The number of positional inputs the program's root scope expects. */
DTFL_API size_t dtfl_ddir_program_input_count(const dtfl_ddir_program *p);
/* The number of named exports. */
DTFL_API size_t dtfl_ddir_program_export_count(const dtfl_ddir_program *p);
/* The name of export `i` (borrowed, valid while the program lives). */
DTFL_API const char *dtfl_ddir_program_export_name(const dtfl_ddir_program *p,
    size_t i);

/* ------------------------------------------------------------------ */
/* The explanation rewrite                                             */
/* ------------------------------------------------------------------ */

/*
 * A (k, v) column-count shape: the number of key and value fields of one
 * of the program's positional inputs, which dtfl_ddir_explain needs to
 * place the loop-iteration chain in the demand envelope.
 */
typedef struct dtfl_ddir_shape {
	size_t k;
	size_t v;
} dtfl_ddir_shape;

/*
 * Rewrite `program` into one whose execution produces, for a query against
 * `program`'s first export, the per-source demand-set explanations: the
 * input rows that regenerate the queried output row.  `shapes` gives the
 * (k, v) shape of each of the program's root imports, in order, and
 * `n_shapes` must equal dtfl_ddir_program_input_count.  The rewritten
 * program takes one extra positional input after the originals (the query
 * rows, each a flat demand envelope) and exports one "demand:inputN"
 * collection per source.  Returns the rewritten program through `out` and
 * 0 on success, or a DTFL_E* code (and leaves *out NULL) on a bad
 * argument.  Free the result with dtfl_ddir_program_free.
 */
DTFL_API int dtfl_ddir_explain(const dtfl_ddir_program *program,
    const dtfl_ddir_shape *shapes, size_t n_shapes, dtfl_ddir_program **out);

/*
 * Clone `program` into an equivalent program (the identity transform the
 * explanation rewrite is built on): it computes the same named exports,
 * with each nested scope additionally exporting a host-visible lifted form
 * of its internal collections.  Returns the clone through `out` and 0, or a
 * DTFL_E* code.  Free the result with dtfl_ddir_program_free.
 */
DTFL_API int dtfl_ddir_clone_identity(const dtfl_ddir_program *program,
    dtfl_ddir_program **out);

/* ------------------------------------------------------------------ */
/* Building a program into a live dataflow                             */
/* ------------------------------------------------------------------ */

/*
 * Lower `program` into a differential dataflow inside `scope`, whose time
 * class must be the DDIR time (dtfl_ddir_time_class).  `inputs` are the
 * collections bound to the program's positional inputs, in order;
 * `n_inputs` must equal dtfl_ddir_program_input_count.  On success writes
 * one output collection per named export into `exports` (an array of at
 * least dtfl_ddir_program_export_count entries, in export order) and
 * returns 0.  The caller owns and frees the returned collections.
 */
DTFL_API int dtfl_ddir_build(const dtfl_ddir_program *program,
    dtfl_scope *scope, dtfl_collection *const *inputs, size_t n_inputs,
    dtfl_collection **exports);

/* The DDIR time class: Product<u64, PointStamp<u64>>.  Caller frees it. */
DTFL_API dtfl_time_class *dtfl_ddir_time_class(void);

/* ------------------------------------------------------------------ */
/* Data-in / data-out evaluation                                      */
/* ------------------------------------------------------------------ */

/*
 * One row in an input or output: a (key, value) pair, with a signed
 * multiplicity in `diff` for outputs (inputs are always inserted with
 * multiplicity +1).
 */
typedef struct dtfl_ddir_row {
	dtfl_ddir_value *key;
	dtfl_ddir_value *val;
	int64_t diff;
} dtfl_ddir_row;

/* An export's consolidated final contents. */
typedef struct dtfl_ddir_output {
	char *name;		 /* owned export name */
	dtfl_ddir_row *rows;	 /* owned rows (key/val owned) */
	size_t n;
} dtfl_ddir_output;

/*
 * Evaluate `program` on explicit inputs on a single in-process worker.
 * `inputs[i]` provides the rows of positional input `i` (each inserted
 * with multiplicity +1, all at time 0); the computation runs to
 * quiescence.  On success writes an array of dtfl_ddir_program_export_count
 * outputs (one per export, in order) through `outputs`, writes the count
 * through `n_outputs`, and returns 0.  Free the result with
 * dtfl_ddir_outputs_free.
 */
DTFL_API int dtfl_ddir_evaluate(const dtfl_ddir_program *program,
    const dtfl_ddir_row *const *inputs, const size_t *input_lens,
    size_t n_inputs, dtfl_ddir_output **outputs, size_t *n_outputs);

DTFL_API void dtfl_ddir_outputs_free(dtfl_ddir_output *outputs, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_DDIR_H */
