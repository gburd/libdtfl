/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/progress/operate.c: connectivity and shared progress.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/progress/operate.rs
 *   timely/src/progress/mod.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * __dtfl_port_conn is PortConnectivity kept in canonical form at all
 * times (ports sorted and distinct, antichains non-empty), which is what
 * PortConnectivityBuilder::freeze produces; insertion merges duplicate
 * ports by antichain insertion under the summary order, so the result
 * is independent of insertion order, as upstream's freeze.
 */
#include "progress/operate_int.h"

static size_t
summary_words(const dtfl_time_class *tc)
{
	return DTFL_MAX(tc->sdepth, 1u);
}

void
__dtfl_port_conn_init(__dtfl_port_conn *pc, const dtfl_time_class *tc)
{
	pc->tc = tc;
	__dtfl_vec_init(&pc->entries, sizeof(__dtfl_port_conn_entry));
}

void
__dtfl_port_conn_free(__dtfl_port_conn *pc)
{
	size_t i;

	for (i = 0; i < pc->entries.len; i++)
		__dtfl_vec_free(&DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry,
		    i)->summaries);
	__dtfl_vec_free(&pc->entries);
}

void
__dtfl_port_conn_copy(__dtfl_port_conn *dst, const __dtfl_port_conn *src)
{
	size_t i;

	__dtfl_port_conn_init(dst, src->tc);
	for (i = 0; i < src->entries.len; i++) {
		const __dtfl_port_conn_entry *e =
		    DTFL_VEC_AT(&src->entries, __dtfl_port_conn_entry, i);
		__dtfl_port_conn_entry *d = __dtfl_vec_push(&dst->entries, NULL);

		d->port = e->port;
		__dtfl_vec_init(&d->summaries, sizeof(uint64_t));
		__dtfl_vec_copy(&d->summaries, &e->summaries);
	}
}

/*
 * Antichain::insert over summaries: adds `s` unless some element is
 * less_equal to it, removing elements it is less_equal to.  Returns
 * true if added.
 */
static bool
summary_antichain_insert(const dtfl_time_class *tc, __dtfl_vec *v,
    const uint64_t *s)
{
	size_t w = summary_words(tc), n = v->len / w, i;
	uint64_t *e = (uint64_t *)(void *)v->data;

	for (i = 0; i < n; i++) {
		if (dtfl_summary_less_equal(tc, e + i * w, s))
			return false;
	}
	/* retain(|x| !s.less_equal(x)) */
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

void
__dtfl_port_conn_insert(__dtfl_port_conn *pc, size_t port, const uint64_t *summary)
{
	size_t lo = 0, hi = pc->entries.len;
	__dtfl_port_conn_entry *e;
	uint64_t zero = 0;

	/* Summaries of depth 0 are stored as one zero word. */
	if (pc->tc->sdepth == 0)
		summary = &zero;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		size_t mp = DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, mid)->port;

		if (mp < port)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == pc->entries.len ||
	    DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, lo)->port != port) {
		__dtfl_port_conn_entry fresh;
		__dtfl_port_conn_entry *base;

		fresh.port = port;
		__dtfl_vec_init(&fresh.summaries, sizeof(uint64_t));
		__dtfl_vec_push(&pc->entries, NULL);
		base = (__dtfl_port_conn_entry *)(void *)pc->entries.data;
		memmove(base + lo + 1, base + lo,
		    (pc->entries.len - lo - 1) * sizeof fresh);
		base[lo] = fresh;
	}
	e = DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, lo);
	(void)summary_antichain_insert(pc->tc, &e->summaries, summary);
}

const uint64_t *
__dtfl_port_conn_get(const __dtfl_port_conn *pc, size_t port, size_t *n)
{
	size_t i;

	for (i = 0; i < pc->entries.len; i++) {
		const __dtfl_port_conn_entry *e =
		    DTFL_VEC_AT(&pc->entries, __dtfl_port_conn_entry, i);

		if (e->port == port) {
			*n = e->summaries.len / summary_words(pc->tc);
			return (const uint64_t *)(void *)e->summaries.data;
		}
		if (e->port > port)
			break;
	}
	*n = 0;
	return NULL;
}

void
__dtfl_connectivity_init(__dtfl_connectivity *c, const dtfl_time_class *tc,
    size_t len)
{
	size_t i;

	c->len = len;
	c->ports = __dtfl_calloc(len == 0 ? 1 : len, sizeof *c->ports);
	for (i = 0; i < len; i++)
		__dtfl_port_conn_init(&c->ports[i], tc);
}

void
__dtfl_connectivity_free(__dtfl_connectivity *c)
{
	size_t i;

	if (c->ports == NULL)
		return;
	for (i = 0; i < c->len; i++)
		__dtfl_port_conn_free(&c->ports[i]);
	__dtfl_free(c->ports);
	c->ports = NULL;
	c->len = 0;
}

void
__dtfl_connectivity_identity(__dtfl_connectivity *c, const dtfl_time_class *tc,
    size_t inputs, size_t outputs)
{
	uint64_t s[DTFL_TIME_MAX + 1];
	size_t i, o;

	dtfl_summary_default(tc, s);
	__dtfl_connectivity_init(c, tc, inputs);
	for (i = 0; i < inputs; i++)
		for (o = 0; o < outputs; o++)
			__dtfl_port_conn_insert(&c->ports[i], o, s);
}

__dtfl_shared_progress *
__dtfl_shared_progress_new(const dtfl_time_class *tc, size_t inputs,
    size_t outputs)
{
	__dtfl_shared_progress *sp = __dtfl_calloc(1, sizeof *sp);
	size_t i;

	sp->tc = tc;
	sp->inputs = inputs;
	sp->outputs = outputs;
	sp->frontiers = __dtfl_calloc(DTFL_MAX(inputs, 1), sizeof(__dtfl_change_batch));
	sp->consumeds = __dtfl_calloc(DTFL_MAX(inputs, 1), sizeof(__dtfl_change_batch));
	sp->internals = __dtfl_calloc(DTFL_MAX(outputs, 1), sizeof(__dtfl_change_batch));
	sp->produceds = __dtfl_calloc(DTFL_MAX(outputs, 1), sizeof(__dtfl_change_batch));
	for (i = 0; i < inputs; i++) {
		__dtfl_cb_init(&sp->frontiers[i], tc->depth);
		__dtfl_cb_init(&sp->consumeds[i], tc->depth);
	}
	for (i = 0; i < outputs; i++) {
		__dtfl_cb_init(&sp->internals[i], tc->depth);
		__dtfl_cb_init(&sp->produceds[i], tc->depth);
	}
	return sp;
}

void
__dtfl_shared_progress_free(__dtfl_shared_progress *sp)
{
	size_t i;

	if (sp == NULL)
		return;
	for (i = 0; i < sp->inputs; i++) {
		__dtfl_cb_free(&sp->frontiers[i]);
		__dtfl_cb_free(&sp->consumeds[i]);
	}
	for (i = 0; i < sp->outputs; i++) {
		__dtfl_cb_free(&sp->internals[i]);
		__dtfl_cb_free(&sp->produceds[i]);
	}
	__dtfl_free(sp->frontiers);
	__dtfl_free(sp->consumeds);
	__dtfl_free(sp->internals);
	__dtfl_free(sp->produceds);
	__dtfl_free(sp);
}
