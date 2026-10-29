/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/diag/diag_log.c: the diagnostics log demux.
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   diagnostics/src/logging.rs
 * Copyright (c) 2019 Timely Dataflow Diagnostics contributors.
 *
 * logging.rs replays the timely and differential log streams into
 * differential-dataflow collections, demuxes them into typed
 * DiagnosticUpdate collections (operators, channels, elapsed, messages,
 * arrangement and batcher stats), arranges them, and cross-joins with a
 * client input so a connecting browser receives the full current state
 * as a batch of +1 diffs followed by incremental updates.
 *
 * The C port exploits the public subscriber API (dtfl/log.h), which
 * already delivers decoded event batches on the worker thread, so it
 * does not need a second dataflow to reconstruct the collections.  It
 * maintains the same live state directly (the operators/channels/stats
 * that the DD arrangements would hold) and emits the same JsonUpdate
 * diffs: +1 when an operator or channel appears, -1 on shutdown, and
 * the running statistic deltas.  A new client is replayed the full
 * snapshot (the cross-join on client +1), then follows the incremental
 * Frames.  Timing is kept exactly: each log batch becomes one Frame at
 * the batch time, as server.rs flushes one Frame per closed timestamp.
 */
#include <string.h>

#include "diag/diag_int.h"

/* ------------------------------------------------------------------ */
/* Live-state helpers                                                  */
/* ------------------------------------------------------------------ */

static __dtfl_diag_operator *
find_operator(__dtfl_diag_shared *sh, size_t id)
{
	size_t i;

	for (i = 0; i < sh->operators.len; i++) {
		__dtfl_diag_operator *op = __dtfl_vec_at(&sh->operators, i);

		if (op->id == id)
			return op;
	}
	return NULL;
}

static __dtfl_diag_stat *
find_stat(__dtfl_diag_shared *sh, __dtfl_diag_stat_kind kind, size_t id)
{
	size_t i;

	for (i = 0; i < sh->stats.len; i++) {
		__dtfl_diag_stat *st = __dtfl_vec_at(&sh->stats, i);

		if (st->kind == kind && st->id == id)
			return st;
	}
	return NULL;
}

static size_t *
dup_usizes(const size_t *v, size_t n)
{
	size_t *copy;

	if (n == 0)
		return NULL;
	copy = __dtfl_calloc(n, sizeof(size_t));
	memcpy(copy, v, n * sizeof(size_t));
	return copy;
}

/* Adds a stat delta to the running total; drops the entry at zero. */
/*
 * isize arithmetic as a Rust release build does it: two's complement
 * wrapping, where C signed overflow is undefined.  Stat values and
 * merge lengths come from a recorded stream and may be anything.
 */
static int64_t
wrap_add(int64_t a, int64_t b)
{
	return (int64_t)((uint64_t)a + (uint64_t)b);
}

static int64_t
wrap_neg(int64_t a)
{
	return (int64_t)(0 - (uint64_t)a);
}

static void
apply_stat(__dtfl_diag_shared *sh, __dtfl_diag_stat_kind kind, size_t id,
    int64_t diff)
{
	__dtfl_diag_stat *st;

	if (diff == 0)
		return;
	st = find_stat(sh, kind, id);
	if (st == NULL) {
		__dtfl_diag_stat ns = { kind, id, diff };

		__dtfl_vec_push(&sh->stats, &ns);
		return;
	}
	st->value = wrap_add(st->value, diff);
	if (st->value == 0) {
		size_t idx = (size_t)(st - (__dtfl_diag_stat *)(void *)sh->stats.data);

		__dtfl_vec_swap_remove(&sh->stats, idx);
	}
}

/* ------------------------------------------------------------------ */
/* The subscriber                                                      */
/* ------------------------------------------------------------------ */

/*
 * One decoded log batch becomes one Frame.  Each event updates the live
 * state under the shared lock and adds the matching JsonUpdate to the
 * frame.  The finished Frame payload is enqueued for the server thread.
 */
void
__dtfl_diag_on_batch(const dtfl_log_batch *batch, void *ctx)
{
	__dtfl_diag_shared *sh = ctx;
	size_t n = dtfl_log_batch_len(batch);
	const char *stream = dtfl_log_batch_stream(batch);
	bool is_timely = strcmp(stream, "timely") == 0;
	__dtfl_diag_frame frame;
	uint64_t ts_us = dtfl_log_batch_time(batch) / 1000;
	size_t i;
	char *payload;

	if (n == 0)
		return;	/* flush notification only */

	__dtfl_diag_frame_begin(&frame);
	__dtfl_mutex_lock(&sh->lock);

	for (i = 0; i < n; i++) {
		dtfl_log_event_kind kind = dtfl_log_event_kind_at(batch, i);

		if (is_timely) {
			switch (kind) {
			case DTFL_LOG_OPERATES: {
				size_t id = dtfl_log_operates_id(batch, i);
				size_t alen;
				const size_t *addr =
				    dtfl_log_operates_addr(batch, i, &alen);
				const char *name =
				    dtfl_log_operates_name(batch, i);
				__dtfl_diag_operator op;

				op.id = id;
				op.name = __dtfl_strdup(name != NULL ? name :
				    "");
				op.addr = dup_usizes(addr, alen);
				op.addr_len = alen;
				__dtfl_vec_push(&sh->operators, &op);
				__dtfl_diag_frame_add_operator(&frame, id, name,
				    addr, alen, 1);
				break;
			}
			case DTFL_LOG_SHUTDOWN: {
				size_t id = dtfl_log_shutdown_id(batch, i);
				__dtfl_diag_operator *op =
				    find_operator(sh, id);

				if (op != NULL) {
					size_t idx = (size_t)(op -
					    (__dtfl_diag_operator *)(void *)
					    sh->operators.data);

					__dtfl_diag_frame_add_operator(&frame,
					    op->id, op->name, op->addr,
					    op->addr_len, -1);
					__dtfl_free(op->name);
					__dtfl_free(op->addr);
					__dtfl_vec_swap_remove(&sh->operators,
					    idx);
				}
				break;
			}
			case DTFL_LOG_CHANNELS: {
				size_t id = dtfl_log_channels_id(batch, i);
				size_t slen, sn, sp, tn, tp;
				const size_t *scope =
				    dtfl_log_channels_scope(batch, i, &slen);
				__dtfl_diag_channel ch;

				dtfl_log_channels_source(batch, i, &sn, &sp);
				dtfl_log_channels_target(batch, i, &tn, &tp);
				ch.id = id;
				ch.scope_addr = dup_usizes(scope, slen);
				ch.scope_len = slen;
				ch.src_node = sn;
				ch.src_port = sp;
				ch.tgt_node = tn;
				ch.tgt_port = tp;
				__dtfl_vec_push(&sh->channels, &ch);
				__dtfl_diag_frame_add_channel(&frame, id, scope,
				    slen, sn, sp, tn, tp, 1);
				break;
			}
			case DTFL_LOG_SCHEDULE: {
				/*
				 * Elapsed per operator, derived from the
				 * Start/Stop pair as logging.rs does: record the
				 * Start time, and on the matching Stop emit the
				 * interval in nanoseconds as the Elapsed stat.
				 */
				size_t id = dtfl_log_schedule_id(batch, i);
				uint64_t et = dtfl_log_event_time(batch, i);
				size_t j;

				if (dtfl_log_schedule_start(batch, i)) {
					__dtfl_diag_sched_start s = { id, et };

					__dtfl_vec_push(&sh->sched_starts, &s);
					break;
				}
				for (j = 0; j < sh->sched_starts.len; j++) {
					__dtfl_diag_sched_start *s =
					    __dtfl_vec_at(&sh->sched_starts, j);

					if (s->id != id)
						continue;
					if (et > s->start_ns) {
						int64_t ns = (int64_t)
						    (et - s->start_ns);

						apply_stat(sh,
						    DIAG_STAT_ELAPSED, id,
						    ns);
						__dtfl_diag_frame_add_stat(
						    &frame, DIAG_STAT_ELAPSED,
						    id, ns);
					}
					__dtfl_vec_swap_remove(
					    &sh->sched_starts, j);
					break;
				}
				break;
			}
			case DTFL_LOG_MESSAGES:
				if (dtfl_log_messages_is_send(batch, i)) {
					size_t chan =
					    dtfl_log_messages_channel(batch, i);
					int64_t cnt =
					    dtfl_log_messages_record_count(
					    batch, i);

					if (cnt != 0) {
						apply_stat(sh,
						    DIAG_STAT_MESSAGES, chan,
						    cnt);
						__dtfl_diag_frame_add_stat(
						    &frame, DIAG_STAT_MESSAGES,
						    chan, cnt);
					}
				}
				break;
			default:
				break;
			}
		} else {
			/* differential/arrange */
			size_t op;
			int64_t rd, sd, cd, ad;
			size_t complete;

			switch (kind) {
			case DTFL_LOG_DD_BATCH:
				op = dtfl_log_dd_operator(batch, i);
				apply_stat(sh, DIAG_STAT_ARRANGEMENT_BATCHES,
				    op, 1);
				__dtfl_diag_frame_add_stat(&frame,
				    DIAG_STAT_ARRANGEMENT_BATCHES, op, 1);
				{
					int64_t len = (int64_t)
					    dtfl_log_dd_length(batch, i);

					apply_stat(sh,
					    DIAG_STAT_ARRANGEMENT_RECORDS, op,
					    len);
					__dtfl_diag_frame_add_stat(&frame,
					    DIAG_STAT_ARRANGEMENT_RECORDS, op,
					    len);
				}
				break;
			case DTFL_LOG_DD_MERGE:
				op = dtfl_log_dd_operator(batch, i);
				if (dtfl_log_dd_merge_complete(batch, i,
				    &complete)) {
					int64_t l1 = (int64_t)
					    dtfl_log_dd_merge_length1(batch, i);
					int64_t l2 = (int64_t)
					    dtfl_log_dd_merge_length2(batch, i);
					int64_t diff = (int64_t)((uint64_t)complete -
					    (uint64_t)wrap_add(l1, l2));

					apply_stat(sh,
					    DIAG_STAT_ARRANGEMENT_BATCHES, op,
					    -1);
					__dtfl_diag_frame_add_stat(&frame,
					    DIAG_STAT_ARRANGEMENT_BATCHES, op,
					    -1);
					if (diff != 0) {
						apply_stat(sh,
						    DIAG_STAT_ARRANGEMENT_RECORDS,
						    op, diff);
						__dtfl_diag_frame_add_stat(
						    &frame,
						    DIAG_STAT_ARRANGEMENT_RECORDS,
						    op, diff);
					}
				}
				break;
			case DTFL_LOG_DD_DROP:
				op = dtfl_log_dd_operator(batch, i);
				{
					int64_t len = (int64_t)
					    dtfl_log_dd_length(batch, i);

					apply_stat(sh,
					    DIAG_STAT_ARRANGEMENT_BATCHES, op,
					    -1);
					__dtfl_diag_frame_add_stat(&frame,
					    DIAG_STAT_ARRANGEMENT_BATCHES, op,
					    -1);
					if (len != 0) {
						apply_stat(sh,
						    DIAG_STAT_ARRANGEMENT_RECORDS,
						    op, wrap_neg(len));
						__dtfl_diag_frame_add_stat(
						    &frame,
						    DIAG_STAT_ARRANGEMENT_RECORDS,
						    op, wrap_neg(len));
					}
				}
				break;
			case DTFL_LOG_DD_TRACE_SHARE:
				op = dtfl_log_dd_operator(batch, i);
				{
					int64_t d =
					    dtfl_log_dd_trace_share_diff(batch,
					    i);

					apply_stat(sh, DIAG_STAT_SHARING, op,
					    d);
					__dtfl_diag_frame_add_stat(&frame,
					    DIAG_STAT_SHARING, op, d);
				}
				break;
			case DTFL_LOG_DD_BATCHER:
				op = dtfl_log_dd_operator(batch, i);
				dtfl_log_dd_batcher(batch, i, &rd, &sd, &cd,
				    &ad);
				apply_stat(sh, DIAG_STAT_BATCHER_RECORDS, op,
				    rd);
				apply_stat(sh, DIAG_STAT_BATCHER_SIZE, op, sd);
				apply_stat(sh, DIAG_STAT_BATCHER_CAPACITY, op,
				    cd);
				apply_stat(sh, DIAG_STAT_BATCHER_ALLOCATIONS,
				    op, ad);
				__dtfl_diag_frame_add_stat(&frame,
				    DIAG_STAT_BATCHER_RECORDS, op, rd);
				__dtfl_diag_frame_add_stat(&frame,
				    DIAG_STAT_BATCHER_SIZE, op, sd);
				__dtfl_diag_frame_add_stat(&frame,
				    DIAG_STAT_BATCHER_CAPACITY, op, cd);
				__dtfl_diag_frame_add_stat(&frame,
				    DIAG_STAT_BATCHER_ALLOCATIONS, op, ad);
				break;
			default:
				break;
			}
		}
	}

	payload = __dtfl_diag_frame_finish(&frame, ts_us);
	if (payload != NULL)
		__dtfl_vec_push(&sh->queue, &payload);
	__dtfl_mutex_unlock(&sh->lock);
}
