/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_progress.c: the progress message codec
 * (src/progress/progcaster.c pm_decode).
 *
 * A peer's bytes for one progress message: source, seq, count, key
 * words, then count (key, delta) entries.  The decoder may compact the
 * change batch as it fills it, so the round trip is semantic: decode,
 * encode, decode again, and the two compacted batches must be equal.
 */
#include "fuzz.h"

#include "comm/comm_int.h"
#include "progress/operate_int.h"

static void *
decode(const __dtfl_msg_class *mc, unsigned char *p, size_t len)
{
	__dtfl_bytes b;

	b.store = NULL;
	b.ptr = p;
	b.len = len;
	b.writable = false;
	return mc->decode(mc, &b);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const __dtfl_msg_class *mc = __dtfl_progress_class();
	unsigned char *copy = fuzz_dup(data, size);
	__dtfl_progress_msg *m, *m2;
	unsigned char *out;
	size_t n, i, len;

	m = decode(mc, copy, size);
	if (m == NULL) {
		free(copy);
		return 0;
	}
	n = mc->encoded_len(mc, m);
	out = malloc(n);
	FUZZ_ASSERT(out != NULL);
	mc->encode(mc, m, out);
	m2 = decode(mc, out, n);
	FUZZ_ASSERT(m2 != NULL);
	FUZZ_ASSERT(m2->source == m->source && m2->seq == m->seq);
	FUZZ_ASSERT(m2->changes.kw == m->changes.kw);
	len = __dtfl_cb_iter(&m->changes);
	FUZZ_ASSERT(__dtfl_cb_iter(&m2->changes) == len);
	for (i = 0; i < len; i++) {
		FUZZ_ASSERT(memcmp(__dtfl_cb_key(&m->changes, i),
		    __dtfl_cb_key(&m2->changes, i),
		    m->changes.kw * sizeof(uint64_t)) == 0);
		FUZZ_ASSERT(__dtfl_cb_delta(&m->changes, i) ==
		    __dtfl_cb_delta(&m2->changes, i));
		/*
		 * Compaction drops zero entries, except that upstream's
		 * ChangeBatch::compact skips a batch of one entry, so a lone
		 * zero delta stays (change_batch.rs at timely-v0.31.0).
		 */
		FUZZ_ASSERT(len == 1 || __dtfl_cb_delta(&m->changes, i) != 0);
	}
	mc->free(mc, mc->clone(mc, m));
	mc->free(mc, m2);
	mc->free(mc, m);
	free(out);
	free(copy);
	return 0;
}
