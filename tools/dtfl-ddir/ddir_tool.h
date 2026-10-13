/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tools/dtfl-ddir/ddir_tool.h: the dtfl-ddir subcommand entry points and
 * shared helpers.  Public dtfl_* API only.
 */
#ifndef DTFL_DDIR_TOOL_H
#define DTFL_DDIR_TOOL_H

#include "dtfl/ddir.h"

/* The subcommands.  argv[0] is the subcommand name. */
int dtfl_ddir_cmd_run(int argc, char **argv);
int dtfl_ddir_cmd_server(int argc, char **argv);
int dtfl_ddir_cmd_explain(int argc, char **argv);

/* Read the whole of `path` into a fresh NUL-terminated buffer (free with
 * free()).  Returns NULL and prints to stderr on error. */
char *dtfl_ddir_slurp(const char *path);

/* The surface syntax implied by a path: pipe for .ddp, applicative else. */
dtfl_ddir_syntax dtfl_ddir_syntax_of(const char *path);

/* Load, parse and lower a program file.  Returns NULL (and prints the parse
 * error to stderr) on failure. */
dtfl_ddir_program *dtfl_ddir_load(const char *path);

#endif /* DTFL_DDIR_TOOL_H */
