/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tools/dtfl-diag/diag_graph.c: the operator-graph model.
 *
 * Backs graph.c and profile.c (tdiag/src/commands/{graph,profile}.rs).
 * Collects Operates and Channels, derives the scope set (the parent
 * address of every operator) and the leaf-operator / subgraph split
 * that graph.rs computes with distinct/antijoin/semijoin, and loads the
 * dataflow-graph.html template from the data directory.
 *
 * A tool may read the environment (the api_discipline gate scopes that
 * restriction to src/).  The template is found at DTFL_DATA_DIR if set,
 * else the compiled-in DTFL_PKGDATADIR (the installed location).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag_graph.h"

#ifndef DTFL_PKGDATADIR
#define DTFL_PKGDATADIR "/usr/local/share/dtfl"
#endif

static void *
xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n);

	if (q == NULL && n != 0) {
		fprintf(stderr, "dtfl-diag: out of memory\n");
		exit(1);
	}
	return q;
}

void
diag_graph_init(struct diag_graph *g)
{
	memset(g, 0, sizeof *g);
}

void
diag_graph_free(struct diag_graph *g)
{
	size_t i;

	for (i = 0; i < g->noperators; i++)
		free(g->operators[i].name);
	free(g->operators);
	free(g->channels);
	free(g->scopes);
	free(g->edges);
	memset(g, 0, sizeof *g);
}

static void
copy_addr(size_t *dst, size_t *dst_len, const size_t *src, size_t src_len)
{
	size_t n = src_len < DIAG_MAX_ADDR ? src_len : DIAG_MAX_ADDR;

	memcpy(dst, src, n * sizeof(size_t));
	*dst_len = n;
}

void
diag_graph_add_operator(struct diag_graph *g, size_t id, const char *name,
    const size_t *addr, size_t addr_len)
{
	struct diag_operator *op;

	if (g->noperators == g->cap_operators) {
		g->cap_operators = g->cap_operators ? g->cap_operators * 2 : 16;
		g->operators = xrealloc(g->operators,
		    g->cap_operators * sizeof *g->operators);
	}
	op = &g->operators[g->noperators++];
	memset(op, 0, sizeof *op);
	op->id = id;
	op->name = strdup(name != NULL ? name : "");
	if (op->name == NULL) {
		fprintf(stderr, "dtfl-diag: out of memory\n");
		exit(1);
	}
	copy_addr(op->addr, &op->addr_len, addr, addr_len);
}

void
diag_graph_add_channel(struct diag_graph *g, size_t id, const size_t *scope,
    size_t scope_len, size_t sn, size_t sp, size_t tn, size_t tp)
{
	struct diag_channel *c;

	if (g->nchannels == g->cap_channels) {
		g->cap_channels = g->cap_channels ? g->cap_channels * 2 : 16;
		g->channels = xrealloc(g->channels,
		    g->cap_channels * sizeof *g->channels);
	}
	c = &g->channels[g->nchannels++];
	memset(c, 0, sizeof *c);
	c->id = id;
	copy_addr(c->scope_addr, &c->scope_len, scope, scope_len);
	c->src_node = sn;
	c->src_port = sp;
	c->tgt_node = tn;
	c->tgt_port = tp;
}

void
diag_graph_push_edge(struct diag_graph *g, const struct diag_edge *e)
{
	if (g->nedges == g->cap_edges) {
		g->cap_edges = g->cap_edges ? g->cap_edges * 2 : 16;
		g->edges = xrealloc(g->edges, g->cap_edges * sizeof *g->edges);
	}
	g->edges[g->nedges++] = *e;
}

static int
addr_eq(const size_t *a, size_t na, const size_t *b, size_t nb)
{
	size_t i;

	if (na != nb)
		return 0;
	for (i = 0; i < na; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static void
add_scope(struct diag_graph *g, const size_t *addr, size_t len)
{
	size_t i;

	for (i = 0; i < g->nscopes; i++)
		if (addr_eq(g->scopes[i].addr, g->scopes[i].len, addr, len))
			return;	/* distinct */
	if (g->nscopes == g->cap_scopes) {
		g->cap_scopes = g->cap_scopes ? g->cap_scopes * 2 : 16;
		g->scopes = xrealloc(g->scopes,
		    g->cap_scopes * sizeof *g->scopes);
	}
	copy_addr(g->scopes[g->nscopes].addr, &g->scopes[g->nscopes].len, addr,
	    len);
	g->nscopes++;
}

void
diag_graph_finalize(struct diag_graph *g)
{
	size_t i;

	/*
	 * scopes = { addr[:-1] : operator addr }.  graph.rs pops the last
	 * element of each operator address; the empty [] address is a
	 * valid scope (the root), so it is kept.
	 */
	g->nscopes = 0;
	for (i = 0; i < g->noperators; i++) {
		const struct diag_operator *op = &g->operators[i];
		size_t parent_len = op->addr_len > 0 ? op->addr_len - 1 : 0;

		add_scope(g, op->addr, parent_len);
	}

	/* An operator is a subgraph if its own address is a scope. */
	for (i = 0; i < g->noperators; i++) {
		struct diag_operator *op = &g->operators[i];
		size_t j;

		op->is_scope = 0;
		for (j = 0; j < g->nscopes; j++)
			if (addr_eq(g->scopes[j].addr, g->scopes[j].len,
			    op->addr, op->addr_len)) {
				op->is_scope = 1;
				break;
			}
	}
}

/* ------------------------------------------------------------------ */
/* HTML template loading                                               */
/* ------------------------------------------------------------------ */

static char *g_template_buf;

const char *
diag_graph_template(void)
{
	const char *dir = getenv("DTFL_DATA_DIR");
	char path[4096];
	FILE *f;
	long sz;

	if (g_template_buf != NULL)
		return g_template_buf;
	if (dir == NULL)
		dir = DTFL_PKGDATADIR;
	snprintf(path, sizeof path, "%s/dataflow-graph.html", dir);
	f = fopen(path, "r");
	if (f == NULL) {
		fprintf(stderr, "dtfl-diag: cannot read graph template %s\n",
		    path);
		return "";
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) {
		fclose(f);
		return "";
	}
	g_template_buf = xrealloc(NULL, (size_t)sz + 1);
	if (fread(g_template_buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(g_template_buf);
		g_template_buf = NULL;
		fclose(f);
		return "";
	}
	g_template_buf[sz] = '\0';
	fclose(f);
	return g_template_buf;
}
