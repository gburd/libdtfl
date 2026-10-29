/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/count.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/count.c: count_total, the totally-ordered count operator.
 *
 * count_total counts the occurrences of each key when the time is totally
 * ordered (count.rs CountTotal).  It arranges the collection by self into a
 * shared key trace, then a unary operator gathers every batch of an
 * activation into one cursor list and, for each key in it, reads the key's
 * accumulated difference in the trace before those batches and walks the
 * batches' (time, diff) updates in order, emitting a retraction of
 * the old count and an assertion of the new count at each time.  Because the
 * time is totally ordered the running count is exact without the general
 * reduce engine; the output difference is one of the output class.
 */
#include "dd/arrange_int.h"
#include "dd/collection_int.h"
#include "dtfl/arrange.h"
#include "dtfl/reduce.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

/*
 * Render a native diff value as the bytes of its record descriptor (the
 * order-preserving D1 encoding), so count can turn an accumulated difference
 * into an output value.  Supports the built-in integer classes.
 */
static size_t
diff_value_bytes(const __dtfl_diff_class *dc, const void *native,
    unsigned char *buf)
{
	if (native == NULL || dc->width == 0)
		return 0;
	if (dc->kind == DTFL_DIFF_KIND_SIGNED ||
	    dc->kind == DTFL_DIFF_KIND_WRAPPING) {
		switch (dc->width) {
		case 1: { int8_t v; memcpy(&v, native, 1); return dtfl_put_i8(buf, v); }
		case 2: { int16_t v; memcpy(&v, native, 2); return dtfl_put_i16(buf, v); }
		case 4: { int32_t v; memcpy(&v, native, 4); return dtfl_put_i32(buf, v); }
		default: { int64_t v; memcpy(&v, native, 8); return dtfl_put_i64(buf, v); }
		}
	}
	if (dc->kind == DTFL_DIFF_KIND_UNSIGNED) {
		switch (dc->width) {
		case 1: { uint8_t v; memcpy(&v, native, 1); return dtfl_put_u8(buf, v); }
		case 2: { uint16_t v; memcpy(&v, native, 2); return dtfl_put_u16(buf, v); }
		case 4: { uint32_t v; memcpy(&v, native, 4); return dtfl_put_u32(buf, v); }
		default: { uint64_t v; memcpy(&v, native, 8); return dtfl_put_u64(buf, v); }
		}
	}
	memcpy(buf, native, dc->width);
	return dc->width;
}

/*
 * The count_total operator state.  trace: the shared input key trace (read
 * before each batch).  idc/odc: input and output diff classes.  depth: time
 * words.  The input stream carries DTFL_BATCH_PTR records (one per batch).
 */
typedef struct count_ctx {
	__dtfl_trace_agent trace;	/* clone of the arrangement's agent */
	const dtfl_type *kt;
	const __dtfl_diff_class *idc;
	const __dtfl_diff_class *odc;
	unsigned depth;
	size_t dw;			/* output record: key || count value */
	__dtfl_antichain lower_limit;
	__dtfl_antichain upper_limit;
} count_ctx;

/* The running count accumulated while scanning a key's updates. */
typedef struct count_acc {
	unsigned char *val;	/* idc width, native */
	bool some;
	const __dtfl_diff_class *idc;
} count_acc;

static void
acc_add(count_acc *a, const void *diff)
{
	if (a->idc->width == 0)
		return;
	if (__dtfl_diff_is_int64(a->idc)) {
		/* plus_equals of a 64-bit built-in: a wrapping add. */
		uint64_t x, y = 0;

		memcpy(&x, diff, 8);
		if (a->some)
			memcpy(&y, a->val, 8);
		x += y;
		memcpy(a->val, &x, 8);
		a->some = true;
		return;
	}
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
	count_acc *a = ctx;

	(void)time;
	acc_add(a, diff);
}

/* Per-batch scan context: emits (key || count, time, +/- one) updates. */
typedef struct count_scan {
	count_ctx *c;
	dtfl_session *s;
	const void *key;
	size_t klen;
	count_acc *acc;
} count_scan;

/* Emit (key || count_value, time, odiff) onto the session. */
static void
emit_count(count_scan *w, const uint64_t *time, bool positive)
{
	count_ctx *c = w->c;
	unsigned char valbuf[16] = { 0 };
	size_t vlen;
	size_t tw = (size_t)c->depth * sizeof(uint64_t);
	size_t odw = c->odc->width;
	unsigned char one[64];
	unsigned char *dst;

	if (!w->acc->some || c->idc->is_zero(c->idc,
	    c->idc->width ? w->acc->val : NULL))
		return;
	vlen = diff_value_bytes(c->idc, c->idc->width ? w->acc->val : NULL,
	    valbuf);
	if (odw != 0) {
		c->odc->one(c->odc, one);
		if (!positive)
			c->odc->negate(c->odc, one);
	}
	dst = __dtfl_session_reserve(w->s, w->klen + vlen + tw + odw);
	if (w->klen == 8 && vlen == 8 && tw == 8 && odw == 8) {
		/* (u64 key, i64 count, u64 time, i64 diff): fixed copies. */
		memcpy(dst, w->key, 8);
		memcpy(dst + 8, valbuf, 8);
		memcpy(dst + 16, time, 8);
		memcpy(dst + 24, one, 8);
		return;
	}
	memcpy(dst, w->key, w->klen);
	memcpy(dst + w->klen, valbuf, vlen);
	if (tw != 0)
		memcpy(dst + w->klen + vlen, time, tw);
	if (odw != 0)
		memcpy(dst + w->klen + vlen + tw, one, odw);
}

/*
 * Walk the batch updates of the current key in time order (CountTotal's
 * map_times loop): before applying each update emit the old count with -1,
 * apply the update, then emit the new count with +1.
 */
static void
batch_on_time(const uint64_t *time, const void *diff, void *ctx)
{
	count_scan *w = ctx;

	emit_count(w, time, false);
	acc_add(w->acc, diff);
	emit_count(w, time, true);
}

static bool
count_logic(dtfl_op *op, void *vctx)
{
	count_ctx *c = vctx;
	dtfl_message *m;
	dtfl_cap *cap = NULL;
	__dtfl_vec batch_storage;	/* __dtfl_batch_ord *, retained */
	size_t i;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	__dtfl_vec_init(&batch_storage, sizeof(__dtfl_batch_ord *));

	/* Downgrade previous upper limit to be current lower limit. */
	__dtfl_antichain_copy(&c->lower_limit, &c->upper_limit);

	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *mcap = dtfl_op_input_cap(op, 0, 0);
		size_t n = dtfl_message_len(m);

		/* NB: Assumes batches are in-order. */
		if (cap == NULL)
			cap = mcap;
		else
			dtfl_cap_drop(mcap);
		for (i = 0; i < n; i++) {
			size_t len;
			const void *recp = dtfl_message_record(m, i, &len);
			__dtfl_batch_ord *batch;

			memcpy(&batch, recp, sizeof batch);
			/* NB: Assumes batches are in-order. */
			__dtfl_antichain_copy(&c->upper_limit,
			    __dtfl_batch_ord_upper(batch));
			/* The record is a borrow; batch_storage holds its own. */
			batch = __dtfl_batch_ord_retain(batch);
			__dtfl_vec_push(&batch_storage, &batch);
		}
	}

	if (cap != NULL) {
		dtfl_session *s = dtfl_op_session(op, 0, cap);
		__dtfl_cursor_list bcl, tcl;
		bool have_trace;

		/* cursor_list takes the batch_storage references. */
		__dtfl_cursor_list_init(&bcl,
		    (__dtfl_batch_ord **)(void *)batch_storage.data,
		    batch_storage.len, c->trace.kt, c->trace.vt);
		have_trace = __dtfl_trace_agent_cursor_through(&c->trace,
		    &c->lower_limit, &tcl);
		/* Upstream unwraps: the cut at lower_limit is clean. */
		DTFL_ASSERT(have_trace);

		while (__dtfl_cursor_list_key_valid(&bcl)) {
			size_t klen;
			const void *key = __dtfl_cursor_list_key(&bcl, &klen);
			unsigned char accbuf[64];
			count_acc acc;
			count_scan w;

			acc.val = accbuf;
			acc.some = false;
			acc.idc = c->idc;
			/* The key's count in the trace before the batches. */
			__dtfl_cursor_list_seek_key(&tcl, key, klen);
			if (__dtfl_cursor_list_key_valid(&tcl)) {
				size_t tkl;
				const void *tk = __dtfl_cursor_list_key(&tcl,
				    &tkl);

				if (tkl == klen && memcmp(tk, key, klen) == 0)
					__dtfl_cursor_list_map_times(&tcl,
					    acc_on_time, &acc);
			}
			/* The key batch has one value (unit). */
			w.c = c;
			w.s = s;
			w.key = key;
			w.klen = klen;
			w.acc = &acc;
			__dtfl_cursor_list_map_times(&bcl, batch_on_time, &w);
			__dtfl_cursor_list_step_key(&bcl);
		}
		__dtfl_cursor_list_free(&tcl);
		__dtfl_cursor_list_free(&bcl);
		dtfl_session_close(s);
		dtfl_cap_drop(cap);
	}
	/* No message means no batch, so nothing is left to release. */
	__dtfl_vec_free(&batch_storage);

	/*
	 * Tidy up the shared input trace (count.rs / threshold.rs): step
	 * upper_limit over the empty batches the writer sealed without
	 * sending, then compact to it.  Not the input frontier: that may run
	 * ahead of the trace's upper, and a physical frontier beyond the next
	 * batch's lower would leave the next cursor_through no clean cut.
	 */
	__dtfl_trace_agent_advance_upper(&c->trace, &c->upper_limit);
	__dtfl_trace_agent_set_logical(&c->trace,
	    (const uint64_t *)(void *)c->upper_limit.elems.data,
	    __dtfl_antichain_len(&c->upper_limit));
	__dtfl_trace_agent_set_physical(&c->trace,
	    (const uint64_t *)(void *)c->upper_limit.elems.data,
	    __dtfl_antichain_len(&c->upper_limit));
	return false;
}

static void
count_free(void *vctx)
{
	count_ctx *c = vctx;

	__dtfl_trace_agent_free(&c->trace);
	__dtfl_antichain_free(&c->lower_limit);
	__dtfl_antichain_free(&c->upper_limit);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_collection_count_total(dtfl_collection *c, const dtfl_diff *out_diff)
{
	dtfl_scope *scope = dtfl_collection_scope(c);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	const dtfl_type *kt = dtfl_collection_type(c);
	const __dtfl_diff_class *idc = dtfl_collection_diff(c)->cls;
	const __dtfl_diff_class *odc = out_diff->cls;
	const dtfl_type *out_val = __dtfl_diff_type(idc);
	dtfl_arranged *arr;
	count_ctx *ctx = __dtfl_alloc(sizeof *ctx);
	dtfl_op_builder *b;
	dtfl_stream *out;
	const dtfl_type *fields[2];
	const dtfl_type *out_rec;
	uint64_t minimum[DTFL_TIME_MAX] = { 0 };

	DTFL_ASSERT(dtfl_time_is_total(tc));
	DTFL_ASSERT(odc->negate != NULL && odc->one != NULL);

	arr = dtfl_collection_arrange(c, "Arrange: CountTotal");

	__dtfl_trace_agent_clone(__dtfl_arranged_agent(arr), &ctx->trace);
	ctx->kt = kt;
	ctx->idc = idc;
	ctx->odc = odc;
	ctx->depth = dtfl_time_depth(tc);
	ctx->dw = odc->width;
	__dtfl_antichain_init_elem(&ctx->lower_limit, tc, minimum);
	__dtfl_antichain_init_elem(&ctx->upper_limit, tc, minimum);

	fields[0] = kt;
	fields[1] = out_val;
	out_rec = dtfl_type_tuple(fields, 2);
	{
		const dtfl_type *ut = __dtfl_update_type(out_rec,
		    dtfl_time_depth(tc), odc->width);

		b = dtfl_op_builder_new(scope, "CountTotal");
		dtfl_op_builder_input(b, __dtfl_arranged_stream(arr),
		    dtfl_pact_pipeline());
		dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
		out = dtfl_op_builder_output(b, ut);
		dtfl_op_builder_build(b, count_logic, ctx, count_free);
		dtfl_type_free(ut);
	}
	dtfl_arranged_free(arr);
	{
		dtfl_collection *rc = __dtfl_as_collection(out, out_rec,
		    dtfl_diff_retain(out_diff), dtfl_time_depth(tc));

		dtfl_type_free(out_rec);
		return rc;
	}
}
