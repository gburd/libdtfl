/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/examples/ddir_vec.rs
 *   interactive/examples/ddir_server.rs
 *   interactive/examples/dump_explain.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tools/dtfl-ddir/main.c: the dtfl-ddir command-line tool.
 *
 * Three subcommands, following the three upstream example binaries:
 *
 *   run <program> [rows-file]   ddir_vec: parse, evaluate, print each
 *                               export's consolidated contents.  Rows come
 *                               from `rows-file` (or stdin): one row per
 *                               line, whitespace-separated i64 fields,
 *                               assigned round-robin to the program's inputs
 *                               (line % n_inputs), each inserted at +1.
 *   server [session-file]       ddir_server: run the live server on a
 *                               command session (or stdin): install / feed /
 *                               tick / drop / peek / list / exit.
 *   explain <program> [arity]   dump_explain: report the program and the
 *                               explanation rewrite (input and export shape).
 *
 * A tool uses only the public dtfl_* API (it is a program, not library
 * code), so it may use getopt/getenv.  .ddp files parse as pipe syntax,
 * else as applicative.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtfl/ddir.h"
#include "dtfl/server.h"

#include "ddir_tool.h"

static void
usage(FILE *out)
{
	fprintf(out,
"usage: dtfl-ddir CMD [args]\n"
"\n"
"Build and run Differential Dataflow IR (DDIR) programs.\n"
"\n"
"commands:\n"
"  run <program> [rows-file]   evaluate a program and print each export's\n"
"                              consolidated contents.  Rows come from\n"
"                              rows-file (or stdin): one row per line,\n"
"                              whitespace-separated integers, assigned\n"
"                              round-robin to the program's inputs.\n"
"  server [session-file]       run the live DDIR server on a command\n"
"                              session (or stdin): install/feed/tick/drop/\n"
"                              peek/list/exit.\n"
"  explain <program> [arity]   report a program and its explanation rewrite.\n"
"\n"
"A .ddp program parses as pipe syntax; anything else as applicative.\n");
}

int
main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 2) {
		usage(stderr);
		return 2;
	}
	cmd = argv[1];
	if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
		usage(stdout);
		return 0;
	}
	/* Shift argv so each subcommand sees argv[0] = subcommand. */
	if (strcmp(cmd, "run") == 0)
		return dtfl_ddir_cmd_run(argc - 1, argv + 1);
	if (strcmp(cmd, "server") == 0)
		return dtfl_ddir_cmd_server(argc - 1, argv + 1);
	if (strcmp(cmd, "explain") == 0)
		return dtfl_ddir_cmd_explain(argc - 1, argv + 1);
	fprintf(stderr, "dtfl-ddir: unknown command \"%s\"\n", cmd);
	usage(stderr);
	return 2;
}
