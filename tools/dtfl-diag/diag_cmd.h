/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tools/dtfl-diag/diag_cmd.h: shared options and the subcommand entry
 * points for the dtfl-diag tool.  Uses only the public dtfl_* API
 * (AGENTS.md section 5, the api_discipline gate).
 */
#ifndef DTFL_DIAG_CMD_H
#define DTFL_DIAG_CMD_H

#include <stddef.h>
#include <stdint.h>

#define DIAG_MAX_FILES 64

struct diag_opts {
	const char *command;
	const char *iface;
	uint16_t port;
	uint16_t diff_port;
	size_t source_peers;
	const char *files[DIAG_MAX_FILES];
	size_t nfiles;
	const char *diff_files[DIAG_MAX_FILES];
	size_t ndiff_files;
	const char *output_path;
	unsigned long long output_interval_ms;
};

/*
 * Opens the recorded-stream files (opts->files) or listens for
 * source_peers TCP connections on iface:port, filling fds[] with up to
 * DIAG_MAX_FILES descriptors.  Returns the count, or -1 with a message
 * on stderr.  `which` selects "timely" (files/port) or "differential"
 * (diff_files/diff_port).  The caller closes each fd.
 */
int diag_connect(const struct diag_opts *opts, int which, int *fds,
    size_t maxfds);
#define DIAG_STREAM_TIMELY 0
#define DIAG_STREAM_DIFFERENTIAL 1

int diag_cmd_graph(const struct diag_opts *opts);
int diag_cmd_profile(const struct diag_opts *opts);
int diag_cmd_arrangements(const struct diag_opts *opts);

#endif /* DTFL_DIAG_CMD_H */
