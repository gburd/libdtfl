/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tools/dtfl-diag/diag_graph.h: the operator-graph model the graph and
 * profile subcommands build from Operates and Channels events.
 */
#ifndef DTFL_DIAG_GRAPH_H
#define DTFL_DIAG_GRAPH_H

#include <stddef.h>

#define DIAG_MAX_ADDR 32
#define DIAG_MAX_EDGE_IDS 4

struct diag_operator {
	size_t id;
	char *name;		/* owned */
	size_t addr[DIAG_MAX_ADDR];
	size_t addr_len;
	int is_scope;		/* set by diag_graph_finalize */
};

struct diag_channel {
	size_t id;
	size_t scope_addr[DIAG_MAX_ADDR];
	size_t scope_len;
	size_t src_node, src_port;
	size_t tgt_node, tgt_port;
};

struct diag_scope {
	size_t addr[DIAG_MAX_ADDR];
	size_t len;
};

struct diag_edge {
	size_t ids[DIAG_MAX_EDGE_IDS];
	size_t nids;
	int subgraph;
	size_t from_addr[DIAG_MAX_ADDR];
	size_t from_n;
	size_t to_addr[DIAG_MAX_ADDR];
	size_t to_n;
	size_t from_port, to_port;
};

struct diag_graph {
	struct diag_operator *operators;
	size_t noperators, cap_operators;
	struct diag_channel *channels;
	size_t nchannels, cap_channels;
	struct diag_scope *scopes;
	size_t nscopes, cap_scopes;
	struct diag_edge *edges;
	size_t nedges, cap_edges;
};

void diag_graph_init(struct diag_graph *g);
void diag_graph_free(struct diag_graph *g);
void diag_graph_add_operator(struct diag_graph *g, size_t id, const char *name,
    const size_t *addr, size_t addr_len);
void diag_graph_add_channel(struct diag_graph *g, size_t id,
    const size_t *scope, size_t scope_len, size_t sn, size_t sp, size_t tn,
    size_t tp);
void diag_graph_push_edge(struct diag_graph *g, const struct diag_edge *e);
/* Derives the scope set and marks each operator is_scope. */
void diag_graph_finalize(struct diag_graph *g);

/* The embedded dataflow-graph.html template, or NULL if unavailable. */
const char *diag_graph_template(void);
int diag_graph_render(struct diag_graph *g, const char *html_template,
    const char *output_path);

#endif /* DTFL_DIAG_GRAPH_H */
