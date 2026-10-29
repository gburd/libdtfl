/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_event.c: the capture/replay event codec, the
 * EventReader framing (src/timely/event.c __dtfl_event_decode, as
 * dtfl_event_reader_replay in src/timely/replay.c calls it).
 *
 * Input: byte 0 picks the time class (unit, u64, a two-coordinate
 * product, a pointstamp), the rest is the byte stream from the capture
 * file or socket.  Frames are decoded one after another as the replay
 * operator does.  For each decoded event the round trip holds: encode,
 * decode again, and the event is the same (a Messages body re-encodes
 * byte for byte; a Progress batch is compared after compaction, since
 * the encoder writes it compacted).
 */
#include "fuzz.h"

#include "dtfl/record.h"
#include "progress/progress_int.h"
#include "timely/capture_int.h"

static dtfl_time_class *classes[4];

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	dtfl_time_class *a, *b;

	(void)argc;
	(void)argv;
	classes[0] = dtfl_time_unit();
	classes[1] = dtfl_time_u64();
	a = dtfl_time_u64();
	b = dtfl_time_u32();
	classes[2] = dtfl_time_product(a, b);
	dtfl_time_class_free(a);
	dtfl_time_class_free(b);
	classes[3] = dtfl_time_pointstamp(
	    (dtfl_coord){ DTFL_COORD_UNSIGNED, 64 }, 3);
	return 0;
}

static void
same_event(const dtfl_time_class *tc, dtfl_event *a, dtfl_event *b)
{
	unsigned d = dtfl_time_depth(tc), i;

	FUZZ_ASSERT(a->tag == b->tag && a->depth == d && b->depth == d);
	if (a->tag == DTFL_EVENT_PROGRESS) {
		size_t n = __dtfl_cb_iter(&a->u.progress), k;

		FUZZ_ASSERT(__dtfl_cb_iter(&b->u.progress) == n);
		for (k = 0; k < n; k++) {
			FUZZ_ASSERT(d == 0 ||
			    memcmp(__dtfl_cb_key(&a->u.progress, k),
			    __dtfl_cb_key(&b->u.progress, k),
			    d * sizeof(uint64_t)) == 0);
			FUZZ_ASSERT(__dtfl_cb_delta(&a->u.progress, k) ==
			    __dtfl_cb_delta(&b->u.progress, k));
		}
	} else {
		const __dtfl_batch *x = a->u.messages.data;
		const __dtfl_batch *y = b->u.messages.data;
		size_t k;

		for (i = 0; i < d; i++)
			FUZZ_ASSERT(a->u.messages.time[i] ==
			    b->u.messages.time[i]);
		FUZZ_ASSERT(__dtfl_batch_len(x) == __dtfl_batch_len(y));
		for (k = 0; k < __dtfl_batch_len(x); k++) {
			size_t lx, ly;
			const void *rx = __dtfl_batch_rec(x, k, &lx);
			const void *ry = __dtfl_batch_rec(y, k, &ly);

			FUZZ_ASSERT(lx == ly);
			FUZZ_ASSERT(lx == 0 || memcmp(rx, ry, lx) == 0);
		}
	}
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const dtfl_time_class *tc;
	const dtfl_type *type = dtfl_type_u64();
	unsigned char *copy;
	size_t off = 0, len;

	if (size < 1)
		return 0;
	tc = classes[data[0] % 4];
	len = size - 1;
	copy = fuzz_dup(data + 1, len);
	for (;;) {
		dtfl_event e, e2;
		size_t consumed, consumed2;
		__dtfl_vec enc;

		if (!__dtfl_event_decode(tc, type, copy + off, len - off, &e,
		    &consumed))
			break;
		FUZZ_ASSERT(consumed >= 8 && consumed <= len - off);
		__dtfl_vec_init(&enc, 1);
		__dtfl_event_encode(tc, &e, &enc);
		if (e.tag == DTFL_EVENT_MESSAGES) {
			FUZZ_ASSERT(enc.len == consumed);
			FUZZ_ASSERT(memcmp(enc.data, copy + off, consumed) == 0);
		}
		FUZZ_ASSERT(__dtfl_event_decode(tc, type, enc.data, enc.len,
		    &e2, &consumed2));
		FUZZ_ASSERT(consumed2 == enc.len);
		same_event(tc, &e, &e2);
		__dtfl_event_clear(&e2);
		__dtfl_event_clear(&e);
		__dtfl_vec_free(&enc);
		off += consumed;
	}
	free(copy);
	return 0;
}
