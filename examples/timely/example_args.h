/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/example_args.h: argument handling shared by the timely
 * examples.  Upstream examples read leading positional arguments
 * (e.g. "hashjoin keys vals batch") and pass the remaining arguments,
 * which hold timely's -w/-n/-p flags, to execute_from_args.  These
 * helpers reproduce that: ex_u64 reads the i'th argv word as a u64 (with
 * a default when absent), and ex_config builds a dtfl_config from the
 * arguments after skipping the first `skip` positionals.
 */
#ifndef DTFL_EXAMPLE_ARGS_H
#define DTFL_EXAMPLE_ARGS_H

#include "dtfl/config.h"

#include <stdlib.h>
#include <string.h>

/* argv[i] parsed as u64, or def when i >= argc. */
static inline uint64_t
ex_u64(int argc, char **argv, int i, uint64_t def)
{
	if (i < argc)
		return strtoull(argv[i], NULL, 10);
	return def;
}

static inline size_t
ex_size(int argc, char **argv, int i, size_t def)
{
	return (size_t)ex_u64(argc, argv, i, def);
}

/*
 * Builds a config from the arguments after the first `skip` positionals
 * (which the example has already consumed).  argv[0] is kept as the
 * program name so dtfl_config_from_args sees the usual argv shape.  The
 * caller frees the returned config.
 */
static inline dtfl_config *
ex_config(int argc, char **argv, int skip)
{
	dtfl_config *cfg = dtfl_config_new();
	char **rest = calloc((size_t)argc + 1, sizeof *rest);
	char **sub = calloc((size_t)argc + 1, sizeof *sub);
	int nsub = 1, nrest = 0, i;

	sub[0] = argv[0];
	for (i = 1 + skip; i < argc; i++)
		sub[nsub++] = argv[i];
	dtfl_config_from_args(cfg, nsub, sub, rest, &nrest);
	free(rest);
	free(sub);
	return cfg;
}

#endif /* DTFL_EXAMPLE_ARGS_H */
