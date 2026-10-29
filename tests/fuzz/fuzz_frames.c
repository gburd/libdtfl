/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_frames.c: the MessageHeader framing of the TCP data
 * plane (src/comm/networking.c) and the BytesSlab receive buffer it is
 * carved from (src/comm/zc_bytes_slab.c), driven the way recv_loop in
 * src/comm/zc_tcp.c drives them.
 *
 * Input: byte 0 picks the read size (1 to 256 bytes per "recv"), the
 * rest is the byte stream from the peer.  Checks:
 *   - try_read never reports a frame longer than the bytes available;
 *   - write_to(try_read(bytes)) reproduces the 48 header bytes;
 *   - the frames carved off tile the stream exactly, in order.
 */
#include "fuzz.h"

#include "comm/zc_int.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	__dtfl_bytes_slab slab;
	size_t chunk, at = 1, consumed = 1;

	if (size < 1)
		return 0;
	chunk = (size_t)data[0] + 1;
	__dtfl_slab_init(&slab, 6);
	while (at < size) {
		unsigned char *empty, *valid;
		size_t cap, n, valid_len;

		__dtfl_slab_ensure_capacity(&slab, 1);
		empty = __dtfl_slab_empty(&slab, &cap);
		FUZZ_ASSERT(cap > 0);
		n = size - at < chunk ? size - at : chunk;
		n = n < cap ? n : cap;
		memcpy(empty, data + at, n);
		__dtfl_slab_make_valid(&slab, n);
		at += n;

		for (;;) {
			__dtfl_message_header h;
			__dtfl_bytes frame;
			unsigned char again[DTFL_HEADER_BYTES];
			size_t need;

			valid = __dtfl_slab_valid(&slab, &valid_len);
			if (!__dtfl_header_try_read(&h, valid, valid_len))
				break;
			need = __dtfl_header_required_bytes(&h);
			FUZZ_ASSERT(need >= DTFL_HEADER_BYTES);
			FUZZ_ASSERT(need <= valid_len);
			if (sizeof(size_t) == 8) {
				__dtfl_header_write_to(&h, again);
				FUZZ_ASSERT(memcmp(again, valid,
				    DTFL_HEADER_BYTES) == 0);
			}
			__dtfl_slab_extract(&slab, need, &frame);
			FUZZ_ASSERT(frame.len == need);
			FUZZ_ASSERT(memcmp(frame.ptr, data + consumed,
			    need) == 0);
			consumed += need;
			__dtfl_bytes_release(&frame);
		}
	}
	/* What is left in the slab is exactly the unconsumed tail. */
	{
		size_t valid_len;
		unsigned char *valid = __dtfl_slab_valid(&slab, &valid_len);

		FUZZ_ASSERT(consumed + valid_len == size);
		FUZZ_ASSERT(valid_len == 0 ||
		    memcmp(valid, data + consumed, valid_len) == 0);
	}
	__dtfl_slab_free(&slab);
	return 0;
}
