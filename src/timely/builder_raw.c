/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/builder_raw.c: the raw operator shape and core.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/generic/builder_raw.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * OperatorCore is the Operate / Schedule that backs the generic builder.
 * On initialize it announces its connectivity (the summary the builder
 * accumulated), reserves one capability for each output at the minimum
 * time with the full peers magnitude (so progress does not advance until
 * the operator releases them), and requests to be scheduled once.  Its
 * schedule calls the logic with the shared progress.  The rc builder
 * (builder.c) supplies logic that drains frontiers, runs user logic, and
 * publishes progress.
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

typedef struct op_core {
	__dtfl_operate operate;
	__dtfl_schedule sched;
	__dtfl_worker *w;
	const dtfl_time_class *tc;	/* retained */
	size_t peers;
	size_t inputs, outputs;
	__dtfl_frontier_interest *notify;	/* per input; owned */
	__dtfl_connectivity summary;		/* moved out at initialize */
	__dtfl_shared_progress *sp;		/* owned until initialize */
	__dtfl_raw_logic_fn logic;
	void *ctx;
	void (*ctx_free)(void *);
} op_core;

static bool
oc_local(const __dtfl_operate *op)
{
	(void)op;
	return true;	/* generic operators are worker-local */
}

static size_t
oc_inputs(const __dtfl_operate *op)
{
	return ((const op_core *)op)->inputs;
}

static size_t
oc_outputs(const __dtfl_operate *op)
{
	return ((const op_core *)op)->outputs;
}

static const __dtfl_frontier_interest *
oc_notify_me(const __dtfl_operate *op)
{
	return ((const op_core *)op)->notify;
}

static bool
oc_schedule(__dtfl_schedule *s)
{
	op_core *o = (op_core *)((char *)s - offsetof(op_core, sched));

	return o->logic(o->ctx, o->sp);
}

static void
oc_free(op_core *o)
{
	if (o->ctx_free != NULL)
		o->ctx_free(o->ctx);
	__dtfl_free(o->notify);
	__dtfl_connectivity_free(&o->summary);
	dtfl_time_class_free(o->tc);
	__dtfl_free(o->sched.name);
	__dtfl_free(o->sched.path);
	__dtfl_free(o);
}

static void
oc_sched_destroy(__dtfl_schedule *s)
{
	op_core *o = (op_core *)((char *)s - offsetof(op_core, sched));

	oc_free(o);
}

static const __dtfl_schedule_ops oc_sched_ops = { oc_schedule, oc_sched_destroy };

static __dtfl_schedule *
oc_initialize(__dtfl_operate *op, __dtfl_connectivity *conn,
    __dtfl_shared_progress **progress)
{
	op_core *o = (op_core *)op;
	uint64_t minimum[DTFL_TIME_MAX];
	size_t i;

	/* Request to be scheduled at least once. */
	__dtfl_activations_activate(&o->w->activations, o->sched.path, o->sched.path_len);

	/* Reserve one capability per output at the minimum time, peers wide. */
	dtfl_time_minimum(o->tc, minimum);
	for (i = 0; i < o->outputs; i++)
		__dtfl_cb_update(&o->sp->internals[i], minimum, (int64_t)o->peers);

	/* Hand the summary to the parent (moved, not cloned). */
	*conn = o->summary;
	__dtfl_connectivity_init(&o->summary, o->tc, 0);
	*progress = o->sp;
	return &o->sched;
}

static void
oc_operate_destroy(__dtfl_operate *op)
{
	op_core *o = (op_core *)op;

	__dtfl_shared_progress_free(o->sp);
	oc_free(o);
}

static const __dtfl_operate_ops oc_ops = {
	oc_local, oc_inputs, oc_outputs, oc_notify_me, oc_initialize,
	oc_operate_destroy
};

__dtfl_operate *
__dtfl_op_core_new(__dtfl_worker *w, const char *name, const size_t *path,
    size_t path_len, const dtfl_time_class *tc, size_t peers, size_t inputs,
    size_t outputs, const __dtfl_frontier_interest *notify,
    __dtfl_connectivity *summary, __dtfl_raw_logic_fn logic, void *ctx,
    void (*ctx_free)(void *))
{
	op_core *o = __dtfl_calloc(1, sizeof *o);
	size_t i;

	o->operate.ops = &oc_ops;
	o->sched.ops = &oc_sched_ops;
	o->sched.name = __dtfl_strdup(name);
	o->sched.path = __dtfl_memdup(path, path_len * sizeof(size_t));
	o->sched.path_len = path_len;
	o->w = w;
	o->tc = dtfl_time_class_retain(tc);
	o->peers = peers;
	o->inputs = inputs;
	o->outputs = outputs;
	o->notify = __dtfl_calloc(DTFL_MAX(inputs, 1), sizeof *o->notify);
	for (i = 0; i < inputs; i++)
		o->notify[i] = notify[i];
	o->summary = *summary;	/* take ownership of its contents */
	o->sp = __dtfl_shared_progress_new(o->tc, inputs, outputs);
	o->logic = logic;
	o->ctx = ctx;
	o->ctx_free = ctx_free;
	return &o->operate;
}
