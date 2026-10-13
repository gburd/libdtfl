/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/examples/ddir_server.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tools/dtfl-ddir/server_cmd.c: the `server` subcommand (ddir_server).
 *
 * Run the live DDIR server on a command session: a stream of commands, one
 * per line, from a session file argument (or stdin).  A line starting with
 * '#' or '--', or blank, is a comment.  Commands:
 *
 *   install <name> <file>
 *   feed <prog> <in#> <value> [val=<value>] [time=<t>] [diff=<int>]
 *   tick
 *   drop <name>
 *   peek <trace> [key]
 *   list
 *   help
 *   exit | quit
 *
 * A <value> is a comma-separated integer row (becoming a Tuple of Ints;
 * '_' or empty = unit), or any closed scalar term written without spaces
 * (tuple(1,2), list(1,2,3), inject(2,tuple(3,4))).  Malformed programs and
 * values are reported to the prompt and the server keeps running: bad
 * input never aborts the session.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtfl/ddir.h"
#include "dtfl/server.h"

#include "ddir_tool.h"

/* Parse a <value> token.  A comma-separated integer row -> Tuple of Ints;
 * '_'/empty -> unit; otherwise a closed scalar term.  Returns NULL and
 * prints an error on a malformed value. */
static dtfl_ddir_value *
parse_value(const char *s)
{
	dtfl_ddir_value **fields = NULL;
	size_t nf = 0, cap = 0;
	const char *p = s;
	bool all_int = true;
	const char *q;

	if (s[0] == '\0' || (s[0] == '_' && s[1] == '\0'))
		return dtfl_ddir_value_unit();

	/* Fast path: a plain a,b,c integer row. */
	for (q = s; *q != '\0'; q++)
		if (!((*q >= '0' && *q <= '9') || *q == ',' || *q == '-')) {
			all_int = false;
			break;
		}
	if (all_int) {
		while (*p != '\0') {
			char *end;
			long long v = strtoll(p, &end, 10);

			if (end == p)
				break;
			if (nf == cap) {
				cap = cap ? cap * 2 : 8;
				fields = realloc(fields, cap * sizeof *fields);
			}
			fields[nf++] = dtfl_ddir_value_int((int64_t)v);
			p = end;
			if (*p == ',')
				p++;
		}
		if (*p == '\0') {
			dtfl_ddir_value *t = dtfl_ddir_value_tuple(fields, nf);

			free(fields); /* builder copies the array; free ours */
			return t;
		}
		/* fell through: free partial and try as a term */
		while (nf > 0)
			dtfl_ddir_value_free(fields[--nf]);
		free(fields);
	}
	/* Otherwise: a closed scalar term. */
	{
		dtfl_ddir_value *v = dtfl_ddir_value_parse(s);

		if (v == NULL)
			fprintf(stdout, "error: malformed value \"%s\"\n", s);
		return v;
	}
}

/* Split a line into whitespace tokens (in place).  Returns the count. */
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
dispatch(dtfl_ddir_server *s, char *line)
{
	char *toks[16];
	size_t n = tokenize(line, toks, 16);
	int rc;

	if (n == 0)
		return true;

	if (strcmp(toks[0], "install") == 0 && n == 3) {
		dtfl_ddir_program *p = dtfl_ddir_load(toks[2]);

		if (p == NULL) {
			printf("error: cannot load program %s\n", toks[2]);
			return true;
		}
		rc = dtfl_ddir_server_install(s, toks[1], p);
		if (rc == 0)
			printf("installed \"%s\"\n", toks[1]);
		else
			printf("error: %s\n", dtfl_last_error_detail());
		dtfl_ddir_program_free(p);
	} else if (strcmp(toks[0], "feed") == 0 && n >= 4) {
		size_t input;
		dtfl_ddir_value *key, *val = NULL;
		bool has_time = false;
		uint64_t time = 0;
		int64_t diff = 1;
		char *end;
		size_t i;
		bool ok = true;

		input = (size_t)strtoull(toks[2], &end, 10);
		if (*end != '\0') {
			printf("error: feed: <in#> must be a number, got "
			    "\"%s\"\n", toks[2]);
			return true;
		}
		key = parse_value(toks[3]);
		if (key == NULL)
			return true;
		for (i = 4; i < n && ok; i++) {
			if (strncmp(toks[i], "val=", 4) == 0) {
				val = parse_value(toks[i] + 4);
				if (val == NULL)
					ok = false;
			} else if (strncmp(toks[i], "time=", 5) == 0) {
				time = strtoull(toks[i] + 5, &end, 10);
				has_time = true;
				if (*end != '\0') {
					printf("error: feed: time= must be a "
					    "number, got \"%s\"\n", toks[i] + 5);
					ok = false;
				}
			} else if (strncmp(toks[i], "diff=", 5) == 0) {
				diff = strtoll(toks[i] + 5, &end, 10);
				if (*end != '\0') {
					printf("error: feed: diff= must be an "
					    "integer, got \"%s\"\n",
					    toks[i] + 5);
					ok = false;
				}
			} else {
				printf("error: feed: unrecognized argument "
				    "\"%s\"\n", toks[i]);
				ok = false;
			}
		}
		if (ok) {
			rc = dtfl_ddir_server_feed(s, toks[1], input, key, val,
			    has_time, time, diff);
			if (rc != 0)
				printf("error: %s\n", dtfl_last_error_detail());
		}
		dtfl_ddir_value_free(key);
		if (val != NULL)
			dtfl_ddir_value_free(val);
	} else if (strcmp(toks[0], "tick") == 0) {
		dtfl_ddir_server_tick(s);
		printf("tick -> epoch %llu\n",
		    (unsigned long long)dtfl_ddir_server_epoch(s));
	} else if (strcmp(toks[0], "drop") == 0 && n == 2) {
		rc = dtfl_ddir_server_drop(s, toks[1]);
		if (rc == 0)
			printf("dropped \"%s\"\n", toks[1]);
		else
			printf("error: %s\n", dtfl_last_error_detail());
	} else if (strcmp(toks[0], "peek") == 0 && (n == 2 || n == 3)) {
		dtfl_ddir_value *key = NULL;

		if (n == 3) {
			key = parse_value(toks[2]);
			if (key == NULL)
				return true;
		}
		rc = dtfl_ddir_server_peek(s, toks[1], key);
		if (rc != 0)
			printf("error: %s\n", dtfl_last_error_detail());
		if (key != NULL)
			dtfl_ddir_value_free(key);
	} else if (strcmp(toks[0], "list") == 0) {
		dtfl_ddir_server_list(s);
	} else if (strcmp(toks[0], "help") == 0) {
		printf("commands:\n");
		printf("  install <name> <file>\n");
		printf("  feed <prog> <in#> <value> [val=<value>] [time=<t>] "
		    "[diff=<int>]\n");
		printf("  tick\n");
		printf("  drop <name>\n");
		printf("  peek <trace> [key]\n");
		printf("  list\n");
		printf("  exit\n");
	} else if (strcmp(toks[0], "exit") == 0 || strcmp(toks[0], "quit") ==
	    0) {
		return false;
	} else {
		printf("error: unknown or malformed command \"%s\" (try "
		    "`help`)\n", toks[0]);
	}
	return true;
}

int
dtfl_ddir_cmd_server(int argc, char **argv)
{
	FILE *in = stdin;
	dtfl_ddir_server *s;
	char line[8192];

	if (argc >= 2) {
		in = fopen(argv[1], "r");
		if (in == NULL) {
			fprintf(stderr, "dtfl-ddir: cannot read session %s\n",
			    argv[1]);
			return 1;
		}
	}
	s = dtfl_ddir_server_new(stdout);

	while (fgets(line, sizeof line, in) != NULL) {
		char *t = line;

		/* Trim leading space to recognise comment markers. */
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

	dtfl_ddir_server_free(s);
	if (in != stdin)
		fclose(in);
	return 0;
}
