/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/algo.h: graph and sequence algorithms built on the differential
 * dataflow stack (differential-dataflow src/algorithms).
 *
 * These are differential programs assembled from map, filter, join,
 * reduce, concat and iterate.  Each takes and returns dtfl_collection
 * handles over fixed record layouts of 64-bit node identifiers:
 *
 *   - a node N is a u64 record (dtfl_type_u64);
 *   - an edge (N, N) is a pair record (dtfl_type_pair of two u64);
 *   - a labelled node (N, L) is a pair record;
 *   - bfs emits (N, u32): a u64 node followed by a u32 distance, i.e.
 *     a 12-byte record (dtfl_type_pair of u64 and u32), with the u32
 *     iteration/distance counter kept as upstream (bfs.rs).
 *
 * The diff class of every input must be the signed default (i64); the
 * output carries the same class.  Node identifiers compare and hash
 * through dtfl_type_u64's order-preserving big-endian encoding, which
 * matches Rust's derived Ord for usize/u64 nodes.
 *
 * Ownership: each function returns a new dtfl_collection the caller frees
 * with dtfl_collection_free; it does not take ownership of its inputs
 * (the caller still frees them), matching the Collection methods.  All
 * run inside the collection's own scope and may create nested iterative
 * subscopes themselves.
 */
#ifndef DTFL_ALGO_H
#define DTFL_ALGO_H

#include "dtfl/base.h"
#include "dtfl/collection.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Graph algorithms (algorithms/graphs)                                */
/* ------------------------------------------------------------------ */

/*
 * bfs (bfs.rs): breadth-first distance labeling.  Given `edges`, a
 * collection of (u64 src, u64 dst) edge records, and `roots`, a
 * collection of bare u64 node records, returns a collection of
 * (u64 node, u32 dist) records giving the minimum number of steps from
 * any root to each reachable node.  The output record is a pair of a u64
 * and a u32 (12 bytes).  Nodes unreachable from a root are absent.
 *
 * The computation seeds each root at distance 0, then iterates to a
 * fixed point: join each labelled node against outgoing edges to reach
 * the destination at distance + 1, concatenate the roots back in, and
 * reduce to keep the minimum distance for each node.  The distance
 * counter is a u32, as upstream keeps it.
 */
DTFL_API dtfl_collection *dtfl_algo_bfs(dtfl_collection *edges,
    dtfl_collection *roots);

/*
 * strongly_connected (scc.rs): the subset of edges whose endpoints lie
 * in the same strongly connected component.  `graph` is a collection of
 * (u64 src, u64 dst) edge records; the result is the same record layout,
 * holding only intra-component edges.
 *
 * The computation iterates a variable seeded from the whole edge set,
 * trimming it twice per round: once by forward label propagation and
 * once by propagation on the transposed graph.  An edge survives a trim
 * when both of its endpoints carry the same minimum label.
 */
DTFL_API dtfl_collection *dtfl_algo_scc(dtfl_collection *graph);

/*
 * propagate (propagate.rs): directed label reachability.  `edges` is a
 * collection of (u64 src, u64 dst) edge records; `nodes` is a collection
 * of (u64 node, u64 label) records.  The result assigns each node the
 * minimum label reachable along directed edges, as (u64 node, u64 label)
 * records.  Labels propagate forward along edges; each node keeps the
 * smallest label it can be reached from.
 */
DTFL_API dtfl_collection *dtfl_algo_propagate(dtfl_collection *edges,
    dtfl_collection *nodes);

/*
 * sequence (sequential.rs): apply `logic` to nodes sequentially, in
 * order of node identifier.  `state` is a collection of (u64 node,
 * u64 value) records giving each node's initial state; `edges` is a
 * collection of (u64 src, u64 dst) edge records.  A node fires once all
 * of its lower-identifier neighbours have fired, and `logic` computes the
 * node's new value from the states of its neighbours.  The result is a
 * collection of (u64 node, option(u64)) records: None until the node has
 * fired, then Some(value).
 *
 * `logic` receives the node and the accumulated (value, diff) multiset of
 * its contributing neighbours, sorted by value, and writes the new u64
 * value through `out`.  It is called only on complete input.
 */
typedef void (*dtfl_algo_sequence_fn)(void *ctx, const void *node,
    size_t n, const void *const *vals, const size_t *vlens, const void *diffs,
    void *out);
DTFL_API dtfl_collection *dtfl_algo_sequence(dtfl_collection *state,
    dtfl_collection *edges, dtfl_algo_sequence_fn logic, dtfl_ctx ctx);

/*
 * bidijkstra (bijkstra.rs): bidirectional Dijkstra distance labeling.
 * `edges` is a collection of (u64 src, u64 dst) edge records; `goals` is
 * a collection of (u64 source, u64 target) pairs.  The result is a
 * collection of ((u64 source, u64 target), u32 dist) records: for each
 * goal whose source can reach its target, the shortest distance between
 * them.  The output record is a pair ((u64, u64), u32), 20 bytes.
 */
DTFL_API dtfl_collection *dtfl_algo_bidijkstra(dtfl_collection *edges,
    dtfl_collection *goals);

/* ------------------------------------------------------------------ */
/* Identifiers (identifiers.rs)                                        */
/* ------------------------------------------------------------------ */

/*
 * identifiers (identifiers.rs): assign a unique u64 identifier to each
 * element of `c`.  The result is a collection of (record, u64 id)
 * records, where `record` is the original record of `c` and the id is a
 * hash-derived unique value.  Colliding proposals are resolved by round:
 * the lower round wins, ties broken by record, and losers retry with an
 * incremented round, so every element receives a distinct identifier.
 * `rec_type` describes the records of `c`.
 */
DTFL_API dtfl_collection *dtfl_algo_identifiers(dtfl_collection *c,
    const dtfl_type *rec_type);

/* ------------------------------------------------------------------ */
/* Prefix sum (prefix_sum.rs)                                          */
/* ------------------------------------------------------------------ */

/*
 * The combine function for a prefix sum: given the key and two u64 data
 * values, write their combination through `out`.  It must be associative
 * for the prefix sum to be well defined.
 */
typedef void (*dtfl_algo_combine_fn)(void *ctx, const void *key, size_t klen,
    const void *a, const void *b, void *out);

/*
 * prefix_sum (prefix_sum.rs): data-parallel prefix sum.  `c` is a
 * collection of ((u64 index, u64 key), u64 data) records; the result is
 * the same layout where each element's data is the combination of all
 * data at the same key with index less than or equal to its own.  `zero`
 * is the identity u64 value; `combine` is the associative combiner,
 * applied per key.  The sums are computed independently for each key.
 */
DTFL_API dtfl_collection *dtfl_algo_prefix_sum(dtfl_collection *c,
    uint64_t zero, dtfl_algo_combine_fn combine, dtfl_ctx ctx);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_ALGO_H */
