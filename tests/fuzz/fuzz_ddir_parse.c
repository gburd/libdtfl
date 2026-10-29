/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_ddir_parse.c: the DDIR program parser and lowering
 * (dtfl_ddir_parse: src/ddir/parse_pipe.c, parse_applicative.c, lower.c
 * and the optimizer), the path dtfl-ddir and the ddir server take for
 * every program file they are handed.
 *
 * Input: byte 0 picks the syntax (even: pipe, odd: applicative), the
 * rest is the program text.  dtfl_ddir_parse must return 0 with a
 * program or a DTFL_E* code with none; it must not crash, leak or
 * abort.  A parsed program is queried and cloned (the identity rewrite
 * explain builds on), so the lowered IR is walked too.
 */
#include "fuzz.h"

#include "dtfl/ddir.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	dtfl_ddir_program *p = NULL, *c = NULL;
	dtfl_ddir_syntax syn;
	char *text;
	int rc;

	if (size < 1)
		return 0;
	syn = (data[0] & 1) != 0 ? DTFL_DDIR_APPLICATIVE : DTFL_DDIR_PIPE;
	text = fuzz_str(data + 1, size - 1);
	rc = dtfl_ddir_parse(text, syn, &p);
	if (rc == 0) {
		size_t i, n;

		FUZZ_ASSERT(p != NULL);
		(void)dtfl_ddir_program_input_count(p);
		n = dtfl_ddir_program_export_count(p);
		for (i = 0; i < n; i++)
			FUZZ_ASSERT(dtfl_ddir_program_export_name(p, i) != NULL);
		if (dtfl_ddir_clone_identity(p, &c) == 0)
			dtfl_ddir_program_free(c);
		dtfl_ddir_program_free(p);
	} else {
		FUZZ_ASSERT(p == NULL);
	}
	free(text);
	return 0;
}
