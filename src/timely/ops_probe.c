/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_probe.c: observe a stream's frontier.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/core/probe.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A probe is a pass-through operator (one pipeline input, one output)
 * that mirrors its input frontier into a shared MutableAntichain the
 * handle reads.  Upstream shares the antichain with Rc<RefCell<...>> and
 * feeds it the incremental frontier changes each schedule; here the
 * generic builder exposes the resolved frontier, so each schedule
 * republishes the input frontier into the handle's antichain (an
 * equivalent fixpoint).  The handle starts holding the minimum time so
 * done() is false before the operator first runs, matching upstream's
 * conservative initial hold.  The handle outlives the operator (the user
 * reads it after the dataflow is dropped), so the antichain is reference
 * counted and both hold a reference.
 *
 * less_than / less_equal / done ask the shared antichain, so a program
 * steps the worker until probe.done() or until the probe passes the
 * input's current epoch (probe.rs Handle).
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

#include "dtfl/timely_ops.h"

struct dtfl_probe {
	__dtfl_arc ref;
	dtfl_time_class *tc;		/* retained once attached; else NULL */
	__dtfl_mutable_antichain frontier;	/* valid once tc is set */
};

dtfl_probe *
dtfl_probe_new(void)
{
	dtfl_probe *p = __dtfl_calloc(1, sizeof *p);

	__dtfl_arc_init(&p->ref);
	return p;
}

static dtfl_probe *
probe_retain(dtfl_probe *p)
{
	__dtfl_arc_retain(&p->ref);
	return p;
}

static void
probe_release(dtfl_probe *p)
{
	if (p == NULL || !__dtfl_arc_release(&p->ref))
		return;
	if (p->tc != NULL) {
		__dtfl_mantichain_free(&p->frontier);
		dtfl_time_class_free(p->tc);
	}
	__dtfl_free(p);
}

void
dtfl_probe_free(dtfl_probe *p)
{
	probe_release(p);
}

/* ------------------------------------------------------------------ */
/* The operator logic                                                  */
/* ------------------------------------------------------------------ */

typedef struct probe_ctx {
	dtfl_probe *handle;	/* retained */
} probe_ctx;

/*
 * Republish the input frontier into the handle's antichain: retract
 * every element currently held and assert every element of the input
 * frontier, leaving the handle's antichain equal to the input frontier
 * (and empty once the input is exhausted, so done() becomes true).  The
 * handle starts holding the minimum (set in dtfl_stream_probe_with), so
 * the first publish retracts that conservative hold as its "previous"
 * frontier.
 */
static void
probe_publish(dtfl_probe *h, const uint64_t *frontier, size_t n)
{
	const uint64_t *held;
	size_t nheld, i;
	unsigned depth = h->tc->depth;

	/* Retract the previous frontier (snapshot it first; update mutates). */
	nheld = __dtfl_mantichain_frontier_len(&h->frontier);
	if (nheld > 0) {
		uint64_t *snap = __dtfl_alloc((size_t)nheld * DTFL_MAX(depth, 1u) *
		    sizeof(uint64_t));

		held = __dtfl_mantichain_frontier(&h->frontier);
		memcpy(snap, held, (size_t)nheld * depth * sizeof(uint64_t));
		for (i = 0; i < nheld; i++)
			__dtfl_mantichain_update(&h->frontier,
			    snap + (size_t)i * depth, -1);
		__dtfl_free(snap);
	}
	/* Assert the current input frontier. */
	for (i = 0; i < n; i++)
		__dtfl_mantichain_update(&h->frontier, frontier + (size_t)i * depth, 1);
}

static bool
probe_logic(dtfl_op *op, void *vctx)
{
	probe_ctx *c = vctx;
	const uint64_t *frontier;
	size_t n;
	dtfl_cap *cap;
	dtfl_message *m;

	/* Mirror the input frontier into the shared handle. */
	frontier = dtfl_op_frontier(op, 0, &n);
	probe_publish(c->handle, frontier, n);

	/* Discard the held output capability on the first schedule. */
	cap = dtfl_op_initial_cap(op, 0);
	if (cap != NULL)
		dtfl_cap_drop(cap);

	/* Forward every input message to the output at the message's time. */
	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		dtfl_cap *mc = dtfl_op_input_cap(op, 0, 0);
		dtfl_session *s = dtfl_op_session(op, 0, mc);

		dtfl_session_give_message(s, m);
		dtfl_session_close(s);
		dtfl_cap_drop(mc);
	}
	return false;
}

static void
probe_ctx_free(void *vctx)
{
	probe_ctx *c = vctx;

	probe_release(c->handle);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_probe_with(dtfl_stream *st, dtfl_probe *p)
{
	dtfl_scope *scope = dtfl_stream_scope(st);
	const dtfl_time_class *tc = dtfl_scope_time_class(scope);
	dtfl_op_builder *b = dtfl_op_builder_new(scope, "Probe");
	dtfl_stream *out;
	probe_ctx *c;
	uint64_t minimum[DTFL_TIME_MAX];

	/* Attach the handle to this stream's class; a probe serves one. */
	if (p->tc == NULL) {
		p->tc = dtfl_time_class_retain(tc);
		__dtfl_mantichain_init(&p->frontier, p->tc);
		/* Conservative hold at the minimum until the operator runs. */
		dtfl_time_minimum(p->tc, minimum);
		__dtfl_mantichain_update(&p->frontier, minimum, 1);
	}

	dtfl_op_builder_input(b, st, dtfl_pact_pipeline());
	out = dtfl_op_builder_output(b, dtfl_stream_type(st));

	c = __dtfl_alloc(sizeof *c);
	c->handle = probe_retain(p);
	dtfl_op_builder_build(b, probe_logic, c, probe_ctx_free);
	return out;
}

/* ------------------------------------------------------------------ */
/* Handle queries (probe.rs Handle)                                    */
/* ------------------------------------------------------------------ */

bool
dtfl_probe_less_than(const dtfl_probe *p, const uint64_t *t)
{
	if (p->tc == NULL)
		return false;	/* an unattached probe holds the minimum */
	return __dtfl_mantichain_less_than(&p->frontier, t);
}

bool
dtfl_probe_less_equal(const dtfl_probe *p, const uint64_t *t)
{
	if (p->tc == NULL)
		return false;
	return __dtfl_mantichain_less_equal(&p->frontier, t);
}

bool
dtfl_probe_done(const dtfl_probe *p)
{
	if (p->tc == NULL)
		return false;	/* never attached: not done */
	return __dtfl_mantichain_is_empty(&p->frontier);
}

const uint64_t *
dtfl_probe_frontier(const dtfl_probe *p, size_t *n)
{
	if (p->tc == NULL) {
		*n = 0;
		return NULL;
	}
	*n = __dtfl_mantichain_frontier_len(&p->frontier);
	return __dtfl_mantichain_frontier(&p->frontier);
}
