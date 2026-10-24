/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/capture.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/capture.c: capture and replay of differential collections, the
 * CDC V2 protocol (capture.rs).
 *
 * The protocol records facts that once true stay true: Updates messages,
 * each an irrevocable (data, time, diff) change, and Progress messages,
 * each an exact count of distinct updates over an interval (lower, upper].
 * A replay reconstructs the original multiset even when the message
 * stream duplicates and reorders messages, because every message is a
 * monotone statement that can be deduplicated and reordered.
 *
 * dtfl_cdc_log is the recorded Message stream (capture.rs Message).
 * dtfl_collection_capture is the sink (sink::build): it records each
 * update of a consolidated collection as a one-triple Updates message
 * and each strict advance of the input frontier as a Progress message
 * carrying the per-time counts that just closed.  dtfl_cdc_iter is the
 * single-threaded, deduplicating, reordering reader (iterator::Iter):
 * the reference against which the data-parallel source::build is built.
 */
#include "dd/collection_int.h"

#include "progress/progress_int.h"
#include "base/base_int.h"

#include "dtfl/dd_capture.h"
#include "dtfl/record.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

/* ------------------------------------------------------------------ */
/* The message log                                                     */
/* ------------------------------------------------------------------ */

/*
 * One recorded message.  An Updates message holds exactly one triple
 * (as the capturer emits one triple per update); its record bytes live
 * in `rec` with length `reclen`, its time in `time` (tw words), its diff
 * in `diff` (dw bytes).  A Progress message holds `lower`/`upper`
 * antichains (nlower/nupper times of tw words) and `counts` entries, the
 * times packed tw words each in `ctimes` and the counts in `ccounts`.
 */
typedef struct cdc_msg {
	dtfl_cdc_kind kind;
	/* Updates */
	__dtfl_vec rec;		/* record bytes (byte elems) */
	uint64_t time[DTFL_TIME_MAX];
	__dtfl_vec diff;	/* diff bytes (byte elems) */
	/* Progress */
	__dtfl_vec lower;	/* uint64_t elems: nlower * tw words */
	__dtfl_vec upper;	/* uint64_t elems: nupper * tw words */
	__dtfl_vec ctimes;	/* uint64_t elems: ncounts * tw words */
	__dtfl_vec ccounts;	/* size_t elems */
} cdc_msg;

struct dtfl_cdc_log {
	const dtfl_type *rec;		/* retained */
	const dtfl_time_class *tc;	/* retained */
	dtfl_diff *diff;		/* retained */
	unsigned tw;			/* time words */
	size_t dw;			/* diff width */
	__dtfl_vec msgs;		/* cdc_msg elems */
};

static void
msg_init(cdc_msg *m)
{
	m->kind = DTFL_CDC_UPDATES;
	__dtfl_vec_init(&m->rec, 1);
	__dtfl_vec_init(&m->diff, 1);
	__dtfl_vec_init(&m->lower, sizeof(uint64_t));
	__dtfl_vec_init(&m->upper, sizeof(uint64_t));
	__dtfl_vec_init(&m->ctimes, sizeof(uint64_t));
	__dtfl_vec_init(&m->ccounts, sizeof(size_t));
	memset(m->time, 0, sizeof m->time);
}

static void
msg_free(cdc_msg *m)
{
	__dtfl_vec_free(&m->rec);
	__dtfl_vec_free(&m->diff);
	__dtfl_vec_free(&m->lower);
	__dtfl_vec_free(&m->upper);
	__dtfl_vec_free(&m->ctimes);
	__dtfl_vec_free(&m->ccounts);
}

dtfl_cdc_log *
dtfl_cdc_log_new(const dtfl_type *rec, const dtfl_time_class *tc,
    const dtfl_diff *diff)
{
	dtfl_cdc_log *log = __dtfl_alloc(sizeof *log);

	log->rec = dtfl_type_retain(rec);
	log->tc = dtfl_time_class_retain(tc);
	log->diff = dtfl_diff_retain(diff);
	log->tw = dtfl_time_depth(tc);
	log->dw = dtfl_diff_width(diff);
	__dtfl_vec_init(&log->msgs, sizeof(cdc_msg));
	return log;
}

void
dtfl_cdc_log_free(dtfl_cdc_log *log)
{
	size_t i;

	if (log == NULL)
		return;
	for (i = 0; i < log->msgs.len; i++)
		msg_free(DTFL_VEC_AT(&log->msgs, cdc_msg, i));
	__dtfl_vec_free(&log->msgs);
	dtfl_type_free(log->rec);
	dtfl_time_class_free(log->tc);
	dtfl_diff_free(log->diff);
	__dtfl_free(log);
}

size_t
dtfl_cdc_log_len(const dtfl_cdc_log *log)
{
	return log->msgs.len;
}

dtfl_cdc_kind
dtfl_cdc_log_kind(const dtfl_cdc_log *log, size_t i)
{
	return DTFL_VEC_AT(&log->msgs, cdc_msg, i)->kind;
}

void
dtfl_cdc_log_push_update(dtfl_cdc_log *log, const void *rec, size_t reclen,
    const uint64_t *time, const void *diff)
{
	cdc_msg m;

	msg_init(&m);
	m.kind = DTFL_CDC_UPDATES;
	if (reclen != 0)
		__dtfl_vec_push_n(&m.rec, rec, reclen);
	if (log->tw != 0)
		memcpy(m.time, time, (size_t)log->tw * sizeof(uint64_t));
	if (log->dw != 0)
		__dtfl_vec_push_n(&m.diff, diff, log->dw);
	__dtfl_vec_push(&log->msgs, &m);
}

void
dtfl_cdc_log_push_progress(dtfl_cdc_log *log, const uint64_t *lower,
    size_t nlower, const uint64_t *upper, size_t nupper,
    const uint64_t *count_times, const size_t *counts, size_t ncounts)
{
	cdc_msg m;
	unsigned tw = log->tw;

	msg_init(&m);
	m.kind = DTFL_CDC_PROGRESS;
	if (nlower != 0 && tw != 0)
		__dtfl_vec_push_n(&m.lower, lower, nlower * tw);
	if (nupper != 0 && tw != 0)
		__dtfl_vec_push_n(&m.upper, upper, nupper * tw);
	if (ncounts != 0) {
		if (tw != 0)
			__dtfl_vec_push_n(&m.ctimes, count_times, ncounts * tw);
		__dtfl_vec_push_n(&m.ccounts, counts, ncounts);
	}
	__dtfl_vec_push(&log->msgs, &m);
}

void
dtfl_cdc_log_append(dtfl_cdc_log *dst, const dtfl_cdc_log *src, size_t i)
{
	const cdc_msg *s = DTFL_VEC_AT(&src->msgs, cdc_msg, i);
	cdc_msg m;

	msg_init(&m);
	m.kind = s->kind;
	memcpy(m.time, s->time, sizeof m.time);
	__dtfl_vec_copy(&m.rec, &s->rec);
	__dtfl_vec_copy(&m.diff, &s->diff);
	__dtfl_vec_copy(&m.lower, &s->lower);
	__dtfl_vec_copy(&m.upper, &s->upper);
	__dtfl_vec_copy(&m.ctimes, &s->ctimes);
	__dtfl_vec_copy(&m.ccounts, &s->ccounts);
	__dtfl_vec_push(&dst->msgs, &m);
}

/* ------------------------------------------------------------------ */
/* The sink (capture.rs sink::build)                                   */
/* ------------------------------------------------------------------ */

/*
 * The capturer tracks the per-time counts of updates seen but not yet
 * announced, and the frontier through which it has announced.  On each
 * frontier advance it emits a Progress over (announced, new] carrying
 * every count whose time has closed (is not beyond the new frontier),
 * mirroring sink::build's UpdatesWriter (records each update and its
 * timestamp count) and ProgressWriter (announces counts as the frontier
 * passes them).
 */
typedef struct capture_ctx {
	dtfl_cdc_log *log;		/* not owned */
	const dtfl_time_class *tc;
	unsigned tw;
	size_t dw;
	__dtfl_change_batch counts;	/* per-time update counts (unannounced) */
	__dtfl_antichain announced;	/* frontier already announced */
	bool have_announced;
} capture_ctx;

static void
capture_logic(void *vctx, const dtfl_message *msg, const uint64_t *frontier,
    size_t nfrontier)
{
	capture_ctx *c = vctx;

	if (msg != NULL) {
		/*
		 * An Updates batch.  A collection update record is laid out as
		 * data || time || diff (collection_int.h): the time is embedded
		 * in the record, not the message time, and the diff follows it.
		 * Decode each triple the way dtfl_collection_inspect does.
		 */
		size_t n = dtfl_message_len(msg), i;

		for (i = 0; i < n; i++) {
			size_t len;
			const void *rec = dtfl_message_record(msg, i, &len);
			size_t rl = __dtfl_update_rec_len(c->tw, c->dw, len);
			uint64_t tbuf[DTFL_TIME_MAX];
			const uint64_t *et = __dtfl_update_time_copy(rec, c->tw,
			    c->dw, len, tbuf);
			const void *diff = c->dw != 0 ?
			    __dtfl_update_diff(rec, c->dw, len) : NULL;

			dtfl_cdc_log_push_update(c->log, rec, rl, et, diff);
			__dtfl_cb_update(&c->counts, et, 1);
		}
		return;
	}

	/*
	 * A frontier advance: announce the counts for times that have now
	 * closed, as a Progress over (announced, frontier].  Only emit when
	 * the frontier strictly advances past the announced one, as upstream
	 * ProgressWriter does (less_than before producing an utterance).
	 */
	{
		__dtfl_antichain newf;
		bool advanced;
		size_t i, nentries, kept = 0;
		__dtfl_vec ctimes, ccounts;
		__dtfl_change_batch retain;

		__dtfl_antichain_init(&newf, c->tc);
		for (i = 0; i < nfrontier; i++)
			__dtfl_antichain_insert(&newf,
			    frontier + (size_t)i * c->tw);

		if (!c->have_announced) {
			uint64_t minimum[DTFL_TIME_MAX];

			dtfl_time_minimum(c->tc, minimum);
			__dtfl_antichain_init_elem(&c->announced, c->tc,
			    minimum);
			c->have_announced = true;
		}

		/*
		 * Strict advance (Antichain::less_than): the announced frontier
		 * is less_equal to the new one (announced dominates new: every
		 * element of new is >= some announced element) and they differ.
		 */
		advanced = !__dtfl_antichain_eq(&c->announced, &newf) &&
		    __dtfl_antichain_dominates(&c->announced, &newf);
		if (!advanced) {
			__dtfl_antichain_free(&newf);
			return;
		}

		/* Partition counts: those closed by newf announce, rest retain. */
		__dtfl_vec_init(&ctimes, sizeof(uint64_t));
		__dtfl_vec_init(&ccounts, sizeof(size_t));
		__dtfl_cb_init(&retain, c->tw);
		nentries = __dtfl_cb_iter(&c->counts);
		for (i = 0; i < nentries; i++) {
			const uint64_t *t = __dtfl_cb_key(&c->counts, i);
			int64_t cnt = __dtfl_cb_delta(&c->counts, i);

			if (cnt == 0)
				continue;
			if (!__dtfl_antichain_less_equal(&newf, t)) {
				size_t uc = (size_t)cnt;

				if (c->tw != 0)
					__dtfl_vec_push_n(&ctimes, t, c->tw);
				__dtfl_vec_push(&ccounts, &uc);
				kept++;
			} else {
				__dtfl_cb_update(&retain, t, cnt);
			}
		}

		dtfl_cdc_log_push_progress(c->log,
		    (const uint64_t *)(void *)c->announced.elems.data,
		    __dtfl_antichain_len(&c->announced),
		    (const uint64_t *)(void *)newf.elems.data,
		    __dtfl_antichain_len(&newf),
		    kept != 0 ? (const uint64_t *)(void *)ctimes.data : NULL,
		    kept != 0 ? (const size_t *)(void *)ccounts.data : NULL, kept);

		/* The retained counts become the new unannounced set. */
		__dtfl_cb_clear(&c->counts);
		__dtfl_cb_extend(&c->counts, &retain);
		__dtfl_cb_free(&retain);

		/* Advance the announced frontier. */
		__dtfl_antichain_copy(&c->announced, &newf);

		__dtfl_vec_free(&ctimes);
		__dtfl_vec_free(&ccounts);
		__dtfl_antichain_free(&newf);
	}
}

static void
capture_free(void *vctx)
{
	capture_ctx *c = vctx;

	__dtfl_cb_free(&c->counts);
	if (c->have_announced)
		__dtfl_antichain_free(&c->announced);
	__dtfl_free(c);
}

dtfl_collection *
dtfl_collection_capture(dtfl_collection *c, dtfl_cdc_log *log)
{
	capture_ctx *cc = __dtfl_alloc(sizeof *cc);
	dtfl_stream *st;

	cc->log = log;
	cc->tc = log->tc;
	cc->tw = log->tw;
	cc->dw = log->dw;
	__dtfl_cb_init(&cc->counts, cc->tw);
	cc->have_announced = false;
	st = dtfl_stream_inspect_core(c->stream, capture_logic,
	    DTFL_CTX(cc, capture_free));
	return __dtfl_as_collection(st, dtfl_type_retain(c->type),
	    dtfl_diff_retain(c->diff), c->depth);
}

/* ------------------------------------------------------------------ */
/* The replay iterator (capture.rs iterator::Iter)                     */
/* ------------------------------------------------------------------ */

/* One deduplicated update held in the iterator's update set. */
typedef struct iter_upd {
	__dtfl_vec rec;		/* record bytes */
	uint64_t time[DTFL_TIME_MAX];
	__dtfl_vec diff;	/* diff bytes */
} iter_upd;

struct dtfl_cdc_iter {
	const dtfl_type *rec;		/* retained */
	const dtfl_time_class *tc;	/* retained */
	dtfl_diff *diff;		/* retained */
	unsigned tw;
	size_t dw;
	/* Updates received but still beyond reported_frontier (dedup set). */
	__dtfl_vec updates;		/* iter_upd elems */
	/* Frontiers (frontier.rs Antichain / MutableAntichain). */
	__dtfl_antichain reported;	/* reported_frontier */
	__dtfl_antichain progress;	/* progress_frontier */
	__dtfl_mutable_antichain messages;	/* messages_frontier */
	/* Pending progress statements not yet actionable. */
	__dtfl_vec queue;		/* cdc_msg elems (progress only) */
	/* Scratch for the last drained batch's frontier (returned to caller). */
	__dtfl_vec last_frontier;	/* uint64_t elems */
};

dtfl_cdc_iter *
dtfl_cdc_iter_new(const dtfl_type *rec, const dtfl_time_class *tc,
    const dtfl_diff *diff)
{
	dtfl_cdc_iter *it = __dtfl_alloc(sizeof *it);
	uint64_t minimum[DTFL_TIME_MAX];

	it->rec = dtfl_type_retain(rec);
	it->tc = dtfl_time_class_retain(tc);
	it->diff = dtfl_diff_retain(diff);
	it->tw = dtfl_time_depth(tc);
	it->dw = dtfl_diff_width(diff);
	__dtfl_vec_init(&it->updates, sizeof(iter_upd));
	dtfl_time_minimum(tc, minimum);
	__dtfl_antichain_init_elem(&it->reported, tc, minimum);
	__dtfl_antichain_init_elem(&it->progress, tc, minimum);
	__dtfl_mantichain_init(&it->messages, tc);
	__dtfl_vec_init(&it->queue, sizeof(cdc_msg));
	__dtfl_vec_init(&it->last_frontier, sizeof(uint64_t));
	return it;
}

void
dtfl_cdc_iter_free(dtfl_cdc_iter *it)
{
	size_t i;

	if (it == NULL)
		return;
	for (i = 0; i < it->updates.len; i++) {
		iter_upd *u = DTFL_VEC_AT(&it->updates, iter_upd, i);

		__dtfl_vec_free(&u->rec);
		__dtfl_vec_free(&u->diff);
	}
	__dtfl_vec_free(&it->updates);
	for (i = 0; i < it->queue.len; i++)
		msg_free(DTFL_VEC_AT(&it->queue, cdc_msg, i));
	__dtfl_vec_free(&it->queue);
	__dtfl_antichain_free(&it->reported);
	__dtfl_antichain_free(&it->progress);
	__dtfl_mantichain_free(&it->messages);
	__dtfl_vec_free(&it->last_frontier);
	dtfl_type_free(it->rec);
	dtfl_time_class_free(it->tc);
	dtfl_diff_free(it->diff);
	__dtfl_free(it);
}

/* True if update u equals (rec, time, diff): the dedup identity (D,T,R). */
static bool
upd_eq(const dtfl_cdc_iter *it, const iter_upd *u, const void *rec,
    size_t reclen, const uint64_t *time, const void *diff)
{
	if (u->rec.len != reclen)
		return false;
	if (reclen != 0 && memcmp(u->rec.data, rec, reclen) != 0)
		return false;
	if (it->tw != 0 &&
	    memcmp(u->time, time, (size_t)it->tw * sizeof(uint64_t)) != 0)
		return false;
	if (it->dw != 0 && memcmp(u->diff.data, diff, it->dw) != 0)
		return false;
	return true;
}

static bool
iter_contains(const dtfl_cdc_iter *it, const void *rec, size_t reclen,
    const uint64_t *time, const void *diff)
{
	size_t i;

	for (i = 0; i < it->updates.len; i++) {
		if (upd_eq(it, DTFL_VEC_AT(&it->updates, iter_upd, i), rec,
		    reclen, time, diff))
			return true;
	}
	return false;
}

void
dtfl_cdc_iter_push(dtfl_cdc_iter *it, const dtfl_cdc_log *log, size_t i)
{
	const cdc_msg *m = DTFL_VEC_AT(&log->msgs, cdc_msg, i);

	if (m->kind == DTFL_CDC_UPDATES) {
		const void *rec = m->rec.len != 0 ? m->rec.data : NULL;
		size_t reclen = m->rec.len;
		const void *diff = it->dw != 0 ? m->diff.data : NULL;
		iter_upd u;

		/*
		 * Discard updates at reported times, or duplicates still beyond
		 * the reported frontier.  (less_equal(reported, time) &&
		 * !updates.contains(dtr)).
		 */
		if (!__dtfl_antichain_less_equal(&it->reported, m->time))
			return;
		if (iter_contains(it, rec, reclen, m->time, diff))
			return;
		/* Decrement the accounted-for message count at this time. */
		__dtfl_mantichain_update(&it->messages, m->time, -1);
		/* Record the update in the dedup set. */
		__dtfl_vec_init(&u.rec, 1);
		if (reclen != 0)
			__dtfl_vec_push_n(&u.rec, rec, reclen);
		memset(u.time, 0, sizeof u.time);
		if (it->tw != 0)
			memcpy(u.time, m->time,
			    (size_t)it->tw * sizeof(uint64_t));
		__dtfl_vec_init(&u.diff, 1);
		if (it->dw != 0)
			__dtfl_vec_push_n(&u.diff, diff, it->dw);
		__dtfl_vec_push(&it->updates, &u);
	} else {
		/* A progress statement may not be immediately actionable. */
		cdc_msg q;

		msg_init(&q);
		q.kind = DTFL_CDC_PROGRESS;
		__dtfl_vec_copy(&q.lower, &m->lower);
		__dtfl_vec_copy(&q.upper, &m->upper);
		__dtfl_vec_copy(&q.ctimes, &m->ctimes);
		__dtfl_vec_copy(&q.ccounts, &m->ccounts);
		__dtfl_vec_push(&it->queue, &q);
	}
}

/* The lower bound of a progress message (as a flat run of tw-word times). */
static const uint64_t *
msg_lower(const cdc_msg *m, size_t *n, unsigned tw)
{
	*n = tw != 0 ? m->lower.len / tw : (m->lower.len != 0 ? 1 : 0);
	return (const uint64_t *)(void *)m->lower.data;
}

static const uint64_t *
msg_upper(const cdc_msg *m, size_t *n, unsigned tw)
{
	*n = tw != 0 ? m->upper.len / tw : (m->upper.len != 0 ? 1 : 0);
	return (const uint64_t *)(void *)m->upper.data;
}

/*
 * Drain actionable progress messages into the frontiers.  A progress
 * message is actionable once progress_frontier is beyond its lower
 * bound: less_equal(lower, progress_frontier), i.e. every element of the
 * progress frontier is >= some element of the message's lower antichain
 * (Antichain::dominates(lower, progress)).  Mirrors Iter::next's inner
 * while loop over progress_queue.
 */
static void
iter_drain_progress(dtfl_cdc_iter *it)
{
	bool acted = true;

	while (acted) {
		size_t i, pos = SIZE_MAX;

		acted = false;
		for (i = 0; i < it->queue.len; i++) {
			const cdc_msg *p = DTFL_VEC_AT(&it->queue, cdc_msg, i);
			size_t nl, j;
			const uint64_t *lower = msg_lower(p, &nl, it->tw);
			__dtfl_antichain lac;
			bool actionable;

			__dtfl_antichain_init(&lac, it->tc);
			for (j = 0; j < nl; j++)
				__dtfl_antichain_insert(&lac,
				    lower + (size_t)j * it->tw);
			actionable = __dtfl_antichain_dominates(&lac,
			    &it->progress);
			__dtfl_antichain_free(&lac);
			if (actionable) {
				pos = i;
				break;
			}
		}
		if (pos == SIZE_MAX)
			break;

		{
			cdc_msg p;
			size_t nu, j, nentries;
			const uint64_t *upper;
			__dtfl_antichain newf;

			/* Remove the actionable statement from the queue. */
			p = *DTFL_VEC_AT(&it->queue, cdc_msg, pos);
			__dtfl_vec_remove(&it->queue, pos);

			/* Incorporate counts for times at/above progress. */
			nentries = p.ccounts.len;
			for (j = 0; j < nentries; j++) {
				const uint64_t *t = (const uint64_t *)(void *)
				    p.ctimes.data + (size_t)j * it->tw;
				size_t cnt = *DTFL_VEC_AT(&p.ccounts, size_t, j);

				if (__dtfl_antichain_less_equal(&it->progress, t))
					__dtfl_mantichain_update(&it->messages,
					    t, (int64_t)cnt);
			}

			/* new_frontier = join(upper, progress) elementwise. */
			upper = msg_upper(&p, &nu, it->tw);
			__dtfl_antichain_init(&newf, it->tc);
			for (j = 0; j < nu; j++) {
				size_t k;
				const uint64_t *u1 = upper +
				    (size_t)j * it->tw;

				for (k = 0; k < __dtfl_antichain_len(
				    &it->progress); k++) {
					uint64_t jn[DTFL_TIME_MAX];

					dtfl_time_join(it->tc, u1,
					    __dtfl_antichain_at(&it->progress,
					    k), jn);
					__dtfl_antichain_insert(&newf, jn);
				}
			}
			__dtfl_antichain_copy(&it->progress, &newf);
			__dtfl_antichain_free(&newf);
			msg_free(&p);
			acted = true;
		}
	}
}

bool
dtfl_cdc_iter_next(dtfl_cdc_iter *it, dtfl_cdc_update_fn fn, void *ctx,
    const uint64_t **frontier, size_t *nfrontier)
{
	__dtfl_antichain lower;
	size_t i, fl;
	const uint64_t *ff;

	iter_drain_progress(it);

	/*
	 * lower_bound = progress_frontier extended with messages_frontier.
	 * If it differs from reported_frontier, publish the updates no longer
	 * beyond it and advance reported_frontier.
	 */
	__dtfl_antichain_init(&lower, it->tc);
	for (i = 0; i < __dtfl_antichain_len(&it->progress); i++)
		__dtfl_antichain_insert(&lower,
		    __dtfl_antichain_at(&it->progress, i));
	fl = __dtfl_mantichain_frontier_len(&it->messages);
	ff = __dtfl_mantichain_frontier(&it->messages);
	for (i = 0; i < fl; i++)
		__dtfl_antichain_insert(&lower, ff + (size_t)i * it->tw);

	if (__dtfl_antichain_eq(&lower, &it->reported)) {
		__dtfl_antichain_free(&lower);
		return false;
	}

	/* Publish updates whose time is now not beyond lower, and retain the rest. */
	{
		size_t w = 0;

		for (i = 0; i < it->updates.len; i++) {
			iter_upd *u = DTFL_VEC_AT(&it->updates, iter_upd, i);

			if (!__dtfl_antichain_less_equal(&lower, u->time)) {
				const void *rec = u->rec.len != 0 ?
				    u->rec.data : NULL;
				const void *diff = it->dw != 0 ?
				    u->diff.data : NULL;

				fn(ctx, rec, u->rec.len, u->time, diff);
				__dtfl_vec_free(&u->rec);
				__dtfl_vec_free(&u->diff);
			} else {
				if (w != i)
					*DTFL_VEC_AT(&it->updates, iter_upd, w) =
					    *u;
				w++;
			}
		}
		__dtfl_vec_truncate(&it->updates, w);
	}

	/* reported_frontier = lower; return it to the caller. */
	__dtfl_antichain_copy(&it->reported, &lower);
	__dtfl_vec_clear(&it->last_frontier);
	for (i = 0; i < __dtfl_antichain_len(&lower); i++)
		__dtfl_vec_push_n(&it->last_frontier,
		    __dtfl_antichain_at(&lower, i), it->tw);
	if (frontier != NULL)
		*frontier = (const uint64_t *)(void *)it->last_frontier.data;
	if (nfrontier != NULL)
		*nfrontier = __dtfl_antichain_len(&lower);
	__dtfl_antichain_free(&lower);
	return true;
}
