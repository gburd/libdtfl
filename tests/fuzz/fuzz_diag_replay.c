/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/fuzz_diag_replay.c: dtfl-diag's input path, end to end.
 *
 * The input is a recorded log stream, the bytes dtfl-diag reads from a
 * --file or a source socket.  It goes through:
 *   - dtfl_diag_replay_fd (src/diag/replay_shutdown.c), the EventWriter
 *     batch framing, for both the timely and the differential stream;
 *     each delivered batch is read through every accessor
 *     (fuzz_log_walk.c) and fed to the in-process diagnostics server's
 *     Frame builder (src/diag/diag_log.c __dtfl_diag_on_batch);
 *   - the tool's three subcommands, graph, profile and arrangements
 *     (tools/dtfl-diag), over the same bytes as their --file, so the
 *     tool's graph model and channel rewiring see the decoded events.
 * Malformed input must end in a DTFL_E* return or a tool error status,
 * never a crash.  The tool's stdout goes to /dev/null.
 */
#include "fuzz_log_walk.h"

#include "diag/diag_int.h"
#include "diag_cmd.h"

#include <fcntl.h>
#include <unistd.h>

static char path[4096];
static char outpath[4096];
static int fd = -1;

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *tmp = getenv("TMPDIR");
	char dir[4096];
	int null;
	FILE *f;

	(void)argc;
	(void)argv;
	snprintf(dir, sizeof dir, "%s/dtfl-fuzz-diag-XXXXXX",
	    tmp != NULL ? tmp : "/tmp");
	FUZZ_ASSERT(mkdtemp(dir) != NULL);
	snprintf(path, sizeof path, "%s/stream.bin", dir);
	snprintf(outpath, sizeof outpath, "%s/graph.html", dir);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	FUZZ_ASSERT(fd >= 0);
	/* The graph command reads its HTML template from DTFL_DATA_DIR. */
	{
		char tmpl[4200];

		snprintf(tmpl, sizeof tmpl, "%s/dataflow-graph.html", dir);
		f = fopen(tmpl, "w");
		FUZZ_ASSERT(f != NULL);
		fputs("<!-- template -->", f);
		fclose(f);
		FUZZ_ASSERT(setenv("DTFL_DATA_DIR", dir, 1) == 0);
	}
	null = open("/dev/null", O_WRONLY);
	FUZZ_ASSERT(null >= 0);
	FUZZ_ASSERT(dup2(null, STDOUT_FILENO) >= 0);
	close(null);
	return 0;
}

static void
on_batch(const dtfl_log_batch *b, void *ctx)
{
	__dtfl_vec out;

	__dtfl_vec_init(&out, 1);
	fuzz_log_walk(b, &out);
	__dtfl_vec_free(&out);
	__dtfl_diag_on_batch(b, ctx);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	static const char *const streams[2] = {
		"timely", "differential/arrange"
	};
	struct diag_opts o;
	size_t s;

	FUZZ_ASSERT(ftruncate(fd, 0) == 0);
	FUZZ_ASSERT(pwrite(fd, data, size, 0) == (ssize_t)size);

	for (s = 0; s < 2; s++) {
		__dtfl_diag_shared *sh = __dtfl_diag_shared_new();
		int rc;

		FUZZ_ASSERT(lseek(fd, 0, SEEK_SET) == 0);
		rc = dtfl_diag_replay_fd(fd, streams[s], on_batch, sh);
		FUZZ_ASSERT(rc == DTFL_OK || rc == DTFL_EPROTO ||
		    rc == DTFL_EIO);
		__dtfl_diag_shared_release(sh);
	}

	memset(&o, 0, sizeof o);
	o.iface = "127.0.0.1";
	o.files[0] = path;
	o.nfiles = 1;
	o.diff_files[0] = path;
	o.ndiff_files = 1;
	o.output_path = outpath;
	(void)diag_cmd_graph(&o);
	(void)diag_cmd_profile(&o);
	(void)diag_cmd_arrangements(&o);
	return 0;
}
