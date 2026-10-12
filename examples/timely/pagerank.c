/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/pagerank.c: mirrors timely/examples/pagerank.rs.
 *
 * Arguments: [nodes edges] [timely flags].  A delta-based PageRank: edge
 * changes enter on one input and rank changes circulate on a feedback
 * loop; a binary operator brings them together, keeping per-source edge
 * lists and ranks and redistributing 5/6 of each node's rank to its
 * out-neighbours whenever an input time completes.  Rank deltas feed back
 * until the computation settles.  Each rank iteration prints
 * "time\tcnt\tsum\tmax".
 *
 * Upstream uses binary_frontier and checks the input frontiers directly;
 * this uses binary_notify and a notificator, which fires a time exactly
 * when both input frontiers have passed it, giving the same gating.  With
 * no positional arguments a small fixed edge set is used so the printed
 * iteration statistics are exact and checkable.  The port assumes a
 * single worker.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A fixed graph on 4 nodes: a small cycle with a spur. */
static const uint64_t fixed_edges[][2] = {
	{ 0, 1 }, { 1, 2 }, { 2, 0 }, { 2, 3 }, { 0, 2 },
};
#define FIXED_E (sizeof fixed_edges / sizeof fixed_edges[0])
#define FIXED_N 4

/* A (key, i64 diff) change. */
typedef struct change { uint64_t key; int64_t diff; } change;

/* A dynamic vector of changes. */
typedef struct cvec {
	change *v;
	size_t n, cap;
} cvec;

static void
cvec_push(cvec *c, uint64_t key, int64_t diff)
{
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 8;
		c->v = realloc(c->v, c->cap * sizeof *c->v);
	}
	c->v[c->n].key = key;
	c->v[c->n].diff = diff;
	c->n++;
}

static int
cmp_change(const void *va, const void *vb)
{
	const change *a = va, *b = vb;

	return a->key < b->key ? -1 : a->key > b->key;
}

/* Sort by key and sum equal keys, dropping zero diffs (compact). */
static void
compact(cvec *c)
{
	size_t i, w = 0;

	if (c->n == 0)
		return;
	if (c->n > 1)
		qsort(c->v, c->n, sizeof *c->v, cmp_change);
	for (i = 0; i < c->n;) {
		uint64_t key = c->v[i].key;
		int64_t sum = 0;

		while (i < c->n && c->v[i].key == key)
			sum += c->v[i++].diff;
		if (sum != 0) {
			c->v[w].key = key;
			c->v[w].diff = sum;
			w++;
		}
	}
	c->n = w;
}

/* Distribute 5/6 of `rank` across `edges` (allocate). */
static void
allocate(int64_t rank, const cvec *edges, cvec *send)
{
	int64_t distribute, degree, share;
	size_t i;

	if (edges->n == 0)
		return;
	distribute = (rank * 5) / 6;
	degree = (int64_t)edges->n;
	share = distribute / degree;
	for (i = 0; i < edges->n; i++) {
		if ((int64_t)i < (distribute % degree))
			cvec_push(send, edges->v[i].key, edges->v[i].diff * (share + 1));
		else
			cvec_push(send, edges->v[i].key, edges->v[i].diff * share);
	}
}

/* Per-time stash of pending changes. */
typedef struct stash_slot {
	uint64_t time;
	cvec changes;
} stash_slot;

typedef struct stash {
	stash_slot *s;
	size_t n, cap;
} stash;

static stash_slot *
stash_for(stash *st, uint64_t time)
{
	size_t i;

	for (i = 0; i < st->n; i++)
		if (st->s[i].time == time)
			return &st->s[i];
	if (st->n == st->cap) {
		st->cap = st->cap ? st->cap * 2 : 4;
		st->s = realloc(st->s, st->cap * sizeof *st->s);
	}
	memset(&st->s[st->n], 0, sizeof st->s[st->n]);
	st->s[st->n].time = time;
	return &st->s[st->n++];
}

static stash_slot *
stash_find(stash *st, uint64_t time)
{
	size_t i;

	for (i = 0; i < st->n; i++)
		if (st->s[i].time == time)
			return &st->s[i];
	return NULL;
}

typedef struct pr_ctx {
	cvec *edges;	/* per source: out-edges (key = dst, diff) */
	int64_t *ranks;	/* per source */
	int64_t *diffs;	/* per source: accumulated un-acted deltas */
	size_t len, cap;
	stash edge_stash;
	stash rank_stash;
} pr_ctx;

static void
pr_grow(pr_ctx *c, size_t src)
{
	size_t i;

	if (src < c->len)
		return;
	if (src + 1 > c->cap) {
		c->cap = src + 1 > c->cap * 2 ? src + 1 : c->cap * 2;
		c->edges = realloc(c->edges, c->cap * sizeof *c->edges);
		c->ranks = realloc(c->ranks, c->cap * sizeof *c->ranks);
		c->diffs = realloc(c->diffs, c->cap * sizeof *c->diffs);
	}
	for (i = c->len; i <= src; i++) {
		memset(&c->edges[i], 0, sizeof c->edges[i]);
		c->ranks[i] = 1000;
		c->diffs[i] = 0;
	}
	c->len = src + 1;
}

/* ------------------------------------------------------------------ */
/* The PageRank operator (binary_notify, frontier via notificator)     */
/* ------------------------------------------------------------------ */

static void
pr_logic(void *vctx, dtfl_op *op, dtfl_notificator *note)
{
	pr_ctx *c = vctx;
	dtfl_message *m;
	dtfl_cap *cap;
	uint64_t count;

	/* input 0: edge changes ((src, dst), diff).  Stash by time. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		stash_slot *slot = stash_for(&c->edge_stash, t[0]);
		size_t i, k = dtfl_message_len(m);

		dtfl_notificator_notify_at(note, dtfl_op_input_cap_delayed(op, 0, 0, t));
		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			uint64_t src = dtfl_get_u64(rec);
			uint64_t dst = dtfl_get_u64((const unsigned char *)rec + 8);
			int64_t diff = dtfl_get_i64((const unsigned char *)rec + 16);

			/* key folds (src, dst) for small node ids (fixed graph). */
			cvec_push(&slot->changes, (src << 20) | dst, diff);
		}
	}

	/* input 1: rank changes (src, diff).  Stash by time. */
	while ((m = dtfl_op_input_next(op, 1)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		stash_slot *slot = stash_for(&c->rank_stash, t[0]);
		size_t i, k = dtfl_message_len(m);

		dtfl_notificator_notify_at(note, dtfl_op_input_cap_delayed(op, 1, 0, t));
		for (i = 0; i < k; i++) {
			size_t len;
			const void *rec = dtfl_message_record(m, i, &len);
			uint64_t src = dtfl_get_u64(rec);
			int64_t diff = dtfl_get_i64((const unsigned char *)rec + 8);

			cvec_push(&slot->changes, src, diff);
		}
	}

	while (dtfl_notificator_next(note, &cap, &count)) {
		const uint64_t *t = dtfl_cap_time(cap);
		stash_slot *es = stash_find(&c->edge_stash, t[0]);
		stash_slot *rs = stash_find(&c->rank_stash, t[0]);
		dtfl_session *sess = dtfl_op_session(op, 0, cap);
		cvec delta;

		memset(&delta, 0, sizeof delta);

		/* Process edge changes at this time. */
		if (es != NULL && es->changes.n > 0) {
			size_t i;

			compact(&es->changes);
			for (i = 0; i < es->changes.n; i++) {
				uint64_t src = es->changes.v[i].key >> 20;
				uint64_t dst = es->changes.v[i].key & 0xfffff;
				int64_t diff = es->changes.v[i].diff;
				size_t j;

				pr_grow(c, (size_t)src);
				/* subtract previous distribution. */
				delta.n = 0;
				allocate(c->ranks[src], &c->edges[src], &delta);
				for (j = 0; j < delta.n; j++)
					delta.v[j].diff *= -1;
				/* update edges. */
				cvec_push(&c->edges[src], dst, diff);
				compact(&c->edges[src]);
				/* re-distribute. */
				allocate(c->ranks[src], &c->edges[src], &delta);
				compact(&delta);
				for (j = 0; j < delta.n; j++) {
					unsigned char buf[16];

					dtfl_put_u64(buf, delta.v[j].key);
					dtfl_put_i64(buf + 8, delta.v[j].diff);
					dtfl_session_give(sess, buf, sizeof buf);
				}
			}
			es->changes.n = 0;
		}

		/* Process rank changes at this time. */
		if (rs != NULL && rs->changes.n > 0) {
			size_t i;
			int64_t cnt = 0, sum = 0, max = 0;

			compact(&rs->changes);
			for (i = 0; i < rs->changes.n; i++) {
				uint64_t src = rs->changes.v[i].key;
				int64_t diff = rs->changes.v[i].diff;
				int64_t ad = diff < 0 ? -diff : diff;
				size_t j;

				cnt++;
				sum += ad;
				if (max < ad)
					max = ad;
				pr_grow(c, (size_t)src);
				delta.n = 0;
				allocate(c->ranks[src], &c->edges[src], &delta);
				for (j = 0; j < delta.n; j++)
					delta.v[j].diff *= -1;
				c->diffs[src] += diff;
				if ((c->diffs[src] < 0 ? -c->diffs[src] : c->diffs[src]) >= 6) {
					c->ranks[src] += c->diffs[src];
					c->diffs[src] = 0;
				}
				allocate(c->ranks[src], &c->edges[src], &delta);
				compact(&delta);
				for (j = 0; j < delta.n; j++) {
					unsigned char buf[16];

					dtfl_put_u64(buf, delta.v[j].key);
					dtfl_put_i64(buf + 8, delta.v[j].diff);
					dtfl_session_give(sess, buf, sizeof buf);
				}
			}
			printf("%llu\t%lld\t%lld\t%lld\n", (unsigned long long)t[0],
			    (long long)cnt, (long long)sum, (long long)max);
			rs->changes.n = 0;
		}

		dtfl_session_close(sess);
		free(delta.v);
		dtfl_cap_drop(cap);
	}
}

static void
stash_free(stash *st)
{
	size_t i;

	for (i = 0; i < st->n; i++)
		free(st->s[i].changes.v);
	free(st->s);
}

static void
pr_free(void *vctx)
{
	pr_ctx *c = vctx;
	size_t i;

	for (i = 0; i < c->len; i++)
		free(c->edges[i].v);
	free(c->edges);
	free(c->ranks);
	free(c->diffs);
	stash_free(&c->edge_stash);
	stash_free(&c->rank_stash);
	free(c);
}

static uint64_t
route_edge_src(const void *rec, size_t len, void *ctx)
{
	(void)len; (void)ctx;
	return dtfl_get_u64(rec);	/* src is first field */
}

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *inner = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	const dtfl_type *edge = dtfl_type_pair(inner, dtfl_type_i64());
	const dtfl_type *rank = dtfl_type_pair(dtfl_type_u64(), dtfl_type_i64());
	uint64_t summary[DTFL_TIME_MAX];
	dtfl_feedback *handle;
	dtfl_stream *edges, *cycle, *changes;
	pr_ctx *pc = calloc(1, sizeof *pc);

	edges = dtfl_input_to_stream(c->input, scope);
	dtfl_summary_default(dtfl_scope_time_class(scope), summary);
	summary[0] = 1;
	cycle = dtfl_feedback_new(scope, rank, summary, &handle);
	changes = dtfl_stream_binary_notify(edges, cycle,
	    dtfl_pact_exchange(route_edge_src, NULL),
	    dtfl_pact_exchange(route_edge_src, NULL), "PageRank", NULL, 0, rank,
	    pr_logic, DTFL_CTX(pc, pr_free));
	changes = dtfl_stream_probe_with(changes, c->probe);
	dtfl_feedback_connect(handle, changes);
	dtfl_type_free(inner);
	dtfl_type_free(edge);
	dtfl_type_free(rank);
}

typedef struct worker_ctx {
	bool fixed;
	size_t nodes, edges;
} worker_ctx;

static void
worker(dtfl_worker *w, void *ctx)
{
	worker_ctx *wc = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *inner = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	const dtfl_type *edge = dtfl_type_pair(inner, dtfl_type_i64());
	build_ctx c;
	uint64_t round = 1;
	size_t i;

	c.input = dtfl_input_new(edge);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "pagerank", tc, build, &c);
	dtfl_type_free(inner);
	dtfl_type_free(edge);

	/* Insert the edges at time 0, then advance and settle. */
	for (i = 0; wc->fixed && i < FIXED_E; i++) {
		unsigned char buf[24];

		dtfl_put_u64(buf, fixed_edges[i][0]);
		dtfl_put_u64(buf + 8, fixed_edges[i][1]);
		dtfl_put_i64(buf + 16, 1);
		dtfl_input_send(c.input, buf, sizeof buf);
	}
	dtfl_input_advance_to(c.input, &round);
	while (dtfl_probe_less_than(c.probe, dtfl_input_time(c.input)))
		dtfl_worker_step(w);
	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	worker_ctx wc;
	dtfl_config *cfg;
	int rc;

	memset(&wc, 0, sizeof wc);
	wc.fixed = argc < 3;
	wc.nodes = wc.fixed ? FIXED_N : ex_size(argc, argv, 1, 100);
	wc.edges = wc.fixed ? FIXED_E : ex_size(argc, argv, 2, 1000);
	cfg = ex_config(argc, argv, wc.fixed ? 0 : 2);
	dtfl_config_set_mode(cfg, DTFL_COMM_THREAD);
	dtfl_config_set_threads(cfg, 1);
	rc = dtfl_execute(cfg, worker, &wc, NULL);
	dtfl_config_free(cfg);
	return rc;
}
