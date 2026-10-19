/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/builder.c: the generic operator builder.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/generic/builder_rc.rs
 *   timely/src/dataflow/operators/generic/operator_info.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * The builder reserves an operator slot, then grows its inputs and
 * outputs.  Each input allocates a channel through its pact, connects
 * the source stream to the operator, and keeps a pull counter and a
 * mutable antichain for the input frontier.  Each output allocates a Tee
 * (the stream), a push counter over it, and a change batch that tracks
 * the output's held capabilities.  build mints one initial capability
 * per output at the minimum time, hands them to the first logic call,
 * and installs a raw operator whose schedule drains the input frontiers,
 * runs the user logic through a dtfl_op handle, then publishes the
 * consumed, internal and produced change batches back to shared progress
 * (builder_rc.rs ProgressBookkeeping).  Returning true keeps the operator
 * live (build_reschedule).
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

/* ------------------------------------------------------------------ */
/* Runtime state (the logic context of the raw operator)              */
/* ------------------------------------------------------------------ */

typedef struct op_runtime {
	dtfl_op op;			/* the handle passed to user logic */
	const dtfl_time_class *tc;	/* retained */
	size_t inputs, outputs;

	__dtfl_pull_counter **pull;	/* per input; owned */
	__dtfl_change_batch *consumed;	/* per input; owned */
	__dtfl_mutable_antichain *frontier;	/* per input; owned */
	__dtfl_connectivity summaries;	/* .ports[input] -> outputs */

	__dtfl_change_batch *internal;	/* per output; owned (capability batches) */
	__dtfl_change_batch *produced;	/* per output; owned */
	dtfl_output *outputs_arr;	/* per output; owned */
	dtfl_stream **streams;		/* per output; owned (freed with the op) */
	__dtfl_tee **tees;		/* per output; retained */

	dtfl_cap **initial_caps;	/* per output; the first call owns them */

	dtfl_logic_fn logic;
	void *ctx;
	void (*ctx_free)(void *);
} op_runtime;

static bool
runtime_logic(void *vctx, __dtfl_shared_progress *sp)
{
	op_runtime *rt = vctx;
	size_t i, o;
	bool incomplete;

	/* drain_frontiers */
	for (i = 0; i < rt->inputs; i++) {
		__dtfl_mantichain_update_batch(&rt->frontier[i], &sp->frontiers[i]);
		__dtfl_cb_clear(&rt->frontier[i].changes);
	}

	/* Run the user logic with a fresh handle. */
	rt->op.have_cur = false;
	incomplete = rt->logic(&rt->op, rt->ctx);
	rt->op.first_call = false;

	/* Release any held input messages (records their consumption). */
	for (i = 0; i < rt->inputs; i++)
		__dtfl_pull_counter_finish(rt->pull[i]);

	/* publish_progress */
	for (i = 0; i < rt->inputs; i++)
		__dtfl_cb_drain_into(&rt->consumed[i], &sp->consumeds[i]);
	for (o = 0; o < rt->outputs; o++) {
		__dtfl_cb_extend(&sp->internals[o], &rt->internal[o]);
		__dtfl_cb_clear(&rt->internal[o]);
		__dtfl_cb_drain_into(&rt->produced[o], &sp->produceds[o]);
	}
	return incomplete;
}

static void
runtime_free(void *vctx)
{
	op_runtime *rt = vctx;
	size_t i, o;

	if (rt->ctx_free != NULL)
		rt->ctx_free(rt->ctx);
	for (i = 0; i < rt->inputs; i++) {
		__dtfl_pull_counter_free(rt->pull[i]);
		__dtfl_cb_free(&rt->consumed[i]);
		__dtfl_mantichain_free(&rt->frontier[i]);
	}
	for (o = 0; o < rt->outputs; o++) {
		/* initial caps not taken by the logic still hold a +1; drop them. */
		if (rt->initial_caps[o] != NULL)
			dtfl_cap_drop(rt->initial_caps[o]);
		__dtfl_push_destroy(rt->outputs_arr[o].pushee);
		__dtfl_cb_free(&rt->internal[o]);
		__dtfl_cb_free(&rt->produced[o]);
		__dtfl_stream_free(rt->streams[o]);
		__dtfl_tee_release(rt->tees[o]);
	}
	__dtfl_connectivity_free(&rt->summaries);
	__dtfl_free(rt->pull);
	__dtfl_free(rt->consumed);
	__dtfl_free(rt->frontier);
	__dtfl_free(rt->internal);
	__dtfl_free(rt->produced);
	__dtfl_free(rt->outputs_arr);
	__dtfl_free(rt->streams);
	__dtfl_free(rt->tees);
	__dtfl_free(rt->initial_caps);
	__dtfl_free(rt->op.inputs_pull);
	__dtfl_free(rt->op.internals);
	__dtfl_free(rt->op.address);
	dtfl_time_class_free(rt->tc);
	__dtfl_free(rt);
}

/* ------------------------------------------------------------------ */
/* The builder                                                         */
/* ------------------------------------------------------------------ */

typedef struct input_slot {
	__dtfl_pull_counter *pull;
	__dtfl_change_batch consumed;
	__dtfl_mutable_antichain frontier;
	__dtfl_port_conn summary;	/* to each output */
} input_slot;

typedef struct output_slot {
	__dtfl_change_batch internal;
	__dtfl_change_batch produced;
	dtfl_output out;
	dtfl_stream *stream;
	__dtfl_tee *tee;
} output_slot;

struct dtfl_op_builder {
	dtfl_scope *scope;
	__dtfl_slot slot;
	size_t *address;
	size_t address_len;
	char *name;
	const dtfl_time_class *tc;	/* not retained; the scope's class */
	size_t peers;
	size_t inputs, outputs;
	__dtfl_vec notify;		/* __dtfl_frontier_interest, per input */
	__dtfl_vec inslots;		/* input_slot */
	__dtfl_vec outslots;		/* output_slot */
};

dtfl_op_builder *
dtfl_op_builder_new(dtfl_scope *s, const char *name)
{
	dtfl_op_builder *b = __dtfl_calloc(1, sizeof *b);

	b->scope = s;
	b->slot = __dtfl_scope_reserve(s);
	b->address = __dtfl_scope_slot_addr(s, &b->slot, &b->address_len);
	b->name = __dtfl_strdup(name);
	b->tc = dtfl_scope_time_class(s);
	b->peers = dtfl_scope_peers(s);
	__dtfl_vec_init(&b->notify, sizeof(__dtfl_frontier_interest));
	__dtfl_vec_init(&b->inslots, sizeof(input_slot));
	__dtfl_vec_init(&b->outslots, sizeof(output_slot));
	return b;
}

/* OperatorInfo::global_id: the worker-unique id the Operates event logs. */
size_t
__dtfl_op_builder_global_id(const dtfl_op_builder *b)
{
	return b->slot.identifier;
}

size_t
dtfl_op_builder_index(const dtfl_op_builder *b)
{
	return b->slot.index;
}

void
dtfl_op_builder_set_notify(dtfl_op_builder *b, size_t input, dtfl_interest interest)
{
	if (input < b->notify.len)
		*DTFL_VEC_AT(&b->notify, __dtfl_frontier_interest, input) =
		    (__dtfl_frontier_interest)interest;
}

/*
 * Allocates input `b->inputs`: its channel, the stream connection and
 * the per-input state, with no connection to any output yet.
 */
static input_slot *
input_port_new(dtfl_op_builder *b, dtfl_stream *in, dtfl_pact pact)
{
	__dtfl_worker *w = b->scope->worker;
	size_t channel = __dtfl_worker_new_identifier(w);
	size_t port = b->inputs;
	__dtfl_push *pusher;
	__dtfl_pull *puller;
	__dtfl_location target = { b->slot.index, port, false };
	input_slot slot;
	__dtfl_frontier_interest always = DTFL_INTEREST_ALWAYS;

	memset(&slot, 0, sizeof slot);
	__dtfl_pact_connect(b->scope, &pact, channel, b->address, b->address_len,
	    b->tc, in->type, &pusher, &puller);
	__dtfl_stream_connect_to(in, target, pusher, channel);

	__dtfl_cb_init(&slot.consumed, b->tc->depth);
	slot.pull = __dtfl_pull_counter_new(puller, &slot.consumed);
	__dtfl_mantichain_init(&slot.frontier, b->tc);
	__dtfl_port_conn_init(&slot.summary, b->tc);
	__dtfl_vec_push(&b->inslots, &slot);
	__dtfl_vec_push(&b->notify, &always);
	b->inputs++;
	return DTFL_VEC_AT(&b->inslots, input_slot, port);
}

static size_t
builder_input(dtfl_op_builder *b, dtfl_stream *in, dtfl_pact pact,
    const uint64_t *const *summaries)
{
	input_slot *slot = input_port_new(b, in, pact);
	size_t o;
	uint64_t def[DTFL_TIME_MAX + 1];

	dtfl_summary_default(b->tc, def);
	for (o = 0; o < b->outputs; o++) {
		if (summaries == NULL)
			__dtfl_port_conn_insert(&slot->summary, o, def);
		else if (summaries[o] != NULL)
			__dtfl_port_conn_insert(&slot->summary, o, summaries[o]);
	}
	return b->inputs - 1;
}

size_t
dtfl_op_builder_input(dtfl_op_builder *b, dtfl_stream *in, dtfl_pact pact)
{
	return builder_input(b, in, pact, NULL);
}

size_t
dtfl_op_builder_input_connection(dtfl_op_builder *b, dtfl_stream *in,
    dtfl_pact pact, const uint64_t *const *summaries)
{
	return builder_input(b, in, pact, summaries);
}

/*
 * new_input_connection with upstream's IntoIterator<(usize, Antichain)>:
 * n (output, summary) pairs; an output may repeat, its summaries merge
 * by antichain insertion.
 */
size_t
dtfl_op_builder_input_sparse(dtfl_op_builder *b, dtfl_stream *in,
    dtfl_pact pact, size_t n, const size_t *outputs,
    const uint64_t *const *summaries)
{
	input_slot *slot = input_port_new(b, in, pact);
	size_t k;

	for (k = 0; k < n; k++) {
		DTFL_ASSERT(outputs[k] < b->outputs);
		__dtfl_port_conn_insert(&slot->summary, outputs[k], summaries[k]);
	}
	return b->inputs - 1;
}

/*
 * Allocates output `b->outputs`: its Tee, stream and push counter, with
 * no connection from any input yet.
 */
static dtfl_stream *
output_port_new(dtfl_op_builder *b, const dtfl_type *type)
{
	size_t port = b->outputs;
	output_slot slot;
	__dtfl_location source = { b->slot.index, port, true };
	__dtfl_push *counter;

	memset(&slot, 0, sizeof slot);
	slot.tee = __dtfl_tee_new();
	slot.stream = __dtfl_stream_new(b->scope, source, slot.tee, type);
	__dtfl_cb_init(&slot.internal, b->tc->depth);
	__dtfl_cb_init(&slot.produced, b->tc->depth);
	counter = __dtfl_push_counter_new(__dtfl_tee_pusher(slot.tee), &slot.produced);
	slot.out.pushee = counter;
	slot.out.port = port;
	slot.out.tc = b->tc;
	slot.out.type = type;
	__dtfl_vec_push(&b->outslots, &slot);
	b->outputs++;
	return slot.stream;
}

static dtfl_stream *
builder_output(dtfl_op_builder *b, const dtfl_type *type,
    const uint64_t *const *summaries)
{
	size_t port = b->outputs;
	dtfl_stream *stream = output_port_new(b, type);
	size_t i;
	uint64_t def[DTFL_TIME_MAX + 1];

	dtfl_summary_default(b->tc, def);
	for (i = 0; i < b->inputs; i++) {
		input_slot *in = DTFL_VEC_AT(&b->inslots, input_slot, i);

		if (summaries == NULL)
			__dtfl_port_conn_insert(&in->summary, port, def);
		else if (summaries[i] != NULL)
			__dtfl_port_conn_insert(&in->summary, port, summaries[i]);
	}
	return stream;
}

dtfl_stream *
dtfl_op_builder_output(dtfl_op_builder *b, const dtfl_type *type)
{
	return builder_output(b, type, NULL);
}

dtfl_stream *
dtfl_op_builder_output_connection(dtfl_op_builder *b, const dtfl_type *type,
    const uint64_t *const *summaries)
{
	return builder_output(b, type, summaries);
}

/*
 * new_output_connection with upstream's IntoIterator<(usize, Antichain)>:
 * n (input, summary) pairs; an input may repeat.
 */
dtfl_stream *
dtfl_op_builder_output_sparse(dtfl_op_builder *b, const dtfl_type *type,
    size_t n, const size_t *inputs, const uint64_t *const *summaries)
{
	size_t port = b->outputs, k;
	dtfl_stream *stream = output_port_new(b, type);

	for (k = 0; k < n; k++) {
		DTFL_ASSERT(inputs[k] < b->inputs);
		__dtfl_port_conn_insert(&DTFL_VEC_AT(&b->inslots, input_slot,
		    inputs[k])->summary, port, summaries[k]);
	}
	return stream;
}

void
dtfl_op_builder_build(dtfl_op_builder *b, dtfl_logic_fn logic, void *ctx,
    void (*ctx_free)(void *))
{
	op_runtime *rt = __dtfl_calloc(1, sizeof *rt);
	__dtfl_connectivity summary;
	__dtfl_operate *op;
	uint64_t minimum[DTFL_TIME_MAX];
	size_t i, o;

	rt->tc = dtfl_time_class_retain(b->tc);
	rt->inputs = b->inputs;
	rt->outputs = b->outputs;
	rt->logic = logic;
	rt->ctx = ctx;
	rt->ctx_free = ctx_free;

	rt->pull = __dtfl_calloc(DTFL_MAX(b->inputs, 1), sizeof(__dtfl_pull_counter *));
	rt->consumed = __dtfl_calloc(DTFL_MAX(b->inputs, 1), sizeof(__dtfl_change_batch));
	rt->frontier = __dtfl_calloc(DTFL_MAX(b->inputs, 1), sizeof(__dtfl_mutable_antichain));
	rt->internal = __dtfl_calloc(DTFL_MAX(b->outputs, 1), sizeof(__dtfl_change_batch));
	rt->produced = __dtfl_calloc(DTFL_MAX(b->outputs, 1), sizeof(__dtfl_change_batch));
	rt->outputs_arr = __dtfl_calloc(DTFL_MAX(b->outputs, 1), sizeof(dtfl_output));
	rt->streams = __dtfl_calloc(DTFL_MAX(b->outputs, 1), sizeof(dtfl_stream *));
	rt->tees = __dtfl_calloc(DTFL_MAX(b->outputs, 1), sizeof(__dtfl_tee *));
	rt->initial_caps = __dtfl_calloc(DTFL_MAX(b->outputs, 1), sizeof(dtfl_cap *));

	/* Build the input->output connectivity and move the per-input state. */
	__dtfl_connectivity_init(&rt->summaries, b->tc, b->inputs);
	__dtfl_connectivity_init(&summary, b->tc, b->inputs);
	for (i = 0; i < b->inputs; i++) {
		input_slot *in = DTFL_VEC_AT(&b->inslots, input_slot, i);

		rt->pull[i] = in->pull;
		rt->consumed[i] = in->consumed;
		rt->frontier[i] = in->frontier;
		/* The pull counter captured the stack address; repoint it. */
		__dtfl_pull_counter_set_consumed(rt->pull[i], &rt->consumed[i]);
		/* The summary serves both the parent (reachability) and the handle. */
		__dtfl_port_conn_copy(&rt->summaries.ports[i], &in->summary);
		__dtfl_port_conn_copy(&summary.ports[i], &in->summary);
		__dtfl_port_conn_free(&in->summary);
	}
	for (o = 0; o < b->outputs; o++) {
		output_slot *out = DTFL_VEC_AT(&b->outslots, output_slot, o);

		rt->internal[o] = out->internal;
		rt->produced[o] = out->produced;
		rt->outputs_arr[o] = out->out;
		rt->streams[o] = out->stream;
		rt->tees[o] = out->tee;
		rt->outputs_arr[o].internal = &rt->internal[o];
		/* The push counter captured the stack address; repoint it. */
		__dtfl_push_counter_set_produced(rt->outputs_arr[o].pushee,
		    &rt->produced[o]);
	}

	/* mint_capabilities: one per output at the minimum, evidence cleared. */
	dtfl_time_minimum(b->tc, minimum);
	for (o = 0; o < b->outputs; o++) {
		rt->initial_caps[o] = __dtfl_cap_new(b->tc, minimum, &rt->internal[o], o);
		__dtfl_cb_clear(&rt->internal[o]);
	}

	/* Fill the operator handle. */
	rt->op.scope = b->scope;
	rt->op.inputs = b->inputs;
	rt->op.outputs = b->outputs;
	rt->op.tc = b->tc;
	rt->op.inputs_pull = __dtfl_calloc(DTFL_MAX(b->inputs, 1),
	    sizeof(__dtfl_pull_counter *));
	for (i = 0; i < b->inputs; i++)
		rt->op.inputs_pull[i] = rt->pull[i];
	rt->op.summaries = &rt->summaries;
	rt->op.frontier = rt->frontier;
	rt->op.internals = __dtfl_calloc(DTFL_MAX(b->outputs, 1),
	    sizeof(__dtfl_change_batch *));
	for (o = 0; o < b->outputs; o++)
		rt->op.internals[o] = &rt->internal[o];
	rt->op.outputs_arr = rt->outputs_arr;
	rt->op.initial_caps = rt->initial_caps;
	rt->op.first_call = true;
	rt->op.worker = b->scope->worker;
	rt->op.address = __dtfl_memdup(b->address, b->address_len * sizeof(size_t));
	rt->op.address_len = b->address_len;

	op = __dtfl_op_core_new(b->scope->worker, b->name, b->address, b->address_len,
	    b->tc, b->peers, b->inputs, b->outputs,
	    (const __dtfl_frontier_interest *)(void *)b->notify.data, &summary,
	    runtime_logic, rt, runtime_free);
	__dtfl_scope_install(b->scope, &b->slot, op);

	/* The builder is consumed. */
	__dtfl_vec_free(&b->inslots);
	__dtfl_vec_free(&b->outslots);
	__dtfl_vec_free(&b->notify);
	__dtfl_free(b->address);
	__dtfl_free(b->name);
	__dtfl_free(b);
}
