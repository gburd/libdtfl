/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/int_proxy/mod.rs
 *   differential-dataflow/src/operators/int_proxy/reduce.rs
 *   differential-dataflow/src/operators/int_proxy/join.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/int_proxy_int.h: backend-agnostic operator tactics using integer
 * proxies.  Internal only: upstream marks the tactic APIs experimental, so
 * nothing here is installed or exported.
 *
 * The tactics run DD's operator logic over consolidated
 * [((u64, u64), time, diff)] lists (a ProxyBridge, rows of
 * __dtfl_bridge_upd), the first integer a hash of the "key" and granule of
 * independence, the second an ephemeral data identifier understood by the
 * backend but opaque to the operator harness.  The tactics first elicit
 * proxy identifiers from the backends, perform their necessary time and
 * difference based computations to stage integer collections, and then
 * re-invoke the backends with those same identifiers to produce the
 * necessary output.  The backend is oblivious to the navigation of time, and
 * the operator to the backend's implementation.
 *
 * key_hash: a content hash of the intended key, identical for identical
 * keys and only assumed distinct, not well distributed; collisions are the
 * backend's to resolve.  value_id: distinct for each distinct datum with the
 * same key_hash, accounting for the whole (key, val) so colliding keys stay
 * apart.  Keys with different hashes are isolated and updated independently.
 */
#ifndef DTFL_DD_INT_PROXY_INT_H
#define DTFL_DD_INT_PROXY_INT_H

#include "dd/arrange_int.h"
#include "dd/common_int.h"
#include "dtfl/arrange.h"

struct dtfl_arranged;

/*
 * debug_assert_sorted_bridge (mod.rs): panic (debug builds) unless the
 * bridge rows are strictly ascending by ((key_hash, value_id), time), so a
 * backend that presents an unconsolidated bridge is flagged.
 */
void __dtfl_bridge_assert_sorted(const __dtfl_vec *rows, unsigned tw,
    const char *who);

/*
 * For the u64 backends (not upstream): append every update of the batches,
 * or only those whose key is in the ascending keys[0..nkeys) when keys is
 * not NULL, as rows (hash, id) = (key, value) read as u64, each time joined
 * with advance_by if given.  Keys and values must be 8-byte order-preserving
 * encodings.  The rows are unsorted across batches.
 */
void __dtfl_proxy_present_u64(__dtfl_batch_ord *const *batches, size_t n,
    const __dtfl_diff_class *dc, unsigned tw, const uint64_t *keys,
    size_t nkeys, const uint64_t *advance_by, __dtfl_vec *rows);

/* ------------------------------------------------------------------ */
/* The proxy reduce framework (reduce.rs).                              */
/* ------------------------------------------------------------------ */

/*
 * ReduceInstance: a unit of proxied reduce work, presented to the backend.
 * The batch arrays are borrowed for the retire: the accumulated input
 * history, the freshly arrived input delta, the accumulated output history,
 * and lower, the compaction frontier for loading (the retire's lower bound).
 */
typedef struct __dtfl_reduce_instance {
	__dtfl_batch_ord *const *source_batches;
	size_t nsource;
	__dtfl_batch_ord *const *input_batches;
	size_t ninput;
	__dtfl_batch_ord *const *output_batches;
	size_t noutput;
	const __dtfl_antichain *lower;
} __dtfl_reduce_instance;

/*
 * ReduceWindow: one window of a retire's changed keys, a bounded,
 * hash-contiguous snip the backend sizes.  keys (uint64_t) is a contiguous,
 * ascending slice of the retire's changed keys; input and output
 * (__dtfl_bridge_upd) are the input (old and new) and output histories
 * restricted to those keys, sorted & consolidated by ((hash, id), time).
 * The tactic owns the vectors; next_window refills them.
 */
typedef struct __dtfl_reduce_window {
	__dtfl_vec keys;
	__dtfl_vec input;
	__dtfl_vec output;
} __dtfl_reduce_window;

/*
 * ProxyReduceBackend: value semantics for a proxy-space reduction, driven by
 * the ProxyReduceTactic.  The protocol for each round of invocation is
 *   seed_times begin [ next_window reduce_corrections* emit ]* finish
 *
 * seed_times: (hash, time) for the updates in the instance's novel input
 *   batches, as __dtfl_bridge_upd rows (id and diff unused), sorted by
 *   hash; seeds each key's interesting times.
 * begin: start a session building one output batch per description; the
 *   descriptions span [lower, upper) and stay owned by the caller.
 * next_window: produce the next window restricted to changed[*cursor..] and
 *   advance *cursor; false when there is none.
 * reduce_corrections: for each key k of keys[0..nkeys), the accumulated
 *   input (id, diff) rows input[in_ends[k-1]..in_ends[k]) and tentative
 *   output rows output[out_ends[k-1]..out_ends[k]); append to corr the
 *   output (id, diff) edits that bring the output in line with the input,
 *   and to corr_ends (size_t) one end offset per key.
 * emit: commit ((hash, id), time, diff) records to the batch in progress for
 *   description `tile`.
 * finish: complete the session; out[i] receives the batch for description
 *   i (owned by the caller), for every description begin was given.
 * rin / rout: the input and output diff classes (RIn, ROut).
 */
typedef struct __dtfl_proxy_reduce_backend_ops {
	void (*seed_times)(void *self, const __dtfl_reduce_instance *inst,
	    __dtfl_vec *seeds);
	void (*begin)(void *self, const __dtfl_description *tiles,
	    size_t ntiles);
	bool (*next_window)(void *self, const __dtfl_reduce_instance *inst,
	    const uint64_t *changed, size_t nchanged, size_t *cursor,
	    __dtfl_reduce_window *window);
	void (*reduce_corrections)(void *self, const uint64_t *keys,
	    size_t nkeys, const size_t *in_ends, const __dtfl_bridge_upd *input,
	    const size_t *out_ends, const __dtfl_bridge_upd *output,
	    __dtfl_vec *corr, __dtfl_vec *corr_ends);
	void (*emit)(void *self, size_t tile, const __dtfl_bridge_upd *records,
	    size_t n);
	void (*finish)(void *self, __dtfl_batch_ord **out);
	void (*free)(void *self);
} __dtfl_proxy_reduce_backend_ops;

typedef struct __dtfl_proxy_reduce_backend {
	const __dtfl_proxy_reduce_backend_ops *ops;
	void *self;			/* owned; freed by ops->free */
	const __dtfl_diff_class *rin;
	const __dtfl_diff_class *rout;
} __dtfl_proxy_reduce_backend;

/*
 * ProxyReduceTactic: matches input and output records by key_hash, holding
 * the pending interesting times beyond the upper frontier per key hash.
 * Takes ownership of the backend.
 */
typedef struct __dtfl_proxy_reduce_tactic __dtfl_proxy_reduce_tactic;

__dtfl_proxy_reduce_tactic *__dtfl_proxy_reduce_tactic_new(
    __dtfl_proxy_reduce_backend backend, const dtfl_time_class *tc);
void __dtfl_proxy_reduce_tactic_free(void *tactic);

/*
 * ReduceTactic::retire.  held must be the capability antichain.  Fills
 * batches[k] and held_index[k] (the index into held of the time to ship
 * batch k at) for k < the returned count, which is at most the length of
 * held, and new_frontier with the times to hold.
 */
size_t __dtfl_proxy_reduce_retire(__dtfl_proxy_reduce_tactic *t,
    const __dtfl_reduce_instance *inst, const __dtfl_antichain *upper,
    const __dtfl_antichain *held, __dtfl_batch_ord **batches,
    size_t *held_index, __dtfl_antichain *new_frontier);

/*
 * The u64 backend (not upstream: upstream ships the trait and no
 * implementation).  The arrangement's keys and values and the output values
 * are 8-byte order-preserving encodings (dtfl_type_u64 or dtfl_type_i64):
 * key_hash is the key's u64 reading and value_id the value's, so ids order
 * as values and there are no collisions.  reduce_corrections runs `logic`
 * (a reduce_core logic, as __dtfl_arranged_reduce_core takes) per key.
 * Takes ownership of ctx (freed by ctx_free).
 */
__dtfl_proxy_reduce_backend __dtfl_proxy_reduce_u64_backend(
    const struct dtfl_arranged *a, const dtfl_type *ovt,
    const __dtfl_diff_class *odc, __dtfl_reduce_logic_fn logic, void *ctx,
    void (*ctx_free)(void *));

/*
 * A standalone change collector for a backend that runs a reduce_core logic
 * outside the reduce engine (defined in src/dd/reduce.c, which owns the
 * __dtfl_reduce_change layout).  Values are interned as `ovt`; at returns
 * the i-th pushed (value, diff) in push order, unconsolidated.
 */
__dtfl_reduce_change *__dtfl_reduce_change_new(const dtfl_type *ovt,
    const __dtfl_diff_class *odc);
size_t __dtfl_reduce_change_len(const __dtfl_reduce_change *ch);
const void *__dtfl_reduce_change_at(const __dtfl_reduce_change *ch, size_t i,
    size_t *vlen, const void **diff);
void __dtfl_reduce_change_free(__dtfl_reduce_change *ch);

/*
 * reduce_with_tactic(trace, name, ProxyReduceTactic::new(backend)): defined
 * in src/dd/reduce.c.  Takes ownership of the backend.
 */
struct dtfl_arranged *__dtfl_arranged_reduce_with_proxy(
    struct dtfl_arranged *a, const char *name, const dtfl_type *ovt,
    const __dtfl_diff_class *odc, __dtfl_proxy_reduce_backend backend);

/* ------------------------------------------------------------------ */
/* The proxy join framework (join.rs).                                  */
/* ------------------------------------------------------------------ */

/*
 * JoinInstance: a unit of proxied join work, presented to the backend: the
 * two inputs' batches (borrowed) and lower, the compaction frontier for
 * loading (the unit's capability time, one element).
 */
typedef struct __dtfl_join_instance {
	__dtfl_batch_ord *const *batches0;
	size_t n0;
	__dtfl_batch_ord *const *batches1;
	size_t n1;
	const uint64_t *lower;
} __dtfl_join_instance;

/* A (key_hash, value_id) pair. */
typedef struct __dtfl_proxy_id {
	uint64_t hash;
	uint64_t id;
} __dtfl_proxy_id;

/*
 * ProxyJoinBackend: a type that can interpret and retire pairs of batches,
 * joined by key hashes.  The protocol invokes present0/present1 with the
 * instance to produce the proxy collection, and then any number (including
 * zero) of calls to cross to produce outputs.
 *
 * present0 / present1: append to `out` the bridge of the first / second
 *   input, restricted to the ascending key hashes filter[0..nfilter) when
 *   filter is not NULL.  The bridge MUST be sorted and consolidated by
 *   ((key_hash, value_id), time).
 * cross: from n left and right identifiers and the corresponding joined
 *   times and multiplied diffs (rout width each), produce the output.  The
 *   C Output is what the backend pushes into `out`, the unit's output
 *   builder, whose batches ship under the unit's capability.
 * r0 / r1 / rout: the input diff classes and their product's class.
 */
typedef struct __dtfl_proxy_join_backend_ops {
	void (*present0)(void *self, const __dtfl_join_instance *inst,
	    const uint64_t *filter, size_t nfilter, __dtfl_vec *out);
	void (*present1)(void *self, const __dtfl_join_instance *inst,
	    const uint64_t *filter, size_t nfilter, __dtfl_vec *out);
	void (*cross)(void *self, const __dtfl_join_instance *inst,
	    const __dtfl_proxy_id *left, const __dtfl_proxy_id *right,
	    const __dtfl_time_val *times, const void *diffs, size_t n,
	    __dtfl_consolidator *out);
	void (*free)(void *self);
} __dtfl_proxy_join_backend_ops;

typedef struct __dtfl_proxy_join_backend {
	const __dtfl_proxy_join_backend_ops *ops;
	void *self;			/* owned; freed by ops->free */
	const __dtfl_diff_class *r0;
	const __dtfl_diff_class *r1;
	const __dtfl_diff_class *rout;
} __dtfl_proxy_join_backend;

/*
 * ProxyJoinTactic::prep (join_prep): present both sides of one unit,
 * merge-match key runs over the sorted hashes, and cross-product matched
 * records into `out`.  fresh_is_input0 selects Fresh::Input0; meet is the
 * unit's capability time.
 */
void __dtfl_proxy_join_prep(__dtfl_proxy_join_backend *backend,
    __dtfl_batch_ord *const *input0, size_t n0,
    __dtfl_batch_ord *const *input1, size_t n1, bool fresh_is_input0,
    const uint64_t *meet, unsigned tw, __dtfl_consolidator *out);

/*
 * The u64 join backend (not upstream): both arrangements have 8-byte keys
 * and values; cross runs `result` per matched (key, val0, val1).  Takes
 * ownership of ctx (freed by ctx_free).
 */
__dtfl_proxy_join_backend __dtfl_proxy_join_u64_backend(
    const struct dtfl_arranged *a0, const struct dtfl_arranged *a1,
    dtfl_join_result_fn result, void *ctx, void (*ctx_free)(void *));

/*
 * Runs a join result closure for one product into `con` at (time, diff)
 * (defined in src/dd/join.c, which owns dtfl_join_emit).
 */
void __dtfl_join_result_emit(dtfl_join_result_fn result, void *user,
    const void *key, size_t klen, const void *v1, size_t v1len,
    const void *v2, size_t v2len, const uint64_t *time, const void *diff,
    __dtfl_consolidator *con);

/*
 * join_with_tactic(a0, a1, ProxyJoinTactic::new(backend)): defined in
 * src/dd/join.c.  Takes ownership of the backend.  out_type is the record
 * type the backend's cross pushes.
 */
struct dtfl_collection *__dtfl_arranged_join_with_proxy(
    struct dtfl_arranged *a0, struct dtfl_arranged *a1,
    const dtfl_type *out_type, __dtfl_proxy_join_backend backend);

#endif /* DTFL_DD_INT_PROXY_INT_H */
