/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_log_walk.c: re-encodes a decoded dtfl_log_batch.
 *
 * fuzz_log_walk reads every event of a batch through the public
 * accessors (dtfl/log.h), touching every string and address the decoder
 * handed out, and appends to `out` one record per event in the stream's
 * bincode encoding (the layout src/timely/timely_log.c, src/dd/dd_log.c
 * and src/comm/comm_log.c write).  An UNKNOWN event appends nothing.
 * Decoding the output again and walking it must give the same bytes:
 * the log decoder's round trip.  Each record is framed as u32 length
 * then bytes, so the caller can split them.
 */
#include "fuzz_log_walk.h"

#include "dtfl/log.h"
#include "timely/log_codec_int.h"

static void
put_str(__dtfl_vec *o, const char *s)
{
	FUZZ_ASSERT(s != NULL);
	__dtfl_bc_str(o, s);	/* strlen: ASan checks the NUL is in bounds */
}

static void
put_usizes(__dtfl_vec *o, const size_t *v, size_t n)
{
	FUZZ_ASSERT(n == 0 || v != NULL);
	__dtfl_bc_usizes(o, v, n);
}

static void
walk_one(const dtfl_log_batch *b, size_t i, __dtfl_vec *r)
{
	dtfl_log_event_kind k = dtfl_log_event_kind_at(b, i);
	size_t n, x, y, z, w, u, v;
	const size_t *a;
	uint64_t t;
	int64_t i0, i1, i2, i3;

	switch (k) {
	case DTFL_LOG_OPERATES:
		__dtfl_bc_u32(r, 0);
		__dtfl_bc_u64(r, dtfl_log_operates_id(b, i));
		a = dtfl_log_operates_addr(b, i, &n);
		put_usizes(r, a, n);
		put_str(r, dtfl_log_operates_name(b, i));
		break;
	case DTFL_LOG_CHANNELS:
		__dtfl_bc_u32(r, 1);
		__dtfl_bc_u64(r, dtfl_log_channels_id(b, i));
		a = dtfl_log_channels_scope(b, i, &n);
		put_usizes(r, a, n);
		dtfl_log_channels_source(b, i, &x, &y);
		dtfl_log_channels_target(b, i, &z, &w);
		__dtfl_bc_u64(r, x);
		__dtfl_bc_u64(r, y);
		__dtfl_bc_u64(r, z);
		__dtfl_bc_u64(r, w);
		put_str(r, dtfl_log_channels_typ(b, i));
		break;
	case DTFL_LOG_PUSH_PROGRESS:
		__dtfl_bc_u32(r, 2);
		__dtfl_bc_u64(r, dtfl_log_push_progress_op_id(b, i));
		break;
	case DTFL_LOG_MESSAGES:
		__dtfl_bc_u32(r, 3);
		__dtfl_bc_u8(r, dtfl_log_messages_is_send(b, i));
		__dtfl_bc_u64(r, dtfl_log_messages_channel(b, i));
		__dtfl_bc_u64(r, dtfl_log_messages_source(b, i));
		__dtfl_bc_u64(r, dtfl_log_messages_target(b, i));
		__dtfl_bc_u64(r, dtfl_log_messages_seq_no(b, i));
		__dtfl_bc_u64(r,
		    (uint64_t)dtfl_log_messages_record_count(b, i));
		break;
	case DTFL_LOG_SCHEDULE:
		__dtfl_bc_u32(r, 4);
		__dtfl_bc_u64(r, dtfl_log_schedule_id(b, i));
		__dtfl_bc_u32(r, dtfl_log_schedule_start(b, i) ? 0 : 1);
		break;
	case DTFL_LOG_SHUTDOWN:
		__dtfl_bc_u32(r, 5);
		__dtfl_bc_u64(r, dtfl_log_shutdown_id(b, i));
		break;
	case DTFL_LOG_COMM_CHANNELS:
		__dtfl_bc_u32(r, 6);
		__dtfl_bc_u64(r, dtfl_log_comm_channels_id(b, i));
		__dtfl_bc_u32(r, dtfl_log_comm_channels_progress(b, i) ? 0 : 1);
		break;
	case DTFL_LOG_PARK:
		__dtfl_bc_u32(r, 7);
		if (dtfl_log_park_is_park(b, i)) {
			__dtfl_bc_u32(r, 0);
			if (dtfl_log_park_timeout(b, i, &t)) {
				__dtfl_bc_u8(r, 1);
				__dtfl_bc_duration(r, t);
			} else {
				__dtfl_bc_u8(r, 0);
			}
		} else {
			__dtfl_bc_u32(r, 1);
		}
		break;
	case DTFL_LOG_TEXT:
		__dtfl_bc_u32(r, 8);
		put_str(r, dtfl_log_text(b, i));
		break;
	case DTFL_LOG_DD_BATCH:
	case DTFL_LOG_DD_DROP:
		__dtfl_bc_u32(r, k == DTFL_LOG_DD_BATCH ? 0 : 2);
		__dtfl_bc_u64(r, dtfl_log_dd_operator(b, i));
		__dtfl_bc_u64(r, dtfl_log_dd_length(b, i));
		break;
	case DTFL_LOG_DD_MERGE:
		__dtfl_bc_u32(r, 1);
		__dtfl_bc_u64(r, dtfl_log_dd_operator(b, i));
		__dtfl_bc_u64(r, dtfl_log_dd_merge_scale(b, i));
		__dtfl_bc_u64(r, dtfl_log_dd_merge_length1(b, i));
		__dtfl_bc_u64(r, dtfl_log_dd_merge_length2(b, i));
		if (dtfl_log_dd_merge_complete(b, i, &x)) {
			__dtfl_bc_u8(r, 1);
			__dtfl_bc_u64(r, x);
		} else {
			__dtfl_bc_u8(r, 0);
		}
		break;
	case DTFL_LOG_DD_MERGE_SHORTFALL:
		__dtfl_bc_u32(r, 3);
		__dtfl_bc_u64(r, dtfl_log_dd_operator(b, i));
		__dtfl_bc_u64(r, dtfl_log_dd_shortfall_scale(b, i));
		__dtfl_bc_u64(r, dtfl_log_dd_shortfall(b, i));
		break;
	case DTFL_LOG_DD_TRACE_SHARE:
		__dtfl_bc_u32(r, 4);
		__dtfl_bc_u64(r, dtfl_log_dd_operator(b, i));
		__dtfl_bc_u64(r, (uint64_t)dtfl_log_dd_trace_share_diff(b, i));
		break;
	case DTFL_LOG_DD_BATCHER:
		__dtfl_bc_u32(r, 5);
		__dtfl_bc_u64(r, dtfl_log_dd_operator(b, i));
		dtfl_log_dd_batcher(b, i, &i0, &i1, &i2, &i3);
		__dtfl_bc_u64(r, (uint64_t)i0);
		__dtfl_bc_u64(r, (uint64_t)i1);
		__dtfl_bc_u64(r, (uint64_t)i2);
		__dtfl_bc_u64(r, (uint64_t)i3);
		break;
	case DTFL_LOG_COMM_MESSAGE:
		__dtfl_bc_u32(r, 0);
		__dtfl_bc_u8(r, dtfl_log_comm_message_is_send(b, i));
		dtfl_log_comm_message_header(b, i, &x, &y, &z, &w, &u, &v);
		__dtfl_bc_u64(r, x);
		__dtfl_bc_u64(r, y);
		__dtfl_bc_u64(r, z);
		__dtfl_bc_u64(r, w);
		__dtfl_bc_u64(r, u);
		__dtfl_bc_u64(r, v);
		break;
	case DTFL_LOG_COMM_STATE:
		__dtfl_bc_u32(r, 1);
		__dtfl_bc_u8(r, dtfl_log_comm_state_send(b, i));
		__dtfl_bc_u64(r, dtfl_log_comm_state_process(b, i));
		__dtfl_bc_u64(r, dtfl_log_comm_state_remote(b, i));
		__dtfl_bc_u8(r, dtfl_log_comm_state_start(b, i));
		break;
	case DTFL_LOG_COMM_SETUP:
		__dtfl_bc_u32(r, 2);
		__dtfl_bc_u8(r, dtfl_log_comm_setup_sender(b, i));
		__dtfl_bc_u64(r, dtfl_log_comm_setup_process(b, i));
		if (dtfl_log_comm_setup_remote(b, i, &x)) {
			__dtfl_bc_u8(r, 1);
			__dtfl_bc_u64(r, x);
		} else {
			__dtfl_bc_u8(r, 0);
		}
		break;
	case DTFL_LOG_UNKNOWN:
	default:
		break;
	}
}

void
fuzz_log_walk(const dtfl_log_batch *b, __dtfl_vec *out)
{
	size_t n = dtfl_log_batch_len(b), i;

	(void)dtfl_log_batch_time(b);
	(void)dtfl_log_batch_worker(b);
	FUZZ_ASSERT(dtfl_log_batch_stream(b) != NULL);
	/* Out-of-range and wrong-kind accessors return zero values. */
	FUZZ_ASSERT(dtfl_log_event_kind_at(b, n) == DTFL_LOG_UNKNOWN);
	FUZZ_ASSERT(dtfl_log_operates_name(b, n) == NULL);
	for (i = 0; i < n; i++) {
		__dtfl_vec rec;

		(void)dtfl_log_event_time(b, i);
		__dtfl_vec_init(&rec, 1);
		walk_one(b, i, &rec);
		if (rec.len > 0) {
			__dtfl_bc_u32(out, (uint32_t)rec.len);
			__dtfl_vec_push_n(out, rec.data, rec.len);
		}
		__dtfl_vec_free(&rec);
	}
}
