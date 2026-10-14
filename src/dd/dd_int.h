/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/dd/dd_int.h: library-internal differential dataflow primitives.
 *
 * Contents:
 *   - diff classes (PLAN D4; difference.rs)
 *   - consolidation of update batches (consolidation.rs)
 *   - trace batch descriptions (trace/description.rs)
 *
 * Nothing here is installed or exported yet.
 */
#ifndef DTFL_DD_INT_H
#define DTFL_DD_INT_H

#include "base/batch_int.h"
#include "progress/progress_int.h"

/* ------------------------------------------------------------------ */
/* Diff classes                                                        */
/* ------------------------------------------------------------------ */

typedef enum __dtfl_diff_kind {
	DTFL_DIFF_KIND_USER = 0,
	DTFL_DIFF_KIND_SIGNED,		/* i8..i64: Abelian, with one */
	DTFL_DIFF_KIND_UNSIGNED,	/* u8..u64: Monoid, with one */
	DTFL_DIFF_KIND_WRAPPING,	/* Wrapping<i8..i64>: Abelian */
	DTFL_DIFF_KIND_PRESENT,		/* Present: Semigroup only */
	DTFL_DIFF_KIND_TUPLE		/* up to four built-ins */
} __dtfl_diff_kind;

/* Built-in classes, for __dtfl_diff_builtin. */
typedef enum __dtfl_diff_id {
	DTFL_DIFF_I8, DTFL_DIFF_I16, DTFL_DIFF_I32, DTFL_DIFF_I64,
	DTFL_DIFF_U8, DTFL_DIFF_U16, DTFL_DIFF_U32, DTFL_DIFF_U64,
	DTFL_DIFF_WRAP_I8, DTFL_DIFF_WRAP_I16, DTFL_DIFF_WRAP_I32,
	DTFL_DIFF_WRAP_I64,
	DTFL_DIFF_PRESENT,
	DTFL_DIFF_NBUILTIN
} __dtfl_diff_id;

#define DTFL_DIFF_TUPLE_MAX 4

typedef struct __dtfl_diff_class __dtfl_diff_class;

/*
 * A diff class: the algebra of one difference type.  A value is `width`
 * bytes in native byte order, unaligned (callers memcpy), so it can sit
 * in a batch's diff column.  Zero-width values may be passed as NULL.
 *
 * The capability pointers encode the Rust trait hierarchy:
 *   plus_equals, is_zero	Semigroup (always present)
 *   zero			Monoid (NULL = Semigroup only)
 *   negate			Abelian (NULL = not Abelian; implies zero)
 *   one			From<i8> as `R::from(1)` (NULL = absent)
 *
 * Limit: fixed width only; variable-width classes (vec) need an
 * arena layout in the diff column and land with their first user.
 */
struct __dtfl_diff_class {
	const char *name;
	size_t width;
	void (*plus_equals)(const __dtfl_diff_class *c, void *acc,
	    const void *x);
	bool (*is_zero)(const __dtfl_diff_class *c, const void *x);
	void (*zero)(const __dtfl_diff_class *c, void *out);
	void (*negate)(const __dtfl_diff_class *c, void *x);
	void (*one)(const __dtfl_diff_class *c, void *out);
	/* Rust's Ord on the values. */
	int (*cmp)(const __dtfl_diff_class *c, const void *a, const void *b);
	/*
	 * Record descriptor for when a diff becomes a record value (D1).
	 * Tuple and user classes own it; built-ins leave it NULL because
	 * the built-in descriptors are not constant expressions.  Read it
	 * through __dtfl_diff_type().  Values are native-endian here, so a
	 * builder of such records converts with dtfl_put_*.
	 */
	const dtfl_type *type;
	void *ctx;			/* user classes: anything */
	void (*ctx_free)(void *ctx);	/* run by __dtfl_diff_class_free */
	/* Set by the library; zero in user definitions. */
	__dtfl_diff_kind kind;
	size_t nfields;			/* tuples */
	const __dtfl_diff_class *field[DTFL_DIFF_TUPLE_MAX];
	size_t off[DTFL_DIFF_TUPLE_MAX];
};

/* A static built-in class; never freed. */
const __dtfl_diff_class *__dtfl_diff_builtin(__dtfl_diff_id id);

/*
 * A tuple of n (1..4) built-in classes, laid out back to back.  Its
 * algebra is the weakest of its components; tuples never have `one`.
 * The caller frees it with __dtfl_diff_class_free.
 */
const __dtfl_diff_class *__dtfl_diff_tuple(const __dtfl_diff_class *const *f,
    size_t n);

/*
 * A user class: copies *def (which must set plus_equals, is_zero and
 * cmp, and has zero for negate) and retains def->type if set.  The
 * caller frees it with __dtfl_diff_class_free.
 */
const __dtfl_diff_class *__dtfl_diff_class_new(const __dtfl_diff_class *def);

/* Frees a tuple or user class (ctx_free, type).  No-op for built-ins. */
void __dtfl_diff_class_free(const __dtfl_diff_class *c);

/* The class's record descriptor (static for built-ins, never NULL). */
const dtfl_type *__dtfl_diff_type(const __dtfl_diff_class *c);

/*
 * Multiply (difference.rs Multiply), for the pairs upstream implements:
 *   integer * the same integer class -> that class (wrapping product);
 *   present * T -> T (out = y);
 *   tuple * integer -> tuple, when every component is that integer.
 * __dtfl_diff_multiply_class returns the output class or NULL.
 * __dtfl_diff_multiply writes `out` (width of the output class) and
 * *out_cls, or returns DTFL_EALGEBRA for an unsupported pair.
 *
 * Limit: a tuple with a present component would need a new output
 * class (present * T -> T per field); add it when a join needs one.
 */
const __dtfl_diff_class *__dtfl_diff_multiply_class(const __dtfl_diff_class *a,
    const __dtfl_diff_class *b);
int __dtfl_diff_multiply(const __dtfl_diff_class *a, const __dtfl_diff_class *b,
    const void *x, const void *y, void *out, const __dtfl_diff_class **out_cls);

/* ------------------------------------------------------------------ */
/* Vec<R> difference algebra (difference.rs vector)                    */
/* ------------------------------------------------------------------ */

/*
 * The Semigroup/Monoid/Abelian/Multiply impls for Vec<R> from
 * difference.rs.  A Vec<R> value is a count and `count` elements of a
 * fixed-width element class `elem` laid out back to back; the vector
 * itself is variable width, so it cannot yet sit in the fixed-width
 * batch diff column (that needs an arena layout; see the __dtfl_diff_class
 * `Limit` note).  These operate on caller-held element arrays so the
 * algebra is usable and testable on its own.
 *
 * __dtfl_diff_vec_is_zero: every element is zero (an empty vector is
 * zero).
 *
 * __dtfl_diff_vec_plus_equals: add rhs into acc element-wise over the
 * overlap, then append rhs's leftover elements.  acc has room for at
 * least max(acc_len, rhs_len) elements; *acc_len is updated.
 *
 * __dtfl_diff_vec_negate: negate every element in place (requires an
 * Abelian element class).
 *
 * __dtfl_diff_vec_multiply: multiply every element of `vec` by the
 * scalar value `rhs` of class `scalar`, writing the products to `out`
 * (which has room for `len` output-class elements).  Returns the output
 * element class, or NULL if the element class has no Multiply with
 * `scalar`.
 */
bool __dtfl_diff_vec_is_zero(const __dtfl_diff_class *elem, const void *acc,
    size_t len);
void __dtfl_diff_vec_plus_equals(const __dtfl_diff_class *elem, void *acc,
    size_t *acc_len, const void *rhs, size_t rhs_len);
void __dtfl_diff_vec_negate(const __dtfl_diff_class *elem, void *acc,
    size_t len);
const __dtfl_diff_class *__dtfl_diff_vec_multiply(const __dtfl_diff_class *elem,
    const __dtfl_diff_class *scalar, const void *vec, size_t len,
    const void *rhs, void *out);

/* ------------------------------------------------------------------ */
/* Consolidation                                                       */
/* ------------------------------------------------------------------ */

/*
 * consolidate / consolidate_updates: sorts b by (record under rt, then
 * time words), sums the diffs of equal (record, time) runs and drops
 * runs that sum to zero.  b->tw == 0 is consolidate over (record, diff).
 * b must be uniquely held and dc->width == b->dw.  Afterwards b is
 * sorted and its (record, time) pairs are distinct with non-zero diffs.
 */
void __dtfl_consolidate_batch(__dtfl_batch *b, const dtfl_type *rt,
    const __dtfl_diff_class *dc);

/*
 * ConsolidatingContainerBuilder: buffers up to twice the preferred
 * capacity, consolidates, and queues full batches of `cap` records.
 * rt and dc are not retained; the caller keeps them alive.
 */
typedef struct __dtfl_consolidator {
	const dtfl_type *rt;
	const __dtfl_diff_class *dc;
	__dtfl_batch *current;	/* owned; uniquely held */
	__dtfl_vec outbound;	/* __dtfl_batch *, owned, FIFO */
	size_t cap;		/* records per emitted batch */
} __dtfl_consolidator;

void __dtfl_consolidator_init(__dtfl_consolidator *c, const dtfl_type *rt,
    unsigned tw, const __dtfl_diff_class *dc);
void __dtfl_consolidator_push(__dtfl_consolidator *c, const void *rec,
    size_t len, const uint64_t *time, const void *diff);
/* A completed batch, owned by the caller, or NULL. */
__dtfl_batch *__dtfl_consolidator_extract(__dtfl_consolidator *c);
/* Flushes everything buffered, then extract(). */
__dtfl_batch *__dtfl_consolidator_finish(__dtfl_consolidator *c);
void __dtfl_consolidator_free(__dtfl_consolidator *c);

/* ------------------------------------------------------------------ */
/* Description                                                         */
/* ------------------------------------------------------------------ */

/*
 * The times a trace batch covers: updates at times in advance of
 * `lower` and not in advance of `upper`, compacted to `since`.  The
 * description owns its three antichains.
 */
typedef struct __dtfl_description {
	__dtfl_antichain lower;
	__dtfl_antichain upper;
	__dtfl_antichain since;
} __dtfl_description;

/*
 * Description::new: takes ownership of the three antichains (the caller
 * must not free them).  Panics if lower is empty.
 */
void __dtfl_description_init(__dtfl_description *d, __dtfl_antichain *lower,
    __dtfl_antichain *upper, __dtfl_antichain *since);
void __dtfl_description_free(__dtfl_description *d);
const __dtfl_antichain *__dtfl_description_lower(const __dtfl_description *d);
const __dtfl_antichain *__dtfl_description_upper(const __dtfl_description *d);
const __dtfl_antichain *__dtfl_description_since(const __dtfl_description *d);
bool __dtfl_description_eq(const __dtfl_description *a,
    const __dtfl_description *b);

#endif /* DTFL_DD_INT_H */
