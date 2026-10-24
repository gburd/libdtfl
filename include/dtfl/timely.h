/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/timely.h: the timely dataflow API.
 *
 * Execution: dtfl_execute() starts the configured workers and calls
 * worker_main on each.  A worker builds dataflows with
 * dtfl_worker_dataflow(), feeds inputs, and steps until probes report
 * completion.  Everything reached from a dtfl_worker belongs to that
 * worker's thread (PLAN section 5).
 *
 * Data: a stream carries batches of records.  A record is a byte string
 * described by a dtfl_type (dtfl/record.h).  Messages on a stream are
 * (time, batch) pairs; operators receive them through dtfl_input_next()
 * and send through output sessions opened with a capability.
 *
 * Times are uint64_t vectors of the scope's dtfl_time_class
 * (dtfl/time.h).  A top-level dataflow's class is chosen at
 * dtfl_worker_dataflow(); nested scopes refine it.
 *
 * Ownership: dtfl_stream values are small handles owned by the scope;
 * they need not be freed.  Capabilities, input handles, probes and
 * activators are owned by the caller and released with their _drop or
 * _free function.  Callback contexts are released by the context's
 * destructor when the operator is dropped (PLAN D5).
 */
#ifndef DTFL_TIMELY_H
#define DTFL_TIMELY_H

#include "dtfl/base.h"
#include "dtfl/config.h"
#include "dtfl/record.h"
#include "dtfl/time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dtfl_worker dtfl_worker;
typedef struct dtfl_scope dtfl_scope;
typedef struct dtfl_stream dtfl_stream;
typedef struct dtfl_guards dtfl_guards;
typedef struct dtfl_op dtfl_op;
typedef struct dtfl_cap dtfl_cap;
typedef struct dtfl_input dtfl_input;
typedef struct dtfl_probe dtfl_probe;
typedef struct dtfl_activator dtfl_activator;
typedef struct dtfl_sync_activator dtfl_sync_activator;
typedef struct dtfl_message dtfl_message;
typedef struct dtfl_session dtfl_session;
typedef struct dtfl_output dtfl_output;

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

typedef void (*dtfl_worker_fn)(dtfl_worker *w, void *ctx);

/*
 * Starts the workers of `cfg` and runs `fn(worker, ctx)` on each, on its
 * own thread (or the calling thread for DTFL_COMM_THREAD).  Returns when
 * all workers have returned and their dataflows have completed, or
 * immediately with *guards set if guards != NULL (join with
 * dtfl_guards_join()).  `ctx` is shared by all workers.
 */
DTFL_API int dtfl_execute(const dtfl_config *cfg, dtfl_worker_fn fn,
    void *ctx, dtfl_guards **guards);
/* Runs one worker on the calling thread (execute_directly / example). */
DTFL_API int dtfl_execute_directly(dtfl_worker_fn fn, void *ctx);
/*
 * Joins an execution as worker `index` of `peers`, on the calling thread,
 * reaching every other worker through `transport` (borrowed until this
 * returns; see dtfl_transport(3)).  Each of the `peers` processes (or
 * threads) calls this once with its own index and transport, and runs
 * the same `fn`.  Returns once fn has returned, the worker's dataflows
 * are complete, and every peer has ended its stream.  Starts no thread:
 * the worker's own steps move the bytes, and it parks in the
 * transport's wait.  `cfg` (may be NULL) supplies the progress mode, log
 * subscriptions, spill and idle merge effort; its mode and thread count
 * are ignored.  With peers == 1 the transport may be NULL.  Fails with
 * DTFL_EINVAL before starting anything.  See dtfl_execute_worker(3).
 */
DTFL_API int dtfl_execute_worker(const dtfl_config *cfg, size_t index,
    size_t peers, const dtfl_transport *transport, dtfl_worker_fn fn,
    void *ctx);
/* Thread-safe: joins worker threads; frees the guards. */
DTFL_API int dtfl_guards_join(dtfl_guards *g);

/*
 * A worker the caller steps by hand on the calling thread (the
 * `threadless` example).  Single worker, Thread communication.
 */
DTFL_API dtfl_worker *dtfl_worker_new_standalone(void);
DTFL_API void dtfl_worker_free(dtfl_worker *w);

DTFL_API size_t dtfl_worker_index(const dtfl_worker *w);
DTFL_API size_t dtfl_worker_peers(const dtfl_worker *w);
/* One step; returns true while dataflows remain. */
DTFL_API bool dtfl_worker_step(dtfl_worker *w);
/* Step or park up to timeout_ns (UINT64_MAX: indefinitely). */
DTFL_API bool dtfl_worker_step_or_park(dtfl_worker *w, uint64_t timeout_ns);
/* Steps while cond(ctx) returns true. */
DTFL_API void dtfl_worker_step_while(dtfl_worker *w, bool (*cond)(void *ctx),
    void *ctx);
DTFL_API bool dtfl_worker_has_dataflows(const dtfl_worker *w);
DTFL_API void dtfl_worker_drop_dataflow(dtfl_worker *w, size_t index);
DTFL_API size_t dtfl_worker_next_dataflow_index(const dtfl_worker *w);

/*
 * Builds a dataflow with times of class `tc` (retained).  `build` runs
 * immediately with the new root scope and `ctx`, and must create all of
 * the dataflow's operators; the dataflow is installed when it returns.
 * Every worker must build the same dataflows in the same order.
 */
typedef void (*dtfl_build_fn)(dtfl_scope *scope, void *ctx);
DTFL_API int dtfl_worker_dataflow(dtfl_worker *w, const char *name,
    const dtfl_time_class *tc, dtfl_build_fn build, void *ctx);

/* ------------------------------------------------------------------ */
/* Scopes                                                              */
/* ------------------------------------------------------------------ */

DTFL_API dtfl_worker *dtfl_scope_worker(const dtfl_scope *s);
DTFL_API const dtfl_time_class *dtfl_scope_time_class(const dtfl_scope *s);
DTFL_API size_t dtfl_scope_index(const dtfl_scope *s);
DTFL_API size_t dtfl_scope_peers(const dtfl_scope *s);
/* The scope's address; stores the length in *len. */
DTFL_API const size_t *dtfl_scope_addr(const dtfl_scope *s, size_t *len);

/*
 * Nested scopes.  `inner` must refine the scope's class (dtfl/time.h):
 * dtfl_time_product(outer, x) for iterative scopes, the same class for a
 * region, or dtfl_time_altneu(outer).  `build` runs with the child scope.
 */
DTFL_API int dtfl_scope_scoped(dtfl_scope *s, const char *name,
    const dtfl_time_class *inner, dtfl_build_fn build, void *ctx);
/* scoped with inner = Product(outer, u64). */
DTFL_API int dtfl_scope_iterative(dtfl_scope *s, const char *name,
    dtfl_build_fn build, void *ctx);
/* scoped with inner = outer (a region for grouping and logging). */
DTFL_API int dtfl_scope_region(dtfl_scope *s, const char *name,
    dtfl_build_fn build, void *ctx);
/* The enclosing scope, or NULL at the root. */
DTFL_API dtfl_scope *dtfl_scope_parent(const dtfl_scope *s);

/* ------------------------------------------------------------------ */
/* Streams                                                             */
/* ------------------------------------------------------------------ */

DTFL_API dtfl_scope *dtfl_stream_scope(const dtfl_stream *st);
DTFL_API const dtfl_type *dtfl_stream_type(const dtfl_stream *st);

/* ------------------------------------------------------------------ */
/* Messages (read side)                                                */
/* ------------------------------------------------------------------ */

/* Time of a received message (scope's class). */
DTFL_API const uint64_t *dtfl_message_time(const dtfl_message *m);
DTFL_API size_t dtfl_message_len(const dtfl_message *m);
/* Record i; stores its length in *len. */
DTFL_API const void *dtfl_message_record(const dtfl_message *m, size_t i,
    size_t *len);

/* ------------------------------------------------------------------ */
/* Capabilities                                                        */
/* ------------------------------------------------------------------ */

DTFL_API const uint64_t *dtfl_cap_time(const dtfl_cap *c);
/* A new capability at t >= time (Capability::delayed); panics otherwise. */
DTFL_API dtfl_cap *dtfl_cap_delayed(const dtfl_cap *c, const uint64_t *t);
/* NULL if t is not >= time (try_delayed). */
DTFL_API dtfl_cap *dtfl_cap_try_delayed(const dtfl_cap *c, const uint64_t *t);
DTFL_API void dtfl_cap_downgrade(dtfl_cap *c, const uint64_t *t);
/* Returns DTFL_EINVAL if t is not >= time. */
DTFL_API int dtfl_cap_try_downgrade(dtfl_cap *c, const uint64_t *t);
DTFL_API dtfl_cap *dtfl_cap_clone(const dtfl_cap *c);
/* Releases the capability (upstream Drop). */
DTFL_API void dtfl_cap_drop(dtfl_cap *c);
/* The output port this capability is for. */
DTFL_API size_t dtfl_cap_output(const dtfl_cap *c);

/* ------------------------------------------------------------------ */
/* Generic operators                                                   */
/* ------------------------------------------------------------------ */

/* Parallelization contracts. */
typedef enum dtfl_pact_kind {
	DTFL_PACT_PIPELINE = 0,	/* stay on this worker */
	DTFL_PACT_EXCHANGE = 1	/* route each record by hash(record) % peers */
} dtfl_pact_kind;

typedef struct dtfl_pact {
	dtfl_pact_kind kind;
	/* EXCHANGE: route function; NULL hashes the record with its type. */
	uint64_t (*route)(const void *rec, size_t len, void *ctx);
	void *ctx;
} dtfl_pact;

DTFL_API dtfl_pact dtfl_pact_pipeline(void);
DTFL_API dtfl_pact dtfl_pact_exchange(uint64_t (*route)(const void *, size_t, void *),
    void *ctx);

/*
 * Operator logic.  Called whenever the operator is scheduled, with the
 * operator handle (valid only during the call) and the user context.
 * Return true to report the operator incomplete (build_reschedule).
 */
typedef bool (*dtfl_logic_fn)(dtfl_op *op, void *ctx);

/*
 * The builder: dtfl_op_builder_new, add inputs and outputs, then
 * dtfl_op_builder_build with the logic.  The initial capabilities (one
 * per output, at the minimum time) are available inside the first call
 * through dtfl_op_initial_cap(); the logic owns them.
 */
typedef struct dtfl_op_builder dtfl_op_builder;

DTFL_API dtfl_op_builder *dtfl_op_builder_new(dtfl_scope *s, const char *name);
/* new_input: returns the input index. */
DTFL_API size_t dtfl_op_builder_input(dtfl_op_builder *b, dtfl_stream *in,
    dtfl_pact pact);
/*
 * new_input_connection: `summaries` gives, for each existing output, a
 * path summary (dtfl_summary_depth words) or NULL for no connection.
 */
DTFL_API size_t dtfl_op_builder_input_connection(dtfl_op_builder *b,
    dtfl_stream *in, dtfl_pact pact, const uint64_t *const *summaries);
/* new_output: a stream of records of `type` (retained). */
DTFL_API dtfl_stream *dtfl_op_builder_output(dtfl_op_builder *b,
    const dtfl_type *type);
DTFL_API dtfl_stream *dtfl_op_builder_output_connection(dtfl_op_builder *b,
    const dtfl_type *type, const uint64_t *const *summaries);
/*
 * Sparse forms of the two above, as upstream's (port, Antichain)
 * iterators: `n` pairs (ports[k], summaries[k]) naming existing outputs
 * (input_sparse) or inputs (output_sparse); unnamed ports are not
 * connected, a repeated port merges its summaries.  Cost is O(n), not
 * O(ports).
 */
DTFL_API size_t dtfl_op_builder_input_sparse(dtfl_op_builder *b,
    dtfl_stream *in, dtfl_pact pact, size_t n, const size_t *outputs,
    const uint64_t *const *summaries);
DTFL_API dtfl_stream *dtfl_op_builder_output_sparse(dtfl_op_builder *b,
    const dtfl_type *type, size_t n, const size_t *inputs,
    const uint64_t *const *summaries);
/* Frontier interest per input (default: always). */
typedef enum dtfl_interest {
	DTFL_NOTIFY_NEVER = 0,
	DTFL_NOTIFY_IF_CAPABILITY = 1,
	DTFL_NOTIFY_ALWAYS = 2
} dtfl_interest;
DTFL_API void dtfl_op_builder_set_notify(dtfl_op_builder *b, size_t input,
    dtfl_interest interest);
/*
 * Builds the operator.  ctx_free (may be NULL) runs when the operator
 * is dropped.  Consumes the builder.
 */
DTFL_API void dtfl_op_builder_build(dtfl_op_builder *b, dtfl_logic_fn logic,
    void *ctx, void (*ctx_free)(void *));
DTFL_API size_t dtfl_op_builder_index(const dtfl_op_builder *b);

/* Inside logic. */
DTFL_API dtfl_scope *dtfl_op_scope(const dtfl_op *op);
DTFL_API size_t dtfl_op_inputs(const dtfl_op *op);
DTFL_API size_t dtfl_op_outputs(const dtfl_op *op);
/*
 * The initial capability for an output, on the first call only; the
 * caller takes ownership.  NULL on later calls, or if already taken.
 */
DTFL_API dtfl_cap *dtfl_op_initial_cap(dtfl_op *op, size_t output);
/*
 * Next message on an input, or NULL.  The message is valid until the
 * next dtfl_input_next() call on the same input or the logic returns.
 * Consumption is recorded when the message is released (next call).
 */
DTFL_API dtfl_message *dtfl_op_input_next(dtfl_op *op, size_t input);
/*
 * A capability for the current message's time on `output`
 * (InputCapability::retain / delayed).  Valid any time before the logic
 * returns; owned by the caller.
 */
DTFL_API dtfl_cap *dtfl_op_input_cap(dtfl_op *op, size_t input, size_t output);
DTFL_API dtfl_cap *dtfl_op_input_cap_delayed(dtfl_op *op, size_t input,
    size_t output, const uint64_t *t);
/* The input's frontier: an antichain of `*n` times. */
DTFL_API const uint64_t *dtfl_op_frontier(const dtfl_op *op, size_t input,
    size_t *n);
/* True if some frontier element is <= t (frontier.less_equal(t)). */
DTFL_API bool dtfl_op_frontier_less_equal(const dtfl_op *op, size_t input,
    const uint64_t *t);
DTFL_API bool dtfl_op_frontier_less_than(const dtfl_op *op, size_t input,
    const uint64_t *t);
/* Opens a session on `output` at the capability's time. */
DTFL_API dtfl_session *dtfl_op_session(dtfl_op *op, size_t output,
    const dtfl_cap *cap);
DTFL_API void dtfl_session_give(dtfl_session *s, const void *rec, size_t len);
/* Gives every record of a received message. */
DTFL_API void dtfl_session_give_message(dtfl_session *s, const dtfl_message *m);
/* Flushes and closes the session. */
DTFL_API void dtfl_session_close(dtfl_session *s);
/* An activator for this operator, owned by the caller. */
DTFL_API dtfl_activator *dtfl_op_activator(dtfl_op *op);
DTFL_API dtfl_sync_activator *dtfl_op_sync_activator(dtfl_op *op);

/* Activators. */
DTFL_API void dtfl_activator_activate(dtfl_activator *a);
DTFL_API void dtfl_activator_activate_after(dtfl_activator *a, uint64_t delay_ns);
DTFL_API void dtfl_activator_free(dtfl_activator *a);
/* Thread-safe; returns DTFL_ESTATE once the worker is gone. */
DTFL_API int dtfl_sync_activator_activate(dtfl_sync_activator *a);
DTFL_API void dtfl_sync_activator_free(dtfl_sync_activator *a);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_TIMELY_H */
