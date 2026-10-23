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

#include <stdlib.h>
#include <string.h>

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

static void
class_release(void *c)
{
	dtfl_time_class_free(c);
}

static void
cb_release(void *p)
{
	__dtfl_cb_free(p);
	__dtfl_free(p);
}

static void
mantichain_release(void *p)
{
	__dtfl_mantichain_free(p);
	__dtfl_free(p);
}

static void
antichain_release(void *p)
{
	__dtfl_antichain_free(p);
	__dtfl_free(p);
}

/* A change batch released when the case ends (PBT_ASSERT may unwind). */
static __dtfl_change_batch *
new_cb(pbt_tc *tc, unsigned kw)
{
	__dtfl_change_batch *cb = __dtfl_alloc(sizeof *cb);

	__dtfl_cb_init(cb, kw);
	pbt_defer(tc, cb_release, cb);
	return cb;
}

static dtfl_time_class *
pair_class(pbt_tc *tc)
{
	dtfl_time_class *u = dtfl_time_u64(), *p = dtfl_time_product(u, u);

	dtfl_time_class_free(u);
	pbt_defer(tc, class_release, p);
	return p;
}

static void
prop_mutable_antichain_model(pbt_tc *tc)
{
	dtfl_time_class *k = pair_class(tc);
	__dtfl_mutable_antichain *map = __dtfl_alloc(sizeof *map);
	__dtfl_change_batch *batchp;
	model m = { .n = 0 };
	uint64_t old[2 * NT], t[2];
	size_t old_n, i, n, rounds = (size_t)pbt_int(tc, 1, 30), j, nb;
	int64_t total;

	__dtfl_mantichain_init(map, k);
	pbt_defer(tc, mantichain_release, map);
	batchp = new_cb(tc, 2);
#define ma (*map)
#define batch (*batchp)
	while (rounds-- > 0) {
		old_n = __dtfl_mantichain_frontier_len(&ma);
		if (old_n > 0)
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
		/* Every element that entered the frontier appears with +1. */
		for (i = 0; i < __dtfl_mantichain_frontier_len(&ma); i++) {
			const uint64_t *f = __dtfl_mantichain_frontier(&ma) + 2 * i;

			if (in_frontier(old, old_n, f))
				continue;
			for (j = 0; j < n; j++) {
				if (memcmp(__dtfl_cb_key(&ma.changes, j), f, 16) == 0)
					break;
			}
			PBT_ASSERT(j < n && __dtfl_cb_delta(&ma.changes, j) == 1);
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
#undef ma
#undef batch
}

static void
prop_antichain_insert_model(pbt_tc *tc)
{
	dtfl_time_class *k = pair_class(tc);
	__dtfl_antichain *ap = __dtfl_alloc(sizeof *ap), *bp = __dtfl_alloc(sizeof *bp);
	model m = { .n = 0 };
	uint64_t t[2];
	size_t steps = (size_t)pbt_int(tc, 1, 40), i, n;
	bool before, added;

	__dtfl_antichain_init(ap, k);
	__dtfl_antichain_init(bp, k);
	pbt_defer(tc, antichain_release, ap);
	pbt_defer(tc, antichain_release, bp);
#define a (*ap)
#define b (*bp)
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
	__dtfl_antichain_copy(&b, &a);
	__dtfl_antichain_sort(&b);
	PBT_ASSERT(__dtfl_antichain_eq(&a, &b));
	PBT_ASSERT(__dtfl_antichain_dominates(&a, &b));
	for (i = 1; i < __dtfl_antichain_len(&b); i++)
		PBT_ASSERT(__dtfl_words_cmp(__dtfl_antichain_at(&b, i - 1),
		    __dtfl_antichain_at(&b, i), 2) < 0);
#undef a
#undef b
}

/*
 * Upstream's compact skips consolidation when there is one update, so a
 * single zero update survives it (change_batch.rs compact, the
 * `updates.len() > 1` guard; dev/upstream-notes.md).  The model allows
 * exactly that.
 */
static bool
lone_zero(const __dtfl_change_batch *cb)
{
	return cb->updates.len == 1 && __dtfl_cb_delta(cb, 0) == 0;
}

/*
 * The model over one-, two- or three-word keys.  The model's key is two
 * words: with kw 1 the key is t[0] and t[1] stays 0; with kw 3 the key
 * is (t[0], 7, t[1]), a fixed middle word, so the same model checks the
 * fixed-width fast paths of compact.
 */
static void
cb_key(unsigned kw, const uint64_t t[2], uint64_t key[3])
{
	key[0] = t[0];
	/* kw 3: a middle word that varies with the key, so a compare that
	 * skipped it would merge distinct keys. */
	key[1] = kw == 3 ? 7 + (t[1] & 1) : t[1];
	key[2] = t[1];
}

static void
change_batch_model(pbt_tc *tc, unsigned kw)
{
	__dtfl_change_batch *cbp = new_cb(tc, kw), *otherp = new_cb(tc, kw);
	model m = { .n = 0 };
	uint64_t t[2], key3[3];
	size_t steps = (size_t)pbt_int(tc, 1, 150), i, n, j;
	int64_t op;
#define cb (*cbp)
#define other (*otherp)
	while (steps-- > 0) {
		op = pbt_int(tc, 0, 9);
		draw_t(tc, t);
		if (kw == 1)
			t[1] = 0;
		if (op <= 6) {
			int64_t delta = pbt_int(tc, -3, 3);

			model_update(&m, t, delta);
			cb_key(kw, t, key3);
			__dtfl_cb_update(&cb, key3, delta);
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
			if (__dtfl_cb_is_empty(&cb) != empty)
				PBT_ASSERT(empty && lone_zero(&cb));
		}
	}
	n = __dtfl_cb_iter(&cb);
	PBT_ASSERT(!__dtfl_cb_is_dirty(&cb));
	if (lone_zero(&cb))
		n = 0;
	j = 0;
	for (i = 0; i < m.n; i++)
		j += m.count[i] != 0;
	PBT_ASSERT(n == j);
	for (i = 0; i < n; i++) {
		const uint64_t *key = __dtfl_cb_key(&cb, i);

		if (i > 0)
			PBT_ASSERT(__dtfl_words_cmp(__dtfl_cb_key(&cb, i - 1), key,
			    kw) < 0);
		for (j = 0; j < m.n; j++) {
			if (m.t[j][0] == key[0] &&
			    m.t[j][1] == (kw == 1 ? 0 : kw == 3 ? key[2] : key[1]))
				break;
		}
		PBT_ASSERT(j < m.n && m.count[j] == __dtfl_cb_delta(&cb, i));
	}
#undef cb
#undef other
}

static void
prop_change_batch_model(pbt_tc *tc)
{
	change_batch_model(tc, 2);
}

static void
prop_change_batch_model_kw1(pbt_tc *tc)
{
	change_batch_model(tc, 1);
}

static void
prop_change_batch_model_kw3(pbt_tc *tc)
{
	change_batch_model(tc, 3);
}

/*
 * One-word TOTAL times (u64, and i64 with its biased storage, drawn
 * across zero): __dtfl_time_le and __dtfl_time_lt, which the antichain
 * code uses, inline this class.  A MutableAntichain under random updates
 * must hold exactly the least positive time, and the antichain and
 * frontier queries must answer as integer comparisons do.
 */
static void
total_model(pbt_tc *tc, bool is_signed)
{
	dtfl_coord co = { is_signed ? DTFL_COORD_SIGNED : DTFL_COORD_UNSIGNED,
	    64 };
	dtfl_time_class *k = dtfl_time_total(co);
	__dtfl_mutable_antichain *map = __dtfl_alloc(sizeof *map);
	__dtfl_antichain *ap = __dtfl_alloc(sizeof *ap);
	int64_t val[16], cnt[16], v, q;
	size_t nv = 0, i, steps = (size_t)pbt_int(tc, 1, 60);
	uint64_t w, qw;

	pbt_defer(tc, class_release, k);
	__dtfl_mantichain_init(map, k);
	pbt_defer(tc, mantichain_release, map);
	__dtfl_antichain_init(ap, k);
	pbt_defer(tc, antichain_release, ap);
	while (steps-- > 0) {
		int64_t d = pbt_int(tc, -2, 3);
		bool have = false, le = false, lt = false;
		int64_t least = 0;

		v = pbt_int(tc, is_signed ? -3 : 0, 3);
		w = is_signed ? dtfl_coord_from_i64(v) : (uint64_t)v;
		for (i = 0; i < nv && val[i] != v; i++)
			;
		if (i == nv) {
			if (nv == 16)
				break;
			val[nv] = v;
			cnt[nv++] = 0;
		}
		cnt[i] += d;
		__dtfl_mantichain_update(map, &w, d);
		__dtfl_cb_clear(&map->changes);
		(void)__dtfl_antichain_insert(ap, &w);
		for (i = 0; i < nv; i++) {
			if (cnt[i] > 0 && (!have || val[i] < least)) {
				least = val[i];
				have = true;
			}
		}
		PBT_ASSERT(__dtfl_mantichain_frontier_len(map) == (have ? 1 : 0));
		if (have) {
			uint64_t lw = is_signed ? dtfl_coord_from_i64(least) :
			    (uint64_t)least;

			PBT_ASSERT(__dtfl_mantichain_frontier(map)[0] == lw);
		}
		q = pbt_int(tc, is_signed ? -4 : 0, 4);
		qw = is_signed ? dtfl_coord_from_i64(q) : (uint64_t)q;
		if (have) {
			le = least <= q;
			lt = least < q;
		}
		PBT_ASSERT(__dtfl_mantichain_less_equal(map, &qw) == le);
		PBT_ASSERT(__dtfl_mantichain_less_than(map, &qw) == lt);
		/* The antichain of every inserted time is its minimum. */
		{
			int64_t mn = val[0];

			for (i = 1; i < nv; i++)
				if (val[i] < mn)
					mn = val[i];
			PBT_ASSERT(__dtfl_antichain_len(ap) == 1);
			PBT_ASSERT(__dtfl_antichain_less_equal(ap, &qw) == (mn <= q));
			PBT_ASSERT(__dtfl_antichain_less_than(ap, &qw) == (mn < q));
		}
	}
}

static void
prop_total_u64_model(pbt_tc *tc)
{
	total_model(tc, false);
}

static void
prop_total_i64_model(pbt_tc *tc)
{
	total_model(tc, true);
}

/*
 * Long batches: compact must equal a reference compaction (sort by key
 * with qsort, sum equal keys, drop zeros) for batches long enough to
 * take the introsort's partitioning path, with key patterns that stress
 * it: few distinct keys, already sorted, reversed, and random.
 */
static unsigned ref_kw;

static int
ref_cmp(const void *a, const void *b)
{
	return __dtfl_words_cmp(a, b, ref_kw);
}

static void
prop_change_batch_long(pbt_tc *tc)
{
	unsigned kw = (unsigned)pbt_int(tc, 1, 4);
	size_t n = (size_t)pbt_int(tc, 21, 3000), i, j, k, words = kw + 1;
	int pattern = (int)pbt_int(tc, 0, 3);
	uint64_t span = (uint64_t)pbt_int(tc, 1, 4000), seed;
	__dtfl_change_batch *cbp = new_cb(tc, kw);
	uint64_t *ref = __dtfl_alloc(n * words * sizeof *ref);
	uint64_t key[5];

	pbt_defer(tc, __dtfl_free, ref);
	seed = (uint64_t)pbt_int(tc, 0, INT64_MAX);
	for (i = 0; i < n; i++) {
		uint64_t v;

		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		v = (seed >> 17) % span;
		if (pattern == 1)
			v = i % span;
		else if (pattern == 2)
			v = (n - i) % span;
		else if (pattern == 3)
			v = (seed >> 17) % 3;
		for (k = 0; k < kw; k++)
			key[k] = k + 1 == kw ? v : (v >> (8 * (kw - k))) & 3;
		__dtfl_cb_update(cbp, key, (int64_t)((seed >> 40) % 5) - 2);
		memcpy(ref + i * words, key, kw * sizeof *key);
		ref[i * words + kw] = (uint64_t)((int64_t)((seed >> 40) % 5) - 2);
	}
	/* Reference: sort, then sum runs of equal keys, then drop zeros. */
	ref_kw = kw;
	qsort(ref, n, words * sizeof *ref, ref_cmp);
	for (i = 0, k = 0; i < n; i = j) {
		uint64_t sum = 0;

		for (j = i; j < n && __dtfl_words_cmp(ref + j * words,
		    ref + i * words, kw) == 0; j++)
			sum += ref[j * words + kw];
		if (sum == 0)
			continue;
		memmove(ref + k * words, ref + i * words, kw * sizeof *ref);
		ref[k * words + kw] = sum;
		k++;
	}
	n = __dtfl_cb_iter(cbp);
	/* maintain_bounds compacts as it goes; the result is the same. */
	PBT_ASSERT(n == k || (k == 0 && n == 1 && __dtfl_cb_delta(cbp, 0) == 0));
	if (k == 0)
		return;
	for (i = 0; i < n; i++) {
		PBT_ASSERT(__dtfl_words_cmp(__dtfl_cb_key(cbp, i), ref + i * words,
		    kw) == 0);
		PBT_ASSERT(__dtfl_cb_delta(cbp, i) == (int64_t)ref[i * words + kw]);
	}
}

static const pbt_prop props[] = {
	{ "mutable_antichain_model", prop_mutable_antichain_model, 300 },
	{ "antichain_insert_model", prop_antichain_insert_model, 300 },
	{ "change_batch_model", prop_change_batch_model, 300 },
	{ "change_batch_model_kw1", prop_change_batch_model_kw1, 300 },
	{ "change_batch_model_kw3", prop_change_batch_model_kw3, 300 },
	{ "change_batch_long", prop_change_batch_long, 300 },
	{ "total_u64_model", prop_total_u64_model, 300 },
	{ "total_i64_model", prop_total_i64_model, 300 },
};

PBT_MAIN("antichain", props)
