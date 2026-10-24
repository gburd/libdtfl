/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/int_proxy/reduce.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/int_proxy_reduce.c: the proxy reduce framework.  A conventional
 * differential reduce against (u64, u64), where the backend supplies the
 * implementation of the interpretation of the integers.
 *
 * ProxyReduceTactic::retire and KeyState are ported literally.  Upstream's
 * BTreeMap<u64, Vec<T>> of pending times is a vector of (hash, time) rows
 * kept sorted by hash (the BTreeMap iteration order), and its BTreeSet of
 * changed keys a sorted, deduplicated uint64_t vector.
 *
 * __dtfl_proxy_reduce_u64_backend is not upstream (upstream ships the
 * ProxyReduceBackend trait and no implementation).  It presents a value
 * arrangement whose keys and values are 8-byte order-preserving encodings:
 * key_hash is the key's u64 reading and value_id the value's, so there are
 * no collisions and ids order as values.  One output batch per tile is built
 * from the emitted records.
 */
#include "dd/int_proxy_int.h"
#include "dd/trace/trace_int.h"
#include "dtfl/record.h"
#include "progress/time_int.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* KeyState                                                            */
/* ------------------------------------------------------------------ */

/*
 * Per-key application state for the tactic's round-batched walk: the key's
 * ordered interesting moments and their suffix meets, its input and output
 * replays (meet-collapsed), the corrections produced this round so far, and
 * a cursor into moments.  Held for all of a window's keys at once so each
 * round's crossing batches across keys; a key's own moments stay sequential
 * (each sees its earlier corrections via produced), but distinct keys are
 * independent.
 */
typedef struct key_state {
	uint64_t key;
	__dtfl_vec moments;		/* __dtfl_time_val */
	__dtfl_vec meets;		/* __dtfl_time_val */
	__dtfl_value_history in_replay;
	__dtfl_value_history out_replay;
	__dtfl_vec produced;		/* __dtfl_bridge_upd ((vid, t), d) */
	size_t cursor;
} key_state;

/*
 * An empty slot, filled by the tactic's phase 1.  The states vector holds
 * these across windows and reloads them in place, so a key's buffers are
 * allocated once per slot and reused.
 */
static void
key_state_init(key_state *st, const dtfl_time_class *tc,
    const __dtfl_diff_class *rin, const __dtfl_diff_class *rout)
{
	st->key = 0;
	__dtfl_vec_init(&st->moments, sizeof(__dtfl_time_val));
	__dtfl_vec_init(&st->meets, sizeof(__dtfl_time_val));
	__dtfl_value_history_init(&st->in_replay, tc, rin);
	__dtfl_value_history_init(&st->out_replay, tc, rout);
	__dtfl_vec_init(&st->produced, sizeof(__dtfl_bridge_upd));
	st->cursor = 0;
}

static void
key_state_free(key_state *st)
{
	__dtfl_vec_free(&st->moments);
	__dtfl_vec_free(&st->meets);
	__dtfl_value_history_free(&st->in_replay);
	__dtfl_value_history_free(&st->out_replay);
	__dtfl_vec_free(&st->produced);
}

/* ------------------------------------------------------------------ */
/* ProxyReduceTactic                                                   */
/* ------------------------------------------------------------------ */

/* A pending interesting time beyond the upper frontier, by key hash. */
typedef struct pend_row {
	uint64_t hash;
	__dtfl_time_val time;
} pend_row;

struct __dtfl_proxy_reduce_tactic {
	__dtfl_proxy_reduce_backend backend;
	/* pending: (hash, time) rows grouped by ascending hash (BTreeMap). */
	__dtfl_vec pending;
	const dtfl_time_class *tc;
	unsigned tw;
};

__dtfl_proxy_reduce_tactic *
__dtfl_proxy_reduce_tactic_new(__dtfl_proxy_reduce_backend backend,
    const dtfl_time_class *tc)
{
	__dtfl_proxy_reduce_tactic *t;

	/* shortcut: bridge rows hold diffs inline, DTFL_COMMON_DIFF_MAX bytes. */
	DTFL_ASSERT(backend.rin->width <= DTFL_COMMON_DIFF_MAX &&
	    backend.rout->width <= DTFL_COMMON_DIFF_MAX);
	t = __dtfl_alloc(sizeof *t);
	t->backend = backend;
	__dtfl_vec_init(&t->pending, sizeof(pend_row));
	t->tc = tc;
	t->tw = dtfl_time_depth(tc);
	return t;
}

void
__dtfl_proxy_reduce_tactic_free(void *vt)
{
	__dtfl_proxy_reduce_tactic *t = vt;

	if (t == NULL)
		return;
	if (t->backend.ops->free != NULL)
		t->backend.ops->free(t->backend.self);
	__dtfl_vec_free(&t->pending);
	__dtfl_free(t);
}

static __dtfl_bridge_upd *
row_at(const __dtfl_vec *v, size_t i)
{
	return &((__dtfl_bridge_upd *)(void *)v->data)[i];
}

static __dtfl_time_val *
tval_at(const __dtfl_vec *v, size_t i)
{
	return &((__dtfl_time_val *)(void *)v->data)[i];
}

static int
u64_cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/* Sorted, deduplicated union of the seed hashes and the pending hashes. */
static void
changed_keys(const __dtfl_vec *seeds, const __dtfl_vec *pending,
    __dtfl_vec *changed)
{
	size_t i, w = 0;
	uint64_t *k;

	for (i = 0; i < seeds->len; i++)
		__dtfl_vec_push(changed, &row_at(seeds, i)->hash);
	for (i = 0; i < pending->len; i++)
		__dtfl_vec_push(changed,
		    &((const pend_row *)(const void *)pending->data)[i].hash);
	if (changed->len == 0)
		return;
	k = (uint64_t *)(void *)changed->data;
	/* DTFL_RAW_OK: qsort on a local u64 array, no allocation. */
	qsort(k, changed->len, sizeof *k, u64_cmp);
	for (i = 0; i < changed->len; i++)
		if (w == 0 || k[w - 1] != k[i])
			k[w++] = k[i];
	changed->len = w;
}

/* Append ((key, vid), t, d) rows of a (vid, d) list whose time <= t. */
static void
accumulate_le(const dtfl_time_class *tc, const __dtfl_value_history *h,
    const __dtfl_vec *extra, const uint64_t *t, __dtfl_vec *out)
{
	size_t i;

	for (i = 0; i < __dtfl_value_history_buffer_len(h); i++) {
		size_t vl;
		const uint64_t *et;
		const void *d;
		const void *v = __dtfl_value_history_buffer_at(h, i, &vl, &et,
		    &d);
		__dtfl_bridge_upd r;

		if (!dtfl_time_less_equal(tc, et, t))
			continue;
		memset(&r, 0, sizeof r);
		r.id = dtfl_get_u64(v);
		if (h->dw != 0)
			memcpy(r.diff, d, h->dw);
		__dtfl_vec_push(out, &r);
	}
	for (i = 0; extra != NULL && i < extra->len; i++) {
		const __dtfl_bridge_upd *p = row_at(extra, i);
		__dtfl_bridge_upd r;

		if (!dtfl_time_less_equal(tc, p->time, t))
			continue;
		memset(&r, 0, sizeof r);
		r.id = p->id;
		memcpy(r.diff, p->diff, sizeof r.diff);
		__dtfl_vec_push(out, &r);
	}
}

size_t
__dtfl_proxy_reduce_retire(__dtfl_proxy_reduce_tactic *self,
    const __dtfl_reduce_instance *instance, const __dtfl_antichain *upper,
    const __dtfl_antichain *held, __dtfl_batch_ord **batches,
    size_t *held_index, __dtfl_antichain *new_frontier)
{
	const __dtfl_proxy_reduce_backend_ops *ops = self->backend.ops;
	void *backend = self->backend.self;
	const __dtfl_diff_class *rin = self->backend.rin;
	const __dtfl_diff_class *rout = self->backend.rout;
	const dtfl_time_class *tc = self->tc;
	unsigned tw = self->tw;
	const uint64_t *up = __dtfl_ac_base(upper);
	size_t nup = __dtfl_antichain_len(upper);
	size_t nheld = __dtfl_antichain_len(held), i, k;
	__dtfl_vec seeds, changed, new_pending;
	__dtfl_description *tile_descs;
	size_t *tile_held, *tile_of, ntiles, cursor = 0, ns = 0, nstates_cap;
	__dtfl_discover_scratch discover_scratch;
	key_state *states = NULL;
	__dtfl_vec *tile_deltas;
	__dtfl_vec batch_keys, in_ends, in_all, out_ends, out_all, active;
	__dtfl_vec in_accum, cur_out, moments_scratch, pended_scratch;
	__dtfl_vec corr, corr_ends;
	__dtfl_reduce_window window;
	bool any;

	__dtfl_antichain_clear(new_frontier);
	any = false;
	for (i = 0; i < nheld; i++)
		if (!__dtfl_frontier_less_equal(tc, up, nup,
		    __dtfl_antichain_at(held, i)))
			any = true;
	if (!any) {
		for (i = 0; i < nheld; i++)
			__dtfl_antichain_insert(new_frontier,
			    __dtfl_antichain_at(held, i));
		return 0;
	}

	__dtfl_vec_init(&seeds, sizeof(__dtfl_bridge_upd));
	ops->seed_times(backend, instance, &seeds);
	for (i = 1; i < seeds.len; i++)
		DTFL_DEBUG_ASSERT(row_at(&seeds, i - 1)->hash <=
		    row_at(&seeds, i)->hash);
	__dtfl_vec_init(&changed, sizeof(uint64_t));
	changed_keys(&seeds, &self->pending, &changed);
	if (changed.len == 0) {
		__dtfl_vec_clear(&self->pending);
		__dtfl_vec_free(&changed);
		__dtfl_vec_free(&seeds);
		return 0;
	}

	/*
	 * The output tiling (identical to the Abelian tactic): one tile per
	 * held time, keeping non-degenerate intervals; tile_of[i] maps held
	 * time i to its tile.
	 */
	tile_descs = __dtfl_alloc(DTFL_MAX(nheld, 1u) * sizeof *tile_descs);
	tile_held = __dtfl_alloc(DTFL_MAX(nheld, 1u) * sizeof *tile_held);
	tile_of = __dtfl_alloc(DTFL_MAX(nheld, 1u) * sizeof *tile_of);
	ntiles = __dtfl_tile_descriptions(instance->lower, upper, held,
	    tile_descs, tile_held, tile_of);
	ops->begin(backend, tile_descs, ntiles);

	__dtfl_vec_init(&new_pending, sizeof(pend_row));

	/*
	 * Retire-wide reusable scratch (cleared per window/round/moment, not
	 * reallocated).
	 */
	__dtfl_discover_scratch_init(&discover_scratch, tc, rin);
	nstates_cap = 0;
	tile_deltas = __dtfl_alloc(DTFL_MAX(nheld, 1u) * sizeof *tile_deltas);
	for (i = 0; i < nheld; i++)
		__dtfl_vec_init(&tile_deltas[i], sizeof(__dtfl_bridge_upd));
	__dtfl_vec_init(&batch_keys, sizeof(uint64_t));
	__dtfl_vec_init(&in_ends, sizeof(size_t));
	__dtfl_vec_init(&in_all, sizeof(__dtfl_bridge_upd));
	__dtfl_vec_init(&out_ends, sizeof(size_t));
	__dtfl_vec_init(&out_all, sizeof(__dtfl_bridge_upd));
	__dtfl_vec_init(&active, sizeof(__dtfl_bridge_upd)); /* (si, t) */
	__dtfl_vec_init(&in_accum, sizeof(__dtfl_bridge_upd));
	__dtfl_vec_init(&cur_out, sizeof(__dtfl_bridge_upd));
	__dtfl_vec_init(&moments_scratch, sizeof(__dtfl_time_val));
	__dtfl_vec_init(&pended_scratch, sizeof(__dtfl_time_val));
	__dtfl_vec_init(&corr, sizeof(__dtfl_bridge_upd));
	__dtfl_vec_init(&corr_ends, sizeof(size_t));
	__dtfl_vec_init(&window.keys, sizeof(uint64_t));
	__dtfl_vec_init(&window.input, sizeof(__dtfl_bridge_upd));
	__dtfl_vec_init(&window.output, sizeof(__dtfl_bridge_upd));

	while (ops->next_window(backend, instance,
	    (const uint64_t *)(void *)changed.data, changed.len, &cursor,
	    &window)) {
		const __dtfl_vec *p_in = &window.input;
		const __dtfl_vec *p_out = &window.output;
		size_t n_states = 0, is = 0, os = 0, wk;

		__dtfl_bridge_assert_sorted(p_in, tw, "next_window.input");
		__dtfl_bridge_assert_sorted(p_out, tw, "next_window.output");

		for (i = 0; i < nheld; i++)
			__dtfl_vec_clear(&tile_deltas[i]);

		/*
		 * Phase 1 (determination): for every key in the window,
		 * discover its interesting times (times only, no accumulation)
		 * and stand up its per-moment replays.  Peak state is
		 * O(window presentation).  states is a long-lived buffer
		 * reloaded slot by slot; n_states is the live prefix.
		 */
		for (wk = 0; wk < window.keys.len; wk++) {
			uint64_t key = ((const uint64_t *)(const void *)
			    window.keys.data)[wk];
			size_t i0, i1, o0, o1, n0, n1, pp0, pp1;
			__dtfl_key_view kv;
			key_state *st;

			while (is < p_in->len && row_at(p_in, is)->hash < key)
				is++;
			i0 = is;
			while (is < p_in->len && row_at(p_in, is)->hash == key)
				is++;
			i1 = is;
			while (os < p_out->len &&
			    row_at(p_out, os)->hash < key)
				os++;
			o0 = os;
			while (os < p_out->len &&
			    row_at(p_out, os)->hash == key)
				os++;
			o1 = os;
			while (ns < seeds.len && row_at(&seeds, ns)->hash < key)
				ns++;
			n0 = ns;
			while (ns < seeds.len &&
			    row_at(&seeds, ns)->hash == key)
				ns++;
			n1 = ns;

			__dtfl_vec_clear(&moments_scratch);
			__dtfl_vec_clear(&pended_scratch);
			{
				/* self.pending.get(&key): the key's run. */
				const pend_row *pr = (const pend_row *)
				    (const void *)self->pending.data;
				__dtfl_vec ptimes;

				for (pp0 = 0; pp0 < self->pending.len &&
				    pr[pp0].hash < key; pp0++)
					;
				for (pp1 = pp0; pp1 < self->pending.len &&
				    pr[pp1].hash == key; pp1++)
					;
				__dtfl_vec_init(&ptimes,
				    sizeof(__dtfl_time_val));
				for (k = pp0; k < pp1; k++)
					__dtfl_vec_push(&ptimes, &pr[k].time);
				kv.p_in = p_in->len != 0 ? row_at(p_in, 0) :
				    NULL;
				kv.i0 = i0;
				kv.i1 = i1;
				kv.pending = (const __dtfl_time_val *)
				    (const void *)ptimes.data;
				kv.npending = ptimes.len;
				__dtfl_discover_times(&kv,
				    n1 > n0 ? row_at(&seeds, n0)->time : NULL,
				    n1 - n0, sizeof(__dtfl_bridge_upd),
				    o1 > o0 ? row_at(p_out, o0)->time : NULL,
				    o1 - o0, sizeof(__dtfl_bridge_upd), up, nup,
				    &discover_scratch, &moments_scratch,
				    &pended_scratch);
				__dtfl_vec_free(&ptimes);
			}
			for (k = 0; k < pended_scratch.len; k++) {
				pend_row r;

				r.hash = key;
				r.time = *tval_at(&pended_scratch, k);
				__dtfl_vec_push(&new_pending, &r);
			}
			if (moments_scratch.len == 0)
				continue;

			/*
			 * Reload slot n_states in place (grow the buffer by one
			 * only when a window is wider than any before).
			 */
			if (n_states == nstates_cap) {
				states = __dtfl_realloc(states,
				    (nstates_cap + 1) * sizeof *states);
				key_state_init(&states[nstates_cap], tc, rin,
				    rout);
				nstates_cap++;
			}
			st = &states[n_states];
			st->key = key;
			st->cursor = 0;
			__dtfl_vec_clear(&st->produced);
			__dtfl_vec_clear(&st->moments);
			__dtfl_vec_swap(&st->moments, &moments_scratch);
			__dtfl_vec_clear(&st->meets);
			for (k = 0; k < st->moments.len; k++)
				__dtfl_vec_push(&st->meets,
				    tval_at(&st->moments, k));
			for (k = st->meets.len; k-- > 1;)
				dtfl_time_meet(tc, tval_at(&st->meets, k - 1)->t,
				    tval_at(&st->meets, k)->t,
				    tval_at(&st->meets, k - 1)->t);
			__dtfl_value_history_load_iter(&st->in_replay,
			    i1 > i0 ? row_at(p_in, i0) : NULL, i1 - i0,
			    tval_at(&st->meets, 0)->t);
			__dtfl_value_history_load_iter(&st->out_replay,
			    o1 > o0 ? row_at(p_out, o0) : NULL, o1 - o0,
			    tval_at(&st->meets, 0)->t);
			n_states++;
		}

		/*
		 * Phase 2 (application): walk all keys' moments in ROUNDS.
		 * Each round assembles every active key's one-moment-deep
		 * input and current-output accumulations and crosses them in a
		 * SINGLE reduce_corrections, batching across keys (a key's own
		 * moments stay sequential, each seeing its earlier corrections
		 * via produced).  produced is meet-collapsed each round,
		 * exactly like the reference.
		 */
		for (;;) {
			bool advanced = false;
			size_t si, cstart, bi;

			__dtfl_vec_clear(&batch_keys);
			__dtfl_vec_clear(&in_ends);
			__dtfl_vec_clear(&in_all);
			__dtfl_vec_clear(&out_ends);
			__dtfl_vec_clear(&out_all);
			__dtfl_vec_clear(&active);
			for (si = 0; si < n_states; si++) {
				key_state *st = &states[si];
				const uint64_t *t, *m;
				__dtfl_bridge_upd act;
				size_t j;

				if (st->cursor >= st->moments.len)
					continue;
				advanced = true;
				j = st->cursor;
				st->cursor++;
				t = tval_at(&st->moments, j)->t;
				m = tval_at(&st->meets, j)->t;
				__dtfl_value_history_step_through(&st->in_replay,
				    t);
				__dtfl_value_history_step_through(
				    &st->out_replay, t);
				__dtfl_value_history_advance_buffer_by(
				    &st->in_replay, m);
				__dtfl_value_history_advance_buffer_by(
				    &st->out_replay, m);
				for (k = 0; k < st->produced.len; k++)
					dtfl_time_join(tc,
					    row_at(&st->produced, k)->time, m,
					    row_at(&st->produced, k)->time);
				__dtfl_bridge_consolidate(&st->produced, tw,
				    rout);

				__dtfl_vec_clear(&in_accum);
				accumulate_le(tc, &st->in_replay, NULL, t,
				    &in_accum);
				__dtfl_bridge_consolidate(&in_accum, tw, rin);
				__dtfl_vec_clear(&cur_out);
				accumulate_le(tc, &st->out_replay, &st->produced,
				    t, &cur_out);
				__dtfl_bridge_consolidate(&cur_out, tw, rout);

				if (in_accum.len == 0 && cur_out.len == 0)
					continue;
				__dtfl_vec_push(&batch_keys, &st->key);
				for (k = 0; k < in_accum.len; k++)
					__dtfl_vec_push(&in_all,
					    row_at(&in_accum, k));
				__dtfl_vec_push(&in_ends, &in_all.len);
				for (k = 0; k < cur_out.len; k++)
					__dtfl_vec_push(&out_all,
					    row_at(&cur_out, k));
				__dtfl_vec_push(&out_ends, &out_all.len);
				memset(&act, 0, sizeof act);
				act.id = si;
				if (tw != 0)
					memcpy(act.time, t,
					    (size_t)tw * sizeof(uint64_t));
				__dtfl_vec_push(&active, &act);
			}
			/*
			 * Terminate only when every key is EXHAUSTED, not merely
			 * when this round produced no crossing: a round can be
			 * empty because every key's current moment is
			 * empty-gated while keys still have later moments.
			 */
			if (!advanced)
				break;
			if (batch_keys.len == 0)
				continue;

			__dtfl_vec_clear(&corr);
			__dtfl_vec_clear(&corr_ends);
			ops->reduce_corrections(backend,
			    (const uint64_t *)(void *)batch_keys.data,
			    batch_keys.len,
			    (const size_t *)(void *)in_ends.data,
			    in_all.len != 0 ? row_at(&in_all, 0) : NULL,
			    (const size_t *)(void *)out_ends.data,
			    out_all.len != 0 ? row_at(&out_all, 0) : NULL,
			    &corr, &corr_ends);
			DTFL_ASSERT(corr_ends.len == active.len);
			cstart = 0;
			for (bi = 0; bi < active.len; bi++) {
				const __dtfl_bridge_upd *a = row_at(&active, bi);
				size_t cend = ((const size_t *)(const void *)
				    corr_ends.data)[bi];
				key_state *st = &states[a->id];

				if (cstart != cend) {
					size_t idx = SIZE_MAX, h;

					for (h = nheld; h-- > 0;)
						if (dtfl_time_less_equal(tc,
						    __dtfl_antichain_at(held, h),
						    a->time)) {
							idx = h;
							break;
						}
					if (idx == SIZE_MAX)
						__dtfl_panic("no held capability "
						    "<= active time");
					for (k = cstart; k < cend; k++) {
						__dtfl_bridge_upd p =
						    *row_at(&corr, k);

						p.hash = 0;
						memcpy(p.time, a->time,
						    sizeof p.time);
						__dtfl_vec_push(&st->produced,
						    &p);
						p.hash = st->key;
						__dtfl_vec_push(&tile_deltas[idx],
						    &p);
					}
				}
				cstart = cend;
			}
		}

		for (i = 0; i < nheld; i++) {
			if (tile_deltas[i].len == 0)
				continue;
			if (tile_of[i] != SIZE_MAX) {
				__dtfl_bridge_consolidate(&tile_deltas[i], tw,
				    rout);
				ops->emit(backend, tile_of[i],
				    tile_deltas[i].len != 0 ?
				    row_at(&tile_deltas[i], 0) : NULL,
				    tile_deltas[i].len);
			}
		}
	}

	/* self.pending = new_pending (ascending hash: windows ascend). */
	__dtfl_vec_swap(&self->pending, &new_pending);
	ops->finish(backend, batches);
	for (k = 0; k < ntiles; k++)
		held_index[k] = tile_held[k];
	for (i = 0; i < self->pending.len; i++)
		__dtfl_antichain_insert(new_frontier,
		    ((const pend_row *)(const void *)self->pending.data)[i].time.t);

	for (k = 0; k < ntiles; k++)
		__dtfl_description_free(&tile_descs[k]);
	__dtfl_free(tile_descs);
	__dtfl_free(tile_held);
	__dtfl_free(tile_of);
	for (i = 0; i < nstates_cap; i++)
		key_state_free(&states[i]);
	if (states != NULL)
		__dtfl_free(states);
	for (i = 0; i < nheld; i++)
		__dtfl_vec_free(&tile_deltas[i]);
	__dtfl_free(tile_deltas);
	__dtfl_discover_scratch_free(&discover_scratch);
	__dtfl_vec_free(&batch_keys);
	__dtfl_vec_free(&in_ends);
	__dtfl_vec_free(&in_all);
	__dtfl_vec_free(&out_ends);
	__dtfl_vec_free(&out_all);
	__dtfl_vec_free(&active);
	__dtfl_vec_free(&in_accum);
	__dtfl_vec_free(&cur_out);
	__dtfl_vec_free(&moments_scratch);
	__dtfl_vec_free(&pended_scratch);
	__dtfl_vec_free(&corr);
	__dtfl_vec_free(&corr_ends);
	__dtfl_vec_free(&window.keys);
	__dtfl_vec_free(&window.input);
	__dtfl_vec_free(&window.output);
	__dtfl_vec_free(&new_pending);
	__dtfl_vec_free(&changed);
	__dtfl_vec_free(&seeds);
	return ntiles;
}

/* ------------------------------------------------------------------ */
/* The u64 backend (not upstream; see the file comment).               */
/* ------------------------------------------------------------------ */

/* Keys per window: small, so a retire crosses several windows. */
#define U64_WINDOW_KEYS 16

typedef struct u64_backend {
	const dtfl_type *kt;		/* not retained: the arrangement's */
	const dtfl_type *vt;
	const dtfl_type *ovt;
	const dtfl_time_class *tc;
	const __dtfl_diff_class *rin;
	const __dtfl_diff_class *rout;
	unsigned tw;
	__dtfl_reduce_logic_fn logic;
	void *ctx;
	void (*ctx_free)(void *);
	/* The session between begin and finish. */
	__dtfl_description *tiles;	/* owned copies */
	__dtfl_vec *records;		/* per tile, __dtfl_bridge_upd */
	size_t ntiles;
} u64_backend;

static void
u64_seed_times(void *self, const __dtfl_reduce_instance *inst,
    __dtfl_vec *seeds)
{
	u64_backend *b = self;

	__dtfl_proxy_present_u64(inst->input_batches, inst->ninput, b->rin,
	    b->tw, NULL, 0, NULL, seeds);
	__dtfl_bridge_sort(seeds, b->tw);
}

static void
u64_begin(void *self, const __dtfl_description *tiles, size_t ntiles)
{
	u64_backend *b = self;
	size_t i;

	b->ntiles = ntiles;
	b->tiles = __dtfl_alloc(DTFL_MAX(ntiles, 1u) * sizeof *b->tiles);
	b->records = __dtfl_alloc(DTFL_MAX(ntiles, 1u) * sizeof *b->records);
	for (i = 0; i < ntiles; i++) {
		__dtfl_antichain lo, hi, since;

		__dtfl_antichain_init(&lo, b->tc);
		__dtfl_antichain_copy(&lo, __dtfl_description_lower(&tiles[i]));
		__dtfl_antichain_init(&hi, b->tc);
		__dtfl_antichain_copy(&hi, __dtfl_description_upper(&tiles[i]));
		__dtfl_antichain_init(&since, b->tc);
		__dtfl_antichain_copy(&since,
		    __dtfl_description_since(&tiles[i]));
		__dtfl_description_init(&b->tiles[i], &lo, &hi, &since);
		__dtfl_vec_init(&b->records[i], sizeof(__dtfl_bridge_upd));
	}
}

static bool
u64_next_window(void *self, const __dtfl_reduce_instance *inst,
    const uint64_t *changed, size_t nchanged, size_t *cursor,
    __dtfl_reduce_window *w)
{
	u64_backend *b = self;
	size_t n;

	if (*cursor >= nchanged)
		return false;
	n = DTFL_MIN(nchanged - *cursor, (size_t)U64_WINDOW_KEYS);
	__dtfl_vec_clear(&w->keys);
	__dtfl_vec_clear(&w->input);
	__dtfl_vec_clear(&w->output);
	__dtfl_vec_push_n(&w->keys, changed + *cursor, n);
	__dtfl_proxy_present_u64(inst->source_batches, inst->nsource, b->rin,
	    b->tw, changed + *cursor, n, NULL, &w->input);
	__dtfl_proxy_present_u64(inst->input_batches, inst->ninput, b->rin,
	    b->tw, changed + *cursor, n, NULL, &w->input);
	__dtfl_bridge_consolidate(&w->input, b->tw, b->rin);
	__dtfl_proxy_present_u64(inst->output_batches, inst->noutput, b->rout,
	    b->tw, changed + *cursor, n, NULL, &w->output);
	__dtfl_bridge_consolidate(&w->output, b->tw, b->rout);
	*cursor += n;
	return true;
}

/* Present (id, diff) rows to the logic as 8-byte values and diffs. */
static void
rows_to_args(const __dtfl_bridge_upd *rows, size_t n, size_t dw,
    unsigned char *vals, const void **vp, size_t *vl, unsigned char *diffs)
{
	size_t i;

	for (i = 0; i < n; i++) {
		dtfl_put_u64(vals + 8 * i, rows[i].id);
		vp[i] = vals + 8 * i;
		vl[i] = 8;
		if (dw != 0)
			memcpy(diffs + dw * i, rows[i].diff, dw);
	}
}

static void
u64_reduce_corrections(void *self, const uint64_t *keys, size_t nkeys,
    const size_t *in_ends, const __dtfl_bridge_upd *input,
    const size_t *out_ends, const __dtfl_bridge_upd *output,
    __dtfl_vec *corr, __dtfl_vec *corr_ends)
{
	u64_backend *b = self;
	size_t k, in0 = 0, out0 = 0;

	for (k = 0; k < nkeys; k++) {
		size_t ni = in_ends[k] - in0, no = out_ends[k] - out0, i;
		size_t n = DTFL_MAX(ni + no, 1u), start = corr->len;
		size_t idw = b->rin->width, odw = b->rout->width;
		unsigned char *vals = __dtfl_alloc(8 * n);
		const void **vp = __dtfl_alloc(n * sizeof *vp);
		size_t *vl = __dtfl_alloc(n * sizeof *vl);
		unsigned char *idiffs = __dtfl_alloc(DTFL_MAX(ni * idw, 1u));
		unsigned char *odiffs = __dtfl_alloc(DTFL_MAX(no * odw, 1u));
		unsigned char kb[8];
		__dtfl_reduce_change *ch = __dtfl_reduce_change_new(b->ovt,
		    b->rout);
		__dtfl_vec part;

		dtfl_put_u64(kb, keys[k]);
		/* A side with no rows may come as NULL: offset it only if set. */
		rows_to_args(input != NULL ? input + in0 : NULL, ni, idw, vals,
		    vp, vl, idiffs);
		rows_to_args(output != NULL ? output + out0 : NULL, no, odw,
		    vals + 8 * ni, vp + ni, vl + ni, odiffs);
		b->logic(b->ctx, kb, sizeof kb, ni, vp, vl, idiffs, no, vp + ni,
		    vl + ni, odiffs, ch);
		__dtfl_vec_init(&part, sizeof(__dtfl_bridge_upd));
		for (i = 0; i < __dtfl_reduce_change_len(ch); i++) {
			size_t len;
			const void *d;
			const void *v = __dtfl_reduce_change_at(ch, i, &len, &d);
			__dtfl_bridge_upd r;

			DTFL_ASSERT(len == 8);
			memset(&r, 0, sizeof r);
			r.id = dtfl_get_u64(v);
			if (odw != 0)
				memcpy(r.diff, d, odw);
			__dtfl_vec_push(&part, &r);
		}
		__dtfl_bridge_consolidate(&part, 0, b->rout);
		for (i = 0; i < part.len; i++)
			__dtfl_vec_push(corr, __dtfl_vec_at(&part, i));
		(void)start;
		__dtfl_vec_push(corr_ends, &corr->len);
		__dtfl_vec_free(&part);
		__dtfl_reduce_change_free(ch);
		__dtfl_free(odiffs);
		__dtfl_free(idiffs);
		__dtfl_free(vl);
		__dtfl_free(vp);
		__dtfl_free(vals);
		in0 = in_ends[k];
		out0 = out_ends[k];
	}
}

static void
u64_emit(void *self, size_t tile, const __dtfl_bridge_upd *records, size_t n)
{
	u64_backend *b = self;
	size_t i;

	DTFL_ASSERT(tile < b->ntiles);
	for (i = 0; i < n; i++)
		__dtfl_vec_push(&b->records[tile], &records[i]);
}

static void
u64_finish(void *self, __dtfl_batch_ord **out)
{
	u64_backend *b = self;
	size_t t, i;

	for (t = 0; t < b->ntiles; t++) {
		__dtfl_vec *rows = &b->records[t];
		__dtfl_builder_ord bld;

		/* Windows ascend by hash, but sort anyway: the builder needs it. */
		__dtfl_bridge_consolidate(rows, b->tw, b->rout);
		__dtfl_builder_ord_init(&bld, false, b->kt, b->ovt, b->tc,
		    b->rout, 0, 0, rows->len);
		for (i = 0; i < rows->len; i++) {
			const __dtfl_bridge_upd *r = __dtfl_vec_at(rows, i);
			unsigned char kb[8], vb[8];

			dtfl_put_u64(kb, r->hash);
			dtfl_put_u64(vb, r->id);
			__dtfl_builder_ord_push(&bld, kb, sizeof kb, vb,
			    sizeof vb, r->time,
			    b->rout->width != 0 ? r->diff : NULL);
		}
		out[t] = __dtfl_builder_ord_done(&bld, &b->tiles[t]);
		__dtfl_vec_free(rows);
	}
	__dtfl_free(b->records);
	__dtfl_free(b->tiles);
	b->records = NULL;
	b->tiles = NULL;
	b->ntiles = 0;
}

static void
u64_free(void *self)
{
	u64_backend *b = self;

	if (b->ctx_free != NULL)
		b->ctx_free(b->ctx);
	__dtfl_free(b);
}

static const __dtfl_proxy_reduce_backend_ops u64_backend_ops = {
	u64_seed_times, u64_begin, u64_next_window, u64_reduce_corrections,
	u64_emit, u64_finish, u64_free
};

__dtfl_proxy_reduce_backend
__dtfl_proxy_reduce_u64_backend(const struct dtfl_arranged *a,
    const dtfl_type *ovt, const __dtfl_diff_class *odc,
    __dtfl_reduce_logic_fn logic, void *ctx, void (*ctx_free)(void *))
{
	const __dtfl_trace_agent *g = __dtfl_arranged_agent(a);
	u64_backend *b = __dtfl_alloc(sizeof *b);
	__dtfl_proxy_reduce_backend be;

	memset(b, 0, sizeof *b);
	b->kt = g->kt;
	b->vt = g->vt;
	b->ovt = ovt;
	b->tc = g->tc;
	b->rin = g->dc;
	b->rout = odc;
	b->tw = dtfl_time_depth(g->tc);
	b->logic = logic;
	b->ctx = ctx;
	b->ctx_free = ctx_free;
	be.ops = &u64_backend_ops;
	be.self = b;
	be.rin = g->dc;
	be.rout = odc;
	return be;
}
