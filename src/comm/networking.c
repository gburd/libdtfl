/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/networking.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/networking.c: MessageHeader framing for byte transmissions.
 *
 * The header is six u64 fields in big-endian byte order, written and
 * read in declaration order.  The socket setup in networking.rs
 * (create_sockets, start_connections, await_connections) lands with the
 * Tcp allocator.
 */
#include "comm/zc_int.h"

/* Stores v at p as 8 big-endian bytes. */
static void
put_be64(unsigned char *p, uint64_t v)
{
	int i;

	for (i = 7; i >= 0; i--) {
		p[i] = (unsigned char)(v & 0xff);
		v >>= 8;
	}
}

/* Loads 8 big-endian bytes at p. */
static uint64_t
get_be64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++)
		v = (v << 8) | p[i];
	return v;
}

/*
 * MessageHeader::try_read.  Fills *h and returns true when `bytes` holds
 * a whole header and its `length` payload bytes; returns false (reading
 * nothing past len) otherwise.  *h is unspecified on false.
 */
bool
__dtfl_header_try_read(__dtfl_message_header *h, const unsigned char *bytes,
    size_t len)
{
	uint64_t f[DTFL_HEADER_FIELDS];
	int i;

	if (len < DTFL_HEADER_BYTES)
		return false;
	for (i = 0; i < DTFL_HEADER_FIELDS; i++)
		f[i] = get_be64(bytes + 8 * i);
	/* Order must match writing order. */
	h->channel = (size_t)f[0];
	h->source = (size_t)f[1];
	h->target_lower = (size_t)f[2];
	h->target_upper = (size_t)f[3];
	h->length = (size_t)f[4];
	h->seqno = (size_t)f[5];
	/* required_bytes wraps in Rust release builds; never trust it here. */
	return h->length <= len - DTFL_HEADER_BYTES;
}

/* MessageHeader::write_to: writes exactly DTFL_HEADER_BYTES to out. */
void
__dtfl_header_write_to(const __dtfl_message_header *h, unsigned char *out)
{
	/* Order must match reading order. */
	put_be64(out + 0, h->channel);
	put_be64(out + 8, h->source);
	put_be64(out + 16, h->target_lower);
	put_be64(out + 24, h->target_upper);
	put_be64(out + 32, h->length);
	put_be64(out + 40, h->seqno);
}

/* The number of bytes required for the header and data. */
size_t
__dtfl_header_required_bytes(const __dtfl_message_header *h)
{
	return __dtfl_header_header_bytes(h) + h->length;
}

/* The number of bytes required for the header. */
size_t
__dtfl_header_header_bytes(const __dtfl_message_header *h)
{
	(void)h;
	return DTFL_HEADER_BYTES;
}
