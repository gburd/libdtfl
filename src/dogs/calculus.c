/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dogs3-v0.25.1
 *   dogsdogsdogs/src/calculus.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dogs/calculus.c: Differentiate and Integrate over AltNeu.
 *
 * differentiate enters an AltNeu scope and, for each update (data, alt(t),
 * diff), also emits (data, neu(t), -diff): the change at the moment it
 * happens, retracted immediately after, so the accumulated collection is
 * almost everywhere empty.  integrate discards every neu update and strips
 * the alt wrapper, reconstructing the collection in the parent scope.
 *
 * An update travels as record bytes, then the scope's time words, then the
 * diff bytes (collection_int.h).  In an AltNeu scope the time is one word
 * encoded 2t + neu, so alt(t) is even and neu(t) is that word with its low
 * bit set.  Both operators read and rewrite the embedded time and diff, so
 * they are custom unary operators rather than map or filter (which keep the
 * time and diff).
 */
#include "dogs/dogs_int.h"
#include "dtfl/collection.h"
#include "dtfl/dogs.h"
#include "timely/timely_int.h"

/* ------------------------------------------------------------------ */
/* differentiate (calculus.rs Differentiate)                           */
/* ------------------------------------------------------------------ */

typedef struct differentiate_ctx {
	const __dtfl_diff_class *dc;	/* via the collection diff */
	unsigned depth;			/* AltNeu scope: one time word */
	size_t dw;
} differentiate_ctx;

static bool
differentiate_logic(dtfl_op *op, void *vctx)
{
	differentiate_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);
			/* The alt update: emit (data, alt(t), diff) verbatim. */
			unsigned char *alt = __dtfl_session_reserve(s, len);
			unsigned char *neu;
			uint64_t t;

			memcpy(alt, rec, len);

			/* The neu update: (data, neu(t), -diff). */
			neu = __dtfl_session_reserve(s, len);
			memcpy(neu, rec, len);
			/* The time at offset rlen may be unaligned; stage it. */
			memcpy(&t, neu + rlen, sizeof t);
			t = (t / 2) * 2 + 1;
			memcpy(neu + rlen, &t, sizeof t);
			c->dc->negate(c->dc, neu + (len - c->dw));
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

dtfl_collection *
dtfl_collection_differentiate(dtfl_collection *c, dtfl_scope *child)
{
	dtfl_collection *inner = dtfl_collection_enter(c, child);
	const dtfl_type *rt = inner->type;
	const __dtfl_diff_class *dc = inner->diff->cls;
	unsigned depth = inner->depth;
	size_t dw = dc->width;
	differentiate_ctx *dctx;
	dtfl_op_builder *b;
	dtfl_stream *st;
	const dtfl_type *ut;
	dtfl_collection *out;

	if (dc->negate == NULL)
		__dtfl_panic("differentiate: the diff class is not Abelian");

	dctx = __dtfl_alloc(sizeof *dctx);
	dctx->dc = dc;
	dctx->depth = depth;
	dctx->dw = dw;

	ut = __dtfl_update_type(rt, depth, dw);
	b = dtfl_op_builder_new(child, "Differentiate");
	dtfl_op_builder_input(b, inner->stream, dtfl_pact_pipeline());
	/* Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, differentiate_logic, dctx, __dtfl_free);
	dtfl_type_free(ut);

	out = __dtfl_as_collection(st, rt, dtfl_diff_retain(inner->diff),
	    depth);
	dtfl_collection_free(inner);
	return out;
}

/* ------------------------------------------------------------------ */
/* integrate (calculus.rs Integrate)                                   */
/* ------------------------------------------------------------------ */

typedef struct integrate_ctx {
	unsigned depth;
	size_t dw;
} integrate_ctx;

/* Keep only updates at alt times (the low bit of the time word is clear). */
static bool
integrate_logic(dtfl_op *op, void *vctx)
{
	integrate_ctx *c = vctx;
	dtfl_message *m;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);
			unsigned char *dst;
			uint64_t t;

			/* The time at offset rlen may be unaligned; stage it. */
			memcpy(&t, (const unsigned char *)rec + rlen, sizeof t);
			if ((t & 1) != 0)
				continue;	/* a neu update: discard */
			dst = __dtfl_session_reserve(s, len);
			memcpy(dst, rec, len);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	return false;
}

dtfl_collection *
dtfl_collection_integrate(dtfl_collection *c)
{
	const dtfl_type *rt = c->type;
	const __dtfl_diff_class *dc = c->diff->cls;
	unsigned depth = c->depth;
	size_t dw = dc->width;
	integrate_ctx *ictx = __dtfl_alloc(sizeof *ictx);
	dtfl_op_builder *b;
	dtfl_stream *st;
	const dtfl_type *ut;
	dtfl_collection *filtered;
	dtfl_collection *out;

	ictx->depth = depth;
	ictx->dw = dw;

	ut = __dtfl_update_type(rt, depth, dw);
	b = dtfl_op_builder_new(dtfl_stream_scope(c->stream), "Integrate");
	dtfl_op_builder_input(b, c->stream, dtfl_pact_pipeline());
	/* Operator::unary (generic/operator.rs): frontier changes alone do not schedule. */
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_NEVER);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, integrate_logic, ictx, __dtfl_free);
	dtfl_type_free(ut);

	filtered = __dtfl_as_collection(st, rt, dtfl_diff_retain(c->diff),
	    depth);
	out = dtfl_collection_leave(filtered);
	dtfl_collection_free(filtered);
	return out;
}
