/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/log_codec_int.h: bincode-1 fixint little-endian writers.
 *
 * The encoding Rust's bincode 1.x uses by default, which every log and
 * capture stream at the pin uses (PLAN D9).  Each appends to a byte
 * vector (element size 1).
 */
#ifndef DTFL_LOG_CODEC_INT_H
#define DTFL_LOG_CODEC_INT_H

#include "base/base_int.h"

void __dtfl_bc_u8(__dtfl_vec *o, uint8_t v);
void __dtfl_bc_u32(__dtfl_vec *o, uint32_t v);
void __dtfl_bc_u64(__dtfl_vec *o, uint64_t v);
/* String: u64 length then UTF-8 bytes. */
void __dtfl_bc_str(__dtfl_vec *o, const char *s);
/* Vec<usize>: u64 length then u64 elements. */
void __dtfl_bc_usizes(__dtfl_vec *o, const size_t *v, size_t n);
/* Duration from nanoseconds: u64 seconds then u32 nanoseconds. */
void __dtfl_bc_duration(__dtfl_vec *o, uint64_t ns);

/*
 * A reader over a fixint little-endian bincode record.  It tracks a
 * cursor and an overrun flag; once a read runs past the end every
 * further read is a no-op and `bad` stays set, so a decoder can read a
 * whole record and check `bad` once at the end rather than at each step.
 */
typedef struct __dtfl_bc_reader {
	const unsigned char *p;
	size_t len;
	size_t off;
	bool bad;
} __dtfl_bc_reader;

void __dtfl_bc_reader_init(__dtfl_bc_reader *r, const void *data, size_t len);
uint8_t __dtfl_bc_read_u8(__dtfl_bc_reader *r);
uint32_t __dtfl_bc_read_u32(__dtfl_bc_reader *r);
uint64_t __dtfl_bc_read_u64(__dtfl_bc_reader *r);
/* isize/i64: read as u64 and reinterpret two's complement. */
int64_t __dtfl_bc_read_i64(__dtfl_bc_reader *r);
/* Duration -> nanoseconds (u64 seconds then u32 nanoseconds). */
uint64_t __dtfl_bc_read_duration(__dtfl_bc_reader *r);
/*
 * A borrowed string: a u64 length then that many bytes, returned as a
 * (pointer, length) slice into the record.  The slice is not
 * NUL-terminated; the decoder copies it when it needs a C string.
 */
const char *__dtfl_bc_read_str(__dtfl_bc_reader *r, size_t *slen);
/*
 * A Vec<usize> as a borrowed count and a cursor positioned at the first
 * element; the caller reads `*count` u64s with __dtfl_bc_read_u64.
 */
size_t __dtfl_bc_read_vec_len(__dtfl_bc_reader *r);

/*
 * Decodes a batch of raw event records and delivers it to a subscriber,
 * the inverse of the per-stream encoders.  Used by both the in-process
 * subscriber (src/timely/log.c) and the recorded-stream replay
 * (src/diag/replay_shutdown.c).  Each record in `recs` is one event in
 * the stream's own bincode encoding; `stream` selects the event enum
 * ("timely", "differential/arrange", "communication").  `time_ns` is
 * the batch's capability time and `worker` its worker index.  The
 * subscriber is called once with the decoded batch, valid only for that
 * call.  `scratch` is a dtfl_log_batch-sized workspace the caller owns
 * (its three vectors initialized once and reused across calls).
 */
typedef struct __dtfl_log_event __dtfl_log_event;	/* base/log_int.h */
void __dtfl_log_decode_and_deliver(void *scratch, const char *stream,
    size_t worker, uint64_t time_ns, const __dtfl_log_event *recs, size_t n,
    void (*fn)(const void *batch, void *ctx), void *ctx);
void *__dtfl_log_scratch_new(void);
void __dtfl_log_scratch_free(void *scratch);

#endif /* DTFL_LOG_CODEC_INT_H */
