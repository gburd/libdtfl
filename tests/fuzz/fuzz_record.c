/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_record.c: the record descriptors (src/base/record.c),
 * which size, compare, hash and print record bytes that arrive from a
 * peer in a batch, from a capture file, or from a spill file.
 *
 * Input: byte 0 picks a descriptor (built-ins and composites: tuple,
 * option, vec, nested), byte 1 a split point, the rest is bytes.  For
 * the whole span and for the two halves:
 *   - value_len is SIZE_MAX or at most the bytes given;
 *   - on a value that is exactly the span, cmp is reflexive, and fmt
 *     and hash run;
 *   - cmp is antisymmetric between the two halves.
 * The batch codec (__dtfl_batch_decode) is checked too: a decoded batch
 * re-encodes to exactly its input.
 */
#include "fuzz.h"

#include "base/batch_int.h"
#include "dtfl/record.h"

#define NTYPES 12
static const dtfl_type *types[NTYPES];
static FILE *sink;

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const dtfl_type *f3[3];

	(void)argc;
	(void)argv;
	types[0] = dtfl_type_u64();
	types[1] = dtfl_type_i32();
	types[2] = dtfl_type_bytes();
	types[3] = dtfl_type_string();
	types[4] = dtfl_type_pair(dtfl_type_u64(), dtfl_type_string());
	types[5] = dtfl_type_option(dtfl_type_i64());
	types[6] = dtfl_type_vec(dtfl_type_u16());
	types[7] = dtfl_type_vec(dtfl_type_string());
	types[8] = dtfl_type_vec(dtfl_type_unit());
	types[9] = dtfl_type_option(dtfl_type_vec(dtfl_type_option(
	    dtfl_type_bytes())));
	f3[0] = dtfl_type_string();
	f3[1] = dtfl_type_vec(dtfl_type_pair(dtfl_type_u8(),
	    dtfl_type_string()));
	f3[2] = dtfl_type_bytes();
	types[10] = dtfl_type_tuple(f3, 3);
	types[11] = dtfl_type_vec(dtfl_type_vec(dtfl_type_unit()));
	sink = fopen("/dev/null", "w");
	FUZZ_ASSERT(sink != NULL);
	return 0;
}

static int
sign(int c)
{
	return (c > 0) - (c < 0);
}

static void
one(const dtfl_type *t, const unsigned char *p, size_t len)
{
	size_t n = dtfl_type_value_len(t, p, len);

	FUZZ_ASSERT(n == SIZE_MAX || n <= len);
	if (n == len) {
		FUZZ_ASSERT(dtfl_type_cmp(t, p, len, p, len) == 0);
		(void)dtfl_type_hash(t, p, len);
	}
	FUZZ_ASSERT(dtfl_type_fmt(t, sink, p, len) >= 0);
}

static void
batch(const unsigned char *p, size_t len)
{
	__dtfl_batch *b = __dtfl_batch_decode(p, len);
	unsigned char *out;
	size_t n;

	if (b == NULL)
		return;
	n = __dtfl_batch_encoded_len(b);
	FUZZ_ASSERT(n == len);
	out = malloc(n);
	FUZZ_ASSERT(out != NULL);
	__dtfl_batch_encode(b, out);
	FUZZ_ASSERT(memcmp(out, p, n) == 0);
	free(out);
	__dtfl_batch_release(b);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const dtfl_type *t;
	unsigned char *a, *b, *all;
	size_t len, cut;

	if (size < 2)
		return 0;
	t = types[data[0] % NTYPES];
	len = size - 2;
	cut = len == 0 ? 0 : data[1] % (len + 1);
	all = fuzz_dup(data + 2, len);
	a = fuzz_dup(data + 2, cut);
	b = fuzz_dup(data + 2 + cut, len - cut);
	one(t, all, len);
	one(t, a, cut);
	one(t, b, len - cut);
	FUZZ_ASSERT(sign(dtfl_type_cmp(t, a, cut, b, len - cut)) ==
	    -sign(dtfl_type_cmp(t, b, len - cut, a, cut)));
	batch(all, len);
	free(b);
	free(a);
	free(all);
	return 0;
}
