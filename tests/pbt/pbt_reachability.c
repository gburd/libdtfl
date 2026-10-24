/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_reachability.c: the tracker against a naive model.
 *
 * For random graphs (random nodes, edges and u64 summaries, with every
 * cycle advancing time) and random pointstamp updates, after each
 * propagate_all the tracker's implications at every target must equal
 * the model's: the minimal elements of { results_in(path, t) : a
 * positive pointstamp at time t at some location reaches the target
 * along a path with summary `path` }.  The model computes reachability
 * from scratch by a Bellman-Ford style closure over (location, time)
 * pairs with times bounded by a horizon.  This is PLAN section 6 tier 4,
 * "reachability against the naive tracker (D10)".
 *
 * Times and summaries are u64, so implications at a target are the
 * single minimum reached time; the model therefore tracks the minimum
 * over all paths, per pointstamp, and takes the minimum over positive
 * pointstamps.  Accumulated pushed changes must also equal the
 * difference between successive implication frontiers.
 */
#include "pbt.h"
#include "progress/operate_int.h"

#define MAX_NODES 6
#define MAX_PORTS 2
#define HORIZON 50
/* Reached times stay below HORIZON + 3 * MAX_NODES * MAX_PORTS * 3. */
#define TMAX (HORIZON + 64)

typedef struct edge {
	size_t sn, sp, tn, tp;
} edge;

typedef struct model {
	size_t nodes;
	size_t ins[MAX_NODES], outs[MAX_NODES];
	/* internal summary input i -> output o, or UINT64_MAX for none */
	uint64_t sum[MAX_NODES][MAX_PORTS][MAX_PORTS];
	edge edges[64];
	size_t nedges;
	/* accumulated pointstamp counts at sources and targets */
	int64_t src[MAX_NODES][MAX_PORTS][8];
	int64_t tgt[MAX_NODES][MAX_PORTS][8];
	uint64_t times[8];	/* the time values pointstamps use */
} model;

/*
 * Minimum time reaching each target from positive pointstamps, by
 * relaxation.  dist_t[n][p] for targets, dist_s for sources.
 */
static void
model_frontier(const model *m, uint64_t dist_t[MAX_NODES][MAX_PORTS])
{
	uint64_t dist_s[MAX_NODES][MAX_PORTS];
	size_t n, p, q, k, round;
	bool changed = true;

	for (n = 0; n < MAX_NODES; n++)
		for (p = 0; p < MAX_PORTS; p++)
			dist_t[n][p] = dist_s[n][p] = UINT64_MAX;
	for (n = 0; n < m->nodes; n++)
		for (k = 0; k < 8; k++) {
			for (p = 0; p < m->outs[n]; p++)
				if (m->src[n][p][k] > 0 && m->times[k] < dist_s[n][p])
					dist_s[n][p] = m->times[k];
			for (p = 0; p < m->ins[n]; p++)
				if (m->tgt[n][p][k] > 0 && m->times[k] < dist_t[n][p])
					dist_t[n][p] = m->times[k];
		}
	for (round = 0; changed && round < 1000; round++) {
		changed = false;
		for (k = 0; k < m->nedges; k++) {
			const edge *e = &m->edges[k];
			if (dist_s[e->sn][e->sp] < dist_t[e->tn][e->tp]) {
				dist_t[e->tn][e->tp] = dist_s[e->sn][e->sp];
				changed = true;
			}
		}
		for (n = 0; n < m->nodes; n++)
			for (p = 0; p < m->ins[n]; p++)
				for (q = 0; q < m->outs[n]; q++) {
					uint64_t s = m->sum[n][p][q], d = dist_t[n][p];
					if (s == UINT64_MAX || d == UINT64_MAX || d + s > TMAX)
						continue;
					if (d + s < dist_s[n][q]) {
						dist_s[n][q] = d + s;
						changed = true;
					}
				}
	}
}

static void
prop_tracker_matches_model(pbt_tc *tc)
{
	dtfl_time_class *u64 = dtfl_time_u64();
	__dtfl_reach_builder *b = __dtfl_reach_builder_new(u64);
	__dtfl_connectivity scope;
	__dtfl_tracker *t;
	model m;
	size_t n, p, q, k, step, steps;
	static int64_t acc[MAX_NODES][MAX_PORTS][TMAX + 1];

	memset(&m, 0, sizeof m);
	memset(acc, 0, sizeof acc);
	m.nodes = (size_t)pbt_int(tc, 1, MAX_NODES);
	for (k = 0; k < 8; k++)
		m.times[k] = (uint64_t)pbt_int(tc, 0, HORIZON);
	for (n = 0; n < m.nodes; n++) {
		__dtfl_connectivity c;

		m.ins[n] = (size_t)pbt_int(tc, 0, MAX_PORTS);
		m.outs[n] = (size_t)pbt_int(tc, 0, MAX_PORTS);
		__dtfl_connectivity_init(&c, u64, m.ins[n]);
		for (p = 0; p < MAX_PORTS; p++)
			for (q = 0; q < MAX_PORTS; q++)
				m.sum[n][p][q] = UINT64_MAX;
		for (p = 0; p < m.ins[n]; p++)
			for (q = 0; q < m.outs[n]; q++) {
				/*
				 * Summaries of at least 1 keep every cycle advancing
				 * time, so the model's relaxation terminates and the
				 * tracker's worklist is finite.
				 */
				if (pbt_bool(tc)) {
					uint64_t s = (uint64_t)pbt_int(tc, 1, 3);
					m.sum[n][p][q] = s;
					__dtfl_port_conn_insert(&c.ports[p], q, &s);
				}
			}
		__dtfl_reach_builder_add_node(b, n, m.ins[n], m.outs[n], &c);
	}
	m.nedges = (size_t)pbt_int(tc, 0, 12);
	for (k = 0; k < m.nedges; k++) {
		edge e;
		__dtfl_location s, d;

		e.sn = (size_t)pbt_int(tc, 0, (int64_t)m.nodes - 1);
		e.tn = (size_t)pbt_int(tc, 0, (int64_t)m.nodes - 1);
		if (m.outs[e.sn] == 0 || m.ins[e.tn] == 0) {
			m.nedges = k;
			break;
		}
		e.sp = (size_t)pbt_int(tc, 0, (int64_t)m.outs[e.sn] - 1);
		e.tp = (size_t)pbt_int(tc, 0, (int64_t)m.ins[e.tn] - 1);
		m.edges[k] = e;
		s.node = e.sn; s.port = e.sp; s.source = true;
		d.node = e.tn; d.port = e.tp; d.source = false;
		__dtfl_reach_builder_add_edge(b, s, d);
	}
	t = __dtfl_reach_builder_build(b, &scope);

	steps = (size_t)pbt_int(tc, 1, 20);
	for (step = 0; step < steps; step++) {
		size_t nu = (size_t)pbt_int(tc, 1, 4), u;
		uint64_t dist[MAX_NODES][MAX_PORTS];
		__dtfl_change_batch *pushed;
		size_t np;

		for (u = 0; u < nu; u++) {
			size_t node = (size_t)pbt_int(tc, 0, (int64_t)m.nodes - 1);
			bool src = pbt_bool(tc);
			size_t ports = src ? m.outs[node] : m.ins[node];
			size_t tk = (size_t)pbt_int(tc, 0, 7);
			int64_t *cnt;
			int64_t delta;
			__dtfl_location l;

			if (ports == 0)
				continue;
			p = (size_t)pbt_int(tc, 0, (int64_t)ports - 1);
			cnt = src ? &m.src[node][p][tk] : &m.tgt[node][p][tk];
			/* Keep counts non-negative, as real progress does. */
			delta = (*cnt > 0 && pbt_bool(tc)) ? -1 : 1;
			*cnt += delta;
			l.node = node; l.port = p; l.source = src;
			__dtfl_tracker_update(t, l, &m.times[tk], delta);
		}
		__dtfl_tracker_propagate_all(t);

		/* Accumulate pushed target implication changes. */
		pushed = __dtfl_tracker_pushed(t);
		np = __dtfl_cb_iter(pushed);
		for (k = 0; k < np; k++) {
			const uint64_t *key = __dtfl_cb_key(pushed, k);
			__dtfl_location l = __dtfl_location_from_key(key);

			if (l.source)
				continue;
			PBT_ASSERT(key[2] <= TMAX);
			acc[l.node][l.port][key[2]] += __dtfl_cb_delta(pushed, k);
		}
		__dtfl_cb_clear(pushed);

		model_frontier(&m, dist);
		for (n = 0; n < m.nodes; n++)
			for (p = 0; p < m.ins[n]; p++) {
				const __dtfl_mutable_antichain *imp =
				    __dtfl_tracker_target_implications(t, n, p);
				size_t fl = __dtfl_mantichain_frontier_len(imp), x;

				if (dist[n][p] == UINT64_MAX) {
					PBT_ASSERT(fl == 0);
				} else {
					PBT_ASSERT(fl == 1);
					PBT_ASSERT(__dtfl_mantichain_frontier(imp)[0] == dist[n][p]);
				}
				/* Pushed changes accumulate to the frontier. */
				for (x = 0; x <= TMAX; x++) {
					int64_t want = (dist[n][p] == x) ? 1 : 0;
					PBT_ASSERT(acc[n][p][x] == want);
				}
			}
	}
	__dtfl_connectivity_free(&scope);
	__dtfl_tracker_free(t);
	dtfl_time_class_free(u64);
}

static const pbt_prop props[] = {
	{ "tracker_matches_model", prop_tracker_matches_model, 300 },
};

PBT_MAIN("reachability", props)
