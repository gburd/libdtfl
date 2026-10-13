/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   interactive/src/parse/pipe.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/ddir/parse_pipe.c: the pipe (.ddp) surface parser.
 *
 * A program is a sequence of statements; an expression is a source piped
 * through operators (expr | op | op).  The collection language describes
 * the dataflow graph; the scalar language (Term) describes per-row value
 * computation.  con declarations declare constructors (parse-time sugar
 * for inject); case/fold introduce binders resolved by de Bruijn depth.
 * This front-end and the applicative one (parse_applicative.c) produce
 * the same Expr/Stmt AST.
 */
#include "ddir_int.h"
#include "parse_int.h"

#include "../base/base_int.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Tokens                                                              */
/* ------------------------------------------------------------------ */

typedef enum tok_kind {
	T_LET, T_VAR, T_EXPORT, T_CON, T_CASE, T_FOLD,
	T_INPUT, T_IMPORT, T_KEY, T_MAP, T_JOIN, T_MIN, T_DISTINCT, T_COUNT,
	T_COLLECT, T_ARRANGE, T_NEGATE, T_FILTER, T_ENTER_AT, T_LIFT_ITER,
	T_FLATMAP, T_INSPECT,
	T_IDENT, T_INT, T_STR,
	T_DOLLAR, T_CARET, T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET,
	T_RBRACKET, T_COMMA, T_SEMI, T_COLON, T_COLONCOLON, T_EQ, T_EQEQ,
	T_NOTEQ, T_LT, T_LTEQ, T_GT, T_GTEQ, T_ANDAND, T_FATARROW,
	T_PIPE, T_PLUS, T_MINUS, T_STAR, T_EOF
} tok_kind;

typedef struct tok {
	tok_kind kind;
	int64_t int_v; /* T_INT */
	char *str;     /* T_IDENT / T_STR (owned) */
} tok;

typedef struct lexer {
	dd_parse_ctx *pc;
	dd_vec toks; /* vec of tok* */
} lexer;

static tok *
tok_new(tok_kind kind)
{
	tok *t = __dtfl_calloc(1, sizeof *t);

	t->kind = kind;
	return t;
}

static void
toks_free(dd_vec *toks)
{
	size_t i;

	for (i = 0; i < toks->len; i++) {
		tok *t = toks->data[i];

		__dtfl_free(t->str);
		__dtfl_free(t);
	}
	dd_vec_free(toks);
}

/* Keyword lookup for the pipe syntax. */
static tok_kind
pipe_keyword(const char *s, bool *is_kw)
{
	static const struct {
		const char *word;
		tok_kind kind;
	} kw[] = {
		{ "let", T_LET }, { "var", T_VAR }, { "export", T_EXPORT },
		{ "con", T_CON }, { "case", T_CASE }, { "fold", T_FOLD },
		{ "input", T_INPUT }, { "import", T_IMPORT },
		{ "key", T_KEY }, { "map", T_MAP }, { "join", T_JOIN },
		{ "min", T_MIN }, { "distinct", T_DISTINCT },
		{ "count", T_COUNT }, { "collect", T_COLLECT },
		{ "flatmap", T_FLATMAP }, { "arrange", T_ARRANGE },
		{ "negate", T_NEGATE }, { "filter", T_FILTER },
		{ "enter_at", T_ENTER_AT }, { "inspect", T_INSPECT },
		{ "lift_iter", T_LIFT_ITER },
	};
	size_t i;

	for (i = 0; i < DTFL_ARRAY_LEN(kw); i++)
		if (strcmp(s, kw[i].word) == 0) {
			*is_kw = true;
			return kw[i].kind;
		}
	*is_kw = false;
	return T_IDENT;
}

static void
tokenize(lexer *lx, const char *input)
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
		case '(': dd_vec_push(&lx->toks, tok_new(T_LPAREN)); p++; continue;
		case ')': dd_vec_push(&lx->toks, tok_new(T_RPAREN)); p++; continue;
		case '{': dd_vec_push(&lx->toks, tok_new(T_LBRACE)); p++; continue;
		case '}': dd_vec_push(&lx->toks, tok_new(T_RBRACE)); p++; continue;
		case '[': dd_vec_push(&lx->toks, tok_new(T_LBRACKET)); p++; continue;
		case ']': dd_vec_push(&lx->toks, tok_new(T_RBRACKET)); p++; continue;
		case ',': dd_vec_push(&lx->toks, tok_new(T_COMMA)); p++; continue;
		case ';': dd_vec_push(&lx->toks, tok_new(T_SEMI)); p++; continue;
		case '|': dd_vec_push(&lx->toks, tok_new(T_PIPE)); p++; continue;
		case '+': dd_vec_push(&lx->toks, tok_new(T_PLUS)); p++; continue;
		case '*': dd_vec_push(&lx->toks, tok_new(T_STAR)); p++; continue;
		case '^': dd_vec_push(&lx->toks, tok_new(T_CARET)); p++; continue;
		case '$': dd_vec_push(&lx->toks, tok_new(T_DOLLAR)); p++; continue;
		case '&':
			p++;
			if (*p == '&') {
				p++;
				dd_vec_push(&lx->toks, tok_new(T_ANDAND));
			} else {
				dd_parse_error(lx->pc, "Expected && after &");
			}
			continue;
		case '=':
			p++;
			if (*p == '=') { p++; dd_vec_push(&lx->toks, tok_new(T_EQEQ)); }
			else if (*p == '>') { p++; dd_vec_push(&lx->toks, tok_new(T_FATARROW)); }
			else dd_vec_push(&lx->toks, tok_new(T_EQ));
			continue;
		case '!':
			p++;
			if (*p == '=') { p++; dd_vec_push(&lx->toks, tok_new(T_NOTEQ)); }
			else dd_parse_error(lx->pc, "Expected != after !");
			continue;
		case '<':
			p++;
			if (*p == '=') { p++; dd_vec_push(&lx->toks, tok_new(T_LTEQ)); }
			else dd_vec_push(&lx->toks, tok_new(T_LT));
			continue;
		case '>':
			p++;
			if (*p == '=') { p++; dd_vec_push(&lx->toks, tok_new(T_GTEQ)); }
			else dd_vec_push(&lx->toks, tok_new(T_GT));
			continue;
		case '-':
			p++;
			dd_vec_push(&lx->toks, tok_new(T_MINUS));
			continue;
		case ':':
			p++;
			if (*p == ':') { p++; dd_vec_push(&lx->toks, tok_new(T_COLONCOLON)); }
			else dd_vec_push(&lx->toks, tok_new(T_COLON));
			continue;
		case '"': {
			tok *t;
			const char *start;
			size_t n;

			p++;
			start = p;
			while (*p != '\0' && *p != '"')
				p++;
			if (*p != '"')
				dd_parse_error(lx->pc, "Unterminated string literal");
			n = (size_t)(p - start);
			t = tok_new(T_STR);
			t->str = __dtfl_alloc(n + 1);
			memcpy(t->str, start, n);
			t->str[n] = '\0';
			dd_vec_push(&lx->toks, t);
			p++; /* past the closing quote */
			continue;
		}
		default:
			break;
		}
		if (c >= '0' && c <= '9') {
			tok *t = tok_new(T_INT);
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
			tok_kind kind;

			while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			    (*p >= '0' && *p <= '9') || *p == '_')
				p++;
			n = (size_t)(p - start);
			id = __dtfl_alloc(n + 1);
			memcpy(id, start, n);
			id[n] = '\0';
			kind = pipe_keyword(id, &is_kw);
			if (is_kw) {
				__dtfl_free(id);
				dd_vec_push(&lx->toks, tok_new(kind));
			} else {
				tok *t = tok_new(T_IDENT);

				t->str = id;
				dd_vec_push(&lx->toks, t);
			}
			continue;
		}
		dd_parse_error(lx->pc, "Unexpected character: %c", c);
	}
	dd_vec_push(&lx->toks, tok_new(T_EOF));
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct binder {
	char *name;
	size_t level;
	size_t field;
} binder;

typedef struct cons_entry {
	char *name;
	int64_t tag;
	size_t arity;
} cons_entry;

typedef struct pparser {
	dd_parse_ctx *pc;
	tok **toks;
	size_t pos;
	dd_vec cons;	 /* vec of cons_entry* */
	dd_vec binders;	 /* vec of binder* (innermost last) */
	size_t depth;	 /* case/fold binders in scope */
} pparser;

static tok *
pk(pparser *p)
{
	return p->toks[p->pos];
}

static tok *
nx(pparser *p)
{
	return p->toks[p->pos++];
}

static void
expect(pparser *p, tok_kind k)
{
	tok *t = nx(p);

	if (t->kind != k)
		dd_parse_error(p->pc, "Expected token %d, got %d", k, t->kind);
}

/* Read an identifier; the returned string is arena-tracked (node-owned). */
static char *
parse_ident(pparser *p)
{
	tok *t = nx(p);

	if (t->kind != T_IDENT)
		dd_parse_error(p->pc, "Expected ident");
	return dd_arena_strdup(p->pc, t->str);
}

/* Read an identifier into a parser-local (not node-owned) string. */
static char *
parse_ident_local(pparser *p)
{
	tok *t = nx(p);

	if (t->kind != T_IDENT)
		dd_parse_error(p->pc, "Expected ident");
	return __dtfl_strdup(t->str);
}

static dd_expr *parse_pipe_expr(pparser *p);
static dd_term *parse_term(pparser *p);
static dd_term *parse_unary(pparser *p);
static dd_vec parse_args(pparser *p);

static const cons_entry *
find_cons(pparser *p, const char *name)
{
	size_t i;

	for (i = 0; i < p->cons.len; i++) {
		cons_entry *c = p->cons.data[i];

		if (strcmp(c->name, name) == 0)
			return c;
	}
	return NULL;
}

/* Resolve a bare name to a pattern binder, if any (innermost first). */
static dd_term *
resolve_binder(pparser *p, const char *name)
{
	size_t i;

	for (i = p->binders.len; i-- > 0;) {
		binder *b = p->binders.data[i];

		if (strcmp(b->name, name) == 0) {
			dd_term *bound = dd_arena_term(p->pc, DD_T_BOUND);
			dd_term *proj = dd_arena_term(p->pc, DD_T_PROJ);

			bound->u.bound = p->depth - b->level;
			proj->u.proj.base = bound;
			proj->u.proj.index = b->field;
			return proj;
		}
	}
	return NULL;
}

/* ---- scalar term grammar ---- */

/*
 * ponytail: the `case`/pattern scratch vecs (tags, tagged, names) are
 * freed on the normal path and on the handled error paths; a longjmp out
 * of parse_term mid-arm leaks their small backing arrays.  Bounded to
 * malformed `case` input; track them in the arena if it ever matters.
 */
static dd_term *
parse_pattern_case(pparser *p)
{
	dd_term *scrutinee;
	dd_vec tagged = { 0 };	/* vec of (tag, Term) via two parallel arrays */
	dd_vec tags = { 0 };
	dd_term *dflt = NULL;
	int64_t max_tag = -1;
	dd_term *result;
	size_t i;

	expect(p, T_CASE);
	scrutinee = parse_term(p);
	expect(p, T_LBRACE);
	while (pk(p)->kind != T_RBRACE) {
		char *name = parse_ident_local(p);

		if (strcmp(name, "_") == 0) {
			__dtfl_free(name);
			expect(p, T_FATARROW);
			dflt = parse_term(p);
		} else {
			const cons_entry *c = find_cons(p, name);
			dd_vec names = { 0 };
			size_t level, j;
			dd_term *arm;
			int64_t *tagp;

			if (c == NULL)
				dd_parse_error(p->pc,
				    "unknown constructor in pattern: `%s`", name);
			expect(p, T_LPAREN);
			if (pk(p)->kind != T_RPAREN) {
				dd_vec_push(&names, parse_ident_local(p));
				while (pk(p)->kind == T_COMMA) {
					nx(p);
					dd_vec_push(&names, parse_ident_local(p));
				}
			}
			expect(p, T_RPAREN);
			if (names.len != c->arity) {
				for (j = 0; j < names.len; j++)
					__dtfl_free(names.data[j]);
				dd_vec_free(&names);
				dd_parse_error(p->pc,
				    "pattern `%s` expects %zu binders, got %zu",
				    name, c->arity, names.len);
			}
			expect(p, T_FATARROW);
			p->depth += 1;
			level = p->depth;
			for (j = 0; j < names.len; j++) {
				binder *b = __dtfl_alloc(sizeof *b);

				b->name = names.data[j];
				b->level = level;
				b->field = j;
				dd_vec_push(&p->binders, b);
			}
			arm = parse_term(p);
			for (j = 0; j < names.len; j++) {
				binder *b = p->binders.data[--p->binders.len];

				__dtfl_free(b->name);
				__dtfl_free(b);
			}
			p->depth -= 1;
			dd_vec_free(&names);
			tagp = dd_arena_raw(p->pc, __dtfl_alloc(sizeof *tagp));
			*tagp = c->tag;
			dd_vec_push(&tags, tagp);
			dd_vec_push(&tagged, arm);
			if (c->tag > max_tag)
				max_tag = c->tag;
			__dtfl_free(name);
		}
		if (pk(p)->kind == T_COMMA)
			nx(p);
	}
	expect(p, T_RBRACE);
	/* Fill the arm table by tag, defaulting gaps. */
	result = dd_arena_term(p->pc, DD_T_CASE);
	result->u.case_.scrutinee = scrutinee;
	result->u.case_.dflt = dflt ? dd_arena_clone(p->pc, dflt) : NULL;
	for (i = 0; (int64_t)i <= max_tag; i++) {
		dd_term *arm = NULL;
		size_t j;

		for (j = 0; j < tags.len; j++)
			if (*(int64_t *)tags.data[j] == (int64_t)i) {
				arm = dd_arena_clone(p->pc, tagged.data[j]);
				break;
			}
		if (arm == NULL) {
			if (dflt == NULL) {
				dd_vec_free(&tagged);
				dd_vec_free(&tags);
				dd_parse_error(p->pc,
				    "case has no arm for tag %zu and no `_` "
				    "default", i);
			}
			arm = dd_arena_clone(p->pc, dflt);
		}
		dd_vec_push(&result->u.case_.arms, arm);
	}
	dd_vec_free(&tagged);
	dd_vec_free(&tags);
	return result;
}

static dd_term *
parse_fold(pparser *p)
{
	dd_term *t = dd_arena_term(p->pc, DD_T_FOLD);

	expect(p, T_FOLD);
	expect(p, T_LPAREN);
	t->u.fold.list = parse_term(p);
	expect(p, T_COMMA);
	t->u.fold.init = parse_term(p);
	expect(p, T_COMMA);
	p->depth += 2; /* acc, element */
	t->u.fold.step = parse_term(p);
	p->depth -= 2;
	expect(p, T_RPAREN);
	return t;
}

static dd_term *
parse_primary_term(pparser *p)
{
	dd_term *base;
	tok *t = pk(p);

	switch (t->kind) {
	case T_DOLLAR: {
		tok *n;

		nx(p);
		n = nx(p);
		if (n->kind != T_INT)
			dd_parse_error(p->pc, "Expected int after $");
		base = dd_arena_term(p->pc, DD_T_VAR);
		base->u.var = (size_t)n->int_v;
		break;
	}
	case T_CARET: {
		tok *k;

		nx(p);
		k = nx(p);
		if (k->kind != T_INT)
			dd_parse_error(p->pc, "Expected int after ^");
		base = dd_arena_term(p->pc, DD_T_BOUND);
		base->u.bound = (size_t)k->int_v;
		break;
	}
	case T_INT:
		nx(p);
		base = dd_arena_term(p->pc, DD_T_INT);
		base->u.int_lit = t->int_v;
		break;
	case T_MINUS:
		nx(p);
		base = dd_arena_term(p->pc, DD_T_UNARY);
		base->u.unary.op = DD_UN_NEG;
		base->u.unary.arg = parse_unary(p);
		break;
	case T_LPAREN:
		nx(p);
		base = parse_term(p);
		expect(p, T_RPAREN);
		break;
	case T_CASE:
		base = parse_pattern_case(p);
		break;
	case T_FOLD:
		base = parse_fold(p);
		break;
	case T_IDENT: {
		const char *name = t->str; /* lives in the token */

		nx(p);
		if (pk(p)->kind == T_LPAREN) {
			const cons_entry *c = find_cons(p, name);

			if (c != NULL) {
				dd_vec args = parse_args(p);
				dd_term *tuple;

				/* args.data is arena-tracked by parse_args. */
				if (args.len != c->arity)
					dd_parse_error(p->pc,
					    "constructor `%s` expects %zu args, "
					    "got %zu", name, c->arity, args.len);
				base = dd_arena_term(p->pc, DD_T_INJECT);
				base->u.inject.tag = dd_arena_term(p->pc, DD_T_INT);
				base->u.inject.tag->u.int_lit = c->tag;
				tuple = dd_arena_term(p->pc, DD_T_TUPLE);
				/* move the backing into the node (node owns it now) */
				dd_arena_untrack(p->pc, args.data);
				tuple->u.seq = args;
				base->u.inject.payload = tuple;
			} else {
				dd_vec args = parse_args(p);

				base = dd_build_builtin(p->pc, name, &args);
			}
		} else {
			base = resolve_binder(p, name);
			if (base == NULL)
				dd_parse_error(p->pc, "unknown name in term: "
				    "`%s` (not a binder or builtin)", name);
		}
		break;
	}
	default:
		dd_parse_error(p->pc, "Unexpected token in term: %d", t->kind);
	}
	/* Postfix field projection base[i], chaining base[i][j]. */
	while (pk(p)->kind == T_LBRACKET) {
		tok *i;
		dd_term *proj;

		nx(p);
		i = nx(p);
		if (i->kind != T_INT)
			dd_parse_error(p->pc, "Expected int index");
		expect(p, T_RBRACKET);
		proj = dd_arena_term(p->pc, DD_T_PROJ);
		proj->u.proj.base = base;
		proj->u.proj.index = (size_t)i->int_v;
		base = proj;
	}
	return base;
}

static dd_term *
parse_unary(pparser *p)
{
	if (pk(p)->kind == T_MINUS) {
		dd_term *t = dd_arena_term(p->pc, DD_T_UNARY);

		nx(p);
		t->u.unary.op = DD_UN_NEG;
		t->u.unary.arg = parse_unary(p);
		return t;
	}
	return parse_primary_term(p);
}

static dd_term *
parse_mul(pparser *p)
{
	dd_term *left = parse_unary(p);

	while (pk(p)->kind == T_STAR) {
		dd_term *bin = dd_arena_term(p->pc, DD_T_BINARY);

		nx(p);
		bin->u.binary.op = DD_BIN_MUL;
		bin->u.binary.l = left;
		bin->u.binary.r = parse_unary(p);
		left = bin;
	}
	return left;
}

static dd_term *
parse_add(pparser *p)
{
	dd_term *left = parse_mul(p);

	for (;;) {
		dd_binop op;
		dd_term *bin;

		if (pk(p)->kind == T_PLUS)
			op = DD_BIN_ADD;
		else if (pk(p)->kind == T_MINUS)
			op = DD_BIN_SUB;
		else
			break;
		nx(p);
		bin = dd_arena_term(p->pc, DD_T_BINARY);
		bin->u.binary.op = op;
		bin->u.binary.l = left;
		bin->u.binary.r = parse_mul(p);
		left = bin;
	}
	return left;
}

static dd_term *
parse_cmp(pparser *p)
{
	dd_term *left = parse_add(p);
	dd_binop op;
	dd_term *bin;

	switch (pk(p)->kind) {
	case T_EQEQ: op = DD_BIN_EQ; break;
	case T_NOTEQ: op = DD_BIN_NE; break;
	case T_LT: op = DD_BIN_LT; break;
	case T_LTEQ: op = DD_BIN_LE; break;
	case T_GT: op = DD_BIN_GT; break;
	case T_GTEQ: op = DD_BIN_GE; break;
	default: return left;
	}
	nx(p);
	bin = dd_arena_term(p->pc, DD_T_BINARY);
	bin->u.binary.op = op;
	bin->u.binary.l = left;
	bin->u.binary.r = parse_add(p);
	return bin;
}

static dd_term *
parse_logic(pparser *p)
{
	dd_term *left = parse_cmp(p);

	while (pk(p)->kind == T_ANDAND) {
		dd_term *bin = dd_arena_term(p->pc, DD_T_BINARY);

		nx(p);
		bin->u.binary.op = DD_BIN_AND;
		bin->u.binary.l = left;
		bin->u.binary.r = parse_cmp(p);
		left = bin;
	}
	return left;
}

static dd_term *
parse_term(pparser *p)
{
	return parse_logic(p);
}

static dd_vec
parse_args(pparser *p)
{
	dd_vec args = { 0 };
	void *prev = NULL;

	expect(p, T_LPAREN);
	if (pk(p)->kind != T_RPAREN) {
		dd_vec_push(&args, parse_term(p));
		dd_arena_retrack(p->pc, prev, args.data);
		prev = args.data;
		while (pk(p)->kind == T_COMMA) {
			nx(p);
			dd_vec_push(&args, parse_term(p));
			dd_arena_retrack(p->pc, prev, args.data);
			prev = args.data;
		}
	}
	expect(p, T_RPAREN);
	return args;
}

/* ---- projections ---- */

/* A single projection field; a bare $n (no index) splices (Spread). */
static dd_term *
parse_proj_field(pparser *p)
{
	if (pk(p)->kind == T_DOLLAR &&
	    p->toks[p->pos + 1]->kind == T_INT &&
	    p->toks[p->pos + 2]->kind != T_LBRACKET) {
		tok *n;
		dd_term *spread, *var;

		nx(p);
		n = nx(p);
		var = dd_arena_term(p->pc, DD_T_VAR);
		var->u.var = (size_t)n->int_v;
		spread = dd_arena_term(p->pc, DD_T_SPREAD);
		spread->u.spread = var;
		return spread;
	}
	return parse_term(p);
}

static dd_vec
parse_field_list_until(pparser *p, const tok_kind *terms, size_t nterms)
{
	dd_vec fields = { 0 };
	void *prev = NULL;
	size_t i;

	for (i = 0; i < nterms; i++)
		if (pk(p)->kind == terms[i])
			return fields;
	dd_vec_push(&fields, parse_proj_field(p));
	dd_arena_retrack(p->pc, prev, fields.data);
	prev = fields.data;
	while (pk(p)->kind == T_COMMA) {
		nx(p);
		dd_vec_push(&fields, parse_proj_field(p));
		dd_arena_retrack(p->pc, prev, fields.data);
		prev = fields.data;
	}
	return fields;
}

/* key = tuple(key fields), val = tuple(val fields). */
static dd_projection
parse_projection_inner(pparser *p)
{
	static const tok_kind key_terms[] = { T_SEMI, T_RPAREN };
	static const tok_kind val_terms[] = { T_RPAREN };
	dd_vec key = parse_field_list_until(p, key_terms, 2);
	dd_vec val = { 0 };
	dd_projection proj;
	dd_term *kt, *vt;

	if (pk(p)->kind == T_SEMI) {
		nx(p);
		val = parse_field_list_until(p, val_terms, 1);
	}
	expect(p, T_RPAREN);
	kt = dd_arena_term(p->pc, DD_T_TUPLE);
	dd_arena_untrack(p->pc, key.data);
	kt->u.seq = key;
	vt = dd_arena_term(p->pc, DD_T_TUPLE);
	dd_arena_untrack(p->pc, val.data);
	vt->u.seq = val;
	proj.key = kt;
	proj.val = vt;
	return proj;
}

static dd_projection
parse_projection(pparser *p)
{
	expect(p, T_LPAREN);
	return parse_projection_inner(p);
}

/* ---- collection language ---- */

static dd_expr *
parse_atom(pparser *p)
{
	tok *t = pk(p);
	dd_expr *e;

	switch (t->kind) {
	case T_INPUT: {
		tok *n;

		nx(p);
		n = nx(p);
		if (n->kind != T_INT)
			dd_parse_error(p->pc, "Expected int");
		e = dd_arena_expr(p->pc, DD_E_INPUT);
		e->u.input = (size_t)n->int_v;
		return e;
	}
	case T_IMPORT: {
		tok *s;

		nx(p);
		s = nx(p);
		if (s->kind != T_STR)
			dd_parse_error(p->pc,
			    "Expected string literal after `import`");
		e = dd_arena_expr(p->pc, DD_E_IMPORT);
		e->u.name = dd_arena_strdup(p->pc, s->str);
		return e;
	}
	case T_IDENT: {
		char *n = parse_ident(p);

		if (pk(p)->kind == T_COLONCOLON) {
			nx(p);
			e = dd_arena_expr(p->pc, DD_E_QUALIFIED);
			e->u.qualified.scope = n;
			e->u.qualified.field = parse_ident(p);
		} else {
			e = dd_arena_expr(p->pc, DD_E_NAME);
			e->u.name = n;
		}
		return e;
	}
	case T_LPAREN:
		nx(p);
		e = parse_pipe_expr(p);
		expect(p, T_RPAREN);
		return e;
	default:
		dd_parse_error(p->pc, "Unexpected token in atom: %d", t->kind);
	}
}

static dd_expr *parse_pipe_op(pparser *p, dd_expr *lhs);

/* A join argument may itself be a short pipe chain. */
static dd_expr *
parse_join_arg(pparser *p)
{
	dd_expr *e = parse_atom(p);

	while (pk(p)->kind == T_PIPE) {
		nx(p);
		e = parse_pipe_op(p, e);
	}
	return e;
}

static dd_expr *
parse_pipe_op(pparser *p, dd_expr *lhs)
{
	dd_expr *e;

	switch (pk(p)->kind) {
	case T_KEY:
	case T_MAP:
		nx(p);
		e = dd_arena_expr(p->pc, DD_E_MAP);
		e->u.map.e = lhs;
		e->u.map.proj = parse_projection(p);
		return e;
	case T_JOIN:
		nx(p);
		expect(p, T_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_JOIN);
		e->u.join.l = lhs;
		e->u.join.r = parse_join_arg(p);
		expect(p, T_COMMA);
		e->u.join.proj = parse_projection(p);
		expect(p, T_RPAREN);
		return e;
	case T_MIN:
		nx(p);
		e = dd_arena_expr(p->pc, DD_E_REDUCE);
		e->u.reduce.e = lhs;
		e->u.reduce.reducer = DD_RED_MIN;
		return e;
	case T_DISTINCT:
		nx(p);
		e = dd_arena_expr(p->pc, DD_E_REDUCE);
		e->u.reduce.e = lhs;
		e->u.reduce.reducer = DD_RED_DISTINCT;
		return e;
	case T_COUNT:
		nx(p);
		e = dd_arena_expr(p->pc, DD_E_REDUCE);
		e->u.reduce.e = lhs;
		e->u.reduce.reducer = DD_RED_COUNT;
		return e;
	case T_COLLECT:
		nx(p);
		e = dd_arena_expr(p->pc, DD_E_REDUCE);
		e->u.reduce.e = lhs;
		e->u.reduce.reducer = DD_RED_COLLECT;
		return e;
	case T_FLATMAP:
		nx(p);
		expect(p, T_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_FLATMAP);
		e->u.op_term.e = lhs;
		e->u.op_term.term = parse_term(p);
		expect(p, T_RPAREN);
		return e;
	case T_ARRANGE:
		nx(p);
		e = dd_arena_expr(p->pc, DD_E_ARRANGE);
		e->u.op.e = lhs;
		return e;
	case T_NEGATE:
		nx(p);
		e = dd_arena_expr(p->pc, DD_E_NEGATE);
		e->u.op.e = lhs;
		return e;
	case T_FILTER:
		nx(p);
		expect(p, T_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_FILTER);
		e->u.op_term.e = lhs;
		e->u.op_term.term = parse_term(p);
		expect(p, T_RPAREN);
		return e;
	case T_ENTER_AT:
		nx(p);
		expect(p, T_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_ENTER_AT);
		e->u.op_term.e = lhs;
		e->u.op_term.term = parse_term(p);
		expect(p, T_RPAREN);
		return e;
	case T_LIFT_ITER:
		nx(p);
		e = dd_arena_expr(p->pc, DD_E_LIFT_ITER);
		e->u.op.e = lhs;
		return e;
	case T_INSPECT:
		nx(p);
		expect(p, T_LPAREN);
		e = dd_arena_expr(p->pc, DD_E_INSPECT);
		e->u.inspect.e = lhs;
		e->u.inspect.label = parse_ident(p);
		expect(p, T_RPAREN);
		return e;
	default:
		dd_parse_error(p->pc, "Expected pipe operator, got %d",
		    pk(p)->kind);
	}
}

static dd_expr *
parse_concat_expr(pparser *p)
{
	dd_vec parts = { 0 };
	void *prev = NULL;
	dd_expr *result;

	dd_vec_push(&parts, parse_atom(p));
	dd_arena_retrack(p->pc, prev, parts.data);
	prev = parts.data;
	for (;;) {
		if (pk(p)->kind == T_PLUS) {
			nx(p);
			dd_vec_push(&parts, parse_atom(p));
		} else if (pk(p)->kind == T_MINUS) {
			dd_expr *neg = dd_arena_expr(p->pc, DD_E_NEGATE);

			nx(p);
			neg->u.op.e = parse_atom(p);
			dd_vec_push(&parts, neg);
		} else {
			break;
		}
		dd_arena_retrack(p->pc, prev, parts.data);
		prev = parts.data;
	}
	if (parts.len == 1) {
		result = parts.data[0];
		dd_arena_untrack(p->pc, parts.data);
		dd_vec_free(&parts);
		return result;
	}
	result = dd_arena_expr(p->pc, DD_E_CONCAT);
	dd_arena_untrack(p->pc, parts.data);
	result->u.concat = parts;
	return result;
}

static dd_expr *
parse_pipe_expr(pparser *p)
{
	dd_expr *e = parse_concat_expr(p);

	while (pk(p)->kind == T_PIPE) {
		nx(p);
		e = parse_pipe_op(p, e);
	}
	return e;
}

static void parse_program_into(pparser *p, dd_vec *out);

/* Fill the pre-allocated, already-registered statement `s`. */
static void
parse_stmt(pparser *p, dd_stmt *s)
{
	switch (pk(p)->kind) {
	case T_LET:
		nx(p);
		s->kind = DD_S_LET;
		s->name = parse_ident(p);
		expect(p, T_EQ);
		s->expr = parse_pipe_expr(p);
		expect(p, T_SEMI);
		return;
	case T_VAR:
		nx(p);
		s->kind = DD_S_VAR;
		s->name = parse_ident(p);
		expect(p, T_EQ);
		s->expr = parse_pipe_expr(p);
		expect(p, T_SEMI);
		return;
	case T_EXPORT: {
		tok *name;

		nx(p);
		s->kind = DD_S_EXPORT;
		name = nx(p);
		if (name->kind != T_STR)
			dd_parse_error(p->pc,
			    "Expected string literal after `export`");
		s->name = dd_arena_strdup(p->pc, name->str);
		expect(p, T_EQ);
		s->expr = parse_pipe_expr(p);
		expect(p, T_SEMI);
		return;
	}
	case T_IDENT:
		s->kind = DD_S_SCOPE;
		s->name = parse_ident(p);
		expect(p, T_COLON);
		expect(p, T_LBRACE);
		parse_program_into(p, &s->body);
		expect(p, T_RBRACE);
		return;
	default:
		dd_parse_error(p->pc, "Unexpected token: %d", pk(p)->kind);
	}
}

/*
 * Parse a program body into `out`, pushing each statement node before it
 * is filled so a mid-statement error unwind (which frees `out`) reclaims
 * the partial node too.
 */
static void
parse_program_into(pparser *p, dd_vec *out)
{
	while (pk(p)->kind != T_EOF && pk(p)->kind != T_RBRACE) {
		dd_stmt *s;

		/* con Name(arity) = tag;  is a parse-time declaration. */
		if (pk(p)->kind == T_CON) {
			cons_entry *c = __dtfl_alloc(sizeof *c);
			tok *arity, *tag;

			nx(p);
			c->name = NULL;
			dd_vec_push(&p->cons, c);
			c->name = parse_ident_local(p);
			expect(p, T_LPAREN);
			arity = nx(p);
			if (arity->kind != T_INT)
				dd_parse_error(p->pc, "con: expected arity");
			c->arity = (size_t)arity->int_v;
			expect(p, T_RPAREN);
			expect(p, T_EQ);
			tag = nx(p);
			if (tag->kind != T_INT)
				dd_parse_error(p->pc, "con: expected tag");
			c->tag = tag->int_v;
			expect(p, T_SEMI);
			continue;
		}
		s = dd_arena_stmt(p->pc, DD_S_LET);
		dd_vec_push(out, s);
		parse_stmt(p, s);
	}
}

static void
pparser_free(pparser *p)
{
	size_t i;

	for (i = 0; i < p->cons.len; i++) {
		cons_entry *c = p->cons.data[i];

		__dtfl_free(c->name);
		__dtfl_free(c);
	}
	dd_vec_free(&p->cons);
	for (i = 0; i < p->binders.len; i++) {
		binder *b = p->binders.data[i];

		__dtfl_free(b->name);
		__dtfl_free(b);
	}
	dd_vec_free(&p->binders);
}

bool
dd_parse_pipe(const char *input, dd_vec *out, dd_parse_ctx *pc)
{
	lexer lx = { pc, { 0 } };
	pparser p = { 0 };

	pc->error = false;
	pc->msg[0] = '\0';
	dd_arena_init(pc);
	*out = (dd_vec){ 0 };
	if (setjmp(pc->jb) != 0) {
		/* Error unwind: the arena owns every AST node; free it all,
		 * plus the root body backing and the parser scratch. */
		dd_arena_free_all(pc);
		__dtfl_free(out->data);
		*out = (dd_vec){ 0 };
		pparser_free(&p);
		toks_free(&lx.toks);
		return false;
	}
	tokenize(&lx, input);
	p.pc = pc;
	p.toks = (tok **)lx.toks.data;
	p.pos = 0;
	parse_program_into(&p, out);
	if (pk(&p)->kind != T_EOF)
		dd_parse_error(pc, "trailing tokens after program");
	pparser_free(&p);
	toks_free(&lx.toks);
	return true;
}

dd_term *
dd_parse_pipe_term(const char *input, dd_parse_ctx *pc)
{
	lexer lx = { pc, { 0 } };
	pparser p = { 0 };
	dd_term *t;

	pc->error = false;
	pc->msg[0] = '\0';
	dd_arena_init(pc);
	if (setjmp(pc->jb) != 0) {
		dd_arena_free_all(pc);
		pparser_free(&p);
		toks_free(&lx.toks);
		return NULL;
	}
	tokenize(&lx, input);
	p.pc = pc;
	p.toks = (tok **)lx.toks.data;
	p.pos = 0;
	t = parse_term(&p);
	if (pk(&p)->kind != T_EOF)
		dd_parse_error(pc, "trailing tokens after term");
	pparser_free(&p);
	toks_free(&lx.toks);
	return t;
}
