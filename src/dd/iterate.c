/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/iterate.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/iterate.c: the iterate operator and the Variable feedback.
 *
 * iterate builds an iterative subscope whose time is Product<outer, u64>:
 * one extra u64 coordinate counts the iteration.  The source collection
 * enters the subscope, a Variable is seeded from it, the user logic
 * transforms the Variable's collection into the next iterate, the Variable
 * is set to that result, and the result leaves the subscope
 * (iterate.rs Iterate::iterate).
 *
 * A Variable is a recursively defined collection.  Variable::new creates
 * an empty one whose collection is a timely feedback stream with the loop
 * step summary (the inner coordinate advanced by one); Variable::new_from
 * concatenates the source into that collection and remembers the negated
 * source.  Variable::set concatenates any remembered source, applies the
 * step summary to every update's embedded differential time (results_in,
 * dropping updates that overflow the loop coordinate), and connects the
 * loop.  The embedded time is advanced in step with the timely message
 * time the feedback operator advances, so a differential update re-enters
 * the cycle at a strictly later iteration; that is what makes the loop
 * sound and lets the fixed point terminate when no change circulates.
 */
#include "dd/collection_int.h"

#include "progress/time_int.h"
#include "timely/timely_int.h"

#include "dtfl/iterate.h"

/* ------------------------------------------------------------------ */
/* results_in: advance each update's embedded time by the step summary */
/* ------------------------------------------------------------------ */

/*
 * Collection::results_in: a unary operator that applies the path summary
 * `step` to every update's embedded differential time, dropping any update
 * whose time overflows the coordinate's width (results_in returns None).
 */
typedef struct results_in_ctx {
	const dtfl_time_class *tc;	/* retained */
	unsigned depth;
	size_t dw;
	uint64_t step[DTFL_TIME_MAX];
} results_in_ctx;

static bool
results_in_logic(dtfl_op *op, void *vctx)
{
	results_in_ctx *c = vctx;
	dtfl_message *m;
	size_t tw = (size_t)c->depth * sizeof(uint64_t);

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);
			const uint64_t *et = __dtfl_update_time(rec, c->depth,
			    c->dw, len);
			uint64_t out[DTFL_TIME_MAX];
			unsigned char *dst;

			/* results_in returns None on overflow: drop the update. */
			if (!dtfl_summary_results_in(c->tc, c->step, et, out))
				continue;
			dst = __dtfl_session_reserve(s, len);
			memcpy(dst, rec, rlen);
			memcpy(dst + rlen, out, tw);
			if (c->dw != 0)
				memcpy(dst + rlen + tw,
				    __dtfl_update_diff(rec, c->dw, len), c->dw);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

static void
results_in_free(void *vctx)
{
	results_in_ctx *c = vctx;

	dtfl_time_class_free(c->tc);
	__dtfl_free(c);
}

/* Returns a collection whose updates' embedded times are advanced by step. */
static dtfl_collection *
collection_results_in(dtfl_collection *c, const uint64_t *step)
{
	const dtfl_time_class *tc =
	    dtfl_scope_time_class(dtfl_stream_scope(c->stream));
	unsigned sw = DTFL_MAX(dtfl_summary_depth(tc), 1u);
	results_in_ctx *rc = __dtfl_alloc(sizeof *rc);
	const dtfl_type *ut;
	dtfl_op_builder *b;
	dtfl_stream *st;

	rc->tc = dtfl_time_class_retain(tc);
	rc->depth = c->depth;
	rc->dw = c->diff->cls->width;
	memcpy(rc->step, step, (size_t)sw * sizeof(uint64_t));
	ut = __dtfl_update_type(c->type, c->depth, rc->dw);
	b = dtfl_op_builder_new(dtfl_stream_scope(c->stream), "ResultsIn");
	dtfl_op_builder_input(b, c->stream, dtfl_pact_pipeline());
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, results_in_logic, rc, results_in_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, c->type, dtfl_diff_retain(c->diff),
	    c->depth);
}

/* ------------------------------------------------------------------ */
/* Variable                                                            */
/* ------------------------------------------------------------------ */

/*
 * A Variable: the feedback handle to connect the loop to, the inner scope,
 * the step summary applied by set (results_in), and the negated source to
 * concatenate before setting (new_from) or NULL (new).  type/diff/depth
 * describe the records, needed to seed the empty feedback collection.
 */
struct dtfl_variable {
	dtfl_feedback *feedback;	/* owned until set */
	dtfl_scope *scope;
	dtfl_collection *source_neg;	/* owned; concatenated by set, or NULL */
	uint64_t step[DTFL_TIME_MAX];
};

/* The loop step summary: Product::new(Default, 1), inner coordinate + 1. */
static void
loop_step(const dtfl_time_class *tc, uint64_t *step)
{
	unsigned sw = dtfl_summary_depth(tc);

	dtfl_summary_default(tc, step);
	/* The inner (last) summary coordinate advances the loop by one. */
	step[sw - 1] = 1;
}

dtfl_variable *
dtfl_variable_new(dtfl_scope *scope, const dtfl_type *type,
    const dtfl_diff *diff, dtfl_collection **collection)
{
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	dtfl_variable *v = __dtfl_alloc(sizeof *v);
	dtfl_stream *updates;
	const dtfl_type *ut;

	loop_step(tc, v->step);
	v->scope = scope;
	v->source_neg = NULL;

	/* scope.feedback(step): a feedback stream of update records. */
	ut = __dtfl_update_type(type, dtfl_time_depth(tc), diff->cls->width);
	updates = dtfl_feedback_new(scope, ut, v->step, &v->feedback);
	dtfl_type_free(ut);
	if (collection != NULL)
		*collection = __dtfl_as_collection(updates, type,
		    dtfl_diff_retain(diff), dtfl_time_depth(tc));
	else
		dtfl_collection_free(__dtfl_as_collection(updates, type,
		    dtfl_diff_retain(diff), dtfl_time_depth(tc)));
	return v;
}

dtfl_variable *
dtfl_variable_new_from(dtfl_collection *source, dtfl_collection **collection)
{
	dtfl_scope *scope = dtfl_stream_scope(source->stream);
	dtfl_variable *v = dtfl_variable_new(scope, source->type, source->diff,
	    collection);

	/* collection = feedback.concat(source); remember negate(source). */
	if (collection != NULL) {
		dtfl_collection *fed = *collection;

		*collection = dtfl_collection_concat(fed, source);
		dtfl_collection_free(fed);
	}
	v->source_neg = dtfl_collection_negate(source);
	return v;
}

void
dtfl_variable_set(dtfl_variable *v, dtfl_collection *result)
{
	dtfl_collection *stepped;

	/* if let Some(source) = source.take() { result = result.concat(source) } */
	if (v->source_neg != NULL) {
		dtfl_collection *con = dtfl_collection_concat(result,
		    v->source_neg);

		dtfl_collection_free(result);
		dtfl_collection_free(v->source_neg);
		v->source_neg = NULL;
		result = con;
	}

	/* result.results_in(step).inner.connect_loop(feedback). */
	stepped = collection_results_in(result, v->step);
	dtfl_feedback_connect(v->feedback, stepped->stream);
	dtfl_collection_free(stepped);
	dtfl_collection_free(result);
	__dtfl_free(v);
}

void
dtfl_variable_set_concat(dtfl_variable *v, dtfl_collection *result,
    dtfl_collection *addend)
{
	dtfl_collection *con = dtfl_collection_concat(result, addend);

	dtfl_collection_free(result);
	dtfl_collection_free(addend);
	dtfl_variable_set(v, con);
}

/* ------------------------------------------------------------------ */
/* iterate                                                             */
/* ------------------------------------------------------------------ */

typedef struct iterate_ctx {
	dtfl_collection *source;	/* the outer-scope source, not owned */
	dtfl_iterate_fn logic;
	void *user;
	dtfl_collection *result;	/* set by the build to the leave result */
} iterate_ctx;

/*
 * The subscope build (Iterate::iterate's scoped closure): enter the source,
 * seed a Variable from it, apply the logic, set the Variable to the result,
 * and leave the result into the outer scope.
 */
static void
iterate_build(dtfl_scope *inner, void *arg)
{
	iterate_ctx *ic = arg;
	dtfl_collection *entered = dtfl_collection_enter(ic->source, inner);
	dtfl_collection *collection;
	dtfl_variable *var = dtfl_variable_new_from(entered, &collection);
	dtfl_collection *result = ic->logic(ic->user, inner, collection);

	/* variable.set(result.clone()); result.leave(outer). */
	ic->result = dtfl_collection_leave(result);
	dtfl_variable_set(var, result);
	dtfl_collection_free(collection);
	dtfl_collection_free(entered);
}

dtfl_collection *
dtfl_collection_iterate(dtfl_collection *c, dtfl_iterate_fn logic, dtfl_ctx ctx)
{
	dtfl_scope *outer = dtfl_stream_scope(c->stream);
	iterate_ctx ic;

	if (c->diff->cls->negate == NULL)
		__dtfl_panic("Collection::iterate: the diff class is not Abelian");
	ic.source = c;
	ic.logic = logic;
	ic.user = ctx.ptr;
	ic.result = NULL;
	dtfl_scope_iterative(outer, "Iterate", iterate_build, &ic);
	if (ctx.free != NULL)
		ctx.free(ctx.ptr);
	return ic.result;
}
