/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_message.c: the data-channel message codec
 * (src/timely/message.c msg_decode, over __dtfl_batch_decode).
 *
 * A peer's bytes for one Message: from, seq, depth, the time words and
 * the batch.  The decoder accepts only the canonical encoding, so a
 * decoded message must re-encode to exactly the input bytes.
 */
#include "fuzz.h"

#include "comm/comm_int.h"
#include "timely/timely_int.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const __dtfl_msg_class *mc = __dtfl_message_class();
	unsigned char *copy = fuzz_dup(data, size);
	__dtfl_bytes b;
	void *m;

	b.store = NULL;
	b.ptr = copy;
	b.len = size;
	b.writable = false;
	m = mc->decode(mc, &b);
	if (m != NULL) {
		size_t n = mc->encoded_len(mc, m);
		unsigned char *out = malloc(n != 0 ? n : 1);
		void *c;

		FUZZ_ASSERT(out != NULL);
		mc->encode(mc, m, out);
		FUZZ_ASSERT(n == size);
		FUZZ_ASSERT(memcmp(out, copy, size) == 0);
		c = mc->clone(mc, m);
		mc->free(mc, c);
		mc->free(mc, m);
		free(out);
	}
	free(copy);
	return 0;
}
