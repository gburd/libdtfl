/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/dd/trace/trace_int.h: library-internal trace / arrangement storage.
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1:
 *   trace/cursor/mod.rs, trace/cursor/cursor_list.rs,
 *   trace/implementations/mod.rs, trace/implementations/merge_batcher.rs,
 *   trace/implementations/ord_neu.rs, trace/implementations/spine_fueled.rs,
 *   trace/mod.rs, trace/implementations/chunker.rs.
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * A trace is an append-only collection of (key, val, time, diff) updates.
 * Upstream is generic over containers; libdtfl has one, so the layered trie
 * of ord_neu (keys -> vals -> (time, diff) updates) is stored as byte arenas
 * with offset arrays plus flat time and diff columns (PLAN D2, D11).  The
 * key/val/time/diff ordering, the singleton update optimization, the fuelled
 * merger, the 8 << level spine sizing and the merge batcher's
 * frontier-is-post-consolidation behavior are kept exactly.
 *
 * Keys and values are opaque record bytes described by a dtfl_type; times are
 * runs of tw uint64_t words compared word-lexicographically (Rust Ord); diffs
 * are dw bytes with a __dtfl_diff_class algebra.  Nothing here is installed or
 * exported.
 */
#ifndef DTFL_TRACE_INT_H
#define DTFL_TRACE_INT_H

#include "dd/dd_int.h"

/*
 * The base pointer of an antichain's flat run, safe when it is empty (so
 * callers may pass it to the __dtfl_frontier_* helpers with n == 0 without
 * tripping the debug bounds check of __dtfl_antichain_at).
 */
static inline const uint64_t *
__dtfl_ac_base(const __dtfl_antichain *a)
{
	return (const uint64_t *)a->elems.data;
}

/*
 * PartialOrder::less_equal on two antichains: a <= b iff every element of b
 * is greater or equal to some element of a (Antichain::dominates with a as
 * the lower bound).  The spine and batcher use this to compare frontiers.
 */
static inline bool
__dtfl_frontier_le(const __dtfl_antichain *a, const __dtfl_antichain *b)
{
	return __dtfl_antichain_dominates(a, b);
}

/*
 * level = len.next_power_of_two().trailing_zeros(): the spine layer a batch
 * of `len` updates belongs at (ceil log2 of max(len, 1)).
 */
static inline size_t
__dtfl_level_of_len(size_t len)
{
	size_t level = 0, p = 1;

	while (p < len) {
		p <<= 1;
		level++;
	}
	return level;
}

/* ------------------------------------------------------------------ */
/* Layout: byte-arena container with an offset array (BatchContainer).  */
/* ------------------------------------------------------------------ */

/*
 * A container of variable-length byte values, the shape of upstream's
 * SliceContainer: a concatenated arena and offsets[len+1], so value i is
 * arena[offs[i], offs[i+1]).  Values compare through a dtfl_type.
 */
typedef struct __dtfl_slices {
	__dtfl_vec arena;	/* bytes */
	__dtfl_vec offs;	/* size_t; len == count + 1, offs[0] == 0 */
} __dtfl_slices;

void __dtfl_slices_init(__dtfl_slices *s);
void __dtfl_slices_free(__dtfl_slices *s);
void __dtfl_slices_clear(__dtfl_slices *s);
/* Appends a value. */
void __dtfl_slices_push(__dtfl_slices *s, const void *p, size_t len);

static inline size_t
__dtfl_slices_len(const __dtfl_slices *s)
{
	return s->offs.len - 1;
}

static inline const void *
__dtfl_slices_get(const __dtfl_slices *s, size_t i, size_t *len)
{
	const size_t *o = (const size_t *)s->offs.data;

	*len = o[i + 1] - o[i];
	return s->arena.data + o[i];
}

/*
 * BatchContainer::advance (containers.rs): count of indices in [start, end)
 * whose value is strictly less than `key`, using the exponential search the
 * cursor's seek relies on.  `cmp` returns <0 when the indexed value is less
 * than key.  The predicate (value < key) must stay false once false.
 */
size_t __dtfl_slices_advance(const __dtfl_slices *s, const dtfl_type *t,
    size_t start, size_t end, const void *key, size_t klen);

/*
 * An offset column (OffsetList): one size_t per entry.  Upstream packs
 * small offsets as u32; that is a memory optimization with identical
 * semantics, so a flat size_t column stands in.
 */
typedef struct __dtfl_offsets {
	__dtfl_vec v;		/* size_t */
} __dtfl_offsets;

void __dtfl_offsets_init(__dtfl_offsets *o);
void __dtfl_offsets_free(__dtfl_offsets *o);
void __dtfl_offsets_clear(__dtfl_offsets *o);
void __dtfl_offsets_push(__dtfl_offsets *o, size_t v);

static inline size_t
__dtfl_offsets_len(const __dtfl_offsets *o)
{
	return o->v.len;
}

static inline size_t
__dtfl_offsets_at(const __dtfl_offsets *o, size_t i)
{
	return *DTFL_VEC_AT(&o->v, size_t, i);
}

/* ------------------------------------------------------------------ */
/* ord_neu storage: the layered trie.                                  */
/* ------------------------------------------------------------------ */

/*
 * Vals (ord_neu::layers::Vals): for each key, a list of values.  offs has
 * len+1 entries; key i's values are vals[offs[i], offs[i+1]).
 */
typedef struct __dtfl_vals {
	__dtfl_offsets offs;	/* key -> val range; starts with 0 */
	__dtfl_slices vals;	/* concatenated values */
} __dtfl_vals;

void __dtfl_vals_init(__dtfl_vals *v);
void __dtfl_vals_free(__dtfl_vals *v);
/* Lower and upper bounds in vals of the list indexed by `index`. */
void __dtfl_vals_bounds(const __dtfl_vals *v, size_t index, size_t *lo,
    size_t *hi);

/*
 * Upds (ord_neu::layers::Upds): for each value, a list of (time, diff)
 * updates.  offs has len+1 entries.  An empty slice (equal lower and
 * upper, lower > 0) encodes "the single update just before here" (the
 * singleton optimization): bounds() returns (lower-1, upper).
 */
typedef struct __dtfl_upds {
	__dtfl_offsets offs;	/* val -> upd range; starts with 0 */
	__dtfl_vec times;	/* tw uint64_t per update */
	__dtfl_vec diffs;	/* dw bytes per update */
	size_t nupds;		/* stored updates (times/diffs length) */
	unsigned tw;
	unsigned dw;
} __dtfl_upds;

void __dtfl_upds_init(__dtfl_upds *u, unsigned tw, unsigned dw);
void __dtfl_upds_free(__dtfl_upds *u);
/* bounds, resolving the singleton encoding. */
void __dtfl_upds_bounds(const __dtfl_upds *u, size_t index, size_t *lo,
    size_t *hi);

static inline size_t
__dtfl_upds_len(const __dtfl_upds *u)
{
	return __dtfl_offsets_len(&u->offs) - 1;
}

static inline const uint64_t *
__dtfl_upds_time(const __dtfl_upds *u, size_t i)
{
	return (const uint64_t *)u->times.data + (size_t)u->tw * i;
}

static inline const void *
__dtfl_upds_diff(const __dtfl_upds *u, size_t i)
{
	return u->diffs.data + (size_t)u->dw * i;
}

/*
 * UpdsBuilder (ord_neu::layers::UpdsBuilder): stages (time, diff) updates,
 * consolidates them, and seals them into an Upds with the singleton
 * optimization.  tc and dc are borrowed; the caller keeps them alive.
 */
typedef struct __dtfl_upds_builder {
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
	__dtfl_vec stash_t;	/* tw words per staged update */
	__dtfl_vec stash_d;	/* dw bytes per staged update */
	size_t total;		/* consolidated updates sealed, incl. singletons */
	unsigned tw;
	unsigned dw;
} __dtfl_upds_builder;

void __dtfl_upds_builder_init(__dtfl_upds_builder *b, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc);
void __dtfl_upds_builder_free(__dtfl_upds_builder *b);
/* Stages one update. */
void __dtfl_upds_builder_push(__dtfl_upds_builder *b, const uint64_t *time,
    const void *diff);
/*
 * Consolidate and insert the staged updates into `u`.  Returns true if the
 * result was non-empty (so the caller writes the value/key).
 */
bool __dtfl_upds_builder_seal(__dtfl_upds_builder *b, __dtfl_upds *u);

/*
 * OrdValStorage / OrdKeyStorage.  A key batch is a val batch whose values
 * are the one unit value; its cursor reports exactly one value per key.
 */
typedef struct __dtfl_ord_storage {
	__dtfl_slices keys;	/* ordered distinct keys */
	__dtfl_vals vals;	/* key -> values (empty.vals for key batches) */
	__dtfl_upds upds;	/* val -> updates (key batches index by key) */
	bool keyed;		/* true: OrdKeyBatch (no value layer) */
} __dtfl_ord_storage;

/*
 * OrdValBatch / OrdKeyBatch: the trie storage, its time description, and the
 * update count (counts singleton-optimized updates too).  Refcounted so the
 * spine shares it (Rc<OrdValBatch>); kw gives the key split for key||val
 * records, and kt/vt/dc describe the key, value and diff ordering.
 */
typedef struct __dtfl_batch_ord {
	__dtfl_arc ref;
	__dtfl_ord_storage storage;
	__dtfl_description description;
	size_t updates;
	const dtfl_type *kt;		/* key type (not retained) */
	const dtfl_type *vt;		/* value type (not retained) */
	const dtfl_time_class *tc;	/* time class (not retained) */
	const __dtfl_diff_class *dc;	/* diff class (not retained) */
} __dtfl_batch_ord;

__dtfl_batch_ord *__dtfl_batch_ord_retain(__dtfl_batch_ord *b);
void __dtfl_batch_ord_release(__dtfl_batch_ord *b);
/* BatchReader::len / is_empty / lower / upper. */
static inline size_t
__dtfl_batch_ord_len(const __dtfl_batch_ord *b)
{
	return b->updates;
}
static inline bool
__dtfl_batch_ord_is_empty(const __dtfl_batch_ord *b)
{
	return b->updates == 0;
}
const __dtfl_antichain *__dtfl_batch_ord_lower(const __dtfl_batch_ord *b);
const __dtfl_antichain *__dtfl_batch_ord_upper(const __dtfl_batch_ord *b);

/*
 * Batch::empty: an empty batch over [lower, upper) with since = minimum.
 * Takes ownership of the two antichains (moves them, as Description::new).
 */
__dtfl_batch_ord *__dtfl_batch_ord_empty(bool keyed, const dtfl_type *kt,
    const dtfl_type *vt, const dtfl_time_class *tc, const __dtfl_diff_class *dc,
    __dtfl_antichain *lower, __dtfl_antichain *upper);

/* ------------------------------------------------------------------ */
/* Builder (OrdValBuilder / OrdKeyBuilder).                            */
/* ------------------------------------------------------------------ */

/*
 * Builds a batch from a chain of sorted, consolidated updates.  Updates are
 * pushed as (key, val, time, diff); the chain must be ordered by (key, val)
 * with each (key, val) group contiguous, as the merge batcher guarantees.
 */
typedef struct __dtfl_builder_ord {
	__dtfl_ord_storage result;
	__dtfl_upds_builder staging;
	bool have_last;		/* a (key, val) group is in progress */
	const dtfl_type *kt;
	const dtfl_type *vt;
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
} __dtfl_builder_ord;

void __dtfl_builder_ord_init(__dtfl_builder_ord *bld, bool keyed,
    const dtfl_type *kt, const dtfl_type *vt, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc, size_t keys, size_t vals, size_t upds);
/* Pushes one update.  val/vlen are ignored for key batches. */
void __dtfl_builder_ord_push(__dtfl_builder_ord *bld, const void *key,
    size_t klen, const void *val, size_t vlen, const uint64_t *time,
    const void *diff);
/* Completes building and returns the batch (moves `desc` in). */
__dtfl_batch_ord *__dtfl_builder_ord_done(__dtfl_builder_ord *bld,
    __dtfl_description *desc);

/*
 * Builder::seal over a chain of chunks (each a __dtfl_batch of consolidated
 * (key||val, time, diff) rows): counts, builds, and returns the batch.  kw is
 * the key split within a record for a value batch (0 for a key batch, whole
 * record is the key).
 */
__dtfl_batch_ord *__dtfl_builder_ord_seal(bool keyed, const dtfl_type *kt,
    const dtfl_type *vt, const dtfl_time_class *tc, const __dtfl_diff_class *dc,
    __dtfl_batch **chunks, size_t nchunks, __dtfl_description *desc);

/* ------------------------------------------------------------------ */
/* Merger (OrdValMerger / OrdKeyMerger).                               */
/* ------------------------------------------------------------------ */

/*
 * A fuelled merge of two batches.  work() does bounded effort measured by the
 * growth of staging.total (upstream's accounting); when fuel remains positive
 * after a call, done() extracts the merged batch.  since is batch1.since join
 * batch2.since join compaction_frontier; copied times are advance_by'd to it.
 */
typedef struct __dtfl_merger_ord {
	size_t key_cursor1;
	size_t key_cursor2;
	__dtfl_ord_storage result;
	__dtfl_description description;
	__dtfl_upds_builder staging;
	__dtfl_batch_ord *src1;		/* retained */
	__dtfl_batch_ord *src2;		/* retained */
	bool keyed;
	const dtfl_type *kt;
	const dtfl_type *vt;
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
} __dtfl_merger_ord;

/* Merger::new: begin a merge; asserts src1->upper == src2->lower. */
void __dtfl_merger_ord_new(__dtfl_merger_ord *m, __dtfl_batch_ord *src1,
    __dtfl_batch_ord *src2, const __dtfl_antichain *compaction_frontier);
/* Merger::work: apply up to *fuel effort. */
void __dtfl_merger_ord_work(__dtfl_merger_ord *m, int64_t *fuel);
/* Merger::done: extract the batch (consumes m; releases the sources). */
__dtfl_batch_ord *__dtfl_merger_ord_done(__dtfl_merger_ord *m);

/* ------------------------------------------------------------------ */
/* Cursor.                                                             */
/* ------------------------------------------------------------------ */

/*
 * A cursor over a single ord batch (OrdValCursor / OrdKeyCursor).  Navigates
 * keys and, per key, values; map_times walks the (time, diff) updates of the
 * current value.  storage is the __dtfl_batch_ord it reads.
 */
typedef struct __dtfl_cursor {
	size_t key_cursor;
	size_t val_cursor;	/* value batches */
	bool val_stepped;	/* key batches: single value consumed */
} __dtfl_cursor;

void __dtfl_cursor_init(__dtfl_cursor *c, const __dtfl_batch_ord *b);
bool __dtfl_cursor_key_valid(const __dtfl_cursor *c, const __dtfl_batch_ord *b);
bool __dtfl_cursor_val_valid(const __dtfl_cursor *c, const __dtfl_batch_ord *b);
/* Current key / value bytes; assert valid.  len receives the byte length. */
const void *__dtfl_cursor_key(const __dtfl_cursor *c,
    const __dtfl_batch_ord *b, size_t *len);
const void *__dtfl_cursor_val(const __dtfl_cursor *c,
    const __dtfl_batch_ord *b, size_t *len);
/* Visit each (time, diff) of the current value. */
void __dtfl_cursor_map_times(__dtfl_cursor *c, const __dtfl_batch_ord *b,
    void (*logic)(const uint64_t *time, const void *diff, void *ctx),
    void *ctx);
void __dtfl_cursor_step_key(__dtfl_cursor *c, const __dtfl_batch_ord *b);
void __dtfl_cursor_seek_key(__dtfl_cursor *c, const __dtfl_batch_ord *b,
    const void *key, size_t klen);
void __dtfl_cursor_step_val(__dtfl_cursor *c, const __dtfl_batch_ord *b);
void __dtfl_cursor_seek_val(__dtfl_cursor *c, const __dtfl_batch_ord *b,
    const void *val, size_t vlen);
void __dtfl_cursor_rewind_keys(__dtfl_cursor *c, const __dtfl_batch_ord *b);
void __dtfl_cursor_rewind_vals(__dtfl_cursor *c, const __dtfl_batch_ord *b);

/* ------------------------------------------------------------------ */
/* CursorList (cursor_list.rs): a merge of per-batch cursors.          */
/* ------------------------------------------------------------------ */

/*
 * A cursor over a sequence of batches, presenting their union in key then
 * value order (cursor_list::CursorList).  It tracks the indices of cursors at
 * the minimum key, and among those the indices at the minimum value, so
 * map_times fires every (time, diff) at the current (key, val) across batches.
 * It owns references to its batches (TraceReader::cursor_through returns the
 * batch vector as storage; here the list holds them).
 */
typedef struct __dtfl_cursor_list {
	__dtfl_cursor *cursors;		/* one per batch */
	__dtfl_batch_ord **batches;	/* retained */
	size_t n;
	__dtfl_vec min_key;		/* size_t indices */
	__dtfl_vec min_val;		/* size_t indices */
	const dtfl_type *kt;
	const dtfl_type *vt;
} __dtfl_cursor_list;

/* Takes ownership of the n batch references in `batches`. */
void __dtfl_cursor_list_init(__dtfl_cursor_list *cl, __dtfl_batch_ord **batches,
    size_t n, const dtfl_type *kt, const dtfl_type *vt);
void __dtfl_cursor_list_free(__dtfl_cursor_list *cl);
bool __dtfl_cursor_list_key_valid(const __dtfl_cursor_list *cl);
bool __dtfl_cursor_list_val_valid(const __dtfl_cursor_list *cl);
const void *__dtfl_cursor_list_key(const __dtfl_cursor_list *cl, size_t *len);
const void *__dtfl_cursor_list_val(const __dtfl_cursor_list *cl, size_t *len);
void __dtfl_cursor_list_map_times(__dtfl_cursor_list *cl,
    void (*logic)(const uint64_t *time, const void *diff, void *ctx),
    void *ctx);
void __dtfl_cursor_list_step_key(__dtfl_cursor_list *cl);
void __dtfl_cursor_list_seek_key(__dtfl_cursor_list *cl, const void *key,
    size_t klen);
void __dtfl_cursor_list_step_val(__dtfl_cursor_list *cl);
void __dtfl_cursor_list_seek_val(__dtfl_cursor_list *cl, const void *val,
    size_t vlen);
void __dtfl_cursor_list_rewind_keys(__dtfl_cursor_list *cl);
void __dtfl_cursor_list_rewind_vals(__dtfl_cursor_list *cl);

/* ------------------------------------------------------------------ */
/* MergeBatcher (merge_batcher.rs) + its VecMerger over __dtfl_batch.   */
/* ------------------------------------------------------------------ */

/*
 * A chain is a sequence of chunks; a chunk is a __dtfl_batch of consolidated
 * (key||val, time, diff) rows sorted by (record, time).  The batcher merges
 * chains geometrically by summed update count and extracts, on seal, the
 * updates not in advance of `upper`, with the frontier taken over the
 * post-consolidation survivors (the behavior the two ported tests check).
 */
typedef struct __dtfl_chain {
	__dtfl_batch **chunks;	/* owned */
	size_t n;
} __dtfl_chain;

typedef struct __dtfl_merge_batcher {
	__dtfl_vec chains;	/* (size_t weight, __dtfl_chain) pairs */
	const dtfl_type *rt;	/* record (key||val) type */
	const __dtfl_diff_class *dc;
	const dtfl_time_class *tc;
	unsigned tw;
	unsigned dw;
	__dtfl_antichain lower;
	__dtfl_antichain frontier;	/* after the last seal */
	size_t cap;		/* target chunk capacity, a power of two */
} __dtfl_merge_batcher;

void __dtfl_merge_batcher_init(__dtfl_merge_batcher *mb, const dtfl_type *rt,
    const dtfl_time_class *tc, const __dtfl_diff_class *dc);
void __dtfl_merge_batcher_free(__dtfl_merge_batcher *mb);
/* PushInto: insert one chunk as a one-element chain (takes ownership). */
void __dtfl_merge_batcher_push(__dtfl_merge_batcher *mb, __dtfl_batch *chunk);
/*
 * chain_push, exposed for the two frontier tests: insert an owned chain
 * (array of chunk pointers) without the geometric fusing of push.
 */
void __dtfl_merge_batcher_chain_push(__dtfl_merge_batcher *mb,
    __dtfl_batch **chunks, size_t n);
/*
 * seal: merge all chains, extract the updates not in advance of `upper`
 * into *out (owned chunk array, *nout entries), set the frontier over the
 * kept survivors, and fill *desc (lower = prior upper, upper, since =
 * minimum).  `upper` is borrowed.
 */
void __dtfl_merge_batcher_seal(__dtfl_merge_batcher *mb,
    const __dtfl_antichain *upper, __dtfl_batch ***out, size_t *nout,
    __dtfl_description *desc);
/* The frontier after the most recent seal (borrowed). */
const __dtfl_antichain *__dtfl_merge_batcher_frontier(
    const __dtfl_merge_batcher *mb);

/* ------------------------------------------------------------------ */
/* Chunker (chunker.rs): consolidate a stream of updates into chunks.  */
/* ------------------------------------------------------------------ */

/*
 * ContainerChunker: accepts updates, consolidates them into chunks of about
 * `cap` rows (default_capacity), and hands finished chunks to the caller.
 * Chunks feed __dtfl_merge_batcher_push.  rt and dc are borrowed.
 */
typedef struct __dtfl_chunker {
	__dtfl_consolidator con;	/* buffers and consolidates */
} __dtfl_chunker;

void __dtfl_chunker_init(__dtfl_chunker *ch, const dtfl_type *rt, unsigned tw,
    const __dtfl_diff_class *dc);
void __dtfl_chunker_free(__dtfl_chunker *ch);
void __dtfl_chunker_push(__dtfl_chunker *ch, const void *rec, size_t len,
    const uint64_t *time, const void *diff);
/* A finished chunk, owned by the caller, or NULL. */
__dtfl_batch *__dtfl_chunker_extract(__dtfl_chunker *ch);
__dtfl_batch *__dtfl_chunker_finish(__dtfl_chunker *ch);

/* ------------------------------------------------------------------ */
/* Spine (spine_fueled.rs): a fuelled geometric stack of batches.      */
/* ------------------------------------------------------------------ */

typedef enum __dtfl_merge_kind {
	DTFL_MERGE_VACANT = 0,
	DTFL_MERGE_SINGLE,	/* one batch (possibly structurally empty) */
	DTFL_MERGE_INPROGRESS,	/* two batches merging */
	DTFL_MERGE_COMPLETE	/* a completed merge, ready to extract */
} __dtfl_merge_kind;

/*
 * One layer of the spine.  SINGLE and COMPLETE carry an optional batch
 * (`present` false is a structurally empty batch used for bookkeeping, the
 * None of MergeState::Single).  INPROGRESS carries both inputs and a merger.
 */
typedef struct __dtfl_merge_state {
	__dtfl_merge_kind kind;
	bool present;			/* SINGLE/COMPLETE: batch is real */
	__dtfl_batch_ord *batch;	/* SINGLE/COMPLETE batch */
	__dtfl_batch_ord *in1;		/* INPROGRESS inputs */
	__dtfl_batch_ord *in2;
	__dtfl_merger_ord merger;	/* INPROGRESS */
} __dtfl_merge_state;

typedef struct __dtfl_spine {
	__dtfl_antichain logical_frontier;
	__dtfl_antichain physical_frontier;
	__dtfl_vec merging;	/* __dtfl_merge_state */
	__dtfl_vec pending;	/* __dtfl_batch_ord * (owned) */
	__dtfl_antichain upper;
	size_t effort;
	bool keyed;
	const dtfl_type *kt;
	const dtfl_type *vt;
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
} __dtfl_spine;

void __dtfl_spine_init(__dtfl_spine *s, bool keyed, const dtfl_type *kt,
    const dtfl_type *vt, const dtfl_time_class *tc, const __dtfl_diff_class *dc);
void __dtfl_spine_free(__dtfl_spine *s);
/* Trace::insert: add a batch (takes ownership).  Asserts contiguity. */
void __dtfl_spine_insert(__dtfl_spine *s, __dtfl_batch_ord *batch);
/* Trace::close: finish with an empty batch to [upper, empty). */
void __dtfl_spine_close(__dtfl_spine *s);
/* TraceReader::set_physical_compaction: advance and consider merges. */
void __dtfl_spine_set_physical_compaction(__dtfl_spine *s,
    const __dtfl_antichain *frontier);
/* TraceReader::set_logical_compaction. */
void __dtfl_spine_set_logical_compaction(__dtfl_spine *s,
    const __dtfl_antichain *frontier);
/*
 * TraceReader::batches_through: the batches whose updates lie at times not in
 * advance of `upper`, as a new owned array (*nout entries); or sets *nout to
 * SIZE_MAX if there is no clean cut.  Pass a NULL/empty upper for all batches.
 */
__dtfl_batch_ord **__dtfl_spine_batches_through(__dtfl_spine *s,
    const __dtfl_antichain *upper, size_t *nout);
/* introduce_batch, exposed for the spine PBT's fuel exercise. */
void __dtfl_spine_introduce_batch(__dtfl_spine *s, __dtfl_batch_ord *batch,
    size_t batch_index);

/* ------------------------------------------------------------------ */
/* Trace (trace/mod.rs): the facade over the spine.                    */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_trace __dtfl_trace;

__dtfl_trace *__dtfl_trace_new(bool keyed, const dtfl_type *kt,
    const dtfl_type *vt, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc);
void __dtfl_trace_free(__dtfl_trace *t);
void __dtfl_trace_insert(__dtfl_trace *t, __dtfl_batch_ord *batch);
void __dtfl_trace_close(__dtfl_trace *t);
void __dtfl_trace_set_logical_compaction(__dtfl_trace *t,
    const __dtfl_antichain *frontier);
void __dtfl_trace_set_physical_compaction(__dtfl_trace *t,
    const __dtfl_antichain *frontier);
/* TraceReader::get_logical_compaction / get_physical_compaction (borrowed). */
const __dtfl_antichain *__dtfl_trace_get_logical_compaction(
    const __dtfl_trace *t);
const __dtfl_antichain *__dtfl_trace_get_physical_compaction(
    const __dtfl_trace *t);
/* Returns false if `upper` has no clean cut; else fills *out. */
bool __dtfl_trace_cursor_through(__dtfl_trace *t,
    const __dtfl_antichain *upper, __dtfl_cursor_list *out);
void __dtfl_trace_cursor(__dtfl_trace *t, __dtfl_cursor_list *out);
/*
 * TraceReader::map_batches: every batch currently in the trace, as a new
 * owned array (*nout entries, each retained; the caller releases them and
 * frees the array).  Used by the agent's new_listener to seed a queue.
 */
__dtfl_batch_ord **__dtfl_trace_map_batches(__dtfl_trace *t, size_t *nout);

#endif /* DTFL_TRACE_INT_H */
