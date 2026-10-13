/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/parse/applicative.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/parse_applicative.c: the applicative (.ddir) surface parser.
 *
 * S-expression-like syntax: MAP(e, proj), JOIN(e1, e2, proj),
 * REDUCE(e, MIN).  It shares the scalar term grammar with the pipe
 * front-end and produces the same Expr/Stmt AST.  There are no
 * constructor declarations or pattern binders here (those are pipe-only
 * sugar); the scalar grammar is the common core.
 */
#include "ddir_int.h"
#include "parse_int.h"

#include "../base/base_int.h"

#include <string.h>

typedef enum atok_kind {
	A_LET, A_VAR, A_SCOPE, A_EXPORT,
	A_INPUT, A_IMPORT, A_MAP, A_JOIN, A_REDUCE, A_CONCAT, A_ARRANGE,
	A_FILTER, A_NEGATE, A_ENTER_AT, A_LIFT_ITER, A_FLATMAP, A_INSPECT,
	A_MIN, A_DISTINCT, A_COUNT, A_COLLECT,
	A_IDENT, A_INT, A_STR,
	A_DOLLAR, A_CARET, A_LPAREN, A_RPAREN, A_LBRACE, A_RBRACE, A_LBRACKET,
	A_RBRACKET, A_COMMA, A_SEMI, A_COLON, A_COLONCOLON, A_EQ, A_EQEQ,
	A_NOTEQ, A_LT, A_LTEQ, A_GT, A_GTEQ, A_ANDAND, A_PLUS, A_MINUS,
	A_STAR, A_EOF
} atok_kind;

typedef struct atok {
	atok_kind kind;
	int64_t int_v;
	char *str;
} atok;

typedef struct alexer {
	dd_parse_ctx *pc;
	dd_vec toks;
} alexer;

static atok *
atok_new(atok_kind kind)
{
	atok *t = __dtfl_calloc(1, sizeof *t);

	t->kind = kind;
	return t;
}

static void
atoks_free(dd_vec *toks)
{
	size_t i;

	for (i = 0; i < toks->len; i++) {
		atok *t = toks->data[i];

		__dtfl_free(t->str);
		__dtfl_free(t);
	}
	dd_vec_free(toks);
}

static atok_kind
appl_keyword(const char *s, bool *is_kw)
{
	static const struct {
		const char *word;
		atok_kind kind;
	} kw[] = {
		{ "let", A_LET }, { "var", A_VAR }, { "scope", A_SCOPE },
		{ "export", A_EXPORT },
		{ "INPUT", A_INPUT }, { "IMPORT", A_IMPORT },
		{ "MAP", A_MAP }, { "JOIN", A_JOIN }, { "REDUCE", A_REDUCE },
		{ "CONCAT", A_CONCAT }, { "ARRANGE", A_ARRANGE },
		{ "FILTER", A_FILTER }, { "NEGATE", A_NEGATE },
		{ "ENTER_AT", A_ENTER_AT }, { "INSPECT", A_INSPECT },
		{ "MIN", A_MIN }, { "DISTINCT", A_DISTINCT }, { "COUNT", A_COUNT },
		{ "COLLECT", A_COLLECT },
		{ "LIFT_ITER", A_LIFT_ITER }, { "FLATMAP", A_FLATMAP },
	};
	size_t i;

	for (i = 0; i < DTFL_ARRAY_LEN(kw); i++)
		if (strcmp(s, kw[i].word) == 0) {
			*is_kw = true;
			return kw[i].kind;
		}
	*is_kw = false;
	return A_IDENT;
}

static void
atokenize(alexer *lx, const char *input)
{
	const char *p = input;

	while (*p != '\0') {
		char c = *p;

		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			p++;
			continue;
		}
		if (c == '-' && p[1] == '-') {
			while (*p != '\0' && *p != '\n')
				p++;
			continue;
		}
		switch (c) {
		case '(': dd_vec_push(&lx->toks, atok_new(A_LPAREN)); p++; continue;
		case ')': dd_vec_push(&lx->toks, atok_new(A_RPAREN)); p++; continue;
		case '{': dd_vec_push(&lx->toks, atok_new(A_LBRACE)); p++; continue;
		case '}': dd_vec_push(&lx->toks, atok_new(A_RBRACE)); p++; continue;
		case '[': dd_vec_push(&lx->toks, atok_new(A_LBRACKET)); p++; continue;
		case ']': dd_vec_push(&lx->toks, atok_new(A_RBRACKET)); p++; continue;
		case ',': dd_vec_push(&lx->toks, atok_new(A_COMMA)); p++; continue;
		case ';': dd_vec_push(&lx->toks, atok_new(A_SEMI)); p++; continue;
		case '+': dd_vec_push(&lx->toks, atok_new(A_PLUS)); p++; continue;
		case '*': dd_vec_push(&lx->toks, atok_new(A_STAR)); p++; continue;
		case '$': dd_vec_push(&lx->toks, atok_new(A_DOLLAR)); p++; continue;
		case '^': dd_vec_push(&lx->toks, atok_new(A_CARET)); p++; continue;
		case '&':
			p++;
			if (*p == '&') { p++; dd_vec_push(&lx->toks, atok_new(A_ANDAND)); }
			else dd_parse_error(lx->pc, "Expected && after &");
			continue;
		case '=':
			p++;
			if (*p == '=') { p++; dd_vec_push(&lx->toks, atok_new(A_EQEQ)); }
			else dd_vec_push(&lx->toks, atok_new(A_EQ));
			continue;
		case '!':
			p++;
			if (*p == '=') { p++; dd_vec_push(&lx->toks, atok_new(A_NOTEQ)); }
			else dd_parse_error(lx->pc, "Expected != after !");
			continue;
		case '<':
			p++;
			if (*p == '=') { p++; dd_vec_push(&lx->toks, atok_new(A_LTEQ)); }
			else dd_vec_push(&lx->toks, atok_new(A_LT));
			continue;
		case '>':
			p++;
			if (*p == '=') { p++; dd_vec_push(&lx->toks, atok_new(A_GTEQ)); }
			else dd_vec_push(&lx->toks, atok_new(A_GT));
			continue;
		case '-':
			p++;
			dd_vec_push(&lx->toks, atok_new(A_MINUS));
			continue;
		case ':':
			p++;
			if (*p == ':') { p++; dd_vec_push(&lx->toks, atok_new(A_COLONCOLON)); }
			else dd_vec_push(&lx->toks, atok_new(A_COLON));
			continue;
		case '"': {
			atok *t;
			const char *start;
			size_t n;

			p++;
			start = p;
			while (*p != '\0' && *p != '"')
				p++;
			if (*p != '"')
				dd_parse_error(lx->pc, "Unterminated string literal");
			n = (size_t)(p - start);
			t = atok_new(A_STR);
			t->str = __dtfl_alloc(n + 1);
			memcpy(t->str, start, n);
			t->str[n] = '\0';
			dd_vec_push(&lx->toks, t);
			p++;
			continue;
		}
		default:
			break;
		}
		if (c >= '0' && c <= '9') {
			atok *t = atok_new(A_INT);
			int64_t n = 0;

			while (*p >= '0' && *p <= '9') {
				n = n * 10 + (*p - '0');
				p++;
			}
			t->int_v = n;
			dd_vec_push(&lx->toks, t);
			continue;
		}
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
			const char *start = p;
			size_t n;
			char *id;
			bool is_kw;
			atok_kind kind;

			while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			    (*p >= '0' && *p <= '9') || *p == '_')
				p++;
			n = (size_t)(p - start);
			id = __dtfl_alloc(n + 1);
			memcpy(id, start, n);
			id[n] = '\0';
			kind = appl_keyword(id, &is_kw);
			if (is_kw) {
				__dtfl_free(id);
				dd_vec_push(&lx->toks, atok_new(kind));
			} else {
				atok *t = atok_new(A_IDENT);

				t->str = id;
				dd_vec_push(&lx->toks, t);
			}
			continue;
		}
		dd_parse_error(lx->pc, "Unexpected character: %c", c);
	}
	dd_vec_push(&lx->toks, atok_new(A_EOF));
}

typedef struct aparser {
	dd_parse_ctx *pc;
	atok **toks;
	size_t pos;
} aparser;

static atok *apk(aparser *p) { return p->toks[p->pos]; }
static atok *anx(aparser *p) { return p->toks[p->pos++]; }

static void
aexpect(aparser *p, atok_kind k)
{
	atok *t = anx(p);

	if (t->kind != k)
		dd_parse_error(p->pc, "Expected token %d, got %d", k, t->kind);
}

static char *
aparse_ident(aparser *p)
{
	atok *t = anx(p);

	if (t->kind != A_IDENT)
		dd_parse_error(p->pc, "Expected ident");
	return dd_arena_strdup(p->pc, t->str);
}

static dd_term *aparse_term(aparser *p);
static dd_term *aparse_unary(aparser *p);
static dd_expr *aparse_expr(aparser *p);

static dd_vec
aparse_args(aparser *p)
{
	dd_vec args = { 0 };

	aexpect(p, A_LPAREN);
	if (apk(p)->kind != A_RPAREN) {
		dd_vec_push(&args, aparse_term(p));
		while (apk(p)->kind == A_COMMA) {
			anx(p);
			dd_vec_push(&args, aparse_term(p));
		}
	}
	aexpect(p, A_RPAREN);
	return args;
}

static dd_term *
aparse_primary_term(aparser *p)
{
	dd_term *base;
	atok *t = apk(p);

	switch (t->kind) {
	case A_DOLLAR: {
		atok *n;

		anx(p);
		n = anx(p);
		if (n->kind != A_INT)
			dd_parse_error(p->pc, "Expected int after $");
		base = dd_arena_term(p->pc, DD_T_VAR);
		base->u.var = (size_t)n->int_v;
		break;
	}
	case A_CARET: {
		atok *k;

		anx(p);
		k = anx(p);
		if (k->kind != A_INT)
			dd_parse_error(p->pc, "Expected int after ^");
		base = dd_arena_term(p->pc, DD_T_BOUND);
		base->u.bound = (size_t)k->int_v;
		break;
	}
	case A_INT:
		anx(p);
		base = dd_arena_term(p->pc, DD_T_INT);
		base->u.int_lit = t->int_v;
		break;
	case A_MINUS:
		anx(p);
		base = dd_arena_term(p->pc, DD_T_UNARY);
		base->u.unary.op = DD_UN_NEG;
		base->u.unary.arg = aparse_unary(p);
		break;
	case A_LPAREN:
		anx(p);
		base = aparse_term(p);
		aexpect(p, A_RPAREN);
		break;
	case A_IDENT: {
		const char *name = t->str; /* lives in the token */
		dd_vec args;

		anx(p);
		args = aparse_args(p);
		base = dd_build_builtin(p->pc, name, &args);
		break;
	}
	default:
		dd_parse_error(p->pc, "Unexpected token in term: %d", t->kind);
	}
	while (apk(p)->kind == A_LBRACKET) {
		atok *i;
		dd_term *proj;

		anx(p);
		i = anx(p);
		if (i->kind != A_INT)
			dd_parse_error(p->pc, "Expected int index");
		aexpect(p, A_RBRACKET);
		proj = dd_arena_term(p->pc, DD_T_PROJ);
		proj->u.proj.base = base;
		proj->u.proj.index = (size_t)i->int_v;
		base = proj;
	}
	return base;
}

static dd_term *
aparse_unary(aparser *p)
{
	if (apk(p)->kind == A_MINUS) {
		dd_term *t = dd_arena_term(p->pc, DD_T_UNARY);

		anx(p);
		t->u.unary.op = DD_UN_NEG;
		t->u.unary.arg = aparse_unary(p);
		return t;
	}
	return aparse_primary_term(p);
}

static dd_term *
aparse_mul(aparser *p)
{
	dd_term *left = aparse_unary(p);

	while (apk(p)->kind == A_STAR) {
		dd_term *bin = dd_arena_term(p->pc, DD_T_BINARY);

		anx(p);
		bin->u.binary.op = DD_BIN_MUL;
		bin->u.binary.l = left;
		bin->u.binary.r = aparse_unary(p);
		left = bin;
	}
	return left;
}

static dd_term *
aparse_add(aparser *p)
{
	dd_term *left = aparse_mul(p);

	for (;;) {
		dd_binop op;
		dd_term *bin;

		if (apk(p)->kind == A_PLUS)
			op = DD_BIN_ADD;
		else if (apk(p)->kind == A_MINUS)
			op = DD_BIN_SUB;
		else
			break;
		anx(p);
		bin = dd_arena_term(p->pc, DD_T_BINARY);
		bin->u.binary.op = op;
		bin->u.binary.l = left;
		bin->u.binary.r = aparse_mul(p);
		left = bin;
	}
	return left;
}

static dd_term *
aparse_cmp(aparser *p)
{
	dd_term *left = aparse_add(p);
	dd_binop op;
	dd_term *bin;

	switch (apk(p)->kind) {
	case A_EQEQ: op = DD_BIN_EQ; break;
	case A_NOTEQ: op = DD_BIN_NE; break;
	case A_LT: op = DD_BIN_LT; break;
	case A_LTEQ: op = DD_BIN_LE; break;
	case A_GT: op = DD_BIN_GT; break;
	case A_GTEQ: op = DD_BIN_GE; break;
	default: return left;
	}
	anx(p);
	bin = dd_arena_term(p->pc, DD_T_BINARY);
	bin->u.binary.op = op;
	bin->u.binary.l = left;
	bin->u.binary.r = aparse_add(p);
	return bin;
}

static dd_term *
aparse_logic(aparser *p)
{
	dd_term *left = aparse_cmp(p);

	while (apk(p)->kind == A_ANDAND) {
		dd_term *bin = dd_arena_term(p->pc, DD_T_BINARY);

		anx(p);
		bin->u.binary.op = DD_BIN_AND;
		bin->u.binary.l = left;
		bin->u.binary.r = aparse_cmp(p);
		left = bin;
	}
	return left;
}

static dd_term *
aparse_term(aparser *p)
{
	return aparse_logic(p);
}

/* A single projection field; a bare $n (no index) splices (Spread). */
static dd_term *
aparse_proj_field(aparser *p)
{
	if (apk(p)->kind == A_DOLLAR &&
	    p->toks[p->pos + 1]->kind == A_INT &&
	    p->toks[p->pos + 2]->kind != A_LBRACKET) {
		atok *n;
		dd_term *spread, *var;

		anx(p);
		n = anx(p);
		var = dd_arena_term(p->pc, DD_T_VAR);
		var->u.var = (size_t)n->int_v;
		spread = dd_arena_term(p->pc, DD_T_SPREAD);
		spread->u.spread = var;
		return spread;
	}
	return aparse_term(p);
}

static dd_vec
aparse_field_list_until(aparser *p, const atok_kind *terms, size_t nterms)
{
	dd_vec fields = { 0 };
	size_t i;

	for (i = 0; i < nterms; i++)
		if (apk(p)->kind == terms[i])
			return fields;
	dd_vec_push(&fields, aparse_proj_field(p));
	while (apk(p)->kind == A_COMMA) {
		anx(p);
		dd_vec_push(&fields, aparse_proj_field(p));
	}
	return fields;
}

static dd_projection
aparse_projection(aparser *p)
{
	static const atok_kind key_terms[] = { A_SEMI, A_RPAREN };
	static const atok_kind val_terms[] = { A_RPAREN };
	dd_vec key, val = { 0 };
	dd_projection proj;
	dd_term *kt, *vt;

	aexpect(p, A_LPAREN);
	key = aparse_field_list_until(p, key_terms, 2);
	if (apk(p)->kind == A_SEMI) {
		anx(p);
		val = aparse_field_list_until(p, val_terms, 1);
	}
	aexpect(p, A_RPAREN);
	kt = dd_arena_term(p->pc, DD_T_TUPLE);
	kt->u.seq = key;
	vt = dd_arena_term(p->pc, DD_T_TUPLE);
	vt->u.seq = val;
	proj.key = kt;
	proj.val = vt;
	return proj;
}

static dd_reducer
aparse_reducer(aparser *p)
{
	switch (anx(p)->kind) {
	case A_MIN: return DD_RED_MIN;
	case A_DISTINCT: return DD_RED_DISTINCT;
	case A_COUNT: return DD_RED_COUNT;
	case A_COLLECT: return DD_RED_COLLECT;
	default:
		dd_parse_error(p->pc, "Expected reducer");
	}
}

static dd_expr *
aparse_expr(aparser *p)
{
	dd_expr *e;
	atok *t = apk(p);

	switch (t->kind) {
	case A_INPUT: {
		atok *n;

		anx(p);
		n = anx(p);
		if (n->kind != A_INT)
			dd_parse_error(p->pc, "Expected int");
		e = dd_arena_expr(p->pc, DD_E_INPUT);
		e->u.input = (size_t)n->int_v;
		return e;
	}
	case A_IMPORT: {
		atok *s;

		anx(p);
		s = anx(p);
		if (s->kind != A_STR)
			dd_parse_error(p->pc,
			    "Expected string literal after IMPORT");
		e = dd_arena_expr(p->pc, DD_E_IMPORT);
		e->u.name = dd_arena_strdup(p->pc, s->str);
		return e;
	}
	case A_MAP:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_MAP);
		e->u.map.e = aparse_expr(p);
		aexpect(p, A_COMMA);
		e->u.map.proj = aparse_projection(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_JOIN:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_JOIN);
		e->u.join.l = aparse_expr(p);
		aexpect(p, A_COMMA);
		e->u.join.r = aparse_expr(p);
		aexpect(p, A_COMMA);
		e->u.join.proj = aparse_projection(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_REDUCE:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_REDUCE);
		e->u.reduce.e = aparse_expr(p);
		aexpect(p, A_COMMA);
		e->u.reduce.reducer = aparse_reducer(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_FILTER:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_FILTER);
		e->u.op_term.e = aparse_expr(p);
		aexpect(p, A_COMMA);
		e->u.op_term.term = aparse_term(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_NEGATE:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_NEGATE);
		e->u.op.e = aparse_expr(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_ENTER_AT:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_ENTER_AT);
		e->u.op_term.e = aparse_expr(p);
		aexpect(p, A_COMMA);
		e->u.op_term.term = aparse_term(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_LIFT_ITER:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_LIFT_ITER);
		e->u.op.e = aparse_expr(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_FLATMAP:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_FLATMAP);
		e->u.op_term.e = aparse_expr(p);
		aexpect(p, A_COMMA);
		e->u.op_term.term = aparse_term(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_INSPECT:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_INSPECT);
		e->u.inspect.e = aparse_expr(p);
		aexpect(p, A_COMMA);
		e->u.inspect.label = aparse_ident(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_CONCAT:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_CONCAT);
		dd_vec_push(&e->u.concat, aparse_expr(p));
		while (apk(p)->kind == A_COMMA) {
			anx(p);
			dd_vec_push(&e->u.concat, aparse_expr(p));
		}
		aexpect(p, A_RPAREN);
		return e;
	case A_ARRANGE:
		anx(p);
		aexpect(p, A_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_ARRANGE);
		e->u.op.e = aparse_expr(p);
		aexpect(p, A_RPAREN);
		return e;
	case A_IDENT: {
		char *n = aparse_ident(p);

		if (apk(p)->kind == A_COLONCOLON) {
			anx(p);
			e = dd_arena_expr(p->pc, DD_E_QUALIFIED);
			e->u.qualified.scope = n;
			e->u.qualified.field = aparse_ident(p);
		} else {
			e = dd_arena_expr(p->pc, DD_E_NAME);
			e->u.name = n;
		}
		return e;
	}
	default:
		dd_parse_error(p->pc, "Unexpected token in expr: %d", t->kind);
	}
}

static void aparse_program_into(aparser *p, dd_vec *out);

/* Fill the pre-allocated, already-registered statement `s`. */
static void
aparse_stmt(aparser *p, dd_stmt *s)
{
	switch (apk(p)->kind) {
	case A_LET:
		anx(p);
		s->kind = DD_S_LET;
		s->name = aparse_ident(p);
		aexpect(p, A_EQ);
		s->expr = aparse_expr(p);
		aexpect(p, A_SEMI);
		return;
	case A_VAR:
		anx(p);
		s->kind = DD_S_VAR;
		s->name = aparse_ident(p);
		aexpect(p, A_EQ);
		s->expr = aparse_expr(p);
		aexpect(p, A_SEMI);
		return;
	case A_EXPORT: {
		atok *name;

		anx(p);
		s->kind = DD_S_EXPORT;
		name = anx(p);
		if (name->kind != A_STR)
			dd_parse_error(p->pc,
			    "Expected string literal after `export`");
		s->name = dd_arena_strdup(p->pc, name->str);
		aexpect(p, A_EQ);
		s->expr = aparse_expr(p);
		aexpect(p, A_SEMI);
		return;
	}
	case A_IDENT:
		s->kind = DD_S_SCOPE;
		s->name = aparse_ident(p);
		aexpect(p, A_COLON);
		if (apk(p)->kind == A_SCOPE)
			anx(p);
		aexpect(p, A_LBRACE);
		aparse_program_into(p, &s->body);
		aexpect(p, A_RBRACE);
		return;
	default:
		dd_parse_error(p->pc, "Unexpected token: %d", apk(p)->kind);
	}
}

static void
aparse_program_into(aparser *p, dd_vec *out)
{
	while (apk(p)->kind != A_EOF && apk(p)->kind != A_RBRACE) {
		dd_stmt *s = dd_arena_stmt(p->pc, DD_S_LET);

		dd_vec_push(out, s);
		aparse_stmt(p, s);
	}
}

bool
dd_parse_applicative(const char *input, dd_vec *out, dd_parse_ctx *pc)
{
	alexer lx = { pc, { 0 } };
	aparser p = { 0 };

	pc->error = false;
	pc->msg[0] = '\0';
	dd_arena_init(pc);
	*out = (dd_vec){ 0 };
	if (setjmp(pc->jb) != 0) {
		dd_arena_free_all(pc);
		__dtfl_free(out->data);
		*out = (dd_vec){ 0 };
		atoks_free(&lx.toks);
		return false;
	}
	atokenize(&lx, input);
	p.pc = pc;
	p.toks = (atok **)lx.toks.data;
	p.pos = 0;
	aparse_program_into(&p, out);
	if (apk(&p)->kind != A_EOF)
		dd_parse_error(pc, "trailing tokens after program");
	atoks_free(&lx.toks);
	return true;
}
