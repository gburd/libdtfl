/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/threshold.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/threshold.c: threshold_total and distinct_total, the totally
 * ordered threshold operators (threshold.rs ThresholdTotal).
 *
 * threshold_total transforms the multiplicity of each key when the time is
 * totally ordered.  It arranges the collection by self, then a unary
 * operator reads each key's accumulated difference in the trace before the
 * current batch and walks the batch updates in order: at each update it maps
 * the accumulation before and after the update through the user predicate
 * and emits the difference of the two thresholded values, as upstream's
 * threshold_total builds threshold_semigroup on top of a thresh that
 * negates the old value.  distinct_total is threshold_total with a predicate
 * that maps any non-zero accumulation to one.
 */
#include "dd/arrange_int.h"
#include "dd/collection_int.h"
#include "dtfl/arrange.h"
#include "dtfl/reduce.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

/*
 * The threshold_total operator state.  trace: the shared input key trace.
 * thresh: the user predicate (key, accumulated input diff) -> optional
 * output diff.  idc/odc: input and output diff classes.
 */
typedef struct thr_ctx {
	__dtfl_trace_agent trace;
	dtfl_threshold_fn thresh;
	void *user;
	void (*user_free)(void *);
	const dtfl_type *kt;
	const __dtfl_diff_class *idc;
	const __dtfl_diff_class *odc;
	unsigned depth;
	__dtfl_antichain lower_limit;
	__dtfl_antichain upper_limit;
} thr_ctx;

typedef struct thr_acc {
	unsigned char *val;	/* idc width, native */
	bool some;
	const __dtfl_diff_class *idc;
} thr_acc;

static void
acc_add(thr_acc *a, const void *diff)
{
	if (a->idc->width == 0)
		return;
	if (!a->some) {
		memcpy(a->val, diff, a->idc->width);
		a->some = true;
	} else {
		a->idc->plus_equals(a->idc, a->val, diff);
	}
}

static void
acc_on_time(const uint64_t *time, const void *diff, void *ctx)
{
	thr_acc *a = ctx;

	(void)time;
	acc_add(a, diff);
}

typedef struct thr_scan {
	thr_ctx *c;
	dtfl_session *s;
	const void *key;
	size_t klen;
	thr_acc *acc;
} thr_scan;

/*
 * Apply the threshold both before and after `diff` is applied to the
 * accumulation and emit the difference of the results (threshold.rs
 * threshold_semigroup via threshold_total).  Here the user predicate maps a
 * single accumulated difference to an output difference; the operator takes
 * the difference new - old itself.
 */
static void
thr_on_time(const uint64_t *time, const void *diff, void *ctx)
{
	thr_scan *w = ctx;
	thr_ctx *c = w->c;
	size_t odw = c->odc->width;
	unsigned char oldbuf[64], newbuf[64];
	bool have_old = false, have_new;
	unsigned char delta[64];
	size_t tw = (size_t)c->depth * sizeof(uint64_t);
	unsigned char *dst;

	/* thresh(old) if the accumulation before the update is non-zero. */
	if (w->acc->some && !c->idc->is_zero(c->idc,
	    c->idc->width ? w->acc->val : NULL))
		have_old = c->thresh(c->user, w->key, w->klen,
		    c->idc->width ? w->acc->val : NULL,
		    odw != 0 ? oldbuf : NULL);

	acc_add(w->acc, diff);

	/* thresh(new) if the accumulation after the update is non-zero. */
	have_new = false;
	if (w->acc->some && !c->idc->is_zero(c->idc,
	    c->idc->width ? w->acc->val : NULL))
		have_new = c->thresh(c->user, w->key, w->klen,
		    c->idc->width ? w->acc->val : NULL,
		    odw != 0 ? newbuf : NULL);

	/* delta = new - old (whichever are present). */
	if (odw != 0) {
		if (have_new)
			memcpy(delta, newbuf, odw);
		else
			c->odc->zero(c->odc, delta);
		if (have_old) {
			unsigned char negold[64];

			memcpy(negold, oldbuf, odw);
			c->odc->negate(c->odc, negold);
			c->odc->plus_equals(c->odc, delta, negold);
		}
	}
	if (!have_old && !have_new)
		return;
	if (odw != 0 && c->odc->is_zero(c->odc, delta))
		return;

	dst = __dtfl_session_reserve(w->s, w->klen + tw + odw);
	memcpy(dst, w->key, w->klen);
	if (tw != 0)
		memcpy(dst + w->klen, time, tw);
	if (odw != 0)
		memcpy(dst + w->klen + tw, delta, odw);
}

static bool
thr_logic(dtfl_op *op, void *vctx)
{
	thr_ctx *c = vctx;
	dtfl_message *m;
	size_t i;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));

	__dtfl_antichain_clear(&c->lower_limit);
	for (i = 0; i < __dtfl_antichain_len(&c->upper_limit); i++)
		__dtfl_antichain_insert(&c->lower_limit,
		    __dtfl_antichain_at(&c->upper_limit, i));

	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		size_t n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			size_t len;
			const void *recp = dtfl_message_record(m, i, &len);
			__dtfl_batch_ord *batch;
			__dtfl_cursor bcur;
			__dtfl_cursor_list tcl;
			bool have_trace;

			memcpy(&batch, recp, sizeof batch);
			__dtfl_antichain_clear(&c->upper_limit);
			{
				const __dtfl_antichain *bu =
				    __dtfl_batch_ord_upper(batch);
				size_t j;

				for (j = 0; j < __dtfl_antichain_len(bu); j++)
					__dtfl_antichain_insert(
					    &c->upper_limit,
					    __dtfl_antichain_at(bu, j));
			}
			have_trace = __dtfl_trace_agent_cursor_through(
			    &c->trace, &c->lower_limit, &tcl);
			/* Upstream unwraps: the cut at lower_limit is clean. */
			DTFL_ASSERT(have_trace);
			__dtfl_cursor_init(&bcur, batch);
			while (__dtfl_cursor_key_valid(&bcur, batch)) {
				size_t klen;
				const void *key = __dtfl_cursor_key(&bcur,
				    batch, &klen);
				unsigned char accbuf[64];
				thr_acc acc;
				thr_scan w;

				acc.val = accbuf;
				acc.some = false;
				acc.idc = c->idc;
				if (have_trace) {
					__dtfl_cursor_list_seek_key(&tcl, key,
					    klen);
					if (__dtfl_cursor_list_key_valid(&tcl)) {
						size_t tkl;
						const void *tk =
						    __dtfl_cursor_list_key(&tcl,
						    &tkl);

						if (tkl == klen && memcmp(tk,
						    key, klen) == 0)
							__dtfl_cursor_list_map_times(
							    &tcl, acc_on_time,
							    &acc);
					}
					/* Batch keys ascend, so the trace
					 * cursor only seeks forward (count.rs /
					 * threshold.rs never rewind it). */
				}
				w.c = c;
				w.s = s;
				w.key = key;
				w.klen = klen;
				w.acc = &acc;
				__dtfl_cursor_map_times(&bcur, batch,
				    thr_on_time, &w);
				__dtfl_cursor_step_key(&bcur, batch);
			}
			if (have_trace)
				__dtfl_cursor_list_free(&tcl);
		}
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}

	/*
	 * Tidy up the shared input trace (count.rs / threshold.rs): step
	 * upper_limit over the empty batches the writer sealed without
	 * sending, then compact to it.  Not the input frontier: that may run
	 * ahead of the trace's upper, and a physical frontier beyond the next
	 * batch's lower would leave the next cursor_through no clean cut.
	 */
	__dtfl_trace_advance_upper(c->trace.box->trace, &c->upper_limit);
	__dtfl_trace_agent_set_logical(&c->trace,
	    (const uint64_t *)(void *)c->upper_limit.elems.data,
	    __dtfl_antichain_len(&c->upper_limit));
	__dtfl_trace_agent_set_physical(&c->trace,
	    (const uint64_t *)(void *)c->upper_limit.elems.data,
	    __dtfl_antichain_len(&c->upper_limit));
	return false;
}

static void
thr_free(void *vctx)
{
	thr_ctx *c = vctx;

	if (c->user_free != NULL)
		c->user_free(c->user);
	__dtfl_trace_agent_free(&c->trace);
	__dtfl_antichain_free(&c->lower_limit);
	__dtfl_antichain_free(&c->upper_limit);
	__dtfl_free(c);
}

static dtfl_collection *
threshold_total_impl(dtfl_collection *c, const dtfl_diff *out_diff,
    dtfl_threshold_fn thresh, void *user, void (*user_free)(void *),
    const char *name)
{
	dtfl_scope *scope = dtfl_collection_scope(c);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	const dtfl_type *kt = dtfl_collection_type(c);
	const __dtfl_diff_class *idc = dtfl_collection_diff(c)->cls;
	const __dtfl_diff_class *odc = out_diff->cls;
	dtfl_arranged *arr;
	thr_ctx *ctx = __dtfl_alloc(sizeof *ctx);
	dtfl_op_builder *b;
	dtfl_stream *out;
	uint64_t minimum[DTFL_TIME_MAX] = { 0 };

	DTFL_ASSERT(dtfl_time_is_total(tc));
	DTFL_ASSERT(odc->negate != NULL);

	arr = dtfl_collection_arrange(c, "Arrange: ThresholdTotal");

	__dtfl_trace_agent_clone(__dtfl_arranged_agent(arr), &ctx->trace);
	ctx->thresh = thresh;
	ctx->user = user;
	ctx->user_free = user_free;
	ctx->kt = kt;
	ctx->idc = idc;
	ctx->odc = odc;
	ctx->depth = dtfl_time_depth(tc);
	__dtfl_antichain_init_elem(&ctx->lower_limit, tc, minimum);
	__dtfl_antichain_init_elem(&ctx->upper_limit, tc, minimum);

	{
		const dtfl_type *ut = __dtfl_update_type(kt,
		    dtfl_time_depth(tc), odc->width);

		b = dtfl_op_builder_new(scope, name);
		dtfl_op_builder_input(b, __dtfl_arranged_stream(arr),
		    dtfl_pact_pipeline());
		dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
		out = dtfl_op_builder_output(b, ut);
		dtfl_op_builder_build(b, thr_logic, ctx, thr_free);
		dtfl_type_free(ut);
	}
	dtfl_arranged_free(arr);
	return __dtfl_as_collection(out, kt, dtfl_diff_retain(out_diff),
	    dtfl_time_depth(tc));
}

dtfl_collection *
dtfl_collection_threshold_total(dtfl_collection *c, const dtfl_diff *out_diff,
    dtfl_threshold_fn thresh, dtfl_ctx ctx)
{
	return threshold_total_impl(c, out_diff, thresh, ctx.ptr, ctx.free,
	    "ThresholdTotal");
}

/* distinct_total: map any non-zero accumulation to one. */
static bool
distinct_total_thresh(void *ctx, const void *key, size_t klen,
    const void *acc_diff, void *out)
{
	const __dtfl_diff_class *odc = ctx;

	(void)key; (void)klen; (void)acc_diff;
	if (odc->width != 0)
		odc->one(odc, out);
	return true;
}

dtfl_collection *
dtfl_collection_distinct_total(dtfl_collection *c, const dtfl_diff *out_diff)
{
	DTFL_ASSERT(out_diff->cls->one != NULL);
	return threshold_total_impl(c, out_diff, distinct_total_thresh,
	    (void *)out_diff->cls, NULL, "DistinctTotal");
}
