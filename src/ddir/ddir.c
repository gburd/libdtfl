/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/lib.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/ddir.c: the public DDIR entry points.
 *
 * dtfl_ddir_parse turns a program string into a lowered, optimized
 * scope-tree program; dtfl_ddir_build renders it into a live dataflow
 * inside a scope; dtfl_ddir_evaluate is the data-in/data-out shortcut that
 * feeds explicit input rows, runs to quiescence, and returns each export's
 * consolidated final contents.  dtfl_ddir_value_parse parses a closed
 * scalar term into a constant value (lib.rs is the small program-level
 * surface; survey_sources is subsumed by the lowered program's import
 * count).
 */
#include "ddir_int.h"
#include "parse_int.h"
#include "backend_int.h"
#include "explain_int.h"

#include "../base/base_int.h"

#include "dtfl/time.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Parse                                                               */
/* ------------------------------------------------------------------ */

int
dtfl_ddir_parse(const char *program, dtfl_ddir_syntax syntax,
    dtfl_ddir_program **out)
{
	dd_parse_ctx pc;
	dd_vec stmts = { 0 };
	bool ok;
	dtfl_ddir_program *p;

	if (program == NULL || out == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_ddir_parse: NULL argument");
	*out = NULL;
	if (syntax == DTFL_DDIR_PIPE)
		ok = dd_parse_pipe(program, &stmts, &pc);
	else if (syntax == DTFL_DDIR_APPLICATIVE)
		ok = dd_parse_applicative(program, &stmts, &pc);
	else
		return __dtfl_err(DTFL_EINVAL, "dtfl_ddir_parse: bad syntax");
	if (!ok)
		return __dtfl_err(DTFL_EINVAL, "%s", pc.msg);

	/* Lower and optimize; the AST arena is freed afterward. */
	p = dd_lower_tree(&stmts);
	__dtfl_free(stmts.data);
	dd_arena_free_all(&pc);
	dd_program_optimize(p);
	*out = p;
	return 0;
}

void
dtfl_ddir_program_free(dtfl_ddir_program *p)
{
	dd_program_free(p);
}

size_t
dtfl_ddir_program_input_count(const dtfl_ddir_program *p)
{
	size_t i, max = 0;
	bool any = false;

	for (i = 0; i < p->root.imports.len; i++) {
		dd_import *imp = p->root.imports.data[i];

		if (imp->from.kind == DD_SRC_INPUT) {
			any = true;
			if (imp->from.u.input + 1 > max)
				max = imp->from.u.input + 1;
		}
	}
	return any ? max : 0;
}

size_t
dtfl_ddir_program_export_count(const dtfl_ddir_program *p)
{
	return p->root.exports.len;
}

const char *
dtfl_ddir_program_export_name(const dtfl_ddir_program *p, size_t i)
{
	if (i >= p->root.exports.len)
		return NULL;
	return ((dd_export *)p->root.exports.data[i])->name;
}

/* ------------------------------------------------------------------ */
/* Explain                                                             */
/* ------------------------------------------------------------------ */

int
dtfl_ddir_explain(const dtfl_ddir_program *program,
    const dtfl_ddir_shape *shapes, size_t n_shapes, dtfl_ddir_program **out)
{
	size_t (*raw)[2];
	size_t i;

	if (program == NULL || out == NULL || (n_shapes != 0 && shapes == NULL))
		return __dtfl_err(DTFL_EINVAL, "dtfl_ddir_explain: NULL argument");
	*out = NULL;
	if (n_shapes != dtfl_ddir_program_input_count(program))
		return __dtfl_err(DTFL_EINVAL, "dtfl_ddir_explain: expected %zu "
		    "shapes, got %zu",
		    dtfl_ddir_program_input_count(program), n_shapes);
	if (program->root.exports.len == 0)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_ddir_explain: program has no export");
	raw = n_shapes ? __dtfl_alloc(n_shapes * sizeof(*raw)) : NULL;
	for (i = 0; i < n_shapes; i++) {
		raw[i][0] = shapes[i].k;
		raw[i][1] = shapes[i].v;
	}
	*out = dd_explain(program, (const size_t (*)[2])raw, n_shapes);
	__dtfl_free(raw);
	return 0;
}

int
dtfl_ddir_clone_identity(const dtfl_ddir_program *program,
    dtfl_ddir_program **out)
{
	if (program == NULL || out == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_ddir_clone_identity: NULL argument");
	*out = dd_clone_identity(program);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Build and time class                                                */
/* ------------------------------------------------------------------ */

dtfl_time_class *
dtfl_ddir_time_class(void)
{
	/*
	 * DDIR's time is Product<u64, PointStamp<u64>> (ir.rs:9): a u64 outer
	 * (host time) and a PointStamp inner that holds the per-depth
	 * iteration coordinates.  The whole program renders in one iterative
	 * dynamic scope of this class, so iterate pushes coordinates without
	 * changing the timestamp type.  The PointStamp is padded to a fixed
	 * maximum nesting depth (DD_POINTSTAMP_MAX).
	 */
	dtfl_coord u64c = { DTFL_COORD_UNSIGNED, 64 };
	const dtfl_time_class *outer = dtfl_time_u64();
	const dtfl_time_class *inner = dtfl_time_pointstamp(u64c,
	    DD_POINTSTAMP_MAX);
	dtfl_time_class *tc = dtfl_time_product(outer, inner);

	dtfl_time_class_free(outer);
	dtfl_time_class_free(inner);
	return tc;
}

int
dtfl_ddir_build(const dtfl_ddir_program *program, dtfl_scope *scope,
    dtfl_collection *const *inputs, size_t n_inputs, dtfl_collection **exports)
{
	if (program == NULL || scope == NULL || exports == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_ddir_build: NULL argument");
	if (n_inputs != dtfl_ddir_program_input_count(program))
		return __dtfl_err(DTFL_EINVAL, "dtfl_ddir_build: expected %zu "
		    "inputs, got %zu",
		    dtfl_ddir_program_input_count(program), n_inputs);
	return dd_render_program(program, scope, inputs, n_inputs, exports);
}

/* ------------------------------------------------------------------ */
/* Evaluate                                                            */
/* ------------------------------------------------------------------ */

int
dtfl_ddir_evaluate(const dtfl_ddir_program *program,
    const dtfl_ddir_row *const *inputs, const size_t *input_lens,
    size_t n_inputs, dtfl_ddir_output **outputs, size_t *n_outputs)
{
	size_t ne = program->root.exports.len;
	out_acc *accs;
	dtfl_ddir_output *out;
	size_t i, j;
	int rc;

	if (program == NULL || outputs == NULL || n_outputs == NULL)
		return __dtfl_err(DTFL_EINVAL,
		    "dtfl_ddir_evaluate: NULL argument");
	if (n_inputs != dtfl_ddir_program_input_count(program))
		return __dtfl_err(DTFL_EINVAL, "dtfl_ddir_evaluate: expected "
		    "%zu inputs, got %zu",
		    dtfl_ddir_program_input_count(program), n_inputs);

	accs = ne ? __dtfl_calloc(ne, sizeof(out_acc)) : NULL;
	rc = dd_evaluate(program, inputs, input_lens, n_inputs, accs);
	if (rc != 0) {
		for (i = 0; i < ne; i++) {
			for (j = 0; j < accs[i].n; j++) {
				dd_value_free(accs[i].keys[j]);
				dd_value_free(accs[i].vals[j]);
			}
			__dtfl_free(accs[i].keys);
			__dtfl_free(accs[i].vals);
			__dtfl_free(accs[i].diffs);
		}
		__dtfl_free(accs);
		return rc;
	}

	/* Convert the accumulators into outputs, dropping zero-net rows. */
	out = ne ? __dtfl_calloc(ne, sizeof(dtfl_ddir_output)) : NULL;
	for (i = 0; i < ne; i++) {
		dd_export *e = program->root.exports.data[i];
		size_t kept = 0;

		out[i].name = __dtfl_strdup(e->name);
		for (j = 0; j < accs[i].n; j++)
			if (accs[i].diffs[j] != 0)
				kept++;
		out[i].rows = kept
		    ? __dtfl_alloc(kept * sizeof(dtfl_ddir_row)) : NULL;
		out[i].n = kept;
		kept = 0;
		for (j = 0; j < accs[i].n; j++) {
			if (accs[i].diffs[j] != 0) {
				out[i].rows[kept].key = accs[i].keys[j];
				out[i].rows[kept].val = accs[i].vals[j];
				out[i].rows[kept].diff = accs[i].diffs[j];
				kept++;
			} else {
				dd_value_free(accs[i].keys[j]);
				dd_value_free(accs[i].vals[j]);
			}
		}
		__dtfl_free(accs[i].keys);
		__dtfl_free(accs[i].vals);
		__dtfl_free(accs[i].diffs);
	}
	__dtfl_free(accs);
	*outputs = out;
	*n_outputs = ne;
	return 0;
}

void
dtfl_ddir_outputs_free(dtfl_ddir_output *outputs, size_t n)
{
	size_t i, j;

	for (i = 0; i < n; i++) {
		for (j = 0; j < outputs[i].n; j++) {
			dd_value_free(outputs[i].rows[j].key);
			dd_value_free(outputs[i].rows[j].val);
		}
		__dtfl_free(outputs[i].rows);
		__dtfl_free(outputs[i].name);
	}
	__dtfl_free(outputs);
}

/* ------------------------------------------------------------------ */
/* Value parse (closed scalar term -> constant value)                  */
/* ------------------------------------------------------------------ */

dtfl_ddir_value *
dtfl_ddir_value_parse(const char *term)
{
	dd_parse_ctx pc;
	dd_term *t;
	dd_value **env = NULL;
	size_t len = 0, cap = 0;
	dd_value *v;

	if (term == NULL)
		return NULL;
	t = dd_parse_pipe_term(term, &pc);
	if (t == NULL)
		return NULL;
	/* A closed term evaluates with an empty environment. */
	v = dd_eval(t, &env, &len, &cap);
	__dtfl_free(env);
	dd_arena_free_all(&pc);
	return v;
}

/* ------------------------------------------------------------------ */
/* Value Debug form (Rust {:?})                                        */
/* ------------------------------------------------------------------ */

/* Append `s` to the growable string *buf (len = used, cap = capacity),
 * keeping it NUL-terminated.  Grows via __dtfl_realloc (never NULL). */
static void
dd_sb_puts(char **buf, size_t *len, size_t *cap, const char *s)
{
	size_t add = strlen(s);

	if (*len + add + 1 > *cap) {
		size_t want = *cap ? *cap * 2 : 64;

		while (want < *len + add + 1)
			want *= 2;
		*buf = __dtfl_realloc(*buf, want);
		*cap = want;
	}
	memcpy(*buf + *len, s, add + 1);
	*len += add;
}

/* Append the Debug form of `v` to the growable buffer *buf (len/cap). */
static void
dd_value_debug(const dd_value *v, char **buf, size_t *len, size_t *cap)
{
	char num[32];
	size_t i;

	switch (v->kind) {
	case DD_V_INT:
		snprintf(num, sizeof num, "Int(%lld)",
		    (long long)v->u.int_v);
		dd_sb_puts(buf, len, cap, num);
		break;
	case DD_V_TUPLE:
	case DD_V_LIST:
		dd_sb_puts(buf, len, cap,
		    v->kind == DD_V_TUPLE ? "Tuple([" : "List([");
		for (i = 0; i < v->u.seq.len; i++) {
			if (i)
				dd_sb_puts(buf, len, cap, ", ");
			dd_value_debug(v->u.seq.items[i], buf, len, cap);
		}
		dd_sb_puts(buf, len, cap, "])");
		break;
	case DD_V_VARIANT:
		snprintf(num, sizeof num, "Variant(%lu, ",
		    (unsigned long)v->u.variant.tag);
		dd_sb_puts(buf, len, cap, num);
		dd_value_debug(v->u.variant.payload, buf, len, cap);
		dd_sb_puts(buf, len, cap, ")");
		break;
	}
}

size_t
dtfl_ddir_value_format(const dtfl_ddir_value *v, char *buf, size_t cap)
{
	char *sb = NULL;
	size_t len = 0, sbcap = 0, n;

	if (v == NULL) {
		if (cap)
			buf[0] = '\0';
		return 0;
	}
	dd_value_debug(v, &sb, &len, &sbcap);
	n = len; /* the Debug form length, excluding the NUL */
	if (cap) {
		size_t copy = len < cap - 1 ? len : cap - 1;

		memcpy(buf, sb, copy);
		buf[copy] = '\0';
	}
	__dtfl_free(sb);
	return n;
}
