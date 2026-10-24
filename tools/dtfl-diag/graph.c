/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tools/dtfl-diag/graph.c: the "graph" subcommand.
 *
 * Ported from dataflow-diag (HEAD 0fa322c9)
 *   tdiag/src/commands/graph.rs
 *   tdiag/src/commands/graph/dataflow-graph.html
 * Copyright (c) 2017 the timely-dataflow diagnostics contributors.
 *
 * graph.rs replays the timely log stream, keeps the Operates and
 * Channels events, and reconstructs the dataflow graph with a
 * differential computation that re-wires channels crossing a scope
 * boundary: a channel into a subscope is joined with the subscope's
 * internal ingress channel, a channel out of a subscope with its egress
 * channel, and every other channel is passed through.  It writes an
 * HTML file embedding the operator list and the re-wired channel list.
 *
 * This C port keeps the exact rewiring logic but drives it from the
 * decoded event batches the public replay delivers (dtfl/diag.h), with
 * plain arrays and linear scans instead of DD joins: the graphs are
 * small (a few hundred operators), so O(n*m) joins are not a concern,
 * and the result is byte-equivalent to the DD computation.  The
 * generated script escapes operator names so a name with a quote cannot
 * break the embedded JSON.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dtfl/diag.h"
#include "diag_cmd.h"
#include "diag_graph.h"

/* ------------------------------------------------------------------ */
/* Address helpers                                                     */
/* ------------------------------------------------------------------ */

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

/* True if `addr` (len na) is a known scope address (in g->scopes). */
static int
is_scope(const struct diag_graph *g, const size_t *addr, size_t na)
{
	size_t i;

	for (i = 0; i < g->nscopes; i++)
		if (addr_eq(g->scopes[i].addr, g->scopes[i].len, addr, na))
			return 1;
	return 0;
}

/* Emits a JSON array of the usize address into the file. */
static void
write_addr(FILE *f, const size_t *addr, size_t n)
{
	size_t i;

	fputc('[', f);
	for (i = 0; i < n; i++)
		fprintf(f, "%zu, ", addr[i]);
	fputc(']', f);
}

/* Appends a channel edge to the graph's channel output list. */
static void
emit_channel(struct diag_graph *g, const size_t *ids, size_t nids, int subgraph,
    const size_t *from_addr, size_t from_n, const size_t *to_addr, size_t to_n,
    size_t from_port, size_t to_port)
{
	struct diag_edge e;
	size_t i;

	memset(&e, 0, sizeof e);
	for (i = 0; i < nids && i < DIAG_MAX_EDGE_IDS; i++)
		e.ids[e.nids++] = ids[i];
	e.subgraph = subgraph;
	e.from_n = from_n < DIAG_MAX_ADDR ? from_n : DIAG_MAX_ADDR;
	memcpy(e.from_addr, from_addr, e.from_n * sizeof(size_t));
	e.to_n = to_n < DIAG_MAX_ADDR ? to_n : DIAG_MAX_ADDR;
	memcpy(e.to_addr, to_addr, e.to_n * sizeof(size_t));
	e.from_port = from_port;
	e.to_port = to_port;
	diag_graph_push_edge(g, &e);
}

/* ------------------------------------------------------------------ */
/* Channel rewiring (graph.rs)                                         */
/* ------------------------------------------------------------------ */

/*
 * Finds the channel in g whose scope_addr equals `scope` and whose
 * source == (sn, sp) (ingress match) or target == (tn, tp) (egress
 * match).  Returns the channel index or SIZE_MAX.
 */
static size_t
find_channel_by_source(const struct diag_graph *g, const size_t *scope,
    size_t sn, size_t sp_node, size_t sp_port)
{
	size_t i;

	for (i = 0; i < g->nchannels; i++) {
		const struct diag_channel *c = &g->channels[i];

		if (addr_eq(c->scope_addr, c->scope_len, scope, sn) &&
		    c->src_node == sp_node && c->src_port == sp_port)
			return i;
	}
	return SIZE_MAX;
}

static size_t
find_channel_by_target(const struct diag_graph *g, const size_t *scope,
    size_t sn, size_t tn_node, size_t tn_port)
{
	size_t i;

	for (i = 0; i < g->nchannels; i++) {
		const struct diag_channel *c = &g->channels[i];

		if (addr_eq(c->scope_addr, c->scope_len, scope, sn) &&
		    c->tgt_node == tn_node && c->tgt_port == tn_port)
			return i;
	}
	return SIZE_MAX;
}

/*
 * Re-wires one channel, emitting zero or one edge.  Mirrors graph.rs:
 *   - a channel entering a subscope joins with the subscope's internal
 *     ingress channel (source port 0);
 *   - a channel leaving a subscope joins with its internal egress
 *     channel (target port 0);
 *   - any other channel passes straight through.
 */
static void
rewire(struct diag_graph *g, const struct diag_channel *c)
{
	size_t subscope_in[DIAG_MAX_ADDR];
	size_t subscope_out[DIAG_MAX_ADDR];
	size_t in_n, out_n;

	/* subscope entered: scope_addr + target node. */
	in_n = c->scope_len;
	if (in_n < DIAG_MAX_ADDR) {
		memcpy(subscope_in, c->scope_addr, in_n * sizeof(size_t));
		subscope_in[in_n] = c->tgt_node;
		in_n++;
	}
	/* subscope left: scope_addr + source node. */
	out_n = c->scope_len;
	if (out_n < DIAG_MAX_ADDR) {
		memcpy(subscope_out, c->scope_addr, out_n * sizeof(size_t));
		subscope_out[out_n] = c->src_node;
		out_n++;
	}

	if (is_scope(g, subscope_in, in_n)) {
		/*
		 * Ingress: the external channel enters subscope_in.  Join
		 * with the internal channel at (subscope_in, source 0,
		 * target subscope_port).  graph.rs keys the external by
		 * (subscope_addr, (0, to.1)) and joins with the internal
		 * channel's (scope_addr, from) == (subscope_in, (0, to.1)).
		 */
		size_t internal = find_channel_by_source(g, subscope_in, in_n,
		    0, c->tgt_port);

		if (internal != SIZE_MAX) {
			const struct diag_channel *ic = &g->channels[internal];
			size_t orig_addr[DIAG_MAX_ADDR];
			size_t to_addr[DIAG_MAX_ADDR];
			size_t oa_n = c->scope_len, ta_n = c->scope_len;
			size_t ids[2] = { c->id, ic->id };

			/* orig_addr = scope_addr + source node. */
			if (oa_n < DIAG_MAX_ADDR) {
				memcpy(orig_addr, c->scope_addr,
				    oa_n * sizeof(size_t));
				orig_addr[oa_n] = c->src_node;
				oa_n++;
			}
			/* to_addr = ic->scope_addr + ic->target node. */
			ta_n = ic->scope_len;
			if (ta_n < DIAG_MAX_ADDR) {
				memcpy(to_addr, ic->scope_addr,
				    ta_n * sizeof(size_t));
				to_addr[ta_n] = ic->tgt_node;
				ta_n++;
			}
			emit_channel(g, ids, 2, 1, orig_addr, oa_n, to_addr,
			    ta_n, c->src_port, ic->tgt_port);
		}
		return;
	}

	if (is_scope(g, subscope_out, out_n)) {
		/*
		 * Egress: the external channel leaves subscope_out.  Join
		 * with the internal channel at (subscope_out, target 0,
		 * source subscope_port).
		 */
		size_t internal = find_channel_by_target(g, subscope_out, out_n,
		    0, c->src_port);

		if (internal != SIZE_MAX) {
			const struct diag_channel *ec = &g->channels[internal];
			size_t from_addr[DIAG_MAX_ADDR];
			size_t dest_addr[DIAG_MAX_ADDR];
			size_t fa_n = ec->scope_len, da_n = c->scope_len;
			size_t ids[2] = { ec->id, c->id };

			/* from_addr = ec->scope_addr + ec->source node. */
			if (fa_n < DIAG_MAX_ADDR) {
				memcpy(from_addr, ec->scope_addr,
				    fa_n * sizeof(size_t));
				from_addr[fa_n] = ec->src_node;
				fa_n++;
			}
			/* dest_addr = scope_addr + target node. */
			if (da_n < DIAG_MAX_ADDR) {
				memcpy(dest_addr, c->scope_addr,
				    da_n * sizeof(size_t));
				dest_addr[da_n] = c->tgt_node;
				da_n++;
			}
			emit_channel(g, ids, 2, 1, from_addr, fa_n, dest_addr,
			    da_n, ec->src_port, c->tgt_port);
		}
		return;
	}

	/* Non-subgraph channel: pass through. */
	{
		size_t from_addr[DIAG_MAX_ADDR];
		size_t to_addr[DIAG_MAX_ADDR];
		size_t fa_n = c->scope_len, ta_n = c->scope_len;
		size_t ids[1] = { c->id };

		if (fa_n < DIAG_MAX_ADDR) {
			memcpy(from_addr, c->scope_addr, fa_n * sizeof(size_t));
			from_addr[fa_n] = c->src_node;
			fa_n++;
		}
		if (ta_n < DIAG_MAX_ADDR) {
			memcpy(to_addr, c->scope_addr, ta_n * sizeof(size_t));
			to_addr[ta_n] = c->tgt_node;
			ta_n++;
		}
		emit_channel(g, ids, 1, 0, from_addr, fa_n, to_addr, ta_n,
		    c->src_port, c->tgt_port);
	}
}

/* ------------------------------------------------------------------ */
/* HTML output                                                         */
/* ------------------------------------------------------------------ */

static void
write_json_escaped(FILE *f, const char *s)
{
	for (; *s != '\0'; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		fputc(*s, f);
	}
}

int
diag_graph_render(struct diag_graph *g, const char *html_template,
    const char *output_path)
{
	FILE *f;
	size_t i;

	/* Derive scopes and leaf operators, then re-wire channels. */
	diag_graph_finalize(g);
	for (i = 0; i < g->nchannels; i++)
		rewire(g, &g->channels[i]);

	f = fopen(output_path, "w");
	if (f == NULL) {
		fprintf(stderr, "dtfl-diag: cannot write %s\n", output_path);
		return 1;
	}
	fputs("<body>\n", f);
	fputs(html_template, f);
	fputs("\n<script type=\"text/javascript\">\n", f);

	fputs("let operate = [\n", f);
	for (i = 0; i < g->noperators; i++) {
		const struct diag_operator *op = &g->operators[i];

		if (op->is_scope)
			continue;	/* leaf operators only */
		fputs("{ \"name\": \"", f);
		write_json_escaped(f, op->name);
		fputs("\", \"addr\": ", f);
		write_addr(f, op->addr, op->addr_len);
		fputs(" },\n", f);
	}
	fputs("];\n", f);

	fputs("let channel = [\n", f);
	for (i = 0; i < g->nedges; i++) {
		const struct diag_edge *e = &g->edges[i];
		size_t j;

		fputs("{ \"id\": [", f);
		for (j = 0; j < e->nids; j++)
			fprintf(f, "%zu, ", e->ids[j]);
		fprintf(f, "], \"subgraph\": %s, \"from_addr\": ",
		    e->subgraph ? "true" : "false");
		write_addr(f, e->from_addr, e->from_n);
		fputs(", \"to_addr\": ", f);
		write_addr(f, e->to_addr, e->to_n);
		fprintf(f, ", \"from_port\": %zu, \"to_port\": %zu },\n",
		    e->from_port, e->to_port);
	}
	fputs("];\n", f);

	fputs("run(operate, channel);\n", f);
	fputs("</script>\n", f);
	fclose(f);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Subcommand entry                                                    */
/* ------------------------------------------------------------------ */

static void
on_batch(const dtfl_log_batch *batch, void *ctx)
{
	struct diag_graph *g = ctx;
	size_t n = dtfl_log_batch_len(batch);
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_log_event_kind k = dtfl_log_event_kind_at(batch, i);

		if (k == DTFL_LOG_OPERATES) {
			size_t alen;
			const size_t *addr =
			    dtfl_log_operates_addr(batch, i, &alen);

			diag_graph_add_operator(g,
			    dtfl_log_operates_id(batch, i),
			    dtfl_log_operates_name(batch, i), addr, alen);
		} else if (k == DTFL_LOG_CHANNELS) {
			size_t slen, sn, sp, tn, tp;
			const size_t *scope =
			    dtfl_log_channels_scope(batch, i, &slen);

			dtfl_log_channels_source(batch, i, &sn, &sp);
			dtfl_log_channels_target(batch, i, &tn, &tp);
			diag_graph_add_channel(g,
			    dtfl_log_channels_id(batch, i), scope, slen, sn, sp,
			    tn, tp);
		}
	}
}

int
diag_cmd_graph(const struct diag_opts *opts)
{
	struct diag_graph g;
	int fds[DIAG_MAX_FILES];
	int nfds, rc, i;
	const char *tmpl;

	nfds = diag_connect(opts, DIAG_STREAM_TIMELY, fds, DIAG_MAX_FILES);
	if (nfds < 0)
		return 1;

	diag_graph_init(&g);
	rc = dtfl_diag_replay_fds(fds, (size_t)nfds, "timely", on_batch, &g);
	for (i = 0; i < nfds; i++)
		(void)close(fds[i]);
	if (rc != DTFL_OK) {
		fprintf(stderr, "dtfl-diag graph: replay failed: %s\n",
		    dtfl_strerror(rc));
		diag_graph_free(&g);
		return 1;
	}

	tmpl = diag_graph_template();
	rc = diag_graph_render(&g, tmpl, opts->output_path);
	diag_graph_free(&g);
	if (rc == 0)
		fprintf(stderr, "Graph generated in %s\n", opts->output_path);
	return rc;
}
