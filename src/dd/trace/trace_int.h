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
#include "base/log_int.h"
#include "dtfl/timely.h"	/* dtfl_activator */

/*
 * The base pointer of an antichain's flat run, safe when it is empty (so
 * callers may pass it to the __dtfl_frontier_* helpers with n == 0 without
 * tripping the debug bounds check of __dtfl_antichain_at).
 */
static inline const uint64_t *
__dtfl_ac_base(const __dtfl_antichain *a)
{
	return (const uint64_t *)(void *)a->elems.data;
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
 *
 * DTFL_COLUMNAR (deliberate non-fidelity divergence from PLAN D2 "one
 * container", gated behind --enable-columnar): when the stored type is
 * fixed-width and memcmp-ordered (fixed_size != 0 and cmp == NULL), the
 * values live in a flat stride column instead -- `arena` holds back-to-back
 * fixed-size records with no offsets array -- and `stride` is that width.
 * stride == 0 keeps the faithful arena+offsets path.  The comparison result
 * is identical: memcmp over a fixed `stride` bytes reproduces cmp_raw when
 * every value is that width.
 */
typedef struct __dtfl_slices {
	__dtfl_vec arena;	/* bytes */
	__dtfl_vec offs;	/* size_t; len == count + 1, offs[0] == 0 */
	unsigned stride;	/* 0: arena+offs; >0: flat fixed-width column */
	/*
	 * True while every value pushed is 8 bytes, so offs[i] == 8 * i and
	 * value i is at arena + 8 * i.  A cache of the layout, kept by
	 * init, clear and push; the offsets are still written.
	 */
	bool all8;
} __dtfl_slices;

void __dtfl_slices_init(__dtfl_slices *s);
void __dtfl_slices_free(__dtfl_slices *s);
void __dtfl_slices_clear(__dtfl_slices *s);
/* Appends a value. */
void __dtfl_slices_push(__dtfl_slices *s, const void *p, size_t len);

/*
 * The fixed stride a type qualifies for in the columnar path, or 0 to keep
 * the arena+offsets path.  A type qualifies when it is memcmp-ordered
 * (cmp == NULL) and every value has the same non-zero encoded width; see
 * __dtfl_type_const_width, which (unlike fixed_size) reports 8 for a
 * (u64, unit) key.  Without DTFL_COLUMNAR nothing qualifies (faithful path).
 *
 * Applied to the trie's KEY container only.  A value batch's stored key is
 * value_len(kt, rec) bytes, which is exactly the key's constant width (the
 * builder asserts klen <= len), so every stored key is exactly stride.
 * The VALUE container is deliberately left on the arena path: a value is
 * "the record minus the key split by kt", whose width is not guaranteed to
 * equal the value type's width (e.g. arrange_by_key(u64, u64) over a
 * key-only 8-byte record stores a 0-byte value), so a value stride would
 * not be faithful.
 */
static inline unsigned
__dtfl_slices_stride_for(const dtfl_type *t)
{
#ifdef DTFL_COLUMNAR
	if (t != NULL && t->cmp == NULL) {
		size_t w = __dtfl_type_const_width(t);

		if (w != SIZE_MAX && w != 0 && w <= 0xffffu)
			return (unsigned)w;
	}
#else
	(void)t;
#endif
	return 0;
}

/*
 * Set the columnar stride on a freshly initialized, still-empty slice.
 * A no-op when stride is 0 (the faithful path).
 */
static inline void
__dtfl_slices_set_stride(__dtfl_slices *s, unsigned stride)
{
	s->stride = stride;
}

static inline size_t
__dtfl_slices_len(const __dtfl_slices *s)
{
	if (s->stride != 0)
		return s->arena.len / s->stride;
	return s->offs.len - 1;
}

static inline const void *
__dtfl_slices_get(const __dtfl_slices *s, size_t i, size_t *len)
{
	const size_t *o;

	if (s->stride != 0) {
		*len = s->stride;
		return s->arena.data + (size_t)s->stride * i;
	}
	if (s->all8) {
		*len = 8;
		return s->arena.data + 8 * i;
	}
	o = (const size_t *)(void *)s->offs.data;
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
/*
 * Upds::bounds: equal lower and upper encodes "singleton; just before
 * here", so return (lower - 1, upper) in that case.  lower must then be
 * greater than zero, as upstream asserts.
 */
static inline void
__dtfl_upds_bounds(const __dtfl_upds *u, size_t index, size_t *lo, size_t *hi)
{
	size_t lower = __dtfl_offsets_at(&u->offs, index);
	size_t upper = __dtfl_offsets_at(&u->offs, index + 1);

	if (lower == upper) {
		DTFL_ASSERT(lower > 0);
		lower -= 1;
	}
	*lo = lower;
	*hi = upper;
}

static inline size_t
__dtfl_upds_len(const __dtfl_upds *u)
{
	return __dtfl_offsets_len(&u->offs) - 1;
}

static inline const uint64_t *
__dtfl_upds_time(const __dtfl_upds *u, size_t i)
{
	return (const uint64_t *)(void *)u->times.data + (size_t)u->tw * i;
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

static inline bool
__dtfl_cursor_key_valid(const __dtfl_cursor *c, const __dtfl_batch_ord *b)
{
	return c->key_cursor < __dtfl_slices_len(&b->storage.keys);
}

bool __dtfl_cursor_val_valid(const __dtfl_cursor *c, const __dtfl_batch_ord *b);

/* Current key / value bytes; assert valid.  len receives the byte length. */
static inline const void *
__dtfl_cursor_key(const __dtfl_cursor *c, const __dtfl_batch_ord *b,
    size_t *len)
{
	DTFL_ASSERT(__dtfl_cursor_key_valid(c, b));
	return __dtfl_slices_get(&b->storage.keys, c->key_cursor, len);
}
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
	bool all_keyed;			/* every batch is a key batch */
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
	__dtfl_logger *logger;	/* retained; NULL: no size accounting */
	size_t operator_id;	/* timely operator global id */
} __dtfl_merge_batcher;

void __dtfl_merge_batcher_init(__dtfl_merge_batcher *mb, const dtfl_type *rt,
    const dtfl_time_class *tc, const __dtfl_diff_class *dc);
void __dtfl_merge_batcher_free(__dtfl_merge_batcher *mb);
/*
 * MergeBatcher::new(logger, operator_id): installs the size-accounting
 * logger (retained; NULL disables accounting).  Call right after init.
 */
void __dtfl_merge_batcher_set_logger(__dtfl_merge_batcher *mb,
    __dtfl_logger *logger, size_t operator_id);
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
	/*
	 * COMPLETE: true when the batch was merged from two inputs, whose
	 * lengths are len1/len2 (upstream keeps Some((b1, b2)) for the
	 * completion MergeEvent; the inputs themselves are released early).
	 */
	bool from_parts;
	size_t len1, len2;
} __dtfl_merge_state;

/*
 * ExertionLogic (trace/mod.rs): whether and how much effort a trace
 * should exert in the absence of updates.  `levels` lists the spine's
 * layers from large to small as (index, count, length) triples: the
 * level, the number of batches in it (0 Vacant, 1 Single, 2 Double), and
 * their total length in updates.  Returns true and sets *effort to a
 * number of updates' worth of effort to apply, or false if no work is
 * required.  Upstream shares one Arc'd closure per worker; here each
 * trace owns its logic, and ctx_free (may be NULL) releases ctx.
 */
typedef struct __dtfl_exert_level {
	size_t index;
	size_t count;
	size_t length;
} __dtfl_exert_level;

typedef bool (*__dtfl_exert_fn)(void *ctx, const __dtfl_exert_level *levels,
    size_t n, size_t *effort);

typedef struct __dtfl_exert_logic {
	__dtfl_exert_fn fn;	/* NULL: no logic (upstream None) */
	void *ctx;
	void (*ctx_free)(void *);
} __dtfl_exert_logic;

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
	__dtfl_logger *logger;	/* retained; NULL: no differential logging */
	size_t operator_id;	/* timely operator global id */
	/*
	 * The owning operator's activator (owned, may be NULL).  Used only on
	 * the worker thread that owns the operator; the writer clears it when
	 * the operator drops (__dtfl_trace_writer_free), so a trace that
	 * outlives its operator never activates a dead worker.
	 */
	dtfl_activator *activator;
	/* Parameters to `exert_logic`, containing tuples of (index, count, length). */
	__dtfl_vec exert_logic_param;	/* __dtfl_exert_level */
	/* Logic to indicate whether and how many records we should introduce in the absence of actual updates. */
	__dtfl_exert_logic exert_logic;
} __dtfl_spine;

/*
 * The default exertion logic `configure` installs (lib.rs) for
 * Config::idle_merge_effort Some(effort): Some(effort) while any level
 * holds two batches or more than one level is non-empty, else None.
 * Defined in src/dd/collection.c.
 */
__dtfl_exert_logic __dtfl_default_exert_logic(size_t effort);

/* Spine::new: with_effort(1, ..). */
void __dtfl_spine_init(__dtfl_spine *s, bool keyed, const dtfl_type *kt,
    const dtfl_type *vt, const dtfl_time_class *tc, const __dtfl_diff_class *dc);
/*
 * Spine::with_effort: each inserted batch applies `effort` times its
 * length in effort to each merge.  Zero is replaced by one.
 */
void __dtfl_spine_with_effort(__dtfl_spine *s, size_t effort, bool keyed,
    const dtfl_type *kt, const dtfl_type *vt, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc);
void __dtfl_spine_free(__dtfl_spine *s);
/* Trace::exert: apply effort as `exert_logic` directs, even without updates. */
void __dtfl_spine_exert(__dtfl_spine *s);
/* Trace::set_exert_logic: replaces (and releases) any previous logic. */
void __dtfl_spine_set_exert_logic(__dtfl_spine *s, __dtfl_exert_logic logic);
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
/*
 * Trace::new(info, logging, ..): installs the "differential/arrange"
 * logger (retained; may be NULL) and the operator's global id the spine
 * logs under.  Call before the first insert.
 */
void __dtfl_trace_set_logger(__dtfl_trace *t, __dtfl_logger *logger,
    size_t operator_id);
/* The trace's logger (borrowed, may be NULL) and its operator id. */
__dtfl_logger *__dtfl_trace_logger(const __dtfl_trace *t, size_t *operator_id);
/*
 * Trace::new(.., activator): installs the owning operator's activator
 * (takes ownership; NULL clears it), which the spine activates while
 * its exert logic asks for more work.
 */
void __dtfl_trace_set_activator(__dtfl_trace *t, dtfl_activator *activator);
/* Trace::exert and Trace::set_exert_logic (the spine's). */
void __dtfl_trace_exert(__dtfl_trace *t);
void __dtfl_trace_set_exert_logic(__dtfl_trace *t, __dtfl_exert_logic logic);
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
/* The trace's current upper frontier (includes empty seal-batch advances). */
const __dtfl_antichain *__dtfl_trace_upper(const __dtfl_trace *t);
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
/*
 * The number of batches the trace holds (pending plus every merge layer's
 * inputs), for tests that bound spine growth.  A trace whose compaction
 * holds advance keeps this logarithmic in the number of inserted batches.
 */
size_t __dtfl_trace_batch_count(const __dtfl_trace *t);
/* The trace's spine, for tests that inspect its layers. */
const __dtfl_spine *__dtfl_trace_spine(const __dtfl_trace *t);
/*
 * TraceReader::advance_upper (trace/mod.rs): advance `upper` across the
 * trace's empty batches.  An empty batch whose lower equals `upper` moves
 * `upper` to its upper; walked over every batch in order (map_batches,
 * which unlike batches_through includes empty batches), this steps `upper`
 * over the empty seal batches a writer inserts without sending them.
 */
void __dtfl_trace_advance_upper(const __dtfl_trace *t,
    __dtfl_antichain *upper);

/* ------------------------------------------------------------------ */
/* DifferentialEvent encoders (src/dd/dd_log.c), logging to `l`.        */
/* ------------------------------------------------------------------ */

void __dtfl_dd_log_batch(__dtfl_logger *l, size_t op, size_t length);
void __dtfl_dd_log_merge(__dtfl_logger *l, size_t op, size_t scale,
    size_t length1, size_t length2, bool has_complete, size_t complete);
void __dtfl_dd_log_drop(__dtfl_logger *l, size_t op, size_t length);
void __dtfl_dd_log_merge_shortfall(__dtfl_logger *l, size_t op, size_t scale,
    size_t shortfall);
void __dtfl_dd_log_trace_share(__dtfl_logger *l, size_t op, int64_t diff);
void __dtfl_dd_log_batcher(__dtfl_logger *l, size_t op, int64_t records_diff,
    int64_t size_diff, int64_t capacity_diff, int64_t allocations_diff);

#endif /* DTFL_TRACE_INT_H */
