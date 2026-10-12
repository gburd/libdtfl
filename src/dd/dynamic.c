/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/dynamic/mod.rs
 *   differential-dataflow/src/dynamic/pointstamp.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/dynamic.c: enter_dynamic, leave_dynamic and feedback_summary.
 *
 * A dynamic scope has a PointStamp inner time whose number of coordinates
 * is a run-time value, so iteration levels are discovered dynamically
 * rather than from the type.  The order, lattice, summaries and codec of
 * PointStamp are in src/progress/time.c (the POINTSTAMP shape); this file
 * ports only the three thin collection operations of dynamic/mod.rs.
 *
 * enter_dynamic is the identity (dynamic/mod.rs: the stream is already in
 * the scope).  leave_dynamic strips all but the first `level - 1`
 * PointStamp coordinates of every update's embedded time and of its
 * capability, so an update leaves one dynamic iteration level; in the
 * padded C representation (PLAN D3) stripping a coordinate is setting it
 * to the minimum, which is 0.  feedback_summary builds the PointStamp
 * summary for a dynamic feedback at a given coordinate: retain None,
 * identity actions below the coordinate, and the step at that coordinate.
 */
#include "dd/collection_int.h"

#include "progress/time_int.h"
#include "timely/timely_int.h"

#include "dtfl/iterate.h"

/* The word offset and coordinate count of the PointStamp inside `tc`. */
static bool
pointstamp_span(const dtfl_time_class *tc, unsigned *off, unsigned *k)
{
	if (tc->shape == DTFL_SHAPE_POINTSTAMP) {
		*off = 0;
		*k = tc->depth;
		return true;
	}
	if (tc->shape == DTFL_SHAPE_PRODUCT && tc->b != NULL &&
	    tc->b->shape == DTFL_SHAPE_POINTSTAMP) {
		*off = tc->a->depth;
		*k = tc->b->depth;
		return true;
	}
	return false;
}

/* The PointStamp summary's word offset inside `tc`'s summary. */
static unsigned
pointstamp_summary_off(const dtfl_time_class *tc)
{
	return tc->shape == DTFL_SHAPE_PRODUCT ? tc->a->sdepth : 0;
}

/* ------------------------------------------------------------------ */
/* enter_dynamic                                                       */
/* ------------------------------------------------------------------ */

dtfl_collection *
dtfl_collection_enter_dynamic(dtfl_collection *c, size_t level)
{
	(void)level;
	/* The stream is already in the scope; return a fresh handle. */
	return __dtfl_as_collection(c->stream, c->type,
	    dtfl_diff_retain(c->diff), c->depth);
}

/* ------------------------------------------------------------------ */
/* leave_dynamic                                                       */
/* ------------------------------------------------------------------ */

/*
 * The capability time is truncated with a path summary on the input
 * connection (retain level - 1), and each update's embedded time is
 * truncated by the operator.  Both set the PointStamp coordinates at or
 * past level - 1 to the minimum (0 in the padded representation).
 */
typedef struct leave_dynamic_ctx {
	const dtfl_time_class *tc;	/* retained */
	unsigned depth;
	size_t dw;
	unsigned ps_off;	/* word offset of the PointStamp coordinates */
	unsigned keep;		/* PointStamp coordinates to keep (level - 1) */
	unsigned k;		/* total PointStamp coordinates */
} leave_dynamic_ctx;

/* Zero PointStamp coordinates from `keep` to the end, in place. */
static void
truncate_pointstamp(const leave_dynamic_ctx *c, uint64_t *time)
{
	unsigned i;

	for (i = c->keep; i < c->k; i++)
		time[c->ps_off + i] = 0;
}

static bool
leave_dynamic_logic(dtfl_op *op, void *vctx)
{
	leave_dynamic_ctx *c = vctx;
	dtfl_message *m;
	size_t tw = (size_t)c->depth * sizeof(uint64_t);

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		uint64_t new_time[DTFL_TIME_MAX];
		dtfl_cap *cap;
		dtfl_session *s;
		size_t i, n = dtfl_message_len(m);

		/* new_cap = cap.delayed(truncate(cap.time())). */
		__dtfl_time_copy(c->tc, new_time, dtfl_message_time(m));
		truncate_pointstamp(c, new_time);
		cap = dtfl_op_input_cap_delayed(op, 0, 0, new_time);
		s = dtfl_op_session(op, 0, cap);
		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			size_t rlen = __dtfl_update_rec_len(c->depth, c->dw, len);
			const uint64_t *et = __dtfl_update_time(rec, c->depth,
			    c->dw, len);
			unsigned char *dst = __dtfl_session_reserve(s, len);
			uint64_t trunc[DTFL_TIME_MAX];

			__dtfl_time_copy(c->tc, trunc, et);
			truncate_pointstamp(c, trunc);
			memcpy(dst, rec, rlen);
			memcpy(dst + rlen, trunc, tw);
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
leave_dynamic_free(void *vctx)
{
	leave_dynamic_ctx *c = vctx;

	dtfl_time_class_free(c->tc);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_collection_leave_dynamic(dtfl_collection *c, size_t level)
{
	const dtfl_time_class *tc =
	    dtfl_scope_time_class(dtfl_stream_scope(c->stream));
	leave_dynamic_ctx *lc;
	unsigned off, k;
	const dtfl_type *ut;
	dtfl_op_builder *b;
	dtfl_stream *st;

	if (level < 1 || !pointstamp_span(tc, &off, &k))
		__dtfl_panic("Collection::leave_dynamic: not a PointStamp scope");
	lc = __dtfl_alloc(sizeof *lc);
	lc->tc = dtfl_time_class_retain(tc);
	lc->depth = c->depth;
	lc->dw = c->diff->cls->width;
	lc->ps_off = off;
	lc->k = k;
	lc->keep = (unsigned)(level - 1) < k ? (unsigned)(level - 1) : k;
	ut = __dtfl_update_type(c->type, c->depth, lc->dw);
	b = dtfl_op_builder_new(dtfl_stream_scope(c->stream), "LeaveDynamic");
	dtfl_op_builder_input(b, c->stream, dtfl_pact_pipeline());
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, leave_dynamic_logic, lc, leave_dynamic_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, c->type, dtfl_diff_retain(c->diff),
	    c->depth);
}

/* ------------------------------------------------------------------ */
/* feedback_summary                                                    */
/* ------------------------------------------------------------------ */

void
dtfl_feedback_summary(const dtfl_time_class *tc, size_t level, uint64_t step,
    uint64_t *out)
{
	unsigned off, k, soff, sw = dtfl_summary_depth(tc);

	if (level < 1 || !pointstamp_span(tc, &off, &k) || (size_t)k < level)
		__dtfl_panic("feedback_summary: level out of range");
	soff = pointstamp_summary_off(tc);
	/*
	 * PointStampSummary { retain: None, actions: [identity * (level-1),
	 * step] }.  The PointStamp summary occupies words [soff .. soff + 1 + k)
	 * of the class summary: word soff is retain, words soff+1 .. are the
	 * per-coordinate actions.  Any outer summary words stay default (0).
	 */
	memset(out, 0, (size_t)sw * sizeof(uint64_t));
	out[soff] = UINT64_MAX;		/* retain None */
	out[soff + 1 + (level - 1)] = step;
}
