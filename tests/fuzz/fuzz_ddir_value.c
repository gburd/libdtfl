/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_ddir_value.c: the DDIR value parser and the DDIR
 * value record codec.
 *
 * Byte 0 selects the half:
 *   even: the rest is a closed scalar term, the text a ddir server
 *     session's `feed` command hands to dtfl_ddir_value_parse
 *     (src/ddir/ddir.c over the pipe term parser and evaluator).  A
 *     value that parses must format, and encode/decode to an equal
 *     value.
 *   odd: the rest is record bytes for the DDIR value type
 *     (src/ddir/backend_vec.c dd_value_type).  value_len decides
 *     whether they hold one well-formed value; when it says so,
 *     dd_value_decode must read exactly that many bytes and re-encode
 *     them byte for byte, and the type's cmp must order the value
 *     equal to itself.
 */
#include "fuzz.h"

#include "base/base_int.h"
#include "ddir/ddir_int.h"

static void
value_round_trip(const dd_value *v)
{
	uint8_t *buf = NULL;
	size_t len = 0, cap = 0;
	const uint8_t *p;
	dd_value *back;

	dd_value_encode(v, &buf, &len, &cap);
	FUZZ_ASSERT(dtfl_type_value_len(dd_value_type(), buf, len) == len);
	p = buf;
	back = dd_value_decode(&p, buf + len);
	FUZZ_ASSERT(p == buf + len);
	FUZZ_ASSERT(dd_value_cmp(v, back) == 0);
	FUZZ_ASSERT(dtfl_type_cmp(dd_value_type(), buf, len, buf, len) == 0);
	dd_value_free(back);
	__dtfl_free(buf);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 1)
		return 0;
	if ((data[0] & 1) == 0) {
		char *text = fuzz_str(data + 1, size - 1);
		dtfl_ddir_value *v = dtfl_ddir_value_parse(text);

		if (v != NULL) {
			char small[16];
			size_t need = dtfl_ddir_value_format(v, small,
			    sizeof small);
			char *full = malloc(need + 1);

			FUZZ_ASSERT(full != NULL);
			FUZZ_ASSERT(dtfl_ddir_value_format(v, full, need + 1) ==
			    need);
			FUZZ_ASSERT(strlen(full) == need);
			value_round_trip(v);
			free(full);
			dtfl_ddir_value_free(v);
		}
		free(text);
	} else {
		size_t len = size - 1, n;
		unsigned char *copy = fuzz_dup(data + 1, len);

		n = dtfl_type_value_len(dd_value_type(), copy, len);
		if (n != SIZE_MAX) {
			const uint8_t *p = copy;
			dd_value *v;
			uint8_t *buf = NULL;
			size_t blen = 0, cap = 0;

			FUZZ_ASSERT(n <= len);
			v = dd_value_decode(&p, copy + n);
			FUZZ_ASSERT(p == copy + n);
			dd_value_encode(v, &buf, &blen, &cap);
			FUZZ_ASSERT(blen == n);
			FUZZ_ASSERT(memcmp(buf, copy, n) == 0);
			FUZZ_ASSERT(dtfl_type_cmp(dd_value_type(), copy, n,
			    copy, n) == 0);
			__dtfl_free(buf);
			dd_value_free(v);
		}
		(void)dtfl_type_value_len(dd_pair_type(), copy, len);
		free(copy);
	}
	return 0;
}
