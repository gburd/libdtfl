/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/dd/progress.c: mirrors differential-dataflow/examples/progress.rs.
 *
 * Timely dataflow progress tracking written with differential operators.
 * A dataflow graph is given as nodes (Target, Source, Summary): an input
 * port connects to an output port of the same operator along a path
 * summary, and edges (Source, Target) from an output port to an input
 * port.  Three computations run over it:
 *
 *   find_cycles  repeatedly restricts the summary-free transitions to
 *                those whose source has an incoming transition; what is
 *                left lies on a cycle that does not advance timestamps;
 *   summarize    the minimal path summaries from each location to the
 *                inputs of operator zero;
 *   frontier     for a set of (Location, time) capabilities, the minimal
 *                times that can reach each location.
 *
 * Upstream instantiates the progress timestamp T as usize, whose
 * PathSummary is usize.  Here a summary is a u64 record field holding the
 * summary word of the u64 time class, and the computations apply it with
 * the time class's summary operations (dtfl_summary_results_in,
 * dtfl_summary_followed_by, dtfl_summary_less_equal), so results_in and
 * followed_by keep usize's checked-add semantics.
 *
 * A Location is (node, port), port being Target(i) or Source(i), encoded
 * as three u64 fields (node, tag, i) with tag 0 for Target and 1 for
 * Source, which orders as upstream's derived Ord.  Divergence: the
 * elapsed-time column is omitted so the output is deterministic.
 *
 * Known wrong (why this is built but not smoke-tested, and why its
 * port-ledger row is deferred): find_cycles and summarize match the
 * pinned Rust binary, and frontier matches it at times 1 and 2.  At time
 * 3, when the last capability is retracted, Rust retracts all eight
 * frontier rows; here the frontier reduce over the +1 cycle (2.T0 ->
 * 2.S0 -> 3.T1 ...) emits wrong retractions and leaves rows behind, and
 * with -w 2 it counts to infinity.  The defect is in the library's
 * iterate/reduce under Product<u64, u64>, not in this example; see the
 * port-ledger note for the minimal reproducer.
 */
#include "example_args.h"

#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "dtfl/iterate.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <stdio.h>
#include <string.h>

#define LOC 24		/* bytes of an encoded Location */
#define TARGET 0
#define SOURCE 1

static void
put_loc(unsigned char *buf, uint64_t node, uint64_t tag, uint64_t port)
{
	dtfl_put_u64(buf, node);
	dtfl_put_u64(buf + 8, tag);
	dtfl_put_u64(buf + 16, port);
}

/* Location's derived Debug: "Location { node: 3, port: Target(0) }". */
static void
print_loc(const unsigned char *p)
{
	printf("Location { node: %llu, port: %s(%llu) }",
	    (unsigned long long)dtfl_get_u64(p),
	    dtfl_get_u64(p + 8) == TARGET ? "Target" : "Source",
	    (unsigned long long)dtfl_get_u64(p + 16));
}

/*
 * The record types, built once per worker.  A Target or Source record
 * field is (node, port); Location::from(target) inserts the tag.
 */
typedef struct types {
	const dtfl_type *loc;		/* (node, tag, port) */
	const dtfl_type *node;		/* (tnode, tport, snode, sport, summary) */
	const dtfl_type *edge;		/* (snode, sport, tnode, tport) */
	const dtfl_type *time;		/* (Location, T) */
	const dtfl_type *loc_summ;	/* (Location, Summary) */
	const dtfl_type *trans;		/* (Location, (Location, Summary)) */
	const dtfl_type *loc_loc;	/* (Location, Location) */
	dtfl_time_class *tc;		/* T = usize */
	dtfl_diff *i64;
} types;

static void
types_init(types *ty)
{
	const dtfl_type *u64 = dtfl_type_u64();
	const dtfl_type *f5[5] = { u64, u64, u64, u64, u64 };
	const dtfl_type *f3[3] = { u64, u64, u64 };

	ty->loc = dtfl_type_tuple(f3, 3);
	ty->node = dtfl_type_tuple(f5, 5);
	ty->edge = dtfl_type_tuple(f5, 4);
	ty->time = dtfl_type_pair(ty->loc, u64);
	ty->loc_summ = dtfl_type_pair(ty->loc, u64);
	ty->trans = dtfl_type_pair(ty->loc, ty->loc_summ);
	ty->loc_loc = dtfl_type_pair(ty->loc, ty->loc);
	ty->tc = dtfl_time_u64();
	ty->i64 = dtfl_diff_i64();
}

static void
types_free(types *ty)
{
	dtfl_type_free(ty->loc_loc);
	dtfl_type_free(ty->trans);
	dtfl_type_free(ty->loc_summ);
	dtfl_type_free(ty->time);
	dtfl_type_free(ty->edge);
	dtfl_type_free(ty->node);
	dtfl_type_free(ty->loc);
	dtfl_diff_free(ty->i64);
	dtfl_time_class_free(ty->tc);
}

/* Default::default() of the summary: the identity summary word. */
static uint64_t
summary_default(const types *ty)
{
	uint64_t s[DTFL_TIME_MAX];

	dtfl_summary_default(ty->tc, s);
	return s[0];
}

/* PartialOrder::less_than on summaries. */
static bool
summary_less_than(const types *ty, uint64_t a, uint64_t b)
{
	return a != b && dtfl_summary_less_equal(ty->tc, &a, &b);
}

/* Writes (Location a, (Location b, summary)). */
static void
emit_trans(dtfl_collection_emit *out, const unsigned char *a,
    const unsigned char *b, uint64_t summ)
{
	unsigned char buf[2 * LOC + 8];

	memcpy(buf, a, LOC);
	memcpy(buf + LOC, b, LOC);
	dtfl_put_u64(buf + 2 * LOC, summ);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* Location::from(Target(n, p)) and Location::from(Source(n, p)). */
static void
target_loc(unsigned char *loc, const unsigned char *np)
{
	put_loc(loc, dtfl_get_u64(np), TARGET, dtfl_get_u64(np + 8));
}

static void
source_loc(unsigned char *loc, const unsigned char *np)
{
	put_loc(loc, dtfl_get_u64(np), SOURCE, dtfl_get_u64(np + 8));
}

/* ------------------------------------------------------------------ */
/* frontier                                                            */
/* ------------------------------------------------------------------ */

/* nodes.map(|(target, source, summary)| (target, (source, summary))) */
static void
node_forward(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *r = rec;
	unsigned char t[LOC], s[LOC];

	(void)ctx; (void)len;
	target_loc(t, r);
	source_loc(s, r + 16);
	emit_trans(out, t, s, dtfl_get_u64(r + 32));
}

/* edges.map(|(source, target)| (source, (target, Default::default()))) */
static void
edge_forward(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const types *ty = ctx;
	const unsigned char *r = rec;
	unsigned char s[LOC], t[LOC];

	(void)len;
	source_loc(s, r);
	target_loc(t, r + 16);
	emit_trans(out, s, t, summary_default(ty));
}

/* join_map: (dest, summ.results_in(time)), dropping None. */
static void
reach_step(void *ctx, const void *key, size_t klen, const void *v1, size_t v1l,
    const void *v2, size_t v2l, dtfl_join_emit *out)
{
	const types *ty = ctx;
	const unsigned char *dest = v1;
	uint64_t summ = dtfl_get_u64(dest + LOC);
	uint64_t time = dtfl_get_u64(v2), next;
	unsigned char buf[LOC + 8];

	(void)key; (void)klen; (void)v1l; (void)v2l;
	if (!dtfl_summary_results_in(ty->tc, &summ, &time, &next))
		return;
	memcpy(buf, dest, LOC);
	dtfl_put_u64(buf + LOC, next);
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/* reduce: retain the lower envelope of times. */
static void
lower_envelope_times(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	const types *ty = ctx;
	int64_t one = 1;
	size_t i, j;

	(void)key; (void)klen; (void)vlens; (void)diffs;
	for (i = 0; i < n; i++) {
		uint64_t t1 = dtfl_get_u64(vals[i]);
		bool dominated = false;

		for (j = 0; j < n && !dominated; j++) {
			uint64_t t2 = dtfl_get_u64(vals[j]);

			dominated = dtfl_time_less_than(ty->tc, &t2, &t1);
		}
		if (!dominated)
			dtfl_reduce_emit_pair(out, vals[i], 8, &one);
	}
}

typedef struct iter_ctx {
	const types *ty;
	dtfl_collection *transitions;	/* outer scope */
	dtfl_collection *seed;		/* outer scope */
} iter_ctx;

static dtfl_collection *
frontier_logic(void *vctx, dtfl_scope *inner, dtfl_collection *reach)
{
	iter_ctx *ic = vctx;
	const types *ty = ic->ty;
	dtfl_collection *trans = dtfl_collection_enter(ic->transitions, inner);
	dtfl_collection *joined = dtfl_collection_join_map(trans, reach, ty->loc,
	    ty->loc_summ, dtfl_type_u64(), ty->time, reach_step,
	    DTFL_CTX((void *)ty, NULL));
	dtfl_collection *times = dtfl_collection_enter(ic->seed, inner);
	dtfl_collection *merged = dtfl_collection_concat(joined, times);
	dtfl_collection *reduced = dtfl_collection_reduce(merged, ty->loc,
	    dtfl_type_u64(), dtfl_type_u64(), ty->i64, lower_envelope_times,
	    DTFL_CTX((void *)ty, NULL));

	dtfl_collection_free(merged);
	dtfl_collection_free(times);
	dtfl_collection_free(joined);
	dtfl_collection_free(trans);
	return reduced;
}

static dtfl_collection *
frontier(const types *ty, dtfl_collection *nodes, dtfl_collection *edges,
    dtfl_collection *times)
{
	dtfl_collection *n = dtfl_collection_map(nodes, ty->trans, node_forward,
	    DTFL_NOCTX);
	dtfl_collection *e = dtfl_collection_map(edges, ty->trans, edge_forward,
	    DTFL_CTX((void *)ty, NULL));
	dtfl_collection *transitions = dtfl_collection_concat(n, e);
	iter_ctx ic = { ty, transitions, times };
	dtfl_collection *it = dtfl_collection_iterate(times, frontier_logic,
	    DTFL_CTX(&ic, NULL));
	dtfl_collection *res = dtfl_collection_consolidate(it);

	dtfl_collection_free(it);
	dtfl_collection_free(transitions);
	dtfl_collection_free(e);
	dtfl_collection_free(n);
	return res;
}

/* ------------------------------------------------------------------ */
/* summarize                                                           */
/* ------------------------------------------------------------------ */

/* edges.map(target).filter(node == 0).map(|l| (l, (l, Default))) */
static void
zero_input(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const types *ty = ctx;
	const unsigned char *r = rec;
	unsigned char t[LOC];

	(void)len;
	target_loc(t, r + 16);
	if (dtfl_get_u64(t) == 0)
		emit_trans(out, t, t, summary_default(ty));
}

/* nodes.map(|(target, source, summary)| (source, (target, summary))) */
static void
node_backward(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *r = rec;
	unsigned char t[LOC], s[LOC];

	(void)ctx; (void)len;
	target_loc(t, r);
	source_loc(s, r + 16);
	emit_trans(out, s, t, dtfl_get_u64(r + 32));
}

/* edges.map(|(source, target)| (target, (source, Default::default()))) */
static void
edge_backward(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const types *ty = ctx;
	const unsigned char *r = rec;
	unsigned char s[LOC], t[LOC];

	(void)len;
	source_loc(s, r);
	target_loc(t, r + 16);
	emit_trans(out, t, s, summary_default(ty));
}

/* join_map: (from, to, summ1.followed_by(summ2)), dropping None. */
static void
summary_step(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1l, const void *v2, size_t v2l, dtfl_join_emit *out)
{
	const types *ty = ctx;
	const unsigned char *from = v1, *to = v2;
	uint64_t s1 = dtfl_get_u64(from + LOC), s2 = dtfl_get_u64(to + LOC), s;
	unsigned char buf[2 * LOC + 8];

	(void)key; (void)klen; (void)v1l; (void)v2l;
	if (!dtfl_summary_followed_by(ty->tc, &s1, &s2, &s))
		return;
	memcpy(buf, from, LOC);
	memcpy(buf + LOC, to, LOC);
	dtfl_put_u64(buf + 2 * LOC, s);
	dtfl_join_emit_record(out, buf, sizeof buf);
}

/*
 * map(|(from, (to, summary))| ((from, to), summary)) and back.  The two
 * layouts are the same bytes (records are concatenated fields), so this
 * is a copy that changes only the type the next operator splits by.
 */
static void
copy_record(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx;
	dtfl_collection_emit_record(out, rec, len);
}

/* reduce: retain the lower envelope of summaries. */
static void
lower_envelope_summaries(void *ctx, const void *key, size_t klen, size_t n,
    const void *const *vals, const size_t *vlens, const void *diffs,
    dtfl_reduce_emit *out)
{
	const types *ty = ctx;
	int64_t one = 1;
	size_t i, j;

	(void)key; (void)klen; (void)vlens; (void)diffs;
	for (i = 0; i < n; i++) {
		uint64_t s1 = dtfl_get_u64(vals[i]);
		bool dominated = false;

		for (j = 0; j < n && !dominated; j++)
			dominated = summary_less_than(ty,
			    dtfl_get_u64(vals[j]), s1);
		if (!dominated)
			dtfl_reduce_emit_pair(out, vals[i], 8, &one);
	}
}

static dtfl_collection *
summarize_logic(void *vctx, dtfl_scope *inner, dtfl_collection *summaries)
{
	iter_ctx *ic = vctx;
	const types *ty = ic->ty;
	dtfl_collection *trans = dtfl_collection_enter(ic->transitions, inner);
	dtfl_collection *joined = dtfl_collection_join_map(trans, summaries,
	    ty->loc, ty->loc_summ, ty->loc_summ, ty->trans, summary_step,
	    DTFL_CTX((void *)ty, NULL));
	dtfl_collection *zero = dtfl_collection_enter(ic->seed, inner);
	dtfl_collection *merged = dtfl_collection_concat(joined, zero);
	const dtfl_type *keyed_type = dtfl_type_pair(ty->loc_loc,
	    dtfl_type_u64());
	dtfl_collection *keyed = dtfl_collection_map(merged, keyed_type,
	    copy_record, DTFL_NOCTX);
	dtfl_collection *reduced = dtfl_collection_reduce(keyed, ty->loc_loc,
	    dtfl_type_u64(), dtfl_type_u64(), ty->i64,
	    lower_envelope_summaries, DTFL_CTX((void *)ty, NULL));
	dtfl_collection *res = dtfl_collection_map(reduced, ty->trans,
	    copy_record, DTFL_NOCTX);

	dtfl_collection_free(reduced);
	dtfl_collection_free(keyed);
	dtfl_type_free(keyed_type);
	dtfl_collection_free(merged);
	dtfl_collection_free(zero);
	dtfl_collection_free(joined);
	dtfl_collection_free(trans);
	return res;
}

static dtfl_collection *
summarize(const types *ty, dtfl_collection *nodes, dtfl_collection *edges)
{
	dtfl_collection *zero = dtfl_collection_flat_map(edges, ty->trans,
	    zero_input, DTFL_CTX((void *)ty, NULL));
	dtfl_collection *n = dtfl_collection_map(nodes, ty->trans,
	    node_backward, DTFL_NOCTX);
	dtfl_collection *e = dtfl_collection_map(edges, ty->trans,
	    edge_backward, DTFL_CTX((void *)ty, NULL));
	dtfl_collection *transitions = dtfl_collection_concat(n, e);
	iter_ctx ic = { ty, transitions, zero };
	dtfl_collection *it = dtfl_collection_iterate(zero, summarize_logic,
	    DTFL_CTX(&ic, NULL));
	dtfl_collection *res = dtfl_collection_consolidate(it);

	dtfl_collection_free(it);
	dtfl_collection_free(transitions);
	dtfl_collection_free(e);
	dtfl_collection_free(n);
	dtfl_collection_free(zero);
	return res;
}

/* ------------------------------------------------------------------ */
/* find_cycles                                                         */
/* ------------------------------------------------------------------ */

/* Keep (target, source) for nodes whose summary is the default. */
static void
node_if_default(void *ctx, const void *rec, size_t len,
    dtfl_collection_emit *out)
{
	const types *ty = ctx;
	const unsigned char *r = rec;
	unsigned char buf[2 * LOC];

	(void)len;
	if (dtfl_get_u64(r + 32) != summary_default(ty))
		return;
	target_loc(buf, r);
	source_loc(buf + LOC, r + 16);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* edges.map(|(source, target)| (source, target)) as Locations. */
static void
edge_locs(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	const unsigned char *r = rec;
	unsigned char buf[2 * LOC];

	(void)ctx; (void)len;
	source_loc(buf, r);
	target_loc(buf + LOC, r + 16);
	dtfl_collection_emit_record(out, buf, sizeof buf);
}

/* locations.map(|(_source, target)| target) */
static void
second_loc(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	(void)ctx; (void)len;
	dtfl_collection_emit_record(out, (const unsigned char *)rec + LOC, LOC);
}

static dtfl_collection *
cycles_logic(void *vctx, dtfl_scope *inner, dtfl_collection *locations)
{
	iter_ctx *ic = vctx;
	const types *ty = ic->ty;
	dtfl_collection *targets = dtfl_collection_map(locations, ty->loc,
	    second_loc, DTFL_NOCTX);
	dtfl_collection *active = dtfl_collection_distinct(targets, ty->i64);
	dtfl_collection *trans = dtfl_collection_enter(ic->transitions, inner);
	dtfl_collection *res = dtfl_collection_semijoin(trans, active, ty->loc,
	    ty->loc, ty->loc_loc);

	dtfl_collection_free(trans);
	dtfl_collection_free(active);
	dtfl_collection_free(targets);
	return res;
}

static dtfl_collection *
find_cycles(const types *ty, dtfl_collection *nodes, dtfl_collection *edges)
{
	dtfl_collection *n = dtfl_collection_flat_map(nodes, ty->loc_loc,
	    node_if_default, DTFL_CTX((void *)ty, NULL));
	dtfl_collection *e = dtfl_collection_map(edges, ty->loc_loc, edge_locs,
	    DTFL_NOCTX);
	dtfl_collection *transitions = dtfl_collection_concat(n, e);
	iter_ctx ic = { ty, transitions, NULL };
	dtfl_collection *it = dtfl_collection_iterate(transitions, cycles_logic,
	    DTFL_CTX(&ic, NULL));
	dtfl_collection *res = dtfl_collection_consolidate(it);

	dtfl_collection_free(it);
	dtfl_collection_free(transitions);
	dtfl_collection_free(e);
	dtfl_collection_free(n);
	return res;
}

/* ------------------------------------------------------------------ */
/* Output and driver                                                   */
/* ------------------------------------------------------------------ */

static void
print_tail(const uint64_t *time, const void *diff)
{
	int64_t d;

	memcpy(&d, diff, sizeof d);
	printf(", %llu, %lld)\n", (unsigned long long)time[0], (long long)d);
}

static void
inspect_cycles(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	const unsigned char *r = rec;

	(void)ctx; (void)len;
	printf("cycles: ((");
	print_loc(r);
	printf(", ");
	print_loc(r + LOC);
	printf(")");
	print_tail(time, diff);
}

static void
inspect_summary(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	const unsigned char *r = rec;

	(void)ctx; (void)len;
	printf("summary: ((");
	print_loc(r);
	printf(", (");
	print_loc(r + LOC);
	printf(", %llu))", (unsigned long long)dtfl_get_u64(r + 2 * LOC));
	print_tail(time, diff);
}

static void
inspect_frontier(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	const unsigned char *r = rec;

	(void)ctx; (void)len;
	printf("frontier: ((");
	print_loc(r);
	printf(", %llu)", (unsigned long long)dtfl_get_u64(r + LOC));
	print_tail(time, diff);
}

typedef struct worker_ctx {
	types ty;
	dtfl_input_session *nodes, *edges, *times;
	dtfl_probe *probe;
} worker_ctx;

/* inspect then probe_with, freeing the intermediate handles. */
static void
report(dtfl_collection *c, dtfl_collection_inspect_fn fn, dtfl_probe *probe)
{
	dtfl_collection *ins = dtfl_collection_inspect(c, fn, DTFL_NOCTX);
	dtfl_collection *probed = dtfl_collection_probe_with(ins, probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(c);
}

static void
build(dtfl_scope *scope, void *arg)
{
	worker_ctx *w = arg;
	dtfl_collection *nodes = dtfl_input_session_to_collection(w->nodes,
	    scope);
	dtfl_collection *edges = dtfl_input_session_to_collection(w->edges,
	    scope);
	dtfl_collection *times = dtfl_input_session_to_collection(w->times,
	    scope);

	/* Detect cycles that do not increment timestamps. */
	report(find_cycles(&w->ty, nodes, edges), inspect_cycles, w->probe);
	/* Summarize all paths to inputs of operator zero. */
	report(summarize(&w->ty, nodes, edges), inspect_summary, w->probe);
	/* Track the frontier at each dataflow location. */
	report(frontier(&w->ty, nodes, edges, times), inspect_frontier,
	    w->probe);

	dtfl_collection_free(times);
	dtfl_collection_free(edges);
	dtfl_collection_free(nodes);
}

static void
insert_node(worker_ctx *w, uint64_t tn, uint64_t tp, uint64_t sn, uint64_t sp,
    uint64_t summ)
{
	unsigned char buf[40];

	dtfl_put_u64(buf, tn);
	dtfl_put_u64(buf + 8, tp);
	dtfl_put_u64(buf + 16, sn);
	dtfl_put_u64(buf + 24, sp);
	dtfl_put_u64(buf + 32, summ);
	dtfl_input_session_insert(w->nodes, buf, sizeof buf);
}

static void
insert_edge(worker_ctx *w, uint64_t sn, uint64_t sp, uint64_t tn, uint64_t tp)
{
	unsigned char buf[32];

	dtfl_put_u64(buf, sn);
	dtfl_put_u64(buf + 8, sp);
	dtfl_put_u64(buf + 16, tn);
	dtfl_put_u64(buf + 24, tp);
	dtfl_input_session_insert(w->edges, buf, sizeof buf);
}

/* times.insert / remove ((location, t)) with a +1 / -1 change. */
static void
update_time(worker_ctx *w, uint64_t node, uint64_t tag, uint64_t port,
    uint64_t t, bool insert)
{
	unsigned char buf[LOC + 8];

	put_loc(buf, node, tag, port);
	dtfl_put_u64(buf + LOC, t);
	if (insert)
		dtfl_input_session_insert(w->times, buf, sizeof buf);
	else
		dtfl_input_session_remove(w->times, buf, sizeof buf);
}

/* Advance all three inputs to `t` and step until the probe passes it. */
static void
advance(dtfl_worker *wk, worker_ctx *w, uint64_t t)
{
	dtfl_input_session_advance_to(w->nodes, &t);
	dtfl_input_session_flush(w->nodes);
	dtfl_input_session_advance_to(w->edges, &t);
	dtfl_input_session_flush(w->edges);
	dtfl_input_session_advance_to(w->times, &t);
	dtfl_input_session_flush(w->times);
	while (dtfl_probe_less_than(w->probe, dtfl_input_session_time(w->times)))
		dtfl_worker_step(wk);
}

static void
worker(dtfl_worker *wk, void *ctx)
{
	worker_ctx w;

	(void)ctx;
	types_init(&w.ty);
	w.nodes = dtfl_input_session_new(w.ty.node, w.ty.i64);
	w.edges = dtfl_input_session_new(w.ty.edge, w.ty.i64);
	w.times = dtfl_input_session_new(w.ty.time, w.ty.i64);
	w.probe = dtfl_probe_new();
	dtfl_worker_dataflow(wk, "progress", w.ty.tc, build, &w);

	/*
	 * A PageRank-like graph, as represented here:
	 * https://github.com/TimelyDataflow/diagnostics/blob/master/examples/pagerank.png
	 * Every worker inserts it, as upstream does.
	 */
	insert_node(&w, 2, 0, 2, 0, 1);
	insert_node(&w, 3, 0, 3, 0, 0);
	insert_node(&w, 3, 1, 3, 0, 0);
	insert_node(&w, 4, 0, 4, 0, 0);

	insert_edge(&w, 1, 0, 3, 0);
	insert_edge(&w, 3, 0, 4, 0);
	insert_edge(&w, 4, 0, 2, 0);
	insert_edge(&w, 2, 0, 3, 1);

	/* Initially no capabilities. */
	advance(wk, &w, 1);

	/* Introduce a new input capability at time zero. */
	update_time(&w, 1, SOURCE, 0, 0, true);
	advance(wk, &w, 2);

	/* Remove input capability and produce a message. */
	update_time(&w, 1, SOURCE, 0, 0, false);
	update_time(&w, 3, TARGET, 0, 0, true);
	advance(wk, &w, 3);

	/* Consume the message, and .. do nothing, I guess. */
	update_time(&w, 3, TARGET, 0, 0, false);
	advance(wk, &w, 4);

	printf("finished\n");

	dtfl_input_session_close(w.nodes);
	dtfl_input_session_close(w.edges);
	dtfl_input_session_close(w.times);
	while (dtfl_worker_step(wk))
		;
	dtfl_probe_free(w.probe);
	types_free(&w.ty);
}

int
main(int argc, char **argv)
{
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc = dtfl_execute(cfg, worker, NULL, NULL);

	dtfl_config_free(cfg);
	return rc;
}
