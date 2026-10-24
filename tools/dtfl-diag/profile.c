/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tools/dtfl-diag/profile.c: the "profile" subcommand.
 *
 * Ported from dataflow-diag (HEAD 0fa322c9)
 *   tdiag/src/commands/profile.rs
 * Copyright (c) 2017 the timely-dataflow diagnostics contributors.
 *
 * profile.rs replays the timely log stream, pairs Schedule Start and
 * Stop events per operator to accumulate elapsed nanoseconds, joins
 * with the operator names and scope flags, and prints the total time
 * per operator sorted descending.  This C port keeps the same Start/Stop
 * pairing (profile.rs's HashMap of (worker, id) -> start) and the same
 * output: "[scope]?  name  (id=.., addr=[..]):  <seconds>e0 s".
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dtfl/diag.h"
#include "diag_cmd.h"
#include "diag_graph.h"

struct prof_entry {
	size_t id;
	int64_t ns;
};

struct prof_state {
	struct diag_graph graph;	/* for operator names/scopes */
	/* Pending Schedule starts, keyed by operator id. */
	struct { size_t id; uint64_t start_ns; } *starts;
	size_t nstarts, cap_starts;
	/* Accumulated elapsed ns per operator id. */
	struct prof_entry *totals;
	size_t ntotals, cap_totals;
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
add_elapsed(struct prof_state *s, size_t id, int64_t ns)
{
	size_t i;

	for (i = 0; i < s->ntotals; i++)
		if (s->totals[i].id == id) {
			s->totals[i].ns += ns;
			return;
		}
	if (s->ntotals == s->cap_totals) {
		s->cap_totals = s->cap_totals ? s->cap_totals * 2 : 64;
		s->totals = xrealloc(s->totals,
		    s->cap_totals * sizeof *s->totals);
	}
	s->totals[s->ntotals].id = id;
	s->totals[s->ntotals].ns = ns;
	s->ntotals++;
}

static void
push_start(struct prof_state *s, size_t id, uint64_t t)
{
	if (s->nstarts == s->cap_starts) {
		s->cap_starts = s->cap_starts ? s->cap_starts * 2 : 64;
		s->starts = xrealloc(s->starts,
		    s->cap_starts * sizeof *s->starts);
	}
	s->starts[s->nstarts].id = id;
	s->starts[s->nstarts].start_ns = t;
	s->nstarts++;
}

/* Pops the most recent pending start for `id`; returns 1 and its time. */
static int
pop_start(struct prof_state *s, size_t id, uint64_t *t)
{
	size_t i;

	for (i = s->nstarts; i > 0; i--)
		if (s->starts[i - 1].id == id) {
			*t = s->starts[i - 1].start_ns;
			s->starts[i - 1] = s->starts[s->nstarts - 1];
			s->nstarts--;
			return 1;
		}
	return 0;
}

static void
on_batch(const dtfl_log_batch *batch, void *ctx)
{
	struct prof_state *s = ctx;
	size_t n = dtfl_log_batch_len(batch);
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_log_event_kind k = dtfl_log_event_kind_at(batch, i);

		if (k == DTFL_LOG_OPERATES) {
			size_t alen;
			const size_t *addr =
			    dtfl_log_operates_addr(batch, i, &alen);

			diag_graph_add_operator(&s->graph,
			    dtfl_log_operates_id(batch, i),
			    dtfl_log_operates_name(batch, i), addr, alen);
		} else if (k == DTFL_LOG_SCHEDULE) {
			size_t id = dtfl_log_schedule_id(batch, i);
			uint64_t t = dtfl_log_event_time(batch, i);

			if (dtfl_log_schedule_start(batch, i)) {
				push_start(s, id, t);
			} else {
				uint64_t start;

				if (pop_start(s, id, &start) && t > start)
					add_elapsed(s, id, (int64_t)(t -
					    start));
			}
		}
	}
}

static int
cmp_desc(const void *a, const void *b)
{
	const struct prof_entry *x = a, *y = b;

	if (x->ns < y->ns)
		return 1;
	if (x->ns > y->ns)
		return -1;
	return 0;
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
diag_cmd_profile(const struct diag_opts *opts)
{
	struct prof_state s;
	int fds[DIAG_MAX_FILES];
	int nfds, rc, i;
	size_t k;

	nfds = diag_connect(opts, DIAG_STREAM_TIMELY, fds, DIAG_MAX_FILES);
	if (nfds < 0)
		return 1;

	memset(&s, 0, sizeof s);
	diag_graph_init(&s.graph);
	rc = dtfl_diag_replay_fds(fds, (size_t)nfds, "timely", on_batch, &s);
	for (i = 0; i < nfds; i++)
		(void)close(fds[i]);
	if (rc != DTFL_OK) {
		fprintf(stderr, "dtfl-diag profile: replay failed: %s\n",
		    dtfl_strerror(rc));
		goto out;
	}
	diag_graph_finalize(&s.graph);

	if (s.ntotals > 1)
		qsort(s.totals, s.ntotals, sizeof *s.totals, cmp_desc);
	for (k = 0; k < s.ntotals; k++) {
		const struct diag_operator *op = find_op(&s.graph,
		    s.totals[k].id);
		const char *name = op != NULL ? op->name : "?";
		size_t j, alen = op != NULL ? op->addr_len : 0;

		printf("%s\t%s\t(id=%zu, addr=[", op != NULL && op->is_scope ?
		    "[scope]" : "", name, s.totals[k].id);
		for (j = 0; j < alen; j++)
			printf("%zu%s", op->addr[j], j + 1 < alen ? ", " : "");
		printf("]):\t%e s\n", (double)s.totals[k].ns / 1e9);
	}

out:
	free(s.starts);
	free(s.totals);
	diag_graph_free(&s.graph);
	return rc == DTFL_OK ? 0 : 1;
}
