/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_comm_msg.h: a message class for communication tests.
 * A message is a heap-allocated uint64_t.  `test_msg_live` counts
 * messages alive (atomic: they are freed on whichever thread drops
 * them), so a test can check that every message is freed exactly once.
 */
#ifndef DTFL_TEST_COMM_MSG_H
#define DTFL_TEST_COMM_MSG_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "comm/comm_int.h"

static atomic_long test_msg_live;
static atomic_long test_msg_clones;

static void *
test_msg_new(uint64_t v)
{
	uint64_t *m = malloc(sizeof *m);

	if (m == NULL)
		abort();
	*m = v;
	/* relaxed: a statistic; tests read it after joining threads. */
	atomic_fetch_add_explicit(&test_msg_live, 1, memory_order_relaxed);
	return m;
}

static uint64_t
test_msg_val(const void *m)
{
	return *(const uint64_t *)m;
}

static size_t
test_msg_encoded_len(const __dtfl_msg_class *mc, const void *m)
{
	(void)mc; (void)m;
	return sizeof(uint64_t);
}

static void
test_msg_encode(const __dtfl_msg_class *mc, const void *m, unsigned char *out)
{
	(void)mc;
	memcpy(out, m, sizeof(uint64_t));
}

static void *
test_msg_decode(const __dtfl_msg_class *mc, const __dtfl_bytes *b)
{
	uint64_t v;

	(void)mc;
	if (b->len != sizeof v)
		return NULL;
	memcpy(&v, b->ptr, sizeof v);
	return test_msg_new(v);
}

static void *
test_msg_clone(const __dtfl_msg_class *mc, const void *m)
{
	(void)mc;
	atomic_fetch_add_explicit(&test_msg_clones, 1, memory_order_relaxed);
	return test_msg_new(test_msg_val(m));
}

static void
test_msg_free(const __dtfl_msg_class *mc, void *m)
{
	(void)mc;
	atomic_fetch_sub_explicit(&test_msg_live, 1, memory_order_relaxed);
	free(m);
}

static const __dtfl_msg_class test_msg_class = {
	"u64", test_msg_encoded_len, test_msg_encode, test_msg_decode,
	test_msg_clone, test_msg_free, NULL
};

/* A second class, for the cast check. */
static const __dtfl_msg_class test_msg_class2 = {
	"u64b", test_msg_encoded_len, test_msg_encode, test_msg_decode,
	test_msg_clone, test_msg_free, NULL
};

/* Payload layout: sender, channel and sequence number in one word. */
#define TM_PACK(src, chan, seq) \
	(((uint64_t)(src) << 48) | ((uint64_t)(chan) << 32) | (uint64_t)(seq))
#define TM_SRC(v) ((size_t)((v) >> 48))
#define TM_CHAN(v) ((size_t)(((v) >> 32) & 0xffff))
#define TM_SEQ(v) ((size_t)((v) & 0xffffffff))

#endif /* DTFL_TEST_COMM_MSG_H */
