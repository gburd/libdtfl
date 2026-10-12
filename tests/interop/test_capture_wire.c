/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/interop/test_capture_wire.c: libdtfl's capture wire bytes against
 * real timely's EventWriter output (PLAN D9).
 *
 * Reads tests/interop/fixtures/capture_wire.bin, which real timely at the
 * pin produced through its own EventWriter (dev/gen-fixtures.d/
 * capture_wire.rs): for each event, a u64 LE serialized_size then the
 * bincode-1 Event<u64, Vec<u64>>.  The stream is
 *   event 0: Progress(vec![(0, 1), (3, -1)])
 *   event 1: Messages(0, vec![10, 20, 30])
 *
 * WHAT IS PROVEN BYTE-IDENTICAL TO RUST
 *   - the outer u64 LE length framing on every event;
 *   - the u32 LE Event variant tag (Progress = 0, Messages = 1);
 *   - the Progress event body for a depth-1 u64 time: a u64 LE count,
 *     then per entry a u64 LE time word and an i64 LE delta.  The C
 *     encoder (src/timely/event.c __dtfl_event_encode) is asked to encode
 *     the SAME logical Progress event and its bytes are compared to the
 *     Rust Progress event byte for byte.
 *
 * WHAT IS NOT BYTE-IDENTICAL (and why)
 *   - the Messages PAYLOAD.  Rust bincodes the typed value C (here a
 *     Vec<u64>: a u64 LE length then little-endian u64 elements).  libdtfl
 *     does not encode records as bincode of a typed value; a record is a
 *     byte string under its own type descriptor (PLAN D1), and a Messages
 *     event carries libdtfl's batch byte-arena layout (batch_int.h), not
 *     bincode of a Rust value.  So for Messages this test asserts only the
 *     shared prefix that both sides agree on: the length framing, the tag,
 *     and the depth-1 u64 time word.  The payload divergence is by design,
 *     documented here rather than hidden by a weaker assertion.
 */
#include "dtfl_test.h"

#include "dtfl/record.h"
#include "dtfl/time.h"

#include "timely/capture_int.h"
#include "progress/time_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE "tests/interop/fixtures/capture_wire.bin"

static uint64_t
rd_u64_le(const unsigned char *p)
{
	uint64_t v = 0;
	size_t i;

	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static uint32_t
rd_u32_le(const unsigned char *p)
{
	uint32_t v = 0;
	size_t i;

	for (i = 0; i < 4; i++)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}

static unsigned char *
slurp(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	long n;
	unsigned char *buf;

	if (f == NULL) {
		const char *base[2] = { getenv("top_srcdir"), getenv("srcdir") };
		size_t k;
		char alt[1024];

		for (k = 0; k < 2 && f == NULL; k++) {
			if (base[k] == NULL)
				continue;
			snprintf(alt, sizeof alt, "%s/%s", base[k], path);
			f = fopen(alt, "rb");
		}
	}
	munit_assert_not_null(f);
	munit_assert_int(fseek(f, 0, SEEK_END), ==, 0);
	n = ftell(f);
	munit_assert_long(n, >, 0);
	munit_assert_int(fseek(f, 0, SEEK_SET), ==, 0);
	buf = malloc((size_t)n);
	munit_assert_not_null(buf);
	munit_assert_size(fread(buf, 1, (size_t)n, f), ==, (size_t)n);
	fclose(f);
	*out_len = (size_t)n;
	return buf;
}

DTFL_TEST(capture_wire_matches_rust)
{
	size_t flen, off;
	unsigned char *buf;
	dtfl_time_class *tc = dtfl_time_u64();	/* depth-1 u64 time */
	uint64_t p0_len, p1_len;
	const unsigned char *p0, *p1;

	(void)params; (void)data;
	buf = slurp(FIXTURE, &flen);
	munit_assert_size(flen, >=, 16);

	/* Event 0: Progress.  Read its outer u64 LE length and body. */
	p0_len = rd_u64_le(buf);
	munit_assert_size(8 + (size_t)p0_len, <=, flen);
	p0 = buf + 8;
	/* Rust bincode: u32 LE variant tag. */
	munit_assert_uint32(rd_u32_le(p0), ==, DTFL_EVENT_PROGRESS);

	/*
	 * Build the SAME logical Progress event in libdtfl and encode it with
	 * the capture codec.  The whole framed event (u64 LE length + body)
	 * must be byte-identical to the Rust framed Progress event.
	 */
	{
		dtfl_event e;
		uint64_t t0 = 0, t3 = 3;
		__dtfl_vec enc;
		size_t rust_total = 8 + (size_t)p0_len;

		e.tag = DTFL_EVENT_PROGRESS;
		e.depth = tc->depth;
		__dtfl_cb_init(&e.u.progress, tc->depth);
		__dtfl_cb_update(&e.u.progress, &t0, 1);
		__dtfl_cb_update(&e.u.progress, &t3, -1);

		__dtfl_vec_init(&enc, 1);
		__dtfl_event_encode(tc, &e, &enc);

		munit_assert_size(enc.len, ==, rust_total);
		munit_assert_memory_equal(rust_total, enc.data, buf);

		__dtfl_vec_free(&enc);
		__dtfl_cb_free(&e.u.progress);
	}

	/* Event 1: Messages.  Shared prefix only (payload diverges by design). */
	off = 8 + (size_t)p0_len;
	munit_assert_size(off + 8, <=, flen);
	p1_len = rd_u64_le(buf + off);
	munit_assert_size(off + 8 + (size_t)p1_len, <=, flen);
	p1 = buf + off + 8;
	/* tag == Messages (u32 LE). */
	munit_assert_size((size_t)p1_len, >=, 4 + 8);
	munit_assert_uint32(rd_u32_le(p1), ==, DTFL_EVENT_MESSAGES);
	/*
	 * The depth-1 u64 time follows the tag in BOTH encodings: Rust writes
	 * bincode of T = u64 (8 bytes LE), libdtfl writes the time word with
	 * __dtfl_bc_u64 (8 bytes LE).  The captured time is 0.
	 */
	munit_assert_uint64(rd_u64_le(p1 + 4), ==, 0);
	/*
	 * Beyond the time the payloads differ: Rust has bincode(Vec<u64>) =
	 * {u64 LE len = 3}{10, 20, 30 as u64 LE}; libdtfl would carry its own
	 * batch byte-arena layout.  We assert the Rust side matches its own
	 * documented shape (so the fixture is what we think it is) but do NOT
	 * claim libdtfl produces these payload bytes.
	 */
	munit_assert_uint64(rd_u64_le(p1 + 4 + 8), ==, 3);	/* Vec len */
	munit_assert_uint64(rd_u64_le(p1 + 4 + 8 + 8), ==, 10);
	munit_assert_uint64(rd_u64_le(p1 + 4 + 8 + 16), ==, 20);
	munit_assert_uint64(rd_u64_le(p1 + 4 + 8 + 24), ==, 30);

	/* The fixture is exactly two framed events, nothing trailing. */
	munit_assert_size(off + 8 + (size_t)p1_len, ==, flen);

	free(buf);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(capture_wire_matches_rust),
	DTFL_T_END
};

DTFL_TEST_MAIN("interop", tests)
