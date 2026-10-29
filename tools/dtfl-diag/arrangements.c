/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tools/dtfl-diag/arrangements.c: the "arrangements" subcommand.
 *
 * Ported from dataflow-diag (HEAD 0fa322c9)
 *   tdiag/src/commands/arrangements.rs
 * Copyright (c) 2017 the timely-dataflow diagnostics contributors.
 *
 * arrangements.rs replays the timely and differential log streams,
 * tracks the logical size of each arrangement from Batch (+length),
 * Merge (complete - length1 - length2) and Drop (-length) events, and
 * reports the current count per operator joined with the operator name.
 * This C port keeps the same size accounting; it prints the final size
 * once per operator (the --output-interval windowing of the live tool
 * collapses to the end-of-stream snapshot when replaying a file).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dtfl/diag.h"
#include "diag_cmd.h"
#include "diag_graph.h"

struct arr_state {
	struct diag_graph graph;	/* operator names */
	struct { size_t id; int64_t count; } *sizes;
	size_t nsizes, cap_sizes;
};

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

static void
add_size(struct arr_state *s, size_t id, int64_t delta)
{
	size_t i;

	for (i = 0; i < s->nsizes; i++)
		if (s->sizes[i].id == id) {
			/* isize wraps in Rust release; C overflow is UB. */
			s->sizes[i].count = (int64_t)((uint64_t)s->sizes[i].count +
			    (uint64_t)delta);
			return;
		}
	if (s->nsizes == s->cap_sizes) {
		s->cap_sizes = s->cap_sizes ? s->cap_sizes * 2 : 64;
		s->sizes = xrealloc(s->sizes, s->cap_sizes * sizeof *s->sizes);
	}
	s->sizes[s->nsizes].id = id;
	s->sizes[s->nsizes].count = delta;
	s->nsizes++;
}

static void
on_timely(const dtfl_log_batch *batch, void *ctx)
{
	struct arr_state *s = ctx;
	size_t n = dtfl_log_batch_len(batch);
	size_t i;

	for (i = 0; i < n; i++)
		if (dtfl_log_event_kind_at(batch, i) == DTFL_LOG_OPERATES) {
			size_t alen;
			const size_t *addr =
			    dtfl_log_operates_addr(batch, i, &alen);

			diag_graph_add_operator(&s->graph,
			    dtfl_log_operates_id(batch, i),
			    dtfl_log_operates_name(batch, i), addr, alen);
		}
}

static void
on_differential(const dtfl_log_batch *batch, void *ctx)
{
	struct arr_state *s = ctx;
	size_t n = dtfl_log_batch_len(batch);
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_log_event_kind k = dtfl_log_event_kind_at(batch, i);
		size_t op = dtfl_log_dd_operator(batch, i);

		switch (k) {
		case DTFL_LOG_DD_BATCH:
			add_size(s, op,
			    (int64_t)dtfl_log_dd_length(batch, i));
			break;
		case DTFL_LOG_DD_MERGE: {
			size_t complete;

			if (dtfl_log_dd_merge_complete(batch, i, &complete)) {
				int64_t l1 = (int64_t)
				    dtfl_log_dd_merge_length1(batch, i);
				int64_t l2 = (int64_t)
				    dtfl_log_dd_merge_length2(batch, i);

				add_size(s, op, (int64_t)((uint64_t)complete -
				    ((uint64_t)l1 + (uint64_t)l2)));
			}
			break;
		}
		case DTFL_LOG_DD_DROP:
			add_size(s, op,
			    (int64_t)(0 - (uint64_t)dtfl_log_dd_length(batch, i)));
			break;
		default:
			break;
		}
	}
}

static const struct diag_operator *
find_op(const struct diag_graph *g, size_t id)
{
	size_t i;

	for (i = 0; i < g->noperators; i++)
		if (g->operators[i].id == id)
			return &g->operators[i];
	return NULL;
}

int
diag_cmd_arrangements(const struct diag_opts *opts)
{
	struct arr_state s;
	int tfds[DIAG_MAX_FILES], dfds[DIAG_MAX_FILES];
	int ntfds, ndfds, rc, i;
	size_t k;

	ntfds = diag_connect(opts, DIAG_STREAM_TIMELY, tfds, DIAG_MAX_FILES);
	if (ntfds < 0)
		return 1;
	ndfds = diag_connect(opts, DIAG_STREAM_DIFFERENTIAL, dfds,
	    DIAG_MAX_FILES);
	if (ndfds < 0) {
		for (i = 0; i < ntfds; i++)
			(void)close(tfds[i]);
		return 1;
	}

	memset(&s, 0, sizeof s);
	diag_graph_init(&s.graph);
	rc = dtfl_diag_replay_fds(tfds, (size_t)ntfds, "timely", on_timely, &s);
	if (rc == DTFL_OK)
		rc = dtfl_diag_replay_fds(dfds, (size_t)ndfds,
		    "differential/arrange", on_differential, &s);
	for (i = 0; i < ntfds; i++)
		(void)close(tfds[i]);
	for (i = 0; i < ndfds; i++)
		(void)close(dfds[i]);
	if (rc != DTFL_OK) {
		fprintf(stderr, "dtfl-diag arrangements: replay failed: %s\n",
		    dtfl_strerror(rc));
		diag_graph_free(&s.graph);
		free(s.sizes);
		return 1;
	}

	printf("Op. Id\tName\t# of tuples\n");
	for (k = 0; k < s.nsizes; k++) {
		const struct diag_operator *op = find_op(&s.graph,
		    s.sizes[k].id);

		if (s.sizes[k].count < 0)
			continue;	/* current count only, no retractions */
		printf("%zu\t%s\t%lld\n", s.sizes[k].id,
		    op != NULL ? op->name : "?",
		    (long long)s.sizes[k].count);
	}

	diag_graph_free(&s.graph);
	free(s.sizes);
	return 0;
}
