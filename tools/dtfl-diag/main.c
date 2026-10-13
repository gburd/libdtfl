/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tools/dtfl-diag/main.c: the dtfl-diag command-line inspector.
 *
 * Ported from dataflow-diag (HEAD 0fa322c9)
 *   tdiag/src/main.rs
 *   tdiag/src/lib.rs
 *   tdiag/src/commands/mod.rs
 * Copyright (c) 2017 the timely-dataflow diagnostics contributors.
 *
 * main.rs parses the global flags (--interface, --port, --source-peers,
 * --diag-workers) and dispatches to the graph, profile and differential
 * arrangements subcommands, each of which opens one socket per source
 * worker and replays the log stream.  dtfl-diag keeps the same surface
 * but reads the current bincode stream (PLAN D9), uses getopt for the
 * flags (a tool may, unlike library code), and can read a recorded
 * stream from a file with --file so the pipeline is testable without a
 * live computation.
 *
 * The subcommands fold every event into one state regardless of worker,
 * matching upstream's worker == 0 filter: the operator graph and timing
 * profile are the same whichever source worker reported an Operates or
 * Schedule event.
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_cmd.h"

static void
usage(FILE *out)
{
	fprintf(out,
"usage: dtfl-diag [-i IFACE] [-p PORT] [-s PEERS] [-f FILE ...] CMD [args]\n"
"\n"
"Inspect a timely or differential dataflow computation from its log\n"
"stream.  The source program ships its \"timely\" (and, for\n"
"arrangements, \"differential/arrange\") log streams to dtfl-diag over\n"
"TCP; see dtfl-diag(1) for the few lines to add to the source program.\n"
"A recorded stream can be read from a file with -f instead.\n"
"\n"
"options:\n"
"  -i, --interface IP   interface to listen on (default 127.0.0.1)\n"
"  -p, --port PORT      timely log port (default 51317)\n"
"  -P, --diff-port PORT differential log port (default 51318)\n"
"  -s, --source-peers N number of workers in the source computation\n"
"  -f, --file FILE      read a recorded log stream from FILE (repeatable;\n"
"                       one per source worker).  For arrangements, pass\n"
"                       the timely files, then --diff-file for the\n"
"                       differential files.\n"
"      --diff-file FILE recorded differential stream file (repeatable)\n"
"  -o, --out PATH       graph: output HTML path (default dataflow-graph.html)\n"
"      --output-interval MS  arrangements: report cadence (default 1000)\n"
"\n"
"commands:\n"
"  graph         reconstruct and render the dataflow graph\n"
"  profile       print total time spent in each operator\n"
"  arrangements  print the logical size of each arrangement\n");
}

enum {
	OPT_DIFF_FILE = 1000,
	OPT_OUTPUT_INTERVAL
};

int
main(int argc, char **argv)
{
	struct diag_opts o;
	int c, idx;
	const struct option longopts[] = {
		{ "interface", required_argument, NULL, 'i' },
		{ "port", required_argument, NULL, 'p' },
		{ "diff-port", required_argument, NULL, 'P' },
		{ "source-peers", required_argument, NULL, 's' },
		{ "file", required_argument, NULL, 'f' },
		{ "diff-file", required_argument, NULL, OPT_DIFF_FILE },
		{ "out", required_argument, NULL, 'o' },
		{ "output-interval", required_argument, NULL,
		    OPT_OUTPUT_INTERVAL },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	memset(&o, 0, sizeof o);
	o.iface = "127.0.0.1";
	o.port = 51317;
	o.diff_port = 51318;
	o.source_peers = 1;
	o.output_path = "dataflow-graph.html";
	o.output_interval_ms = 1000;

	while ((c = getopt_long(argc, argv, "i:p:P:s:f:o:h", longopts,
	    &idx)) != -1) {
		switch (c) {
		case 'i':
			o.iface = optarg;
			break;
		case 'p':
			o.port = (uint16_t)strtoul(optarg, NULL, 10);
			break;
		case 'P':
			o.diff_port = (uint16_t)strtoul(optarg, NULL, 10);
			break;
		case 's':
			o.source_peers = (size_t)strtoul(optarg, NULL, 10);
			break;
		case 'f':
			if (o.nfiles < DIAG_MAX_FILES)
				o.files[o.nfiles++] = optarg;
			break;
		case OPT_DIFF_FILE:
			if (o.ndiff_files < DIAG_MAX_FILES)
				o.diff_files[o.ndiff_files++] = optarg;
			break;
		case 'o':
			o.output_path = optarg;
			break;
		case OPT_OUTPUT_INTERVAL:
			o.output_interval_ms = strtoull(optarg, NULL, 10);
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}

	if (optind >= argc) {
		fprintf(stderr, "dtfl-diag: missing command\n");
		usage(stderr);
		return 2;
	}
	o.command = argv[optind];

	if (strcmp(o.command, "graph") == 0)
		return diag_cmd_graph(&o);
	if (strcmp(o.command, "profile") == 0)
		return diag_cmd_profile(&o);
	if (strcmp(o.command, "arrangements") == 0)
		return diag_cmd_arrangements(&o);

	fprintf(stderr, "dtfl-diag: unknown command '%s'\n", o.command);
	usage(stderr);
	return 2;
}
