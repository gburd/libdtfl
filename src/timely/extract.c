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

#include <stdlib.h>

/* One (time, record) pair, flattened for sorting. */
typedef struct pair {
	uint64_t time[DTFL_TIME_MAX];
	const void *rec;
	size_t len;
} pair;

typedef struct extract_sort {
	const dtfl_time_class *tc;
	const dtfl_type *type;
} extract_sort;

static const extract_sort *g_sort;	/* qsort has no context argument */

static int
pair_cmp(const void *va, const void *vb)
{
	const pair *a = va, *b = vb;
	int c = __dtfl_words_cmp(a->time, b->time, g_sort->tc->depth);

	if (c != 0)
		return c;
	return dtfl_type_cmp(g_sort->type, a->rec, a->len, b->rec, b->len);
}

void
dtfl_event_link_extract(dtfl_event_link *l,
    void (*each)(void *ctx, const uint64_t *time, const void *rec, size_t len),
    void *ctx)
{
	const dtfl_event *e;
	__dtfl_vec pairs;	/* pair */
	extract_sort sort;
	size_t i;

	sort.tc = l->tc;
	sort.type = l->type;
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

	/* Sort by time then record (qsort(NULL, 0, ...) trips UBSan). */
	g_sort = &sort;
	if (pairs.len > 1)
		qsort(pairs.data, pairs.len, sizeof(pair), pair_cmp);
	g_sort = NULL;

	for (i = 0; i < pairs.len; i++) {
		pair *pr = DTFL_VEC_AT(&pairs, pair, i);

		each(ctx, pr->time, pr->rec, pr->len);
	}
	__dtfl_vec_free(&pairs);
}
