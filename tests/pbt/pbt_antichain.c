/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_antichain.c: Antichain, MutableAntichain and ChangeBatch
 * against naive models (PLAN section 6, tier 4).
 *
 * Times are Product<u64, u64> with small coordinates, so that elements
 * collide, are comparable and are incomparable often.  The models:
 *
 *   - Antichain: the set of every element inserted; the antichain must
 *     equal its minimal elements.
 *   - MutableAntichain: a map from time to accumulated count; the
 *     frontier must equal the minimal elements with positive count,
 *     recomputed from scratch, and the reported changes must take the
 *     old frontier to the new one.
 *   - ChangeBatch: a map from key to accumulated value; compacted
 *     contents must equal the non-zero entries in key order.
 */
#include "pbt.h"
#include "progress/progress_int.h"

#define NT 64	/* model capacity: distinct times */

typedef struct model {
	uint64_t t[NT][2];
	int64_t count[NT];
	size_t n;
} model;

static void
draw_t(pbt_tc *tc, uint64_t t[2])
{
	t[0] = (uint64_t)pbt_int(tc, 0, 4);
	t[1] = (uint64_t)pbt_int(tc, 0, 4);
}

static bool
le(const uint64_t *a, const uint64_t *b)
{
	return a[0] <= b[0] && a[1] <= b[1];
}

/* Adds delta at t; returns the slot. */
static size_t
model_update(model *m, const uint64_t t[2], int64_t delta)
{
	size_t i;

	for (i = 0; i < m->n; i++) {
		if (m->t[i][0] == t[0] && m->t[i][1] == t[1])
			break;
	}
	if (i == m->n) {
		m->t[i][0] = t[0];
		m->t[i][1] = t[1];
		m->count[i] = 0;
		m->n++;
	}
	m->count[i] += delta;
	return i;
}

/* True if t is minimal among the model's times with count > 0. */
static bool
model_minimal(const model *m, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < m->n; i++) {
		if (m->count[i] > 0 && le(m->t[i], t) &&
		    !(m->t[i][0] == t[0] && m->t[i][1] == t[1]))
			return false;
	}
	return true;
}

static size_t
model_frontier_len(const model *m)
{
	size_t i, n = 0;

	for (i = 0; i < m->n; i++)
		n += m->count[i] > 0 && model_minimal(m, m->t[i]);
	return n;
}

/* The frontier equals the model's minimal positive times. */
static bool
frontier_matches(const __dtfl_mutable_antichain *ma, const model *m)
{
	size_t i, j, n = __dtfl_mantichain_frontier_len(ma);
	const uint64_t *f = __dtfl_mantichain_frontier(ma);

	if (n != model_frontier_len(m))
		return false;
	for (i = 0; i < n; i++) {
		for (j = 0; j < m->n; j++) {
			if (m->t[j][0] == f[2 * i] && m->t[j][1] == f[2 * i + 1])
				break;
		}
		if (j == m->n || m->count[j] <= 0 || !model_minimal(m, m->t[j]))
			return false;
	}
	return true;
}

/* Membership of t in a flat frontier run. */
static bool
in_frontier(const uint64_t *f, size_t n, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (f[2 * i] == t[0] && f[2 * i + 1] == t[1])
			return true;
	}
	return false;
}

static dtfl_time_class *
pair_class(void)
{
	dtfl_time_class *u = dtfl_time_u64(), *p = dtfl_time_product(u, u);

	dtfl_time_class_free(u);
	return p;
}

static void
prop_mutable_antichain_model(pbt_tc *tc)
{
	dtfl_time_class *k = pair_class();
	__dtfl_mutable_antichain ma;
	__dtfl_change_batch batch;
	model m = { .n = 0 };
	uint64_t old[2 * NT], t[2];
	size_t old_n, i, n, rounds = (size_t)pbt_int(tc, 1, 30), j, nb;
	int64_t total;

	__dtfl_mantichain_init(&ma, k);
	__dtfl_cb_init(&batch, 2);
	while (rounds-- > 0) {
		old_n = __dtfl_mantichain_frontier_len(&ma);
		memcpy(old, __dtfl_mantichain_frontier(&ma), old_n * 16);
		/* A batch of updates, or a single one. */
		nb = (size_t)pbt_int(tc, 1, 6);
		for (j = 0; j < nb; j++) {
			int64_t delta = pbt_int(tc, -2, 3);

			draw_t(tc, t);
			if (m.n == NT)
				break;
			model_update(&m, t, delta);
			if (nb == 1)
				__dtfl_mantichain_update(&ma, t, delta);
			else
				__dtfl_cb_update(&batch, t, delta);
		}
		if (nb > 1)
			__dtfl_mantichain_update_batch(&ma, &batch);
		PBT_ASSERT(frontier_matches(&ma, &m));
		/* changes = new - old, as multisets of frontier elements. */
		n = __dtfl_cb_iter(&ma.changes);
		for (i = 0; i < n; i++) {
			const uint64_t *key = __dtfl_cb_key(&ma.changes, i);
			int64_t d = __dtfl_cb_delta(&ma.changes, i);
			int64_t want = (int64_t)in_frontier(
			    __dtfl_mantichain_frontier(&ma),
			    __dtfl_mantichain_frontier_len(&ma), key) -
			    (int64_t)in_frontier(old, old_n, key);

			PBT_ASSERT(d == want && d != 0);
		}
		for (i = 0; i < old_n; i++) {
			if (!in_frontier(__dtfl_mantichain_frontier(&ma),
			    __dtfl_mantichain_frontier_len(&ma), old + 2 * i)) {
				for (j = 0; j < n; j++) {
					if (memcmp(__dtfl_cb_key(&ma.changes, j),
					    old + 2 * i, 16) == 0)
						break;
				}
				PBT_ASSERT(j < n);
			}
		}
		__dtfl_cb_clear(&ma.changes);
		/* count_for and the frontier queries agree with the model. */
		draw_t(tc, t);
		total = 0;
		for (i = 0; i < m.n; i++) {
			if (m.t[i][0] == t[0] && m.t[i][1] == t[1])
				total = m.count[i];
		}
		PBT_ASSERT(__dtfl_mantichain_count_for(&ma, t) == total);
		PBT_ASSERT(__dtfl_mantichain_is_empty(&ma) ==
		    (model_frontier_len(&m) == 0));
	}
	__dtfl_cb_free(&batch);
	__dtfl_mantichain_free(&ma);
	dtfl_time_class_free(k);
}

static void
prop_antichain_insert_model(pbt_tc *tc)
{
	dtfl_time_class *k = pair_class();
	__dtfl_antichain a, b;
	model m = { .n = 0 };
	uint64_t t[2];
	size_t steps = (size_t)pbt_int(tc, 1, 40), i, n;
	bool before, added;

	__dtfl_antichain_init(&a, k);
	while (steps-- > 0 && m.n < NT) {
		draw_t(tc, t);
		before = false;
		for (i = 0; i < m.n; i++)
			before = before || le(m.t[i], t);
		added = __dtfl_antichain_insert(&a, t);
		PBT_ASSERT(added == !before);
		model_update(&m, t, 1);
		/* The antichain is exactly the minimal inserted elements. */
		n = __dtfl_antichain_len(&a);
		PBT_ASSERT(n == model_frontier_len(&m));
		for (i = 0; i < n; i++)
			PBT_ASSERT(model_minimal(&m, __dtfl_antichain_at(&a, i)));
		draw_t(tc, t);
		before = false;
		for (i = 0; i < m.n; i++)
			before = before || le(m.t[i], t);
		PBT_ASSERT(__dtfl_antichain_less_equal(&a, t) == before);
	}
	/* Equality ignores order: a sorted copy is equal. */
	__dtfl_antichain_init(&b, k);
	__dtfl_antichain_copy(&b, &a);
	__dtfl_antichain_sort(&b);
	PBT_ASSERT(__dtfl_antichain_eq(&a, &b));
	PBT_ASSERT(__dtfl_antichain_dominates(&a, &b));
	for (i = 1; i < __dtfl_antichain_len(&b); i++)
		PBT_ASSERT(__dtfl_words_cmp(__dtfl_antichain_at(&b, i - 1),
		    __dtfl_antichain_at(&b, i), 2) < 0);
	__dtfl_antichain_free(&b);
	__dtfl_antichain_free(&a);
	dtfl_time_class_free(k);
}

static void
prop_change_batch_model(pbt_tc *tc)
{
	__dtfl_change_batch cb, other;
	model m = { .n = 0 };
	uint64_t t[2];
	size_t steps = (size_t)pbt_int(tc, 1, 150), i, n, j;
	int64_t op;

	__dtfl_cb_init(&cb, 2);
	__dtfl_cb_init(&other, 2);
	while (steps-- > 0) {
		op = pbt_int(tc, 0, 9);
		draw_t(tc, t);
		if (op <= 6) {
			int64_t delta = pbt_int(tc, -3, 3);

			model_update(&m, t, delta);
			__dtfl_cb_update(&cb, t, delta);
			/* Bounded dirt: maintain_bounds compacts past 32. */
			PBT_ASSERT(cb.updates.len <= 32 ||
			    cb.updates.len >> 1 < cb.clean);
		} else if (op == 7) {
			/* Round trip through another batch. */
			__dtfl_cb_drain_into(&cb, &other);
			PBT_ASSERT(cb.updates.len == 0);
			__dtfl_cb_drain_into(&other, &cb);
		} else if (op == 8) {
			__dtfl_cb_extend(&other, &cb);
			__dtfl_cb_clear(&other);
		} else {
			bool empty = true;

			for (i = 0; i < m.n; i++)
				empty = empty && m.count[i] == 0;
			PBT_ASSERT(__dtfl_cb_is_empty(&cb) == empty);
		}
	}
	n = __dtfl_cb_iter(&cb);
	PBT_ASSERT(!__dtfl_cb_is_dirty(&cb));
	j = 0;
	for (i = 0; i < m.n; i++)
		j += m.count[i] != 0;
	PBT_ASSERT(n == j);
	for (i = 0; i < n; i++) {
		const uint64_t *key = __dtfl_cb_key(&cb, i);

		if (i > 0)
			PBT_ASSERT(__dtfl_words_cmp(__dtfl_cb_key(&cb, i - 1), key,
			    2) < 0);
		for (j = 0; j < m.n; j++) {
			if (m.t[j][0] == key[0] && m.t[j][1] == key[1])
				break;
		}
		PBT_ASSERT(j < m.n && m.count[j] == __dtfl_cb_delta(&cb, i));
	}
	__dtfl_cb_free(&other);
	__dtfl_cb_free(&cb);
}

static const pbt_prop props[] = {
	{ "mutable_antichain_model", prop_mutable_antichain_model, 300 },
	{ "antichain_insert_model", prop_antichain_insert_model, 300 },
	{ "change_batch_model", prop_change_batch_model, 300 },
};

PBT_MAIN("antichain", props)
