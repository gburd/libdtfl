/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/join.rs
 *   differential-dataflow/src/operators/arrange/arrangement.rs (join_core)
 *   differential-dataflow/src/collection.rs (join, join_map, semijoin, antijoin)
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/join.c: the join family of differential operators.
 *
 * join_traces drives an equijoin of two arranged traces: each novel batch
 * on one input is joined against the accumulated batches of the other,
 * key by key, in lockstep.  For a key present in both, every (val1, time1,
 * diff1) is matched with every (val2, time2, diff2) to yield (result(key,
 * val1, val2), time1 join time2, diff1 * diff2), consolidated by a
 * ConsolidatingContainerBuilder (common.c holds the two-history replay and
 * the time-ordered thinker).  The per-activation work is fuel-bounded to
 * 2,000,000 output records, split 1,000,000 per input so a burst on one
 * cannot starve the other, with the operator rescheduling itself while any
 * deferred work remains (join.rs join_with_tactic).  The per-unit work goes
 * through a JoinTactic seam (join_tactic_ops): the cursor tactic
 * (deferred_step) is the default, and int_proxy's ProxyJoinTactic
 * (src/dd/int_proxy_join.c) is reached only through
 * __dtfl_arranged_join_with_proxy.
 *
 * The collection methods arrange both inputs by key and call join_core:
 * join yields (key, (val1, val2)); join_map applies a result closure;
 * semijoin arranges the second input by self (key -> unit) and keeps
 * (key, val1); antijoin is self concat self.semijoin(other).negate().
 *
 * Simplification: the C arrange and join operators are built together over
 * a trace that starts empty, so there are no preloaded batches to skip and
 * no in-flight batch whose updates a concurrent merge could consolidate
 * away before the join reads them.  The elaborate preload/advance_upper
 * boundary bookkeeping of upstream join_traces (which guards a trace shared
 * with an already-running writer) is therefore not reproduced; the
 * acknowledged frontiers that make each (batch1, batch2) pair join exactly
 * once are.  A join over an imported, already-populated trace would need
 * the preload step added here.
 */
#include "dd/arrange_int.h"
#include "dd/common_int.h"
#include "dd/int_proxy_int.h"
#include "dd/wrap_int.h"
#include "dtfl/arrange.h"
#include "dtfl/collection.h"
#include "progress/time_int.h"
#include "timely/timely_int.h"

/* The 2,000,000 output-record fuel per activation (join.rs). */
#define DTFL_JOIN_FUEL 2000000

/* ------------------------------------------------------------------ */
/* The join result sink.                                               */
/* ------------------------------------------------------------------ */

/*
 * A sink the result closure writes output records through; the operator
 * attaches the joined time and the multiplied diff to each and feeds the
 * (record, time, diff) triple into the deferred unit's consolidator.
 */
struct dtfl_join_emit {
	__dtfl_consolidator *con;
	const uint64_t *time;		/* joined time of the current product */
	const void *diff;		/* multiplied diff of the current product */
};

void
dtfl_join_emit_record(dtfl_join_emit *e, const void *rec, size_t len)
{
	__dtfl_consolidator_push(e->con, rec, len, e->time, e->diff);
}

/* ------------------------------------------------------------------ */
/* Deferred join work (join.rs cursors::DeferredIter).                 */
/* ------------------------------------------------------------------ */

/*
 * One unit of deferred join work: the two cursor lists (novel side and
 * accumulated side), the capability its output ships under, and its own
 * consolidator (one builder per unit, as upstream's DeferredIter, so
 * products at one capability never mix with another's).  `meet` is the
 * capability time; the accumulated side's history is advanced by it before
 * the cross product (advance1/advance2 fix which side is accumulated).
 * `done` is set once the merge-join runs the key lockstep to completion;
 * `flushed` once the final partial container has been finished out.
 */
typedef struct deferred {
	dtfl_cap *cap;			/* retained; output capability */
	__dtfl_cursor_list cursor1;	/* input0 side */
	__dtfl_cursor_list cursor2;	/* input1 side */
	bool advance1;			/* advance history1 by meet */
	bool advance2;			/* advance history2 by meet */
	uint64_t meet[DTFL_TIME_MAX];	/* capability time */
	__dtfl_consolidator con;	/* this unit's output builder */
	bool done;			/* key lockstep exhausted */
} deferred;

/*
 * The join operator's state: cloned readers for both traces, the result
 * closure, the output record/diff descriptors, the two work queues, and
 * the acknowledged frontiers that keep each (batch1, batch2) pair joined
 * once.  Two reusable value histories thread through every key so the
 * replay buffers are reset rather than reallocated per key.
 */
typedef struct join_ctx {
	__dtfl_trace_agent reader1;
	__dtfl_trace_agent reader2;
	dtfl_join_result_fn result;
	void *user;
	void (*user_free)(void *);
	const dtfl_type *out_type;	/* retained */
	const __dtfl_diff_class *odc;	/* output diff class */
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc1;
	const __dtfl_diff_class *dc2;
	const dtfl_type *kt;		/* shared key type */
	unsigned depth;
	size_t odw;
	__dtfl_value_history h1;
	__dtfl_value_history h2;
	__dtfl_vec todo0;		/* deferred *, fresh = input0 */
	__dtfl_vec todo1;		/* deferred *, fresh = input1 */
	__dtfl_antichain acknowledged1;
	__dtfl_antichain acknowledged2;
	dtfl_activator *activator;	/* owned; obtained lazily */
	bool activator_set;
	const struct join_tactic_ops *tactic;	/* JoinTactic vtable */
	void *tactic_state;		/* owned; freed by tactic->free */
} join_ctx;

/*
 * The JoinTactic seam (join.rs JoinTactic::prep): `step` plays one deferred
 * unit forward, pushing its products into d->con, and sets d->done once the
 * unit is exhausted; it may return early once a container has filled.  The
 * cursor tactic is deferred_step below; int_proxy's ProxyJoinTactic is the
 * second.  The driver (drive_queue) ships d->con's containers either way.
 */
typedef struct join_tactic_ops {
	void (*step)(join_ctx *jc, deferred *d);
	void (*free)(void *state);
} join_tactic_ops;

/* The per-product emit callback the thinker fires (common.c join driver). */
typedef struct think_ctx {
	join_ctx *jc;
	deferred *d;
	const void *key;
	size_t klen;
} think_ctx;

static void
join_product(void *vctx, const void *v1, size_t v1len, const void *v2,
    size_t v2len, const uint64_t *time, const void *d1, const void *d2)
{
	think_ctx *t = vctx;
	join_ctx *jc = t->jc;
	unsigned char diff[DTFL_COMMON_DIFF_MAX];
	const __dtfl_diff_class *oc;
	dtfl_join_emit e;

	/* diff = d1 * d2 (difference.rs Multiply). */
	if (__dtfl_diff_multiply(jc->dc1, jc->dc2, d1, d2, diff, &oc) != DTFL_OK)
		return;
	e.con = &t->d->con;
	e.time = time;
	e.diff = diff;
	jc->result(jc->user, t->key, t->klen, v1, v1len, v2, v2len, &e);
}

/*
 * Play one deferred unit's merge-join forward (join.rs DeferredIter::next),
 * pushing products into its consolidator, until a full container is ready
 * (break so the driver can ship it under the fuel budget) or the cursors
 * run dry (set done).  The driver finishes the final partial container.
 */
static void
deferred_step(join_ctx *jc, deferred *d)
{
	while (!d->done) {
		const void *k1 = NULL, *k2 = NULL;
		size_t k1len = 0, k2len = 0;
		bool v1, v2;
		int cmp;

		v1 = __dtfl_cursor_list_key_valid(&d->cursor1);
		v2 = __dtfl_cursor_list_key_valid(&d->cursor2);
		if (!v1 || !v2) {
			d->done = true;
			break;
		}
		/*
		 * A valid cursor at a unit (0-length) key returns a NULL data
		 * pointer with length 0, so exhaustion is decided by the
		 * validity flags above, not by the key pointer.
		 */
		k1 = __dtfl_cursor_list_key(&d->cursor1, &k1len);
		k2 = __dtfl_cursor_list_key(&d->cursor2, &k2len);
		cmp = dtfl_type_cmp(jc->kt, k1, k1len, k2, k2len);
		if (cmp < 0) {
			__dtfl_cursor_list_seek_key(&d->cursor1, k2, k2len);
		} else if (cmp > 0) {
			__dtfl_cursor_list_seek_key(&d->cursor2, k1, k1len);
		} else {
			think_ctx t;

			__dtfl_value_history_load_cursor(&jc->h1, &d->cursor1,
			    d->advance1 ? d->meet : NULL);
			__dtfl_value_history_load_cursor(&jc->h2, &d->cursor2,
			    d->advance2 ? d->meet : NULL);
			t.jc = jc;
			t.d = d;
			t.key = k1;
			t.klen = k1len;
			__dtfl_join_think(&jc->h1, &jc->h2, join_product, &t);
			__dtfl_cursor_list_step_key(&d->cursor1);
			__dtfl_cursor_list_step_key(&d->cursor2);
			/* Yield once a container has filled (fuel boundary). */
			if (d->con.outbound.len != 0)
				return;
		}
	}
}

/* The cursor tactic (join.rs cursors::CursorTactic): the default. */
static const join_tactic_ops cursor_join_tactic_ops = { deferred_step, NULL };

/*
 * ProxyJoinTactic (int_proxy/join.rs) on the seam: prep the whole unit at
 * once, as upstream's join_prep returns every container up front, from the
 * batches the unit's two cursor lists hold.
 */
static void
proxy_join_step(join_ctx *jc, deferred *d)
{
	__dtfl_proxy_join_prep(jc->tactic_state, d->cursor1.batches,
	    d->cursor1.n, d->cursor2.batches, d->cursor2.n, d->advance2,
	    d->meet, jc->depth, &d->con);
	d->done = true;
}

static void
proxy_join_free(void *state)
{
	__dtfl_proxy_join_backend *be = state;

	if (be->ops->free != NULL)
		be->ops->free(be->self);
	__dtfl_free(be);
}

static const join_tactic_ops proxy_join_tactic_ops = {
	proxy_join_step, proxy_join_free
};

void
__dtfl_join_result_emit(dtfl_join_result_fn result, void *user,
    const void *key, size_t klen, const void *v1, size_t v1len,
    const void *v2, size_t v2len, const uint64_t *time, const void *diff,
    __dtfl_consolidator *con)
{
	dtfl_join_emit e;

	e.con = con;
	e.time = time;
	e.diff = diff;
	result(user, key, klen, v1, v1len, v2, v2len, &e);
}

/* ------------------------------------------------------------------ */
/* The fuel-bounded driver (join.rs join_with_tactic drain).           */
/* ------------------------------------------------------------------ */

/* Ships one consolidated output batch as (record, time, diff) updates. */
static void
ship_batch(dtfl_op *op, join_ctx *jc, dtfl_cap *cap, const __dtfl_batch *out)
{
	dtfl_session *s = dtfl_op_session(op, 0, cap);
	size_t n = __dtfl_batch_len(out), i;
	size_t tw = (size_t)jc->depth * sizeof(uint64_t);
	size_t dw = jc->odw;

	for (i = 0; i < n; i++) {
		size_t rlen;
		const void *rec = __dtfl_batch_rec(out, i, &rlen);
		const uint64_t *time = __dtfl_batch_time(out, i);
		const void *diff = dw != 0 ? __dtfl_batch_diff(out, i) : NULL;
		unsigned char *dst = __dtfl_session_reserve(s, rlen + tw + dw);

		memcpy(dst, rec, rlen);
		if (tw != 0)
			memcpy(dst + rlen, time, tw);
		if (dw != 0)
			memcpy(dst + rlen + tw, diff, dw);
	}
	dtfl_session_close(s);
}

static void
deferred_free(deferred *d)
{
	__dtfl_cursor_list_free(&d->cursor1);
	__dtfl_cursor_list_free(&d->cursor2);
	__dtfl_consolidator_free(&d->con);
	if (d->cap != NULL)
		dtfl_cap_drop(d->cap);
	__dtfl_free(d);
}

/*
 * Drain one queue against `fuel`: pull completed containers from the front
 * unit's consolidator and ship them under its capability, playing the unit
 * forward as needed; pop a unit once it is done and drained.  Fuel is spent
 * per shipped record.  Returns the fuel left (negative when overspent).
 */
static int64_t
drive_queue(dtfl_op *op, join_ctx *jc, __dtfl_vec *queue, int64_t fuel)
{
	while (fuel >= 0 && queue->len != 0) {
		deferred *d = *DTFL_VEC_AT(queue, deferred *, 0);
		__dtfl_batch *out;

		/* Serve a container completed earlier, else play forward. */
		out = __dtfl_consolidator_extract(&d->con);
		if (out == NULL) {
			if (!d->done)
				jc->tactic->step(jc, d);
			if (d->done)
				out = __dtfl_consolidator_finish(&d->con);
			else
				out = __dtfl_consolidator_extract(&d->con);
		}
		if (out != NULL) {
			size_t n = __dtfl_batch_len(out);

			fuel -= (int64_t)n;
			ship_batch(op, jc, d->cap, out);
			__dtfl_batch_release(out);
			continue;
		}
		/* No more output and done: retire the unit. */
		deferred_free(d);
		__dtfl_vec_remove(queue, 0);
	}
	return fuel;
}

/* ------------------------------------------------------------------ */
/* Work creation and the binary operator logic.                        */
/* ------------------------------------------------------------------ */

/* Initializes a deferred unit's consolidator for the output layout. */
static void
deferred_init_con(join_ctx *jc, deferred *d)
{
	__dtfl_consolidator_init(&d->con, jc->out_type, jc->depth, jc->odc);
}

/*
 * Builds the deferred unit joining `novel` (one freshly-arrived batch, from
 * input `fresh`) against the accumulated opposite trace through its
 * acknowledged frontier.  `fresh` selects which cursor is the novel batch
 * and which is the accumulated trace, and which side is advanced by meet.
 * Retains `cap` and the novel batch; the accumulated cursor retains its own
 * batch references.  Returns NULL (nothing enqueued) if the cut is unclean.
 */
static deferred *
make_deferred(join_ctx *jc, __dtfl_batch_ord *novel, bool fresh_is_input0,
    dtfl_cap *cap)
{
	deferred *d = __dtfl_alloc(sizeof *d);
	__dtfl_batch_ord *one[1];
	const __dtfl_trace_agent *na = fresh_is_input0 ? &jc->reader1
	    : &jc->reader2;
	__dtfl_trace_agent *acc = fresh_is_input0 ? &jc->reader2 : &jc->reader1;
	const __dtfl_antichain *ack = fresh_is_input0 ? &jc->acknowledged2
	    : &jc->acknowledged1;
	__dtfl_cursor_list *novel_cl = fresh_is_input0 ? &d->cursor1
	    : &d->cursor2;
	__dtfl_cursor_list *acc_cl = fresh_is_input0 ? &d->cursor2
	    : &d->cursor1;

	memset(d, 0, sizeof *d);
	d->cap = dtfl_cap_clone(cap);
	memcpy(d->meet, dtfl_cap_time(cap),
	    (size_t)jc->depth * sizeof(uint64_t));
	/*
	 * The accumulated side is advanced by meet to consolidate its
	 * history; the fresh side is left (its times already lie at or beyond
	 * meet).  Input0 fresh -> advance side 2; input1 fresh -> advance
	 * side 1 (join.rs Fresh::Input0 => (false, true)).
	 */
	d->advance1 = !fresh_is_input0;
	d->advance2 = fresh_is_input0;

	/* The novel side: a one-batch cursor list (retain the borrow). */
	one[0] = __dtfl_batch_ord_retain(novel);
	__dtfl_cursor_list_init(novel_cl, one, 1, na->kt, na->vt);

	/* The accumulated side: a cursor through the acknowledged frontier. */
	if (!__dtfl_trace_agent_cursor_through(acc, ack, acc_cl)) {
		/* No clean cut: a whole-trace cursor is a safe superset here. */
		__dtfl_trace_agent_cursor(acc, acc_cl);
	}

	deferred_init_con(jc, d);
	return d;
}

/* Reads a batch pointer out of a stream record. */
static __dtfl_batch_ord *
record_batch(const dtfl_message *m, size_t i)
{
	size_t len;
	const void *recp = dtfl_message_record(m, i, &len);
	__dtfl_batch_ord *b;

	memcpy(&b, recp, sizeof b);
	return b;
}

/* Advances `ack` to the batch's upper (acknowledged frontier update). */
static void
ack_advance(__dtfl_antichain *ack, const __dtfl_batch_ord *b)
{
	const __dtfl_antichain *up = __dtfl_batch_ord_upper(b);
	size_t i;

	__dtfl_antichain_clear(ack);
	for (i = 0; i < __dtfl_antichain_len(up); i++)
		__dtfl_antichain_insert(ack, __dtfl_antichain_at(up, i));
}

static bool
join_logic(dtfl_op *op, void *vctx)
{
	join_ctx *jc = vctx;
	dtfl_message *m;
	int64_t fuel;

	dtfl_cap_drop(dtfl_op_initial_cap(op, 0));
	if (!jc->activator_set) {
		jc->activator = dtfl_op_activator(op);
		jc->activator_set = true;
	}

	/*
	 * Drain input 1 (batch1).  Each non-empty novel batch1 joins against
	 * trace2's accumulated batches (through acknowledged2); then
	 * acknowledged1 advances to batch1.upper so a later batch2 joins it
	 * once.  Input 1 is drained before input 2 within an activation, so a
	 * batch1 does not see a batch2 arriving the same activation (it will
	 * be joined the other way, exactly once).
	 */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 0, 0);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			__dtfl_batch_ord *b = record_batch(m, i);

			if (!__dtfl_batch_ord_is_empty(b)) {
				deferred *d = make_deferred(jc, b, true, cap);

				__dtfl_vec_push(&jc->todo0, &d);
			}
			ack_advance(&jc->acknowledged1, b);
		}
		dtfl_cap_drop(cap);
	}

	/* Drain input 2 (batch2), joined against trace1 through ack1. */
	while ((m = dtfl_op_input_next(op, 1)) != NULL) {
		dtfl_cap *cap = dtfl_op_input_cap(op, 1, 0);
		size_t i, n = dtfl_message_len(m);

		for (i = 0; i < n; i++) {
			__dtfl_batch_ord *b = record_batch(m, i);

			if (!__dtfl_batch_ord_is_empty(b)) {
				deferred *d = make_deferred(jc, b, false, cap);

				__dtfl_vec_push(&jc->todo1, &d);
			}
			ack_advance(&jc->acknowledged2, b);
		}
		dtfl_cap_drop(cap);
	}

	/*
	 * Perform fuel-bounded work: each direction drains half the budget so
	 * a burst on one input cannot starve the other (join.rs splits the
	 * 2,000,000 fuel into 1,000,000 per input).  Reschedule while work
	 * remains.
	 */
	fuel = DTFL_JOIN_FUEL;
	drive_queue(op, jc, &jc->todo0, fuel / 2);
	drive_queue(op, jc, &jc->todo1, fuel / 2);
	if (jc->todo0.len != 0 || jc->todo1.len != 0) {
		if (jc->activator != NULL)
			dtfl_activator_activate(jc->activator);
		return true;
	}

	/*
	 * Trace maintenance: let each trace compact physically up to the
	 * batches we have acknowledged on its input, and logically up to the
	 * opposing input's frontier (join.rs trace maintenance).
	 */
	{
		size_t n1, n2;
		const uint64_t *f1 = dtfl_op_frontier(op, 0, &n1);
		const uint64_t *f2 = dtfl_op_frontier(op, 1, &n2);

		__dtfl_trace_agent_set_logical(&jc->reader1, f2, n2);
		__dtfl_trace_agent_set_physical(&jc->reader1,
		    (const uint64_t *)(void *)jc->acknowledged1.elems.data,
		    __dtfl_antichain_len(&jc->acknowledged1));
		__dtfl_trace_agent_set_logical(&jc->reader2, f1, n1);
		__dtfl_trace_agent_set_physical(&jc->reader2,
		    (const uint64_t *)(void *)jc->acknowledged2.elems.data,
		    __dtfl_antichain_len(&jc->acknowledged2));
	}
	return false;
}

static void
join_free(void *vctx)
{
	join_ctx *jc = vctx;
	size_t i;

	for (i = 0; i < jc->todo0.len; i++)
		deferred_free(*DTFL_VEC_AT(&jc->todo0, deferred *, i));
	for (i = 0; i < jc->todo1.len; i++)
		deferred_free(*DTFL_VEC_AT(&jc->todo1, deferred *, i));
	__dtfl_vec_free(&jc->todo0);
	__dtfl_vec_free(&jc->todo1);
	__dtfl_antichain_free(&jc->acknowledged1);
	__dtfl_antichain_free(&jc->acknowledged2);
	__dtfl_value_history_free(&jc->h1);
	__dtfl_value_history_free(&jc->h2);
	__dtfl_trace_agent_free(&jc->reader1);
	__dtfl_trace_agent_free(&jc->reader2);
	if (jc->activator != NULL)
		dtfl_activator_free(jc->activator);
	if (jc->user_free != NULL)
		jc->user_free(jc->user);
	if (jc->tactic->free != NULL)
		jc->tactic->free(jc->tactic_state);
	dtfl_type_free(jc->out_type);
	__dtfl_free(jc);
}

/* ------------------------------------------------------------------ */
/* dtfl_arranged_join_core (arrangement.rs Arranged::join_core).        */
/* ------------------------------------------------------------------ */

/*
 * join_with_tactic (join.rs): the join operator over two arrangements, its
 * per-unit work routed through `tactic` (which takes tactic_state).
 */
static dtfl_collection *
join_with_tactic(dtfl_arranged *a1, dtfl_arranged *a2,
    const dtfl_type *out_type, dtfl_join_result_fn result, dtfl_ctx ctx,
    const join_tactic_ops *tactic, void *tactic_state)
{
	const __dtfl_trace_agent *g1 = __dtfl_arranged_agent(a1);
	const __dtfl_trace_agent *g2 = __dtfl_arranged_agent(a2);
	const __dtfl_diff_class *dc1 = g1->dc;
	const __dtfl_diff_class *dc2 = g2->dc;
	const __dtfl_diff_class *odc = __dtfl_diff_multiply_class(dc1, dc2);
	const dtfl_time_class *tc = g1->tc;
	unsigned depth = __dtfl_arranged_depth(a1);
	join_ctx *jc;
	dtfl_diff *out_diff;
	const dtfl_type *ut;
	dtfl_op_builder *b;
	dtfl_stream *st;
	uint64_t minimum[DTFL_TIME_MAX] = { 0 };

	if (odc == NULL) {
		(void)__dtfl_err(DTFL_EALGEBRA, "join: no multiply for %s * %s",
		    dc1->name, dc2->name);
		if (tactic->free != NULL)
			tactic->free(tactic_state);
		return NULL;
	}

	jc = __dtfl_alloc(sizeof *jc);
	memset(jc, 0, sizeof *jc);
	__dtfl_trace_agent_clone(g1, &jc->reader1);
	__dtfl_trace_agent_clone(g2, &jc->reader2);
	jc->result = result;
	jc->user = ctx.ptr;
	jc->user_free = ctx.free;
	jc->tactic = tactic;
	jc->tactic_state = tactic_state;
	jc->out_type = dtfl_type_retain(out_type);
	jc->odc = odc;
	jc->odw = odc->width;
	jc->tc = tc;
	jc->dc1 = dc1;
	jc->dc2 = dc2;
	jc->kt = g1->kt;
	jc->depth = depth;
	__dtfl_value_history_init(&jc->h1, tc, dc1);
	__dtfl_value_history_init(&jc->h2, tc, dc2);
	__dtfl_vec_init(&jc->todo0, sizeof(deferred *));
	__dtfl_vec_init(&jc->todo1, sizeof(deferred *));
	__dtfl_antichain_init(&jc->acknowledged1, tc);
	__dtfl_antichain_insert(&jc->acknowledged1, minimum);
	__dtfl_antichain_init(&jc->acknowledged2, tc);
	__dtfl_antichain_insert(&jc->acknowledged2, minimum);

	out_diff = __dtfl_diff_wrap(odc);
	ut = __dtfl_update_type(out_type, depth, odc->width);
	b = dtfl_op_builder_new(dtfl_arranged_scope(a1), "Join");
	dtfl_op_builder_input(b, __dtfl_arranged_stream(a1),
	    dtfl_pact_pipeline());
	dtfl_op_builder_input(b, __dtfl_arranged_stream(a2),
	    dtfl_pact_pipeline());
	dtfl_op_builder_set_notify(b, 0, DTFL_NOTIFY_ALWAYS);
	dtfl_op_builder_set_notify(b, 1, DTFL_NOTIFY_ALWAYS);
	st = dtfl_op_builder_output(b, ut);
	dtfl_op_builder_build(b, join_logic, jc, join_free);
	dtfl_type_free(ut);
	return __dtfl_as_collection(st, out_type, out_diff, depth);
}

dtfl_collection *
dtfl_arranged_join_core(dtfl_arranged *a1, dtfl_arranged *a2,
    const dtfl_type *out_type, dtfl_join_result_fn result, dtfl_ctx ctx)
{
	return join_with_tactic(a1, a2, out_type, result, ctx,
	    &cursor_join_tactic_ops, NULL);
}

/* join_with_tactic(a1, a2, ProxyJoinTactic::new(backend)). */
dtfl_collection *
__dtfl_arranged_join_with_proxy(dtfl_arranged *a1, dtfl_arranged *a2,
    const dtfl_type *out_type, __dtfl_proxy_join_backend backend)
{
	__dtfl_proxy_join_backend *be;

	/* shortcut: bridge rows hold diffs inline, DTFL_COMMON_DIFF_MAX bytes. */
	DTFL_ASSERT(backend.r0->width <= DTFL_COMMON_DIFF_MAX &&
	    backend.r1->width <= DTFL_COMMON_DIFF_MAX &&
	    backend.rout != NULL &&
	    backend.rout->width <= DTFL_COMMON_DIFF_MAX);
	be = __dtfl_alloc(sizeof *be);
	*be = backend;
	return join_with_tactic(a1, a2, out_type, NULL, DTFL_NOCTX,
	    &proxy_join_tactic_ops, be);
}

/* ------------------------------------------------------------------ */
/* Collection-level join family (collection.rs).                       */
/* ------------------------------------------------------------------ */

/*
 * join_map: arrange both inputs by key and join, applying a result closure
 * to each (key, val1, val2).  c1 holds key||val1 records, c2 holds
 * key||val2; `key_type` splits both.  The output records are `out_type`.
 */
dtfl_collection *
dtfl_collection_join_map(dtfl_collection *c1, dtfl_collection *c2,
    const dtfl_type *key_type, const dtfl_type *val1_type,
    const dtfl_type *val2_type, const dtfl_type *out_type,
    dtfl_join_result_fn result, dtfl_ctx ctx)
{
	dtfl_arranged *a1 = __dtfl_collection_arrange_by_key(c1, key_type,
	    val1_type, "Arrange");
	dtfl_arranged *a2 = __dtfl_collection_arrange_by_key(c2, key_type,
	    val2_type, "Arrange");
	dtfl_collection *out = dtfl_arranged_join_core(a1, a2, out_type, result,
	    ctx);

	dtfl_arranged_free(a1);
	dtfl_arranged_free(a2);
	return out;
}

/*
 * memcpy that tolerates a NULL/zero source: a unit (0-byte) key or value
 * is represented by a NULL pointer with length 0, and memcpy(dst, NULL,
 * 0) is undefined behavior (flagged by UBSan).
 */
static inline void
copy_n(void *dst, const void *src, size_t n)
{
	if (n != 0)
		memcpy(dst, src, n);
}

/* The default join result: concatenate key || val1 || val2. */
/*
 * gcc -O2 cannot see that copy_n fills buf[0..total) on the total > 0
 * path (the copies are length-guarded), so it warns maybe-uninitialized
 * on the emit.  The code is correct; silence the known-imprecise warning
 * for these two small result builders.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
static void
join_concat_result(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[4096];
	size_t total = klen + v1len + v2len;
	unsigned char *p;

	(void)ctx;
	if (total == 0) {
		dtfl_join_emit_record(out, NULL, 0);
		return;
	}
	p = total <= sizeof buf ? buf : __dtfl_alloc(total);
	copy_n(p, key, klen);
	copy_n(p + klen, v1, v1len);
	copy_n(p + klen + v1len, v2, v2len);
	dtfl_join_emit_record(out, p, total);
	if (p != buf)
		__dtfl_free(p);
}

/*
 * join: match (key, val1) and (key, val2) by key, yielding key||val1||val2
 * (collection.rs join = join_map(|k,v1,v2| (k,(v1,v2)))).  `out_type`
 * describes the concatenation.
 */
dtfl_collection *
dtfl_collection_join(dtfl_collection *c1, dtfl_collection *c2,
    const dtfl_type *key_type, const dtfl_type *val1_type,
    const dtfl_type *val2_type, const dtfl_type *out_type)
{
	return dtfl_collection_join_map(c1, c2, key_type, val1_type, val2_type,
	    out_type, join_concat_result, DTFL_NOCTX);
}

/* semijoin result: keep key||val1 (drop the unit value of the second). */
static void
semijoin_result(void *ctx, const void *key, size_t klen, const void *v1,
    size_t v1len, const void *v2, size_t v2len, dtfl_join_emit *out)
{
	unsigned char buf[4096];
	size_t total = klen + v1len;
	unsigned char *p;

	(void)ctx; (void)v2; (void)v2len;
	if (total == 0) {
		/* unit key and unit value: an empty output record. */
		dtfl_join_emit_record(out, NULL, 0);
		return;
	}
	p = total <= sizeof buf ? buf : __dtfl_alloc(total);
	copy_n(p, key, klen);
	copy_n(p + klen, v1, v1len);
	dtfl_join_emit_record(out, p, total);
	if (p != buf)
		__dtfl_free(p);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

/*
 * semijoin: match (key, val) in `c` against `keys` by key, keeping the
 * former with multiplicities multiplied (collection.rs semijoin).  `keys`
 * holds bare key records; it is arranged by self (key -> unit).  The
 * output records are key||val of `out_type`.
 */
dtfl_collection *
dtfl_collection_semijoin(dtfl_collection *c, dtfl_collection *keys,
    const dtfl_type *key_type, const dtfl_type *val_type,
    const dtfl_type *out_type)
{
	dtfl_arranged *a1 = __dtfl_collection_arrange_by_key(c, key_type,
	    val_type, "Arrange");
	dtfl_arranged *a2 = dtfl_collection_arrange(keys, "Arrange");
	dtfl_collection *out = dtfl_arranged_join_core(a1, a2, out_type,
	    semijoin_result, DTFL_NOCTX);

	dtfl_arranged_free(a1);
	dtfl_arranged_free(a2);
	return out;
}

/*
 * antijoin: subtract the semijoin with `keys` from `c`
 * (collection.rs antijoin = self.concat(self.semijoin(keys).negate())).
 * `c`'s diff class must be Abelian.  The output records are key||val.
 */
dtfl_collection *
dtfl_collection_antijoin(dtfl_collection *c, dtfl_collection *keys,
    const dtfl_type *key_type, const dtfl_type *val_type,
    const dtfl_type *out_type)
{
	dtfl_collection *semi = dtfl_collection_semijoin(c, keys, key_type,
	    val_type, out_type);
	dtfl_collection *neg = dtfl_collection_negate(semi);
	dtfl_collection *out = dtfl_collection_concat(c, neg);

	dtfl_collection_free(semi);
	dtfl_collection_free(neg);
	return out;
}
