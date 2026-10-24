/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/examples/ddir_vec.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * tools/dtfl-ddir/run.c: the `run` subcommand (ddir_vec).
 *
 * Parse and evaluate a program, then print each export's consolidated
 * contents.  Input rows come from a file argument (or stdin): one row per
 * line, whitespace-separated i64 fields becoming a Tuple of Ints, each
 * inserted at multiplicity +1.  Rows go to the current input, which starts
 * at 0 and is switched by a line "@N" (select positional input N); this is
 * a clearer form of ddir_vec's round-robin EDGES_FILE assignment, so inputs
 * stay separated.  Blank lines and lines starting with '#' are skipped.
 *
 * The output is deterministic (rows sorted by (key, val) Debug form) so a
 * program's result can be checked against a committed transcript.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dtfl/ddir.h"

#include "ddir_tool.h"

/* A growable list of rows for one input. */
typedef struct rowvec {
	dtfl_ddir_row *rows;
	size_t n, cap;
} rowvec;

static void
rowvec_push(rowvec *v, dtfl_ddir_value *key, dtfl_ddir_value *val)
{
	if (v->n == v->cap) {
		size_t nc = v->cap ? v->cap * 2 : 16;
		dtfl_ddir_row *nr = realloc(v->rows, nc * sizeof *nr);

		if (nr == NULL) {
			fprintf(stderr, "dtfl-ddir: out of memory\n");
			exit(1);
		}
		v->rows = nr;
		v->cap = nc;
	}
	v->rows[v->n].key = key;
	v->rows[v->n].val = val;
	v->rows[v->n].diff = 1;
	v->n++;
}

/* Parse one whitespace-separated integer row into a Tuple of Ints (key) and
 * unit (val), the shape a raw input row takes.  Returns false if the line
 * holds no fields (blank/comment). */
static bool
parse_row(const char *line, dtfl_ddir_value **key, dtfl_ddir_value **val)
{
	dtfl_ddir_value **fields = NULL;
	size_t nf = 0, cap = 0;
	const char *p = line;

	while (*p != '\0') {
		char *end;
		long long v;

		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
			p++;
		if (*p == '\0' || *p == '#')
			break;
		v = strtoll(p, &end, 10);
		if (end == p) {
			fprintf(stderr, "dtfl-ddir: bad field in row: %s\n",
			    line);
			while (nf > 0)
				dtfl_ddir_value_free(fields[--nf]);
			free(fields);
			return false;
		}
		if (nf == cap) {
			cap = cap ? cap * 2 : 8;
			fields = realloc(fields, cap * sizeof *fields);
		}
		fields[nf++] = dtfl_ddir_value_int((int64_t)v);
		p = end;
	}
	if (nf == 0) {
		free(fields);
		return false;
	}
	*key = dtfl_ddir_value_tuple(fields, nf);
	*val = dtfl_ddir_value_unit();
	free(fields); /* the builder copies the array; we own ours */
	return true;
}

static int
cmp_rows(const void *a, const void *b)
{
	const dtfl_ddir_row *ra = a, *rb = b;
	int c = dtfl_ddir_value_cmp(ra->key, rb->key);

	return c != 0 ? c : dtfl_ddir_value_cmp(ra->val, rb->val);
}

int
dtfl_ddir_cmd_run(int argc, char **argv)
{
	const char *program_path, *rows_path = NULL;
	FILE *in = stdin;
	dtfl_ddir_program *p;
	size_t n_inputs, i, j, cur_input = 0;
	rowvec *inputs;
	const dtfl_ddir_row **input_ptrs;
	size_t *input_lens;
	dtfl_ddir_output *out = NULL;
	size_t n_out = 0;
	char line[4096];
	int rc;

	if (argc < 2) {
		fprintf(stderr,
		    "usage: dtfl-ddir run <program> [rows-file]\n");
		return 2;
	}
	program_path = argv[1];
	if (argc >= 3)
		rows_path = argv[2];

	p = dtfl_ddir_load(program_path);
	if (p == NULL)
		return 1;
	n_inputs = dtfl_ddir_program_input_count(p);

	if (rows_path != NULL) {
		in = fopen(rows_path, "r");
		if (in == NULL) {
			fprintf(stderr, "dtfl-ddir: cannot open %s\n",
			    rows_path);
			dtfl_ddir_program_free(p);
			return 1;
		}
	}

	inputs = calloc(n_inputs ? n_inputs : 1, sizeof *inputs);
	while (fgets(line, sizeof line, in) != NULL) {
		dtfl_ddir_value *key, *val;
		char *t = line;

		while (*t == ' ' || *t == '\t')
			t++;
		/* "@N" selects the current positional input. */
		if (*t == '@') {
			char *end;
			unsigned long sel = strtoul(t + 1, &end, 10);

			if (end != t + 1 && sel < n_inputs)
				cur_input = sel;
			else
				fprintf(stderr, "dtfl-ddir: bad input "
				    "selector: %s", line);
			continue;
		}
		if (!parse_row(line, &key, &val))
			continue;
		if (n_inputs == 0) {
			/* No inputs: a row has nowhere to go. */
			dtfl_ddir_value_free(key);
			dtfl_ddir_value_free(val);
		} else {
			rowvec_push(&inputs[cur_input], key, val);
		}
	}
	if (in != stdin)
		fclose(in);

	input_ptrs = calloc(n_inputs ? n_inputs : 1, sizeof *input_ptrs);
	input_lens = calloc(n_inputs ? n_inputs : 1, sizeof *input_lens);
	for (i = 0; i < n_inputs; i++) {
		input_ptrs[i] = inputs[i].rows;
		input_lens[i] = inputs[i].n;
	}

	rc = dtfl_ddir_evaluate(p, input_ptrs, input_lens, n_inputs, &out,
	    &n_out);
	if (rc != 0) {
		fprintf(stderr, "dtfl-ddir: evaluate failed: %s\n",
		    dtfl_last_error_detail());
	} else {
		for (i = 0; i < n_out; i++) {
			dtfl_ddir_output *o = &out[i];
			char kbuf[1024], vbuf[1024];

			printf("export \"%s\" (%zu rows):\n", o->name, o->n);
			if (o->n > 1)
				qsort(o->rows, o->n, sizeof(dtfl_ddir_row),
				    cmp_rows);
			for (j = 0; j < o->n; j++) {
				dtfl_ddir_value_format(o->rows[j].key, kbuf,
				    sizeof kbuf);
				dtfl_ddir_value_format(o->rows[j].val, vbuf,
				    sizeof vbuf);
				printf("  (%s, %s)  x%lld\n", kbuf, vbuf,
				    (long long)o->rows[j].diff);
			}
		}
		dtfl_ddir_outputs_free(out, n_out);
	}

	/* Free the input rows (evaluate borrows but does not take them). */
	for (i = 0; i < n_inputs; i++) {
		for (j = 0; j < inputs[i].n; j++) {
			dtfl_ddir_value_free(inputs[i].rows[j].key);
			dtfl_ddir_value_free(inputs[i].rows[j].val);
		}
		free(inputs[i].rows);
	}
	free(inputs);
	free(input_ptrs);
	free(input_lens);
	dtfl_ddir_program_free(p);
	return rc != 0 ? 1 : 0;
}
