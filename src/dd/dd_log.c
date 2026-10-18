/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/dd/dd_log.c: differential event logging.
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/logging.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * The DifferentialEvent encoder: the bincode-1 fixint little-endian
 * encoding of DifferentialEvent (Batch, Merge, Drop, MergeShortfall,
 * TraceShare, Batcher, in that variant order) under the
 * "differential/arrange" stream (PLAN D9), so the subscriber API
 * (dtfl/log.h) decodes it with the pinned Rust types.  Enum variants
 * are u32 tags in declaration order; usize is u64; isize is i64 (two's
 * complement in a u64); the Merge `complete` is an Option<usize> (a
 * one-byte tag then, if Some, a u64).
 *
 * Emission: the trace layer holds the "differential/arrange" logger it
 * was built with (NULL when no subscriber was registered, so a
 * disabled stream costs one NULL test), exactly as upstream threads an
 * Option<Logger> through Spine, MergeBatcher and TraceAgent.  The
 * __dtfl_dd_log_* entry points take that logger; the
 * __dtfl_worker_log_dd_* forms look the logger up on a worker.
 */
#include "timely/log_codec_int.h"
#include "timely/worker_int.h"
#include "dd/arrange_int.h"
#include "timely/scope_int.h"
#include "timely/timely_int.h"

enum differential_event_tag {
	DE_BATCH = 0,
	DE_MERGE = 1,
	DE_DROP = 2,
	DE_MERGE_SHORTFALL = 3,
	DE_TRACE_SHARE = 4,
	DE_BATCHER = 5
};

static const char STREAM[] = "differential/arrange";

/* Logs the encoded event in `buf` to `l` and frees the buffer. */
static void
emit(__dtfl_logger *l, __dtfl_vec *buf)
{
	__dtfl_logger_log(l, buf->data, buf->len);
	__dtfl_vec_free(buf);
}

void
__dtfl_dd_log_batch(__dtfl_logger *l, size_t op, size_t length)
{
	__dtfl_vec b;

	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_BATCH);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, length);
	emit(l, &b);
}

void
__dtfl_dd_log_merge(__dtfl_logger *l, size_t op, size_t scale,
    size_t length1, size_t length2, bool has_complete, size_t complete)
{
	__dtfl_vec b;

	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_MERGE);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, scale);
	__dtfl_bc_u64(&b, length1);
	__dtfl_bc_u64(&b, length2);
	if (has_complete) {
		__dtfl_bc_u8(&b, 1);
		__dtfl_bc_u64(&b, complete);
	} else {
		__dtfl_bc_u8(&b, 0);
	}
	emit(l, &b);
}

void
__dtfl_dd_log_drop(__dtfl_logger *l, size_t op, size_t length)
{
	__dtfl_vec b;

	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_DROP);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, length);
	emit(l, &b);
}

void
__dtfl_dd_log_merge_shortfall(__dtfl_logger *l, size_t op, size_t scale,
    size_t shortfall)
{
	__dtfl_vec b;

	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_MERGE_SHORTFALL);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, scale);
	__dtfl_bc_u64(&b, shortfall);
	emit(l, &b);
}

void
__dtfl_dd_log_trace_share(__dtfl_logger *l, size_t op, int64_t diff)
{
	__dtfl_vec b;

	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_TRACE_SHARE);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, (uint64_t)diff);
	emit(l, &b);
}

void
__dtfl_dd_log_batcher(__dtfl_logger *l, size_t op, int64_t records_diff,
    int64_t size_diff, int64_t capacity_diff, int64_t allocations_diff)
{
	__dtfl_vec b;

	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_BATCHER);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, (uint64_t)records_diff);
	__dtfl_bc_u64(&b, (uint64_t)size_diff);
	__dtfl_bc_u64(&b, (uint64_t)capacity_diff);
	__dtfl_bc_u64(&b, (uint64_t)allocations_diff);
	emit(l, &b);
}

/* ------------------------------------------------------------------ */
/* Worker forms: look the stream's logger up, log if bound.            */
/* ------------------------------------------------------------------ */

#define WITH_LOGGER(w, call)						\
	do {								\
		__dtfl_logger *l_ = __dtfl_worker_logger((w), STREAM);	\
		if (l_ != NULL) {					\
			call;						\
			__dtfl_logger_release(l_);			\
		}							\
	} while (0)

void
__dtfl_worker_log_dd_batch(__dtfl_worker *w, size_t op, size_t length)
{
	WITH_LOGGER(w, __dtfl_dd_log_batch(l_, op, length));
}

void
__dtfl_worker_log_dd_merge(__dtfl_worker *w, size_t op, size_t scale,
    size_t length1, size_t length2, bool has_complete, size_t complete)
{
	WITH_LOGGER(w, __dtfl_dd_log_merge(l_, op, scale, length1, length2,
	    has_complete, complete));
}

void
__dtfl_worker_log_dd_drop(__dtfl_worker *w, size_t op, size_t length)
{
	WITH_LOGGER(w, __dtfl_dd_log_drop(l_, op, length));
}

void
__dtfl_worker_log_dd_merge_shortfall(__dtfl_worker *w, size_t op, size_t scale,
    size_t shortfall)
{
	WITH_LOGGER(w, __dtfl_dd_log_merge_shortfall(l_, op, scale, shortfall));
}

void
__dtfl_worker_log_dd_trace_share(__dtfl_worker *w, size_t op, int64_t diff)
{
	WITH_LOGGER(w, __dtfl_dd_log_trace_share(l_, op, diff));
}

void
__dtfl_worker_log_dd_batcher(__dtfl_worker *w, size_t op, int64_t records_diff,
    int64_t size_diff, int64_t capacity_diff, int64_t allocations_diff)
{
	WITH_LOGGER(w, __dtfl_dd_log_batcher(l_, op, records_diff, size_diff,
	    capacity_diff, allocations_diff));
}

/*
 * The operator-construction step of arrangement.rs / reduce.rs /
 * upsert.rs: acquire the "differential/arrange" logger and hand it, with
 * the operator's global id, to the new trace and (if any) its batcher.
 * Call before TraceAgent::new so the reader's TraceShare is logged.
 */
void
__dtfl_dd_logging_install(dtfl_scope *scope, const dtfl_op_builder *b,
    __dtfl_trace *trace, __dtfl_merge_batcher *batcher)
{
	__dtfl_logger *l = __dtfl_worker_logger(scope->worker, STREAM);
	size_t id;

	if (l == NULL)
		return;
	id = __dtfl_op_builder_global_id(b);
	__dtfl_trace_set_logger(trace, l, id);
	if (batcher != NULL)
		__dtfl_merge_batcher_set_logger(batcher, l, id);
	__dtfl_logger_release(l);
}
