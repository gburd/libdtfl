/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz.h: shared helpers for the libFuzzer targets (PLAN
 * section 6 item 8).
 *
 * Each target defines LLVMFuzzerTestOneInput and calls one decoder of
 * untrusted bytes through the internal API.  A failed FUZZ_ASSERT, a
 * libdtfl panic (the default hook aborts), a sanitizer report or a leak
 * is a crash; libFuzzer saves the input that caused it.  Targets build
 * only under ./configure --enable-fuzz (tests/fuzz/local.mk).
 */
#ifndef DTFL_FUZZ_H
#define DTFL_FUZZ_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE	/* mkdtemp, setenv, pwrite, socketpair */
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

#define FUZZ_ASSERT(cond)						\
	do {								\
		if (!(cond)) {						\
			fprintf(stderr, "fuzz assertion failed: %s (%s:%d)\n", \
			    #cond, __FILE__, __LINE__);			\
			abort();					\
		}							\
	} while (0)

/* A heap copy of the input, so ASan sees an overread of exactly size. */
static inline unsigned char *
fuzz_dup(const uint8_t *data, size_t size)
{
	unsigned char *p = malloc(size != 0 ? size : 1);

	FUZZ_ASSERT(p != NULL);
	if (size != 0)
		memcpy(p, data, size);
	return p;
}

/* A NUL-terminated heap copy of the input (embedded NULs truncate it). */
static inline char *
fuzz_str(const uint8_t *data, size_t size)
{
	char *p = malloc(size + 1);

	FUZZ_ASSERT(p != NULL);
	if (size != 0)
		memcpy(p, data, size);
	p[size] = '\0';
	return p;
}

#endif /* DTFL_FUZZ_H */
