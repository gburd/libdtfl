/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/parse/mod.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/parse.c: the DDIR AST (Expr/Stmt) constructors and the shared
 * scalar builtin builder.
 *
 * The two surface parsers (parse_pipe.c, parse_applicative.c) produce the
 * same Expr/Stmt AST; build_builtin is the function-call-style ADT
 * operator builder (tuple, list, inject, case, fold, proj, len, istag,
 * not, or, if, hash) they share.  A malformed builtin call reports
 * through the parse error path (dd_parse_error) instead of panicking.
 */
#include "ddir_int.h"
#include "parse_int.h"

#include "../base/base_int.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Record a parse error message and unwind to the front-end's guard. */
void
dd_parse_error(dd_parse_ctx *pc, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(pc->msg, sizeof pc->msg, fmt, ap);
	va_end(ap);
	pc->error = true;
	{ stack_t ss; sigaltstack(NULL, &ss); fprintf(stderr, "PROBE dd_parse_error before jump: flags=%d\n", ss.ss_flags); }
	longjmp(pc->jb, 1);
}

/* ------------------------------------------------------------------ */
/* Parse arena: track every AST allocation for shallow cleanup on an    */
/* error unwind.  On success the tree owns the nodes and dd_arena_release */
/* drops the tracking; the caller then deep-frees with dd_stmt_free.     */
/* ------------------------------------------------------------------ */

void
dd_arena_init(dd_parse_ctx *pc)
{
	pc->arena = NULL;
	pc->arena_len = 0;
	pc->arena_cap = 0;
}

static void *
arena_track(dd_parse_ctx *pc, dd_arena_kind kind, void *p)
{
	if (pc->arena_len == pc->arena_cap) {
		size_t ncap = pc->arena_cap ? pc->arena_cap * 2 : 32;

		pc->arena = __dtfl_realloc(pc->arena,
		    ncap * sizeof(dd_arena_ent));
		pc->arena_cap = ncap;
	}
	pc->arena[pc->arena_len].kind = kind;
	pc->arena[pc->arena_len].ptr = p;
	pc->arena_len++;
	return p;
}

dd_term *
dd_arena_term(dd_parse_ctx *pc, dd_term_kind kind)
{
	return arena_track(pc, DD_A_TERM, dd_term_new(kind));
}

dd_expr *
dd_arena_expr(dd_parse_ctx *pc, dd_expr_kind kind)
{
	return arena_track(pc, DD_A_EXPR, dd_expr_new(kind));
}

dd_stmt *
dd_arena_stmt(dd_parse_ctx *pc, dd_stmt_kind kind)
{
	return arena_track(pc, DD_A_STMT, dd_stmt_new(kind));
}

void *
dd_arena_raw(dd_parse_ctx *pc, void *p)
{
	if (p != NULL)
		arena_track(pc, DD_A_RAW, p);
	return p;
}

char *
dd_arena_strdup(dd_parse_ctx *pc, const char *s)
{
	return dd_arena_raw(pc, __dtfl_strdup(s));
}

void
dd_arena_retrack(dd_parse_ctx *pc, void *old, void *new)
{
	size_t i;

	if (old != NULL)
		for (i = 0; i < pc->arena_len; i++)
			if (pc->arena[i].kind == DD_A_RAW &&
			    pc->arena[i].ptr == old) {
				pc->arena[i].ptr = new;
				return;
			}
	(void)dd_arena_raw(pc, new);
}

void
dd_arena_untrack(dd_parse_ctx *pc, void *p)
{
	size_t i;

	if (p == NULL)
		return;
	for (i = 0; i < pc->arena_len; i++)
		if (pc->arena[i].kind == DD_A_RAW && pc->arena[i].ptr == p) {
			pc->arena[i].ptr = NULL; /* NULL frees to a no-op */
			return;
		}
}

/* Free a term box and its own owned vec backing, but not child boxes. */
static void
term_free_shallow(dd_term *t)
{
	switch (t->kind) {
	case DD_T_TUPLE:
	case DD_T_LIST:
		__dtfl_free(t->u.seq.data);
		break;
	case DD_T_HASH:
		__dtfl_free(t->u.hash.data);
		break;
	case DD_T_CASE:
		__dtfl_free(t->u.case_.arms.data);
		break;
	default:
		break;
	}
	__dtfl_free(t);
}

static void
expr_free_shallow(dd_expr *e)
{
	if (e->kind == DD_E_CONCAT)
		__dtfl_free(e->u.concat.data);
	__dtfl_free(e);
}

static void
stmt_free_shallow(dd_stmt *s)
{
	if (s->kind == DD_S_SCOPE)
		__dtfl_free(s->body.data);
	__dtfl_free(s);
}

void
dd_arena_free_all(dd_parse_ctx *pc)
{
	size_t i;

	for (i = 0; i < pc->arena_len; i++) {
		dd_arena_ent *e = &pc->arena[i];

		switch (e->kind) {
		case DD_A_RAW:
			__dtfl_free(e->ptr);
			break;
		case DD_A_TERM:
			term_free_shallow(e->ptr);
			break;
		case DD_A_EXPR:
			expr_free_shallow(e->ptr);
			break;
		case DD_A_STMT:
			stmt_free_shallow(e->ptr);
			break;
		}
	}
	__dtfl_free(pc->arena);
	dd_arena_init(pc);
}

/* Deep-clone a term, registering every produced box/string in the arena. */
dd_term *
dd_arena_clone(dd_parse_ctx *pc, const dd_term *t)
{
	dd_term *o = dd_arena_term(pc, t->kind);
	size_t i;

	switch (t->kind) {
	case DD_T_VAR:
		o->u.var = t->u.var;
		break;
	case DD_T_BOUND:
		o->u.bound = t->u.bound;
		break;
	case DD_T_INT:
		o->u.int_lit = t->u.int_lit;
		break;
	case DD_T_TUPLE:
	case DD_T_LIST:
		for (i = 0; i < t->u.seq.len; i++)
			dd_vec_push(&o->u.seq, dd_arena_clone(pc, t->u.seq.data[i]));
		break;
	case DD_T_HASH:
		for (i = 0; i < t->u.hash.len; i++)
			dd_vec_push(&o->u.hash, dd_arena_clone(pc, t->u.hash.data[i]));
		break;
	case DD_T_SPREAD:
		o->u.spread = dd_arena_clone(pc, t->u.spread);
		break;
	case DD_T_PROJ:
		o->u.proj.base = dd_arena_clone(pc, t->u.proj.base);
		o->u.proj.index = t->u.proj.index;
		break;
	case DD_T_INJECT:
		o->u.inject.tag = dd_arena_clone(pc, t->u.inject.tag);
		o->u.inject.payload = dd_arena_clone(pc, t->u.inject.payload);
		break;
	case DD_T_CASE:
		o->u.case_.scrutinee = dd_arena_clone(pc, t->u.case_.scrutinee);
		for (i = 0; i < t->u.case_.arms.len; i++)
			dd_vec_push(&o->u.case_.arms,
			    dd_arena_clone(pc, t->u.case_.arms.data[i]));
		o->u.case_.dflt = t->u.case_.dflt
		    ? dd_arena_clone(pc, t->u.case_.dflt) : NULL;
		break;
	case DD_T_FOLD:
		o->u.fold.list = dd_arena_clone(pc, t->u.fold.list);
		o->u.fold.init = dd_arena_clone(pc, t->u.fold.init);
		o->u.fold.step = dd_arena_clone(pc, t->u.fold.step);
		break;
	case DD_T_IF:
		o->u.if_.cond = dd_arena_clone(pc, t->u.if_.cond);
		o->u.if_.then_ = dd_arena_clone(pc, t->u.if_.then_);
		o->u.if_.els = dd_arena_clone(pc, t->u.if_.els);
		break;
	case DD_T_UNARY:
		o->u.unary.op = t->u.unary.op;
		o->u.unary.tag = t->u.unary.tag;
		o->u.unary.arg = dd_arena_clone(pc, t->u.unary.arg);
		break;
	case DD_T_BINARY:
		o->u.binary.op = t->u.binary.op;
		o->u.binary.l = dd_arena_clone(pc, t->u.binary.l);
		o->u.binary.r = dd_arena_clone(pc, t->u.binary.r);
		break;
	}
	return o;
}

/* ------------------------------------------------------------------ */
/* Expr                                                                */
/* ------------------------------------------------------------------ */

dd_expr *
dd_expr_new(dd_expr_kind kind)
{
	dd_expr *e = __dtfl_calloc(1, sizeof *e);

	e->kind = kind;
	return e;
}

void
dd_expr_free(dd_expr *e)
{
	if (e == NULL)
		return;
	switch (e->kind) {
	case DD_E_INPUT:
		break;
	case DD_E_IMPORT:
	case DD_E_NAME:
		__dtfl_free(e->u.name);
		break;
	case DD_E_QUALIFIED:
		__dtfl_free(e->u.qualified.scope);
		__dtfl_free(e->u.qualified.field);
		break;
	case DD_E_MAP:
		dd_expr_free(e->u.map.e);
		dd_projection_free_fields(&e->u.map.proj);
		break;
	case DD_E_JOIN:
		dd_expr_free(e->u.join.l);
		dd_expr_free(e->u.join.r);
		dd_projection_free_fields(&e->u.join.proj);
		break;
	case DD_E_REDUCE:
		dd_expr_free(e->u.reduce.e);
		break;
	case DD_E_FILTER:
	case DD_E_ENTER_AT:
	case DD_E_FLATMAP:
		dd_expr_free(e->u.op_term.e);
		dd_term_free(e->u.op_term.term);
		break;
	case DD_E_NEGATE:
	case DD_E_LIFT_ITER:
	case DD_E_ARRANGE:
		dd_expr_free(e->u.op.e);
		break;
	case DD_E_INSPECT:
		dd_expr_free(e->u.inspect.e);
		__dtfl_free(e->u.inspect.label);
		break;
	case DD_E_CONCAT: {
		size_t i;

		for (i = 0; i < e->u.concat.len; i++)
			dd_expr_free(e->u.concat.data[i]);
		dd_vec_free(&e->u.concat);
		break;
	}
	}
	__dtfl_free(e);
}

/* ------------------------------------------------------------------ */
/* Stmt                                                                */
/* ------------------------------------------------------------------ */

dd_stmt *
dd_stmt_new(dd_stmt_kind kind)
{
	dd_stmt *s = __dtfl_calloc(1, sizeof *s);

	s->kind = kind;
	return s;
}

void
dd_stmt_free(dd_stmt *s)
{
	if (s == NULL)
		return;
	__dtfl_free(s->name);
	dd_expr_free(s->expr);
	if (s->kind == DD_S_SCOPE)
		dd_stmts_free(&s->body);
	__dtfl_free(s);
}

void
dd_stmts_free(dd_vec *stmts)
{
	size_t i;

	for (i = 0; i < stmts->len; i++)
		dd_stmt_free(stmts->data[i]);
	dd_vec_free(stmts);
}

/* ------------------------------------------------------------------ */
/* Shared builtin builder (parse/mod.rs build_builtin)                 */
/* ------------------------------------------------------------------ */

/* Pull a value term out of the args vec (consumes ownership). */
static dd_term *
take(dd_vec *args, size_t i)
{
	dd_term *t = args->data[i];

	args->data[i] = NULL;
	return t;
}

/* True if `t` is an int literal; writes its value to *out. */
static bool
is_int_arg(const dd_term *t, int64_t *out)
{
	if (t == NULL || t->kind != DD_T_INT)
		return false;
	*out = t->u.int_lit;
	return true;
}

/*
 * Build a scalar builtin from its parsed argument terms (parse/mod.rs
 * build_builtin).  The args (and every term they hold) are arena-tracked,
 * so this never deep-frees: on an arity/shape error it records the args
 * backing in the arena and reports through pc, which unwinds and the
 * caller's dd_arena_abort reclaims everything.  On success the moved
 * children stay arena-tracked and the result node is tracked too.
 */
dd_term *
dd_build_builtin(dd_parse_ctx *pc, const char *name, dd_vec *args)
{
	dd_term *t;

	/* parse_args left the backing arena-tracked (so a longjmp mid-parse
	 * reclaims it).  Take ownership here: each path below either frees it
	 * (dd_vec_free), moves it into a node, or re-tracks it before an
	 * error longjmp. */
	dd_arena_untrack(pc, args->data);

/* On error: hand the args backing to the arena, then unwind. */
#define NEED(cond, msg)							\
	do {								\
		if (!(cond)) {						\
			dd_arena_raw(pc, args->data);			\
			dd_parse_error(pc, "%s", msg);			\
		}							\
	} while (0)

	if (strcmp(name, "tuple") == 0) {
		t = dd_arena_term(pc, DD_T_TUPLE);
		t->u.seq = *args;
		args->data = NULL;
		args->len = args->cap = 0;
		return t;
	}
	if (strcmp(name, "list") == 0) {
		t = dd_arena_term(pc, DD_T_LIST);
		t->u.seq = *args;
		args->data = NULL;
		args->len = args->cap = 0;
		return t;
	}
	if (strcmp(name, "inject") == 0 || strcmp(name, "variant") == 0) {
		NEED(args->len == 2, "inject(tag, payload)");
		t = dd_arena_term(pc, DD_T_INJECT);
		t->u.inject.tag = take(args, 0);
		t->u.inject.payload = take(args, 1);
		dd_vec_free(args);
		return t;
	}
	if (strcmp(name, "case") == 0) {
		size_t i;

		NEED(args->len >= 2, "case(scrutinee, arm0, ...)");
		t = dd_arena_term(pc, DD_T_CASE);
		t->u.case_.scrutinee = take(args, 0);
		t->u.case_.dflt = NULL;
		for (i = 1; i < args->len; i++)
			dd_vec_push(&t->u.case_.arms, take(args, i));
		dd_vec_free(args);
		return t;
	}
	if (strcmp(name, "fold") == 0) {
		NEED(args->len == 3, "fold(list, init, step)");
		t = dd_arena_term(pc, DD_T_FOLD);
		t->u.fold.list = take(args, 0);
		t->u.fold.init = take(args, 1);
		t->u.fold.step = take(args, 2);
		dd_vec_free(args);
		return t;
	}
	if (strcmp(name, "proj") == 0) {
		int64_t i;

		NEED(args->len == 2, "proj(value, index)");
		if (!is_int_arg(args->data[1], &i)) {
			dd_arena_raw(pc, args->data);
			dd_parse_error(pc, "proj(value, index): index must be "
			    "an int literal");
		}
		t = dd_arena_term(pc, DD_T_PROJ);
		t->u.proj.base = take(args, 0);
		t->u.proj.index = (size_t)i;
		dd_vec_free(args);
		return t;
	}
	if (strcmp(name, "len") == 0) {
		NEED(args->len == 1, "len(value)");
		t = dd_arena_term(pc, DD_T_UNARY);
		t->u.unary.op = DD_UN_LEN;
		t->u.unary.arg = take(args, 0);
		dd_vec_free(args);
		return t;
	}
	if (strcmp(name, "istag") == 0) {
		int64_t tag;

		NEED(args->len == 2, "istag(tag, value)");
		if (!is_int_arg(args->data[0], &tag)) {
			dd_arena_raw(pc, args->data);
			dd_parse_error(pc, "istag(tag, value): tag must be an "
			    "int literal");
		}
		t = dd_arena_term(pc, DD_T_UNARY);
		t->u.unary.op = DD_UN_ISTAG;
		t->u.unary.tag = (uint32_t)tag;
		t->u.unary.arg = take(args, 1);
		dd_vec_free(args);
		return t;
	}
	if (strcmp(name, "not") == 0) {
		NEED(args->len == 1, "not(value)");
		t = dd_arena_term(pc, DD_T_UNARY);
		t->u.unary.op = DD_UN_NOT;
		t->u.unary.arg = take(args, 0);
		dd_vec_free(args);
		return t;
	}
	if (strcmp(name, "or") == 0) {
		NEED(args->len == 2, "or(a, b)");
		t = dd_arena_term(pc, DD_T_BINARY);
		t->u.binary.op = DD_BIN_OR;
		t->u.binary.l = take(args, 0);
		t->u.binary.r = take(args, 1);
		dd_vec_free(args);
		return t;
	}
	if (strcmp(name, "if") == 0) {
		NEED(args->len == 3, "if(cond, then, els)");
		t = dd_arena_term(pc, DD_T_IF);
		t->u.if_.cond = take(args, 0);
		t->u.if_.then_ = take(args, 1);
		t->u.if_.els = take(args, 2);
		dd_vec_free(args);
		return t;
	}
	if (strcmp(name, "hash") == 0) {
		NEED(args->len >= 2, "hash(bound, key, ...)");
		t = dd_arena_term(pc, DD_T_HASH);
		t->u.hash = *args;
		args->data = NULL;
		args->len = args->cap = 0;
		return t;
	}
	dd_arena_raw(pc, args->data);
	dd_parse_error(pc, "Unknown scalar builtin: %s", name);
#undef NEED
}
