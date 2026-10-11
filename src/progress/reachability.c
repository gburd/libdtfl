/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/progress/reachability.c: pointstamp reachability in a dataflow graph.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/progress/reachability.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A literal port (PLAN D10).  The Builder collects nodes (internal
 * connectivity) and edges; build() computes, by a semi-naive fixed
 * point (summarize_outputs), the summaries from every location to every
 * scope output, and returns a Tracker.  The Tracker maintains, for every
 * port, the pointstamps placed there and their implications (the
 * pointstamps that could reach it), and propagate_all() moves changes
 * through the graph in time order with a worklist.
 *
 * Representation: upstream's nested columnar Vecs become flat arrays
 * indexed through per-node offsets.  Summaries are runs of `sw` words
 * (sdepth, at least one).  The worklist is a binary min-heap ordered by
 * (time, location) under Ord, which is word order (PLAN D3), so its
 * pop order equals upstream's BinaryHeap<Reverse<(T, Location, i64)>>
 * among distinct (time, location); entries with equal (time, location)
 * are merged on pop, as upstream does, so the diff tie-break does not
 * matter.
 */
#include "progress/operate_int.h"

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Builder                                                             */
/* ------------------------------------------------------------------ */

typedef struct reach_node {
	size_t inputs, outputs;
	__dtfl_connectivity summary;	/* per input */
	__dtfl_vec *edges;		/* per output: __dtfl_location targets */
	bool present;
} reach_node;

struct __dtfl_reach_builder {
	const dtfl_time_class *tc;
	__dtfl_vec nodes;	/* reach_node */
};

__dtfl_reach_builder *
__dtfl_reach_builder_new(const dtfl_time_class *tc)
{
	__dtfl_reach_builder *b = __dtfl_calloc(1, sizeof *b);

	b->tc = tc;
	__dtfl_vec_init(&b->nodes, sizeof(reach_node));
	return b;
}

static void
node_free(reach_node *n)
{
	size_t o;

	__dtfl_connectivity_free(&n->summary);
	if (n->edges != NULL) {
		for (o = 0; o < n->outputs; o++)
			__dtfl_vec_free(&n->edges[o]);
		__dtfl_free(n->edges);
	}
}

void
__dtfl_reach_builder_free(__dtfl_reach_builder *b)
{
	size_t i;

	if (b == NULL)
		return;
	for (i = 0; i < b->nodes.len; i++)
		node_free(DTFL_VEC_AT(&b->nodes, reach_node, i));
	__dtfl_vec_free(&b->nodes);
	__dtfl_free(b);
}

/*
 * Builder::add_node.  Takes over `summary`'s contents (the caller's
 * struct is left empty).  Re-adding a node with the same output count
 * keeps its edges, as upstream does.
 */
void
__dtfl_reach_builder_add_node(__dtfl_reach_builder *b, size_t index,
    size_t inputs, size_t outputs, __dtfl_connectivity *summary)
{
	reach_node *n;
	size_t o, i;

	DTFL_ASSERT(summary->len == inputs);
	for (i = 0; i < summary->len; i++) {
		const __dtfl_port_conn *pc = &summary->ports[i];
		size_t e;

		for (e = 0; e < pc->entries.len; e++)
			DTFL_ASSERT(DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry,
			    e)->port < outputs);
	}
	while (b->nodes.len <= index)
		__dtfl_vec_push(&b->nodes, NULL);
	n = DTFL_VEC_AT(&b->nodes, reach_node, index);
	__dtfl_connectivity_free(&n->summary);
	n->summary = *summary;
	summary->ports = NULL;
	summary->len = 0;
	if (n->edges == NULL || n->outputs != outputs) {
		if (n->edges != NULL) {
			for (o = 0; o < n->outputs; o++)
				__dtfl_vec_free(&n->edges[o]);
			__dtfl_free(n->edges);
		}
		n->edges = __dtfl_calloc(DTFL_MAX(outputs, 1), sizeof(__dtfl_vec));
		for (o = 0; o < outputs; o++)
			__dtfl_vec_init(&n->edges[o], sizeof(__dtfl_location));
	}
	n->inputs = inputs;
	n->outputs = outputs;
	n->present = true;
}

/* Builder::add_edge, from an operator output to an operator input. */
void
__dtfl_reach_builder_add_edge(__dtfl_reach_builder *b, __dtfl_location source,
    __dtfl_location target)
{
	reach_node *n;

	DTFL_ASSERT(source.source && !target.source);
	DTFL_ASSERT(source.node < b->nodes.len && target.node < b->nodes.len);
	n = DTFL_VEC_AT(&b->nodes, reach_node, source.node);
	DTFL_ASSERT(source.port < n->outputs);
	DTFL_ASSERT(target.port < DTFL_VEC_AT(&b->nodes, reach_node,
	    target.node)->inputs);
	__dtfl_vec_push(&n->edges[source.port], &target);
}

/* Dense index of a location: each node's targets, then its sources. */
static size_t
loc_index(const size_t *offsets, const reach_node *nodes, __dtfl_location l)
{
	return offsets[l.node] + (l.source ? nodes[l.node].inputs + l.port : l.port);
}

/*
 * Builder::is_acyclic: true if no cycle exists along default
 * (identity) summaries, i.e. every cycle advances the timestamp.
 */
bool
__dtfl_reach_builder_is_acyclic(const __dtfl_reach_builder *b)
{
	const reach_node *nodes = (const reach_node *)b->nodes.data;
	size_t nn = b->nodes.len, locations = 0, remaining = 0, i, p, e;
	size_t *offsets = __dtfl_calloc(DTFL_MAX(nn, 1), sizeof *offsets);
	size_t *in_degree;
	__dtfl_vec work;
	uint64_t def[DTFL_TIME_MAX + 1];
	size_t sw = DTFL_MAX(b->tc->sdepth, 1u);
	bool ok;

	dtfl_summary_default(b->tc, def);
	if (b->tc->sdepth == 0)
		def[0] = 0;
	for (i = 0; i < nn; i++) {
		offsets[i] = locations;
		locations += nodes[i].inputs + nodes[i].outputs;
	}
	in_degree = __dtfl_calloc(DTFL_MAX(locations, 1), sizeof *in_degree);
	/* Load edges as default summaries. */
	for (i = 0; i < nn; i++)
		for (p = 0; p < nodes[i].outputs; p++)
			for (e = 0; e < nodes[i].edges[p].len; e++)
				in_degree[loc_index(offsets, nodes,
				    *DTFL_VEC_AT(&nodes[i].edges[p], __dtfl_location, e))]++;
	/* Load default intra-node summaries. */
	for (i = 0; i < nn; i++) {
		for (p = 0; p < nodes[i].summary.len; p++) {
			const __dtfl_port_conn *pc = &nodes[i].summary.ports[p];

			for (e = 0; e < pc->entries.len; e++) {
				const __dtfl_port_conn_entry *ent =
				    DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, e);
				__dtfl_location src = { i, ent->port, true };
				size_t k, ns = ent->summaries.len / sw;

				for (k = 0; k < ns; k++)
					if (memcmp((const uint64_t *)ent->summaries.data + k * sw,
					    def, sw * sizeof(uint64_t)) == 0)
						in_degree[loc_index(offsets, nodes, src)]++;
			}
		}
	}
	for (i = 0; i < locations; i++)
		if (in_degree[i] > 0)
			remaining++;
	__dtfl_vec_init(&work, sizeof(__dtfl_location));
	for (i = 0; i < nn; i++) {
		for (p = 0; p < nodes[i].inputs; p++) {
			__dtfl_location l = { i, p, false };
			if (in_degree[loc_index(offsets, nodes, l)] == 0)
				__dtfl_vec_push(&work, &l);
		}
		for (p = 0; p < nodes[i].outputs; p++) {
			__dtfl_location l = { i, p, true };
			if (in_degree[loc_index(offsets, nodes, l)] == 0)
				__dtfl_vec_push(&work, &l);
		}
	}
	while (work.len > 0) {
		__dtfl_location l = *(__dtfl_location *)__dtfl_vec_last(&work);

		__dtfl_vec_truncate(&work, work.len - 1);
		if (l.source) {
			const __dtfl_vec *tg = &nodes[l.node].edges[l.port];

			for (e = 0; e < tg->len; e++) {
				__dtfl_location t = *DTFL_VEC_AT(tg, __dtfl_location, e);
				size_t ix = loc_index(offsets, nodes, t);

				if (--in_degree[ix] == 0) {
					remaining--;
					__dtfl_vec_push(&work, &t);
				}
			}
		} else {
			const __dtfl_port_conn *pc = &nodes[l.node].summary.ports[l.port];

			for (e = 0; e < pc->entries.len; e++) {
				const __dtfl_port_conn_entry *ent =
				    DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, e);
				__dtfl_location s = { l.node, ent->port, true };
				size_t ix = loc_index(offsets, nodes, s), k;
				size_t ns = ent->summaries.len / sw;

				for (k = 0; k < ns; k++) {
					if (memcmp((const uint64_t *)ent->summaries.data + k * sw,
					    def, sw * sizeof(uint64_t)) != 0)
						continue;
					if (--in_degree[ix] == 0) {
						remaining--;
						__dtfl_vec_push(&work, &s);
					}
				}
			}
		}
	}
	ok = remaining == 0;
	__dtfl_vec_free(&work);
	__dtfl_free(in_degree);
	__dtfl_free(offsets);
	return ok;
}

/* ------------------------------------------------------------------ */
/* summarize_outputs                                                   */
/* ------------------------------------------------------------------ */

/* Location key words for sorting: (node, is_source<<63|port). */
static int
loc_cmp(__dtfl_location a, __dtfl_location b)
{
	uint64_t ka[2], kb[2];

	__dtfl_location_key(a, ka);
	__dtfl_location_key(b, kb);
	return __dtfl_words_cmp(ka, kb, 2);
}

typedef struct rev_edge {
	__dtfl_location target, source;
} rev_edge;

typedef struct rev_internal {
	__dtfl_location source;	/* operator output */
	size_t input;
	const __dtfl_vec *summaries;	/* antichain for (input, output) */
} rev_internal;

/* A (location, output) key with its accumulated antichain. */
typedef struct acc_entry {
	__dtfl_location loc;
	size_t output;
	__dtfl_vec summaries;	/* uint64_t words */
} acc_entry;

typedef struct todo_entry {
	__dtfl_location loc;
	size_t output;
	uint64_t s[DTFL_TIME_MAX + 1];
} todo_entry;

static int
key_cmp(__dtfl_location la, size_t oa, __dtfl_location lb, size_t ob)
{
	int c = loc_cmp(la, lb);

	if (c != 0)
		return c;
	return oa < ob ? -1 : oa > ob;
}

static void
sort_rev_edges(rev_edge *v, size_t n)
{
	size_t i, j;

	/* Insertion sort on small graphs; heap of runs on large ones. */
	for (i = 1; i < n; i++) {
		rev_edge x = v[i];

		for (j = i; j > 0 && loc_cmp(v[j - 1].target, x.target) > 0; j--)
			v[j] = v[j - 1];
		v[j] = x;
	}
}

static void
sort_rev_internal(rev_internal *v, size_t n)
{
	size_t i, j;

	for (i = 1; i < n; i++) {
		rev_internal x = v[i];

		for (j = i; j > 0; j--) {
			int c = loc_cmp(v[j - 1].source, x.source);

			if (c < 0 || (c == 0 && v[j - 1].input <= x.input))
				break;
			v[j] = v[j - 1];
		}
		v[j] = x;
	}
}

/*
 * Finds the accumulated entry for (loc, output) by binary search in a
 * vector kept sorted by key.  Upstream keeps binary-sized runs so a
 * round's batch is sorted only once; this single sorted vector costs
 * O(n) per novel key, acceptable for the graph sizes the tests cover.
 */
static size_t
acc_find(const __dtfl_vec *acc, __dtfl_location loc, size_t output, bool *found)
{
	size_t lo = 0, hi = acc->len;

	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		const acc_entry *e = DTFL_VEC_AT(acc, acc_entry, mid);
		int c = key_cmp(e->loc, e->output, loc, output);

		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	*found = lo < acc->len && key_cmp(DTFL_VEC_AT(acc, acc_entry, lo)->loc,
	    DTFL_VEC_AT(acc, acc_entry, lo)->output, loc, output) == 0;
	return lo;
}

/* Antichain insert on a summary vector; returns true if s was added. */
static bool
acc_insert(const dtfl_time_class *tc, __dtfl_vec *v, const uint64_t *s)
{
	size_t w = DTFL_MAX(tc->sdepth, 1u), n = v->len / w, i;
	uint64_t *e = (uint64_t *)v->data;

	for (i = 0; i < n; i++)
		if (dtfl_summary_less_equal(tc, e + i * w, s))
			return false;
	for (i = n; i-- > 0;) {
		if (dtfl_summary_less_equal(tc, s, e + i * w)) {
			memmove(e + i * w, e + (i + 1) * w,
			    (n - i - 1) * w * sizeof(uint64_t));
			n--;
		}
	}
	__dtfl_vec_truncate(v, n * w);
	__dtfl_vec_push_n(v, s, w);
	return true;
}

/*
 * summarize_outputs: for every location, the antichain of summaries to
 * each scope output (node 0 targets).  Round-based semi-naive fixed
 * point: each round walks reverse edges and reverse internal summaries
 * from the triples that changed in the previous round.  Results land in
 * `acc` sorted by (location, output).
 */
static void
summarize_outputs(const dtfl_time_class *tc, const reach_node *nodes,
    size_t nn, __dtfl_vec *acc)
{
	size_t sw = DTFL_MAX(tc->sdepth, 1u);
	__dtfl_vec rev_edges, rev_int, todo, next;
	size_t i, p, e;

	__dtfl_vec_init(&rev_edges, sizeof(rev_edge));
	__dtfl_vec_init(&rev_int, sizeof(rev_internal));
	__dtfl_vec_init(&todo, sizeof(todo_entry));
	__dtfl_vec_init(&next, sizeof(todo_entry));

	for (i = 0; i < nn; i++)
		for (p = 0; p < nodes[i].outputs; p++)
			for (e = 0; e < nodes[i].edges[p].len; e++) {
				rev_edge r;

				r.target = *DTFL_VEC_AT(&nodes[i].edges[p], __dtfl_location, e);
				r.source.node = i;
				r.source.port = p;
				r.source.source = true;
				__dtfl_vec_push(&rev_edges, &r);
			}
	sort_rev_edges((rev_edge *)rev_edges.data, rev_edges.len);
	/* dedup */
	if (rev_edges.len > 1) {
		rev_edge *v = (rev_edge *)rev_edges.data;
		size_t k = 1;

		for (i = 1; i < rev_edges.len; i++)
			if (loc_cmp(v[i].target, v[k - 1].target) != 0 ||
			    loc_cmp(v[i].source, v[k - 1].source) != 0)
				v[k++] = v[i];
		__dtfl_vec_truncate(&rev_edges, k);
	}
	for (i = 0; i < nn; i++)
		for (p = 0; p < nodes[i].summary.len; p++) {
			const __dtfl_port_conn *pc = &nodes[i].summary.ports[p];

			for (e = 0; e < pc->entries.len; e++) {
				const __dtfl_port_conn_entry *ent =
				    DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, e);
				rev_internal r;

				r.source.node = i;
				r.source.port = ent->port;
				r.source.source = true;
				r.input = p;
				r.summaries = &ent->summaries;
				__dtfl_vec_push(&rev_int, &r);
			}
		}
	sort_rev_internal((rev_internal *)rev_int.data, rev_int.len);

	/* Seed: every edge into a scope output, with the default summary. */
	for (i = 0; i < nn; i++)
		for (p = 0; p < nodes[i].outputs; p++)
			for (e = 0; e < nodes[i].edges[p].len; e++) {
				__dtfl_location t = *DTFL_VEC_AT(&nodes[i].edges[p],
				    __dtfl_location, e);
				todo_entry te;

				if (t.node != 0)
					continue;
				memset(&te, 0, sizeof te);
				te.loc = t;
				te.output = t.port;
				dtfl_summary_default(tc, te.s);
				__dtfl_vec_push(&todo, &te);
			}

	while (todo.len > 0) {
		/* Collect proposals from the triples changed last round. */
		__dtfl_vec proposals;

		__dtfl_vec_init(&proposals, sizeof(todo_entry));
		for (i = 0; i < todo.len; i++) {
			const todo_entry *t = DTFL_VEC_AT(&todo, todo_entry, i);

			if (t->loc.source) {
				/* Walk up the operator to its inputs. */
				const rev_internal *ri = (const rev_internal *)rev_int.data;
				size_t lo = 0, hi = rev_int.len, k;

				while (lo < hi) {
					size_t mid = lo + (hi - lo) / 2;
					if (loc_cmp(ri[mid].source, t->loc) < 0)
						lo = mid + 1;
					else
						hi = mid;
				}
				for (k = lo; k < rev_int.len &&
				    loc_cmp(ri[k].source, t->loc) == 0; k++) {
					size_t ns = ri[k].summaries->len / sw, m;

					for (m = 0; m < ns; m++) {
						todo_entry pr;

						memset(&pr, 0, sizeof pr);
						pr.loc.node = t->loc.node;
						pr.loc.port = ri[k].input;
						pr.loc.source = false;
						pr.output = t->output;
						if (dtfl_summary_followed_by(tc,
						    (const uint64_t *)ri[k].summaries->data + m * sw,
						    t->s, pr.s))
							__dtfl_vec_push(&proposals, &pr);
					}
				}
			} else {
				/* Walk back the unique edge into this target. */
				const rev_edge *re = (const rev_edge *)rev_edges.data;
				size_t lo = 0, hi = rev_edges.len;

				while (lo < hi) {
					size_t mid = lo + (hi - lo) / 2;
					if (loc_cmp(re[mid].target, t->loc) < 0)
						lo = mid + 1;
					else
						hi = mid;
				}
				if (lo < rev_edges.len && loc_cmp(re[lo].target, t->loc) == 0) {
					todo_entry pr = *t;

					pr.loc = re[lo].source;
					__dtfl_vec_push(&proposals, &pr);
				}
			}
		}
		__dtfl_vec_clear(&todo);

		/* Merge proposals into the accumulated antichains. */
		for (i = 0; i < proposals.len; i++) {
			const todo_entry *pr = DTFL_VEC_AT(&proposals, todo_entry, i);
			bool found;
			size_t at = acc_find(acc, pr->loc, pr->output, &found);
			acc_entry *a;

			if (!found) {
				acc_entry fresh;

				fresh.loc = pr->loc;
				fresh.output = pr->output;
				__dtfl_vec_init(&fresh.summaries, sizeof(uint64_t));
				acc_entry *base;

				__dtfl_vec_push(acc, NULL);
				base = (acc_entry *)acc->data;
				memmove(base + at + 1, base + at,
				    (acc->len - at - 1) * sizeof fresh);
				base[at] = fresh;
			}
			a = DTFL_VEC_AT(acc, acc_entry, at);
			if (acc_insert(tc, &a->summaries, pr->s))
				__dtfl_vec_push(&todo, pr);
		}
		__dtfl_vec_free(&proposals);
	}
	__dtfl_vec_free(&rev_edges);
	__dtfl_vec_free(&rev_int);
	__dtfl_vec_free(&todo);
	__dtfl_vec_free(&next);
}

/* ------------------------------------------------------------------ */
/* Tracker                                                             */
/* ------------------------------------------------------------------ */

/* Per-port state (PortInformation). */
typedef struct port_info {
	__dtfl_mutable_antichain pointstamps;
	__dtfl_mutable_antichain implications;
	/* Summaries from this port to scope outputs: (output, summary) runs. */
	__dtfl_vec out_sum;	/* struct { size_t output; uint64_t s[sw]; } as words */
} port_info;

typedef struct per_op {
	size_t inputs, outputs;
	port_info *targets;
	port_info *sources;
	int64_t cap_counts;
	/* Internal summaries per input: (output, summary) flattened. */
	__dtfl_vec *node_sum;	/* per input: words (output, s...) */
	__dtfl_vec *edges;	/* per output: __dtfl_location */
} per_op;

struct __dtfl_tracker {
	const dtfl_time_class *tc;
	unsigned depth, sw;
	size_t nodes;
	per_op *ops;
	__dtfl_change_batch target_changes;	/* key: (Location, T) */
	__dtfl_change_batch source_changes;	/* key: (Location, T) */
	__dtfl_vec worklist;			/* heap of words: T, loc(2), diff */
	__dtfl_change_batch pushed_changes;	/* key: (Location, T) */
	__dtfl_change_batch *output_changes;	/* per scope output: key T */
	size_t scope_outputs;
	int64_t total_counts;
	__dtfl_change_batch scratch;		/* key T */
};

/* Worklist entries are words: [time (depth)] [loc (2)] [diff (1)]. */
static size_t
wl_words(const __dtfl_tracker *t)
{
	return (size_t)t->depth + 3;
}

static int
wl_cmp(const __dtfl_tracker *t, const uint64_t *a, const uint64_t *b)
{
	/* Order by time (Ord), then location; diff does not participate. */
	return __dtfl_words_cmp(a, b, t->depth + 2);
}

static void
wl_push(__dtfl_tracker *t, const uint64_t *time, __dtfl_location loc, int64_t diff)
{
	size_t w = wl_words(t), i;
	uint64_t *e, *heap;

	e = __dtfl_vec_push_n(&t->worklist, NULL, w);
	__dtfl_time_copy(t->tc, e, time);
	__dtfl_location_key(loc, e + t->depth);
	e[t->depth + 2] = (uint64_t)diff;
	heap = (uint64_t *)t->worklist.data;
	i = t->worklist.len / w - 1;
	while (i > 0) {
		size_t parent = (i - 1) / 2;
		uint64_t tmp[DTFL_TIME_MAX + 3];

		if (wl_cmp(t, heap + parent * w, heap + i * w) <= 0)
			break;
		memcpy(tmp, heap + parent * w, w * sizeof(uint64_t));
		memcpy(heap + parent * w, heap + i * w, w * sizeof(uint64_t));
		memcpy(heap + i * w, tmp, w * sizeof(uint64_t));
		i = parent;
	}
}

static bool
wl_peek(const __dtfl_tracker *t, const uint64_t **top)
{
	if (t->worklist.len == 0)
		return false;
	*top = (const uint64_t *)t->worklist.data;
	return true;
}

static void
wl_pop(__dtfl_tracker *t, uint64_t *out)
{
	size_t w = wl_words(t), n = t->worklist.len / w, i = 0;
	uint64_t *heap = (uint64_t *)t->worklist.data;

	memcpy(out, heap, w * sizeof(uint64_t));
	n--;
	if (n > 0)
		memcpy(heap, heap + n * w, w * sizeof(uint64_t));
	__dtfl_vec_truncate(&t->worklist, n * w);
	for (;;) {
		size_t l = 2 * i + 1, r = l + 1, m = i;
		uint64_t tmp[DTFL_TIME_MAX + 3];

		if (l < n && wl_cmp(t, heap + l * w, heap + m * w) < 0)
			m = l;
		if (r < n && wl_cmp(t, heap + r * w, heap + m * w) < 0)
			m = r;
		if (m == i)
			break;
		memcpy(tmp, heap + m * w, w * sizeof(uint64_t));
		memcpy(heap + m * w, heap + i * w, w * sizeof(uint64_t));
		memcpy(heap + i * w, tmp, w * sizeof(uint64_t));
		i = m;
	}
}

static void
port_info_init(port_info *p, const dtfl_time_class *tc)
{
	__dtfl_mantichain_init(&p->pointstamps, tc);
	__dtfl_mantichain_init(&p->implications, tc);
	__dtfl_vec_init(&p->out_sum, sizeof(uint64_t));
}

static void
port_info_free(port_info *p)
{
	__dtfl_mantichain_free(&p->pointstamps);
	__dtfl_mantichain_free(&p->implications);
	__dtfl_vec_free(&p->out_sum);
}

/* Appends (output, summaries...) for every summary in an antichain. */
static void
flatten_conn(__dtfl_vec *dst, const __dtfl_port_conn_entry *ent, size_t sw)
{
	size_t ns = ent->summaries.len / sw, k;

	for (k = 0; k < ns; k++) {
		uint64_t port = ent->port;

		__dtfl_vec_push(dst, &port);
		__dtfl_vec_push_n(dst, (const uint64_t *)ent->summaries.data + k * sw, sw);
	}
}

/*
 * Builder::build / Tracker::allocate_from.  Consumes the builder.
 * *scope_summary receives, for each scope input (node 0 output), its
 * summaries to scope outputs.  Prints a warning if a cycle lacks a
 * timestamp increment, as upstream (which continues anyway).
 */
__dtfl_tracker *
__dtfl_reach_builder_build(__dtfl_reach_builder *b, __dtfl_connectivity *scope_summary)
{
	const dtfl_time_class *tc = b->tc;
	__dtfl_tracker *t = __dtfl_calloc(1, sizeof *t);
	reach_node *nodes = (reach_node *)b->nodes.data;
	size_t nn = b->nodes.len, i, p, e, sw;
	__dtfl_vec acc;

	if (!__dtfl_reach_builder_is_acyclic(b))
		fprintf(stderr, "Cycle detected without timestamp increment\n");
	DTFL_ASSERT(nn > 0);
	t->tc = tc;
	t->depth = tc->depth;
	t->sw = sw = DTFL_MAX(tc->sdepth, 1u);
	t->nodes = nn;
	t->ops = __dtfl_calloc(nn, sizeof *t->ops);
	__dtfl_cb_init(&t->target_changes, 2 + tc->depth);
	__dtfl_cb_init(&t->source_changes, 2 + tc->depth);
	__dtfl_cb_init(&t->pushed_changes, 2 + tc->depth);
	__dtfl_cb_init(&t->scratch, tc->depth);
	__dtfl_vec_init(&t->worklist, sizeof(uint64_t));
	t->scope_outputs = nodes[0].inputs;
	t->output_changes = __dtfl_calloc(DTFL_MAX(t->scope_outputs, 1),
	    sizeof(__dtfl_change_batch));
	for (i = 0; i < t->scope_outputs; i++)
		__dtfl_cb_init(&t->output_changes[i], tc->depth);

	for (i = 0; i < nn; i++) {
		per_op *op = &t->ops[i];

		op->inputs = nodes[i].inputs;
		op->outputs = nodes[i].outputs;
		op->targets = __dtfl_calloc(DTFL_MAX(op->inputs, 1), sizeof(port_info));
		op->sources = __dtfl_calloc(DTFL_MAX(op->outputs, 1), sizeof(port_info));
		op->node_sum = __dtfl_calloc(DTFL_MAX(op->inputs, 1), sizeof(__dtfl_vec));
		op->edges = __dtfl_calloc(DTFL_MAX(op->outputs, 1), sizeof(__dtfl_vec));
		for (p = 0; p < op->inputs; p++) {
			port_info_init(&op->targets[p], tc);
			__dtfl_vec_init(&op->node_sum[p], sizeof(uint64_t));
			if (p < nodes[i].summary.len) {
				const __dtfl_port_conn *pc = &nodes[i].summary.ports[p];

				for (e = 0; e < pc->entries.len; e++)
					flatten_conn(&op->node_sum[p],
					    DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, e), sw);
			}
		}
		for (p = 0; p < op->outputs; p++) {
			port_info_init(&op->sources[p], tc);
			__dtfl_vec_init(&op->edges[p], sizeof(__dtfl_location));
			__dtfl_vec_copy(&op->edges[p], &nodes[i].edges[p]);
		}
	}

	/* Summaries from every location to every scope output. */
	__dtfl_connectivity_init(scope_summary, tc, nodes[0].outputs);
	__dtfl_vec_init(&acc, sizeof(acc_entry));
	summarize_outputs(tc, nodes, nn, &acc);
	for (i = 0; i < acc.len; i++) {
		acc_entry *a = DTFL_VEC_AT(&acc, acc_entry, i);
		size_t ns = a->summaries.len / sw, k;

		if (a->loc.node == 0) {
			/* Scope inputs summarize the scope; outputs are ignored. */
			if (a->loc.source)
				for (k = 0; k < ns; k++)
					__dtfl_port_conn_insert(&scope_summary->ports[a->loc.port],
					    a->output, (const uint64_t *)a->summaries.data + k * sw);
		} else {
			port_info *pi = a->loc.source
			    ? &t->ops[a->loc.node].sources[a->loc.port]
			    : &t->ops[a->loc.node].targets[a->loc.port];

			for (k = 0; k < ns; k++) {
				uint64_t port = a->output;

				__dtfl_vec_push(&pi->out_sum, &port);
				__dtfl_vec_push_n(&pi->out_sum,
				    (const uint64_t *)a->summaries.data + k * sw, sw);
			}
		}
		__dtfl_vec_free(&a->summaries);
	}
	__dtfl_vec_free(&acc);
	__dtfl_reach_builder_free(b);
	return t;
}

void
__dtfl_tracker_free(__dtfl_tracker *t)
{
	size_t i, p;

	if (t == NULL)
		return;
	for (i = 0; i < t->nodes; i++) {
		per_op *op = &t->ops[i];

		for (p = 0; p < op->inputs; p++) {
			port_info_free(&op->targets[p]);
			__dtfl_vec_free(&op->node_sum[p]);
		}
		for (p = 0; p < op->outputs; p++) {
			port_info_free(&op->sources[p]);
			__dtfl_vec_free(&op->edges[p]);
		}
		__dtfl_free(op->targets);
		__dtfl_free(op->sources);
		__dtfl_free(op->node_sum);
		__dtfl_free(op->edges);
	}
	__dtfl_free(t->ops);
	for (i = 0; i < t->scope_outputs; i++)
		__dtfl_cb_free(&t->output_changes[i]);
	__dtfl_free(t->output_changes);
	__dtfl_cb_free(&t->target_changes);
	__dtfl_cb_free(&t->source_changes);
	__dtfl_cb_free(&t->pushed_changes);
	__dtfl_cb_free(&t->scratch);
	__dtfl_vec_free(&t->worklist);
	__dtfl_free(t);
}

/* Tracker::update / update_target / update_source: buffered until propagate_all. */
void
__dtfl_tracker_update(__dtfl_tracker *t, __dtfl_location loc, const uint64_t *time,
    int64_t delta)
{
	uint64_t key[2 + DTFL_TIME_MAX];

	__dtfl_location_key(loc, key);
	__dtfl_time_copy(t->tc, key + 2, time);
	__dtfl_cb_update(loc.source ? &t->source_changes : &t->target_changes,
	    key, delta);
}

bool
__dtfl_tracker_tracking_anything(__dtfl_tracker *t)
{
	return !__dtfl_cb_is_empty(&t->source_changes) ||
	    !__dtfl_cb_is_empty(&t->target_changes) || t->total_counts > 0;
}

/* Projects a pointstamp change at a port onto scope outputs. */
static void
project_outputs(__dtfl_tracker *t, const port_info *pi, const uint64_t *time,
    int64_t diff)
{
	size_t step = 1 + t->sw, n = pi->out_sum.len / step, k;
	const uint64_t *w = (const uint64_t *)pi->out_sum.data;
	uint64_t out[DTFL_TIME_MAX];

	for (k = 0; k < n; k++) {
		size_t output = (size_t)w[k * step];

		if (dtfl_summary_results_in(t->tc, w + k * step + 1, time, out))
			__dtfl_cb_update(&t->output_changes[output], out, diff);
	}
}

/*
 * Step 1 of propagate_all for one class of changes (targets or
 * sources): group by location, filter through the location's
 * pointstamps, and seed the worklist with actual frontier changes.
 */
static void
absorb_changes(__dtfl_tracker *t, __dtfl_change_batch *changes)
{
	size_t n = __dtfl_cb_iter(changes), i = 0;

	while (i < n) {
		const uint64_t *k0 = __dtfl_cb_key(changes, i);
		__dtfl_location loc = __dtfl_location_from_key(k0);
		per_op *op = &t->ops[loc.node];
		port_info *pi = loc.source ? &op->sources[loc.port] : &op->targets[loc.port];
		size_t j, m;

		__dtfl_cb_clear(&t->scratch);
		for (j = i; j < n && __dtfl_words_cmp(__dtfl_cb_key(changes, j), k0, 2) == 0; j++)
			__dtfl_cb_update(&t->scratch, __dtfl_cb_key(changes, j) + 2,
			    __dtfl_cb_delta(changes, j));
		i = j;
		__dtfl_mantichain_update_batch(&pi->pointstamps, &t->scratch);
		m = __dtfl_cb_iter(&pi->pointstamps.changes);
		for (j = 0; j < m; j++) {
			const uint64_t *time = __dtfl_cb_key(&pi->pointstamps.changes, j);
			int64_t diff = __dtfl_cb_delta(&pi->pointstamps.changes, j);

			t->total_counts += diff;
			if (loc.source)
				op->cap_counts += diff;
			project_outputs(t, pi, time, diff);
			wl_push(t, time, loc, diff);
		}
		__dtfl_cb_clear(&pi->pointstamps.changes);
	}
	__dtfl_cb_clear(changes);
}

/*
 * Tracker::propagate_all: drains buffered changes, determines actual
 * frontier changes, and circulates their implications in time order.
 */
void
__dtfl_tracker_propagate_all(__dtfl_tracker *t)
{
	size_t w = wl_words(t);
	uint64_t cur[DTFL_TIME_MAX + 3];

	absorb_changes(t, &t->target_changes);
	absorb_changes(t, &t->source_changes);

	while (t->worklist.len > 0) {
		const uint64_t *top;
		int64_t diff;
		__dtfl_location loc;
		per_op *op;
		size_t m, j;

		wl_pop(t, cur);
		diff = (int64_t)cur[t->depth + 2];
		/* Drain and accumulate updates with the same time and location. */
		while (wl_peek(t, &top) && wl_cmp(t, top, cur) == 0) {
			uint64_t again[DTFL_TIME_MAX + 3];

			wl_pop(t, again);
			diff += (int64_t)again[t->depth + 2];
		}
		(void)w;
		if (diff == 0)
			continue;
		loc = __dtfl_location_from_key(cur + t->depth);
		op = &t->ops[loc.node];
		if (!loc.source) {
			/* Operator input: propagate across the operator. */
			__dtfl_mutable_antichain *imp = &op->targets[loc.port].implications;

			__dtfl_mantichain_update(imp, cur, diff);
			m = __dtfl_cb_iter(&imp->changes);
			for (j = 0; j < m; j++) {
				const uint64_t *time = __dtfl_cb_key(&imp->changes, j);
				int64_t d = __dtfl_cb_delta(&imp->changes, j);
				const __dtfl_vec *ns = &op->node_sum[loc.port];
				size_t step = 1 + t->sw, k, nk = ns->len / step;
				uint64_t key[2 + DTFL_TIME_MAX];

				for (k = 0; k < nk; k++) {
					const uint64_t *ws = (const uint64_t *)ns->data + k * step;
					uint64_t nt[DTFL_TIME_MAX];
					__dtfl_location src = { loc.node, (size_t)ws[0], true };

					if (dtfl_summary_results_in(t->tc, ws + 1, time, nt))
						wl_push(t, nt, src, d);
				}
				__dtfl_location_key(loc, key);
				__dtfl_time_copy(t->tc, key + 2, time);
				__dtfl_cb_update(&t->pushed_changes, key, d);
			}
			__dtfl_cb_clear(&imp->changes);
		} else {
			/* Operator output: propagate along outgoing edges. */
			__dtfl_mutable_antichain *imp = &op->sources[loc.port].implications;

			__dtfl_mantichain_update(imp, cur, diff);
			m = __dtfl_cb_iter(&imp->changes);
			for (j = 0; j < m; j++) {
				const uint64_t *time = __dtfl_cb_key(&imp->changes, j);
				int64_t d = __dtfl_cb_delta(&imp->changes, j);
				const __dtfl_vec *edges = &op->edges[loc.port];
				uint64_t key[2 + DTFL_TIME_MAX];
				size_t k;

				for (k = 0; k < edges->len; k++)
					wl_push(t, time, *DTFL_VEC_AT(edges, __dtfl_location, k), d);
				__dtfl_location_key(loc, key);
				__dtfl_time_copy(t->tc, key + 2, time);
				__dtfl_cb_update(&t->pushed_changes, key, d);
			}
			__dtfl_cb_clear(&imp->changes);
		}
	}
}

__dtfl_change_batch *
__dtfl_tracker_pushed(__dtfl_tracker *t)
{
	return &t->pushed_changes;
}

__dtfl_change_batch *
__dtfl_tracker_pushed_output(__dtfl_tracker *t, size_t output)
{
	DTFL_ASSERT(output < t->scope_outputs);
	return &t->output_changes[output];
}

/*
 * PortInformation::is_global: no implication frontier element is
 * strictly less than `time`, and the time's implication count is not
 * more than one.
 */
bool
__dtfl_tracker_is_global(const __dtfl_tracker *t, __dtfl_location loc,
    const uint64_t *time)
{
	const per_op *op = &t->ops[loc.node];
	const __dtfl_mutable_antichain *imp = loc.source
	    ? &op->sources[loc.port].implications
	    : &op->targets[loc.port].implications;
	size_t n = __dtfl_mantichain_frontier_len(imp);
	bool dominated = __dtfl_frontier_less_than(t->tc,
	    __dtfl_mantichain_frontier(imp), n, time);
	bool redundant = __dtfl_mantichain_count_for(imp, time) > 1;

	return !dominated && !redundant;
}

bool
__dtfl_tracker_node_frontiers_empty(const __dtfl_tracker *t, size_t node)
{
	const per_op *op = &t->ops[node];
	size_t p;

	for (p = 0; p < op->inputs; p++)
		if (!__dtfl_mantichain_is_empty(&op->targets[p].implications))
			return false;
	return true;
}

bool
__dtfl_tracker_node_no_pointstamps(const __dtfl_tracker *t, size_t node)
{
	const per_op *op = &t->ops[node];
	size_t p;

	for (p = 0; p < op->outputs; p++)
		if (!__dtfl_mantichain_is_empty(&op->sources[p].pointstamps))
			return false;
	return true;
}

int64_t
__dtfl_tracker_node_cap_counts(const __dtfl_tracker *t, size_t node)
{
	return t->ops[node].cap_counts;
}

const __dtfl_mutable_antichain *
__dtfl_tracker_target_implications(const __dtfl_tracker *t, size_t node,
    size_t port)
{
	return &t->ops[node].targets[port].implications;
}

const __dtfl_mutable_antichain *
__dtfl_tracker_source_implications(const __dtfl_tracker *t, size_t node,
    size_t port)
{
	return &t->ops[node].sources[port].implications;
}
