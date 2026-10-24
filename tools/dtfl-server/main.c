/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   server/src/bin/dd_server.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tools/dtfl-server/main.c: the dd_server binary.
 *
 * dd_server.rs runs a timely worker, serializes commands onto a Sequencer,
 * and reads lines from stdin (load / drop / list / bind / help / exit),
 * where "load" opens a shared library and binds a symbol into a new
 * dataflow.  This port keeps the command loop and the per-load dataflow but
 * selects plugins from a static table (dd_server.h), the divergence from
 * dlopen described in dtfl/dd_server.h: library code may not dlopen, so the
 * four example plugins are linked in and chosen by name.
 *
 * Commands (one per line; a blank line or a line starting with '#' or '--'
 * is a comment):
 *
 *   load <instance> <plugin> [args...]   build a plugin's dataflow
 *   tick                                 close the epoch and run to quiescence
 *   drop <instance>                      drop a loaded instance
 *   peek <trace>                         print a shared trace's contents
 *   list                                 print the registry
 *   plugins                              list the registered plugins
 *   help                                 print the commands
 *   exit | quit                          stop the session
 *
 * A tool uses only the public dtfl_* API (it is a program, not library
 * code), so it may use getopt/getenv.  A session is read from a file
 * argument or from stdin, as ddir_server does (tools/dtfl-ddir).
 */
#include "dtfl/dd_server.h"

#include "plugins.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The statically registered plugins, the stand-in for a plugin directory. */
static const dtfl_dataflow_plugin *const plugins[] = {
	&dtfl_plugin_degr_dist,
	&dtfl_plugin_reachability,
	&dtfl_plugin_neighborhood,
	&dtfl_plugin_random_graph,
};
#define N_PLUGINS (sizeof plugins / sizeof plugins[0])

static void
usage(FILE *out)
{
	fprintf(out,
"usage: dtfl-server [session-file]\n"
"\n"
"Run the shared-dataflow plugin server (dd_server) on a command session\n"
"(or stdin).  The four example plugins are linked in and selected by name.\n"
"\n"
"commands:\n"
"  load <instance> <plugin> [args...]   build a plugin's dataflow\n"
"  tick                                 close the epoch and run to quiescence\n"
"  drop <instance>                      drop a loaded instance\n"
"  peek <trace>                         print a shared trace's contents\n"
"  list                                 print the registry\n"
"  plugins                              list the registered plugins\n"
"  exit                                 stop the session\n");
}

/* Split a line into whitespace tokens in place.  Returns the count. */
static size_t
tokenize(char *line, char **toks, size_t max)
{
	size_t n = 0;
	char *p = line;

	while (*p != '\0' && n < max) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			*p++ = '\0';
		if (*p == '\0')
			break;
		toks[n++] = p;
		while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' &&
		    *p != '\n')
			p++;
	}
	return n;
}

/* Execute one command line.  Returns false on exit/quit. */
static bool
dispatch(dtfl_dd_server *s, char *line)
{
	char *toks[64];
	size_t n = tokenize(line, toks, 64);
	int rc;

	if (n == 0)
		return true;

	if (strcmp(toks[0], "load") == 0 && n >= 3) {
		rc = dtfl_dd_server_load(s, toks[1], toks[2], n - 3,
		    (const char *const *)(toks + 3));
		if (rc == 0)
			printf("loaded \"%s\" (%s)\n", toks[1], toks[2]);
		else
			printf("error: %s\n", dtfl_last_error_detail());
	} else if (strcmp(toks[0], "tick") == 0) {
		dtfl_dd_server_tick(s);
		printf("tick -> epoch %" PRIu64 "\n",
		    dtfl_dd_server_epoch(s));
	} else if (strcmp(toks[0], "drop") == 0 && n == 2) {
		rc = dtfl_dd_server_drop(s, toks[1]);
		if (rc == 0)
			printf("dropped \"%s\"\n", toks[1]);
		else
			printf("error: %s\n", dtfl_last_error_detail());
	} else if (strcmp(toks[0], "peek") == 0 && n == 2) {
		rc = dtfl_dd_server_peek(s, toks[1]);
		if (rc != 0)
			printf("error: %s\n", dtfl_last_error_detail());
	} else if (strcmp(toks[0], "list") == 0) {
		dtfl_dd_server_list(s);
	} else if (strcmp(toks[0], "plugins") == 0) {
		size_t i;

		printf("plugins (%zu):\n", N_PLUGINS);
		for (i = 0; i < N_PLUGINS; i++)
			printf("  %s\n", plugins[i]->name);
	} else if (strcmp(toks[0], "help") == 0) {
		usage(stdout);
	} else if (strcmp(toks[0], "exit") == 0 ||
	    strcmp(toks[0], "quit") == 0) {
		return false;
	} else {
		printf("error: unknown or malformed command \"%s\" (try "
		    "`help`)\n", toks[0]);
	}
	return true;
}

int
main(int argc, char **argv)
{
	FILE *in = stdin;
	dtfl_dd_server *s;
	char line[8192];
	size_t i;

	if (argc >= 2) {
		if (strcmp(argv[1], "-h") == 0 ||
		    strcmp(argv[1], "--help") == 0) {
			usage(stdout);
			return 0;
		}
		in = fopen(argv[1], "r");
		if (in == NULL) {
			fprintf(stderr, "dtfl-server: cannot read session %s\n",
			    argv[1]);
			return 1;
		}
	}

	s = dtfl_dd_server_new(stdout);
	for (i = 0; i < N_PLUGINS; i++)
		dtfl_dd_server_register(s, plugins[i]);

	while (fgets(line, sizeof line, in) != NULL) {
		char *t = line;

		while (*t == ' ' || *t == '\t')
			t++;
		if (*t == '\0' || *t == '\n' || *t == '#' ||
		    (t[0] == '-' && t[1] == '-'))
			continue;
		fflush(stdout);
		if (!dispatch(s, line))
			break;
		fflush(stdout);
	}

	dtfl_dd_server_free(s);
	if (in != stdin)
		fclose(in);
	return 0;
}
