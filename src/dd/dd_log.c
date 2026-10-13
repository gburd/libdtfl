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
 * Emission scope: the events are logged through the worker's
 * "differential/arrange" logger, the same registry path the timely
 * logger uses, so a subscriber receives them.  The spine and batcher
 * (src/dd/trace) that produce these events in upstream do NOT call
 * these yet (the dd_log ledger row records the gap: this is
 * consumer-side logging with no storage dependency, left to the
 * arrangement owner to wire at the batch/merge/drop points).  So the
 * "differential/arrange" stream exists and is byte-faithful and
 * decodable, populated by whatever calls __dtfl_worker_log_dd_*.
 */
#include "timely/log_codec_int.h"
#include "timely/worker_int.h"

enum differential_event_tag {
	DE_BATCH = 0,
	DE_MERGE = 1,
	DE_DROP = 2,
	DE_MERGE_SHORTFALL = 3,
	DE_TRACE_SHARE = 4,
	DE_BATCHER = 5
};

static const char STREAM[] = "differential/arrange";

/* Logs the encoded event in `buf` to the differential logger, if any. */
static void
emit(__dtfl_worker *w, __dtfl_vec *buf)
{
	__dtfl_logger *l = __dtfl_worker_logger(w, STREAM);

	if (l != NULL) {
		__dtfl_logger_log(l, buf->data, buf->len);
		__dtfl_logger_release(l);
	}
	__dtfl_vec_free(buf);
}

static bool
logging(__dtfl_worker *w)
{
	__dtfl_logger *l = __dtfl_worker_logger(w, STREAM);

	if (l == NULL)
		return false;
	__dtfl_logger_release(l);
	return true;
}

/* DifferentialEvent::Batch(BatchEvent { operator, length }). */
void
__dtfl_worker_log_dd_batch(__dtfl_worker *w, size_t op, size_t length)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_BATCH);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, length);
	emit(w, &b);
}

/*
 * DifferentialEvent::Merge(MergeEvent { operator, scale, length1,
 * length2, complete: Option<usize> }).  has_complete false (a start)
 * encodes None; true (a stop) encodes Some(complete).
 */
void
__dtfl_worker_log_dd_merge(__dtfl_worker *w, size_t op, size_t scale,
    size_t length1, size_t length2, bool has_complete, size_t complete)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
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
	emit(w, &b);
}

/* DifferentialEvent::Drop(DropEvent { operator, length }). */
void
__dtfl_worker_log_dd_drop(__dtfl_worker *w, size_t op, size_t length)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_DROP);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, length);
	emit(w, &b);
}

/*
 * DifferentialEvent::MergeShortfall(MergeShortfall { operator, scale,
 * shortfall }).
 */
void
__dtfl_worker_log_dd_merge_shortfall(__dtfl_worker *w, size_t op, size_t scale,
    size_t shortfall)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_MERGE_SHORTFALL);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, scale);
	__dtfl_bc_u64(&b, shortfall);
	emit(w, &b);
}

/* DifferentialEvent::TraceShare(TraceShare { operator, diff: isize }). */
void
__dtfl_worker_log_dd_trace_share(__dtfl_worker *w, size_t op, int64_t diff)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_TRACE_SHARE);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, (uint64_t)diff);
	emit(w, &b);
}

/*
 * DifferentialEvent::Batcher(BatcherEvent { operator, records_diff,
 * size_diff, capacity_diff, allocations_diff }), four isize deltas.
 */
void
__dtfl_worker_log_dd_batcher(__dtfl_worker *w, size_t op, int64_t records_diff,
    int64_t size_diff, int64_t capacity_diff, int64_t allocations_diff)
{
	__dtfl_vec b;

	if (!logging(w))
		return;
	__dtfl_vec_init(&b, 1);
	__dtfl_bc_u32(&b, DE_BATCHER);
	__dtfl_bc_u64(&b, op);
	__dtfl_bc_u64(&b, (uint64_t)records_diff);
	__dtfl_bc_u64(&b, (uint64_t)size_diff);
	__dtfl_bc_u64(&b, (uint64_t)capacity_diff);
	__dtfl_bc_u64(&b, (uint64_t)allocations_diff);
	emit(w, &b);
}
