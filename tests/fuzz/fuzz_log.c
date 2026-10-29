/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_log.c: the timely, differential and communication log
 * record decoder (src/timely/log.c decode_into, reached through
 * __dtfl_log_decode_and_deliver as the in-process subscriber and the
 * recorded-stream replay both reach it).
 *
 * Input: byte 0 picks the stream, the rest is a run of records, each a
 * u32 LE length then that many bytes (lengths are clamped to what is
 * left).  The decoded batch is read back through every public accessor
 * and re-encoded (fuzz_log_walk.c); decoding that and re-encoding again
 * must reproduce it byte for byte.
 */
#include "fuzz_log_walk.h"

#include "base/log_int.h"
#include "timely/log_codec_int.h"

static const char *const streams[3] = {
	"timely", "differential/arrange", "communication"
};

static void
collect(const void *batch, void *ctx)
{
	fuzz_log_walk(batch, ctx);
}

/* Splits u32-length-framed records; returns the count. */
static size_t
split(const unsigned char *p, size_t len, __dtfl_vec *recs)
{
	size_t off = 0;

	__dtfl_vec_clear(recs);
	while (len - off >= 4) {
		__dtfl_log_event ev;
		uint32_t n = (uint32_t)p[off] | (uint32_t)p[off + 1] << 8 |
		    (uint32_t)p[off + 2] << 16 | (uint32_t)p[off + 3] << 24;

		off += 4;
		if (n > len - off)
			n = (uint32_t)(len - off);
		ev.time_ns = off;
		ev.data = p + off;
		ev.len = n;
		__dtfl_vec_push(recs, &ev);
		off += n;
	}
	return recs->len;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const char *stream;
	unsigned char *copy;
	void *scratch;
	__dtfl_vec recs, out1, out2;

	if (size < 1)
		return 0;
	stream = streams[data[0] % 3];
	copy = fuzz_dup(data + 1, size - 1);
	scratch = __dtfl_log_scratch_new();
	__dtfl_vec_init(&recs, sizeof(__dtfl_log_event));
	__dtfl_vec_init(&out1, 1);
	__dtfl_vec_init(&out2, 1);

	split(copy, size - 1, &recs);
	__dtfl_log_decode_and_deliver(scratch, stream, 0, 7,
	    (const __dtfl_log_event *)(void *)recs.data, recs.len, collect,
	    &out1);
	split(out1.data, out1.len, &recs);
	__dtfl_log_decode_and_deliver(scratch, stream, 0, 7,
	    (const __dtfl_log_event *)(void *)recs.data, recs.len, collect,
	    &out2);
	FUZZ_ASSERT(out1.len == out2.len);
	FUZZ_ASSERT(out1.len == 0 || memcmp(out1.data, out2.data,
	    out1.len) == 0);

	__dtfl_vec_free(&out2);
	__dtfl_vec_free(&out1);
	__dtfl_vec_free(&recs);
	__dtfl_log_scratch_free(scratch);
	free(copy);
	return 0;
}
