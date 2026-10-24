/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_update_align.c: the embedded time of a collection
 * update must be safe to read even when the record length is not a
 * multiple of eight.
 *
 * A collection update is laid out record || time[depth] || diff
 * (collection_int.h).  __dtfl_update_time_copy returns the depth time
 * words through a caller-provided aligned buffer, so reading time[0] is
 * aligned for any record size.  Reading them in place as a uint64_t
 * would be a misaligned load for an odd-sized record (a (u64, u32)
 * twelve-byte record, say), which the sanitizer rejects and which
 * faults on strict-alignment targets.  These tests build odd-sized
 * records at depths one and two and check the words round-trip; under
 * the ASan/UBSan configurations a misaligned read would trap.
 */
#include "dtfl_test.h"
#include "dd/collection_int.h"

/*
 * Builds an update of `rlen` record bytes, `depth` time words and `dw`
 * diff bytes into `buf`, filling the record with 0xAA, the time with the
 * given words and the diff with 0xBB.  Returns the total length.
 */
static size_t
build_update(unsigned char *buf, size_t rlen, const uint64_t *tw,
    unsigned depth, size_t dw)
{
	size_t i, off = 0;

	for (i = 0; i < rlen; i++)
		buf[off++] = 0xAA;
	/* Native-order copy, matching the collection encoding. */
	memcpy(buf + off, tw, (size_t)depth * sizeof(uint64_t));
	off += (size_t)depth * sizeof(uint64_t);
	for (i = 0; i < dw; i++)
		buf[off++] = 0xBB;
	return off;
}

/*
 * A one-byte record at depth 1 (record length 1 is maximally
 * misaligned) and a twelve-byte record at depth 2 (the (u64, u32)
 * bfs record): the embedded time must read back exactly.
 */
DTFL_TEST(update_align__odd_record_time)
{
	/* An over-aligned backing buffer plus a one-byte lead so the record,
	 * and thus the embedded time, starts at an odd address. */
	_Alignas(8) unsigned char backing[64];
	unsigned char *buf = backing + 1;
	uint64_t t1[1] = { UINT64_C(0x0123456789abcdef) };
	uint64_t t2[2] = { UINT64_C(0x1111222233334444),
	    UINT64_C(0x5555666677778888) };
	uint64_t out[DTFL_TIME_MAX];
	const uint64_t *et;
	size_t total;

	(void)params; (void)data;

	/* depth 1, 1-byte record, 8-byte diff (odd time offset = 1). */
	total = build_update(buf, 1, t1, 1, 8);
	assert_size(__dtfl_update_rec_len(1, 8, total), ==, 1);
	et = __dtfl_update_time_copy(buf, 1, 8, total, out);
	assert_uint64(et[0], ==, t1[0]);

	/* depth 2, 12-byte record, 8-byte diff (odd time offset = 12). */
	total = build_update(buf, 12, t2, 2, 8);
	assert_size(__dtfl_update_rec_len(2, 8, total), ==, 12);
	et = __dtfl_update_time_copy(buf, 2, 8, total, out);
	assert_uint64(et[0], ==, t2[0]);
	assert_uint64(et[1], ==, t2[1]);

	/* depth 2, 12-byte record, no diff. */
	total = build_update(buf, 12, t2, 2, 0);
	assert_size(__dtfl_update_rec_len(2, 0, total), ==, 12);
	et = __dtfl_update_time_copy(buf, 2, 0, total, out);
	assert_uint64(et[0], ==, t2[0]);
	assert_uint64(et[1], ==, t2[1]);

	return MUNIT_OK;
}

/* The copy buffer the accessor returns is distinct from the record. */
DTFL_TEST(update_align__returns_caller_buffer)
{
	_Alignas(8) unsigned char buf[64];
	uint64_t t1[1] = { 42 };
	uint64_t out[DTFL_TIME_MAX];
	const uint64_t *et;
	size_t total;

	(void)params; (void)data;

	total = build_update(buf, 3, t1, 1, 4);
	et = __dtfl_update_time_copy(buf, 1, 4, total, out);
	assert_ptr_equal(et, out);
	assert_uint64(out[0], ==, 42);

	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(update_align__odd_record_time),
	DTFL_T(update_align__returns_caller_buffer),
	DTFL_T_END,
};

DTFL_TEST_MAIN("update_align", tests)
