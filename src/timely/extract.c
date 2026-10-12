/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/extract.c: draining a captured event link for assertions.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/capture/extract.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * Extract reads every Event::Messages from an event link, groups records
 * by time, sorts the records within each time by the record type's
 * order, and reports (time, record) pairs in time-then-record order.
 * Progress events are ignored.  A time whose records are all empty is
 * dropped, as upstream drops an empty container.  This is a test helper,
 * run after the computation has finished.
 */
#include "timely/capture_int.h"

/* One (time, record) pair, flattened for sorting. */
typedef struct pair {
	uint64_t time[DTFL_TIME_MAX];
	const void *rec;
	size_t len;
} pair;

/* Three-way order on pairs: time, then record (the type's Ord). */
static int
pair_cmp(const pair *a, const pair *b, const dtfl_time_class *tc,
    const dtfl_type *type)
{
	int c = __dtfl_words_cmp(a->time, b->time, tc->depth);

	if (c != 0)
		return c;
	return dtfl_type_cmp(type, a->rec, a->len, b->rec, b->len);
}

/*
 * Heapsort over the pair array, in place and allocation-free, because
 * qsort has no context argument for the time class and record type and
 * the library allows no writable globals (PLAN section 5).
 */
static void
sift_down(pair *v, size_t root, size_t n, const dtfl_time_class *tc,
    const dtfl_type *type)
{
	for (;;) {
		size_t child = 2 * root + 1, big = root;
		pair tmp;

		if (child < n && pair_cmp(&v[child], &v[big], tc, type) > 0)
			big = child;
		if (child + 1 < n && pair_cmp(&v[child + 1], &v[big], tc, type) > 0)
			big = child + 1;
		if (big == root)
			return;
		tmp = v[root];
		v[root] = v[big];
		v[big] = tmp;
		root = big;
	}
}

static void
sort_pairs(pair *v, size_t n, const dtfl_time_class *tc, const dtfl_type *type)
{
	size_t i;

	for (i = n / 2; i-- > 0;)
		sift_down(v, i, n, tc, type);
	for (i = n; i-- > 1;) {
		pair tmp = v[0];

		v[0] = v[i];
		v[i] = tmp;
		sift_down(v, 0, i, tc, type);
	}
}

void
dtfl_event_link_extract(dtfl_event_link *l,
    void (*each)(void *ctx, const uint64_t *time, const void *rec, size_t len),
    void *ctx)
{
	const dtfl_event *e;
	__dtfl_vec pairs;	/* pair */
	size_t i;

	__dtfl_vec_init(&pairs, sizeof(pair));

	/* Collect every record of every Messages event with its time. */
	while ((e = __dtfl_event_link_next(l)) != NULL) {
		size_t n, k;

		if (e->tag != DTFL_EVENT_MESSAGES)
			continue;
		n = __dtfl_batch_len(e->u.messages.data);
		for (k = 0; k < n; k++) {
			pair pr;

			__dtfl_time_copy(l->tc, pr.time, e->u.messages.time);
			pr.rec = __dtfl_batch_rec(e->u.messages.data, k, &pr.len);
			__dtfl_vec_push(&pairs, &pr);
		}
	}

	if (pairs.len > 1)
		sort_pairs((pair *)pairs.data, pairs.len, l->tc, l->type);

	for (i = 0; i < pairs.len; i++) {
		pair *pr = DTFL_VEC_AT(&pairs, pair, i);

		each(ctx, pr->time, pr->rec, pr->len);
	}
	__dtfl_vec_free(&pairs);
}
