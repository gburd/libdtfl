/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/ddir/parse_int.h: the DDIR parser internals shared by the two
 * surface front-ends.
 *
 * Upstream the parsers panic on malformed input; the library may not
 * crash on bad user text, so a parse error is reported through
 * dd_parse_error, which records a message and longjmps back to the
 * dd_parse_program entry.  Both front-ends run inside that guard, so any
 * error during tokenizing or parsing unwinds cleanly to a NULL result.
 */
#ifndef DTFL_DDIR_PARSE_INT_H
#define DTFL_DDIR_PARSE_INT_H

#include "ddir_int.h"

#include <setjmp.h>

/* A node registered in the parse arena, for shallow cleanup on error. */
typedef enum dd_arena_kind {
	DD_A_RAW,  /* a plain heap block: __dtfl_free */
	DD_A_TERM, /* a dd_term box: free its own vec backing then the box */
	DD_A_EXPR, /* a dd_expr box: free its own concat backing then the box */
	DD_A_STMT  /* a dd_stmt box: free its own body backing then the box */
} dd_arena_kind;

typedef struct dd_arena_ent {
	dd_arena_kind kind;
	void *ptr;
} dd_arena_ent;

/* The parse error context.  One per parse; shared by lexer and parser. */
typedef struct dd_parse_ctx {
	jmp_buf jb;	   /* unwind target on error */
	bool error;	   /* set once an error is recorded */
	char msg[256];	   /* the error message */
	dd_arena_ent *arena; /* every AST allocation, for error cleanup */
	size_t arena_len;
	size_t arena_cap;
} dd_parse_ctx;

/* Arena-tracked allocators used by the parsers.  The whole AST lives in
 * the arena: every node, string and node-owned vec backing is tracked,
 * and dd_arena_free_all reclaims all of it in one shot, on both the
 * success and error paths.  The AST is never freed node by node, so there
 * is no ownership tension; the caller lowers it, then frees the arena. */
dd_term *dd_arena_term(dd_parse_ctx *pc, dd_term_kind kind);
dd_expr *dd_arena_expr(dd_parse_ctx *pc, dd_expr_kind kind);
dd_stmt *dd_arena_stmt(dd_parse_ctx *pc, dd_stmt_kind kind);
void *dd_arena_raw(dd_parse_ctx *pc, void *p); /* track an existing block */
/* Replace a tracked RAW pointer `old` with `new` (a realloc), or add `new`
 * if `old` is not tracked.  Used so a growable scratch vec's backing stays
 * reclaimable across the reallocs its growth triggers. */
void dd_arena_retrack(dd_parse_ctx *pc, void *old, void *new);
/* Stop tracking a RAW pointer (it has moved into a node that now owns it). */
void dd_arena_untrack(dd_parse_ctx *pc, void *p);
char *dd_arena_strdup(dd_parse_ctx *pc, const char *s);
/* Set up an empty arena before a parse. */
void dd_arena_init(dd_parse_ctx *pc);
/* Free every tracked allocation (success or error).  Idempotent-safe. */
void dd_arena_free_all(dd_parse_ctx *pc);
/* Deep-clone a term with every produced box/string arena-tracked. */
dd_term *dd_arena_clone(dd_parse_ctx *pc, const dd_term *t);

/* Record an error and longjmp back to the parse entry (never returns). */
DTFL_NORETURN void dd_parse_error(dd_parse_ctx *pc, const char *fmt, ...)
    DTFL_PRINTF(2, 3);

/* Build a scalar builtin from parsed args (parse/mod.rs build_builtin). */
dd_term *dd_build_builtin(dd_parse_ctx *pc, const char *name, dd_vec *args);

/*
 * The two front-ends: parse `input` into a vec of dd_stmt* (the program's
 * root body).  On a parse error they longjmp back to their own guard,
 * copy pc->msg out, and return false, leaving `out` empty.  dd_parse_term
 * parses a single scalar term (the closed-value entry point).  The caller
 * passes a dd_parse_ctx whose msg holds any error text.
 */
bool dd_parse_pipe(const char *input, dd_vec *out, dd_parse_ctx *pc);
bool dd_parse_applicative(const char *input, dd_vec *out, dd_parse_ctx *pc);
dd_term *dd_parse_pipe_term(const char *input, dd_parse_ctx *pc);

#endif /* DTFL_DDIR_PARSE_INT_H */
