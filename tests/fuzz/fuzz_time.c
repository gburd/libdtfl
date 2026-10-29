/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_time.c: the bincode time and summary codecs
 * (src/progress/time.c __dtfl_time_decode, __dtfl_summary_decode), the
 * T and T::Summary encodings of PLAN D9.
 *
 * Input: byte 0 picks the time class, byte 1 picks time or summary,
 * the rest is the encoded value.  A decode that succeeds must consume
 * no more than it was given, and re-encode to a value that decodes to
 * the same words in exactly __dtfl_time_encoded_len bytes.  The bytes
 * themselves need not match: the decoder accepts non-canonical forms
 * (trailing minimum PointStamp coordinates, Duration nanoseconds past
 * one second), as serde does.
 */
#include "fuzz.h"

#include "progress/time_int.h"

#define NCLASS 11
static dtfl_time_class *classes[NCLASS];

static dtfl_time_class *
both(dtfl_time_class *(*mk)(const dtfl_time_class *, const dtfl_time_class *),
    dtfl_time_class *a, dtfl_time_class *b)
{
	dtfl_time_class *c = mk(a, b);

	dtfl_time_class_free(a);
	dtfl_time_class_free(b);
	FUZZ_ASSERT(c != NULL);
	return c;
}

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const dtfl_coord u64 = { DTFL_COORD_UNSIGNED, 64 };
	const dtfl_coord dur = { DTFL_COORD_DURATION, 64 };
	dtfl_time_class *t;
	int i;

	(void)argc;
	(void)argv;
	classes[0] = dtfl_time_unit();
	classes[1] = dtfl_time_u64();
	classes[2] = dtfl_time_u32();
	classes[3] = dtfl_time_total((dtfl_coord){ DTFL_COORD_SIGNED, 32 });
	classes[4] = dtfl_time_duration();
	t = dtfl_time_u64();
	classes[5] = dtfl_time_altneu(t);
	dtfl_time_class_free(t);
	t = dtfl_time_duration();
	classes[6] = dtfl_time_altneu(t);
	dtfl_time_class_free(t);
	classes[7] = dtfl_time_pointstamp(u64, 3);
	classes[8] = dtfl_time_pointstamp(dur, 2);
	classes[9] = both(dtfl_time_product, dtfl_time_u64(),
	    dtfl_time_pointstamp(u64, 4));
	classes[10] = both(dtfl_time_lex,
	    dtfl_time_total((dtfl_coord){ DTFL_COORD_SIGNED, 16 }),
	    dtfl_time_total((dtfl_coord){ DTFL_COORD_UNSIGNED, 8 }));
	for (i = 0; i < NCLASS; i++)
		FUZZ_ASSERT(classes[i] != NULL);
	return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const dtfl_time_class *tc;
	bool summary;
	unsigned char *copy;
	uint64_t v[DTFL_TIME_MAX], v2[DTFL_TIME_MAX];
	size_t used, used2, len;
	unsigned words;
	__dtfl_vec enc;
	int rc;

	if (size < 2)
		return 0;
	tc = classes[data[0] % NCLASS];
	summary = (data[1] & 1) != 0;
	len = size - 2;
	copy = fuzz_dup(data + 2, len);
	words = summary ? dtfl_summary_depth(tc) : dtfl_time_depth(tc);
	rc = summary ? __dtfl_summary_decode(tc, copy, len, v, &used) :
	    __dtfl_time_decode(tc, copy, len, v, &used);
	FUZZ_ASSERT(rc == 0 || rc == DTFL_EPROTO || rc == DTFL_ERANGE);
	if (rc == 0) {
		FUZZ_ASSERT(used <= len);
		__dtfl_vec_init(&enc, 1);
		if (summary) {
			__dtfl_summary_encode(tc, v, &enc);
			rc = __dtfl_summary_decode(tc, enc.data, enc.len, v2,
			    &used2);
		} else {
			FUZZ_ASSERT(__dtfl_time_encode(tc, v, &enc) ==
			    __dtfl_time_encoded_len(tc, v));
			rc = __dtfl_time_decode(tc, enc.data, enc.len, v2,
			    &used2);
		}
		FUZZ_ASSERT(rc == 0);
		FUZZ_ASSERT(used2 == enc.len);
		FUZZ_ASSERT(words == 0 ||
		    memcmp(v, v2, words * sizeof(uint64_t)) == 0);
		__dtfl_vec_free(&enc);
	}
	free(copy);
	return 0;
}
