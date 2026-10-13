/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/examples/dump_explain.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tools/dtfl-ddir/explain_cmd.c: the `explain` subcommand (dump_explain).
 *
 * Report a program and its explanation rewrite.  dump_explain prints the
 * structural scope-tree dump, which is not on the public API (it is a
 * readability dump, not parseable); instead this reports the program and
 * the rewritten program by their public shape: the input and export
 * counts and names, before and after the rewrite.  The rewrite takes one
 * extra positional input (the query rows) and exports one "demand:inputN"
 * collection per source.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtfl/ddir.h"

#include "ddir_tool.h"

static void
report(const char *title, const dtfl_ddir_program *p)
{
	size_t ni = dtfl_ddir_program_input_count(p);
	size_t ne = dtfl_ddir_program_export_count(p);
	size_t i;

	printf("-- %s\n", title);
	printf("inputs: %zu\n", ni);
	printf("exports (%zu):\n", ne);
	for (i = 0; i < ne; i++)
		printf("  %s\n", dtfl_ddir_program_export_name(p, i));
}

int
dtfl_ddir_cmd_explain(int argc, char **argv)
{
	const char *path;
	size_t arity = 2, ni, i;
	dtfl_ddir_program *orig, *rewritten = NULL;
	dtfl_ddir_shape *shapes;
	int rc;

	if (argc < 2) {
		fprintf(stderr, "usage: dtfl-ddir explain <program> [arity]\n");
		return 2;
	}
	path = argv[1];
	if (argc >= 3)
		arity = (size_t)strtoull(argv[2], NULL, 10);

	orig = dtfl_ddir_load(path);
	if (orig == NULL)
		return 1;
	report("ORIGINAL", orig);

	/* Every root import takes the (arity, 0) shape, matching dump_explain. */
	ni = dtfl_ddir_program_input_count(orig);
	shapes = ni ? calloc(ni, sizeof *shapes) : NULL;
	for (i = 0; i < ni; i++) {
		shapes[i].k = arity;
		shapes[i].v = 0;
	}
	rc = dtfl_ddir_explain(orig, shapes, ni, &rewritten);
	free(shapes);
	if (rc != 0) {
		fprintf(stderr, "dtfl-ddir: explain failed: %s\n",
		    dtfl_last_error_detail());
		dtfl_ddir_program_free(orig);
		return 1;
	}
	printf("\n");
	report("EXPLAIN", rewritten);

	dtfl_ddir_program_free(rewritten);
	dtfl_ddir_program_free(orig);
	return 0;
}
