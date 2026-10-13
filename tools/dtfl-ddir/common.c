/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/lib.rs (load_program)
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tools/dtfl-ddir/common.c: shared helpers for the dtfl-ddir subcommands.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddir_tool.h"

char *
dtfl_ddir_slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;
	size_t got;

	if (f == NULL) {
		fprintf(stderr, "dtfl-ddir: cannot open %s\n", path);
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fprintf(stderr, "dtfl-ddir: cannot size %s\n", path);
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n + 1);
	if (buf == NULL) {
		fclose(f);
		fprintf(stderr, "dtfl-ddir: out of memory\n");
		return NULL;
	}
	got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	buf[got] = '\0';
	return buf;
}

dtfl_ddir_syntax
dtfl_ddir_syntax_of(const char *path)
{
	size_t n = strlen(path);

	if (n >= 4 && strcmp(path + n - 4, ".ddp") == 0)
		return DTFL_DDIR_PIPE;
	return DTFL_DDIR_APPLICATIVE;
}

dtfl_ddir_program *
dtfl_ddir_load(const char *path)
{
	char *src = dtfl_ddir_slurp(path);
	dtfl_ddir_program *p = NULL;
	int rc;

	if (src == NULL)
		return NULL;
	rc = dtfl_ddir_parse(src, dtfl_ddir_syntax_of(path), &p);
	free(src);
	if (rc != 0) {
		fprintf(stderr, "dtfl-ddir: parse error in %s: %s\n", path,
		    dtfl_last_error_detail());
		return NULL;
	}
	return p;
}
