/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/dd/trace/chunk_int.h: library-internal chunked, spillable trace storage.
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1:
 *   trace/chunk/mod.rs, trace/chunk/vec.rs, columnar/trace/spill.rs.
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * A chunk is a non-empty, consolidated, sorted run of (key, val, time, diff)
 * updates; a sequence of chunks is also consolidated and sorted.  Upstream is
 * generic over the chunk layout (a Chunk trait with Vec and columnar
 * implementations); libdtfl has one container (PLAN D2), so a chunk is a
 * __dtfl_batch of consolidated (record = key||val, time, diff) rows, the same
 * chunk format the merge batcher and consolidator already build.  The columnar
 * layout is subsumed by the flat byte arena, so the Chunk trait collapses to
 * concrete free functions over this one layout.
 *
 * The whole-chunk operations (merge, extract, advance, settle) move whole
 * chunks between deques as units, which is what lets a chunk be spilled to and
 * read back from secondary storage transparently.  TARGET, the intended chunk
 * size, is the upstream VecChunk constant (8192) and is kept exactly.
 *
 * ChunkBatch is a vec of chunks plus a Description; a ChunkSpine is the
 * existing fuelled spine (spine_fueled.c) over Rc-shared ChunkBatches, built
 * through ChunkBatchBuilder and merged by ChunkBatchMerger.  Spill pages whole
 * committed chunks out to a pluggable byte store on settle and fetches them
 * back on access, so the cursor sees intact data (columnar/trace/spill.rs).
 */
#ifndef DTFL_TRACE_CHUNK_INT_H
#define DTFL_TRACE_CHUNK_INT_H

#include "dd/trace/trace_int.h"

/* The chunk size: VecChunk's Chunk::TARGET (vec.rs), also the buffer size. */
#define DTFL_CHUNK_TARGET 8192

/* ------------------------------------------------------------------ */
/* Chunk and chunk deque.                                              */
/* ------------------------------------------------------------------ */

/*
 * A chunk is a __dtfl_batch of consolidated (record = key||val, time, diff)
 * rows sorted by (record, time).  Clone is a refcount bump (__dtfl_batch_retain),
 * so the merger shares source chunks rather than copying them.
 */
typedef __dtfl_batch __dtfl_chunk;

/*
 * A deque of chunk pointers (owned references): a VecDeque<Chunk>.  Chunks are
 * pushed at the back and popped at the front; a withheld carry is pushed to the
 * front.  The chunk operations drive these.
 */
typedef struct __dtfl_chunk_deque {
	__dtfl_vec v;		/* __dtfl_chunk * (owned) */
	size_t head;		/* index of the front element in v */
} __dtfl_chunk_deque;

void __dtfl_chunk_deque_init(__dtfl_chunk_deque *d);
void __dtfl_chunk_deque_free(__dtfl_chunk_deque *d);

static inline size_t
__dtfl_chunk_deque_len(const __dtfl_chunk_deque *d)
{
	return d->v.len - d->head;
}

static inline bool
__dtfl_chunk_deque_empty(const __dtfl_chunk_deque *d)
{
	return d->v.len == d->head;
}

/* The i-th chunk from the front (borrowed). */
static inline __dtfl_chunk *
__dtfl_chunk_deque_at(const __dtfl_chunk_deque *d, size_t i)
{
	return *DTFL_VEC_AT(&d->v, __dtfl_chunk *, d->head + i);
}

/* push_back: append a chunk (takes ownership). */
void __dtfl_chunk_deque_push_back(__dtfl_chunk_deque *d, __dtfl_chunk *c);
/* push_front: prepend a chunk (takes ownership); the withheld carry. */
void __dtfl_chunk_deque_push_front(__dtfl_chunk_deque *d, __dtfl_chunk *c);
/* pop_front: remove and return the front chunk (ownership passes out), or NULL. */
__dtfl_chunk *__dtfl_chunk_deque_pop_front(__dtfl_chunk_deque *d);
/* Append every chunk of src to the back of d; src is left empty. */
void __dtfl_chunk_deque_extend(__dtfl_chunk_deque *d, __dtfl_chunk_deque *src);

/* ------------------------------------------------------------------ */
/* The chunk layout context: the record/time/diff descriptors.         */
/* ------------------------------------------------------------------ */

/*
 * The descriptors every chunk operation needs: the record order (key||val, or
 * key for a key batch), the key split within a record, the time depth, and the
 * diff algebra.  Borrowed; the caller keeps them alive.  Upstream carries these
 * in the type parameters; libdtfl threads them explicitly.
 */
typedef struct __dtfl_spill_state __dtfl_spill_state;

typedef struct __dtfl_chunk_ctx {
	const dtfl_type *rt;		/* record (key||val) order */
	const dtfl_type *kt;		/* key order and split */
	const dtfl_type *vt;		/* value order */
	const dtfl_time_class *tc;
	const __dtfl_diff_class *dc;
	__dtfl_spill_state *spill;	/* NULL = never page (resident) */
	bool keyed;			/* key batch: the whole record is the key */
	unsigned tw;
	unsigned dw;
} __dtfl_chunk_ctx;

void __dtfl_chunk_ctx_init(__dtfl_chunk_ctx *x, bool keyed, const dtfl_type *rt,
    const dtfl_type *kt, const dtfl_type *vt, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc);

/* ------------------------------------------------------------------ */
/* The whole-chunk operations (Chunk trait, specialized to the one layout). */
/* ------------------------------------------------------------------ */

/*
 * Merge the fronts of two non-empty deques through their shared horizon (the
 * lesser of the two last (key, val, time) updates): a two-pointer binary merge
 * that consolidates equal triples and bulk-copies disjoint runs, pushing the
 * survivor's unconsumed suffix back to the front of its deque (vec.rs merge).
 * Consumes at least one input; the caller re-invokes until a deque empties.
 */
void __dtfl_chunk_merge(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *in1,
    __dtfl_chunk_deque *in2, __dtfl_chunk_deque *out);

/*
 * Partition one front chunk by `frontier`: rows at times greater-or-equal the
 * frontier go to `keep` (and fold into `residual`), the rest to `ship`
 * (vec.rs extract).  One input chunk per call.
 */
void __dtfl_chunk_extract(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *input,
    const __dtfl_antichain *frontier, __dtfl_antichain *residual,
    __dtfl_chunk_deque *keep, __dtfl_chunk_deque *ship);

/*
 * Advance every complete (key, val) group's times by `frontier` and
 * consolidate, emitting TARGET-sized chunks; withhold the last (possibly
 * growing) group as the carry unless `done` (vec.rs advance).
 */
void __dtfl_chunk_advance(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *input,
    const __dtfl_antichain *frontier, bool done, __dtfl_chunk_deque *out);

/*
 * Reshape `input` to a maximal packing: every chunk at most TARGET, every
 * adjacent pair summing past TARGET.  On `done` the input is fully flushed.
 * This is also where a committed chunk may be sealed (spilled); see spill.c.
 */
void __dtfl_chunk_settle(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *input,
    bool done, __dtfl_chunk_deque *out);

/*
 * The maximal-packing driver settle delegates to (mod.rs pack): coalesce by
 * `combine`, peel over-sized chunks with `split`, pass each committed chunk
 * through `seal`.  Exposed so settle (and tests of the spill seal) can drive
 * it with layout- and spill-specific closures.
 */
void __dtfl_chunk_pack(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *input,
    bool done, __dtfl_chunk_deque *out,
    void (*combine)(const __dtfl_chunk_ctx *, __dtfl_chunk *acc,
	__dtfl_chunk *next, void *ctx),
    void (*split)(const __dtfl_chunk_ctx *, __dtfl_chunk *chunk, size_t n,
	__dtfl_chunk **head, __dtfl_chunk **tail, void *ctx),
    __dtfl_chunk *(*seal)(const __dtfl_chunk_ctx *, __dtfl_chunk *chunk,
	void *ctx),
    void *ctx);

/*
 * is_graded (mod.rs): every chunk at most TARGET, every adjacent pair summing
 * past TARGET.  A test/debug check of settle's output.
 */
bool __dtfl_chunk_is_graded(const __dtfl_chunk_deque *chunks);

/* settle_all (mod.rs): settle `input` to completion into `out` (done = true). */
void __dtfl_chunk_settle_all(const __dtfl_chunk_ctx *x,
    __dtfl_chunk_deque *input, __dtfl_chunk_deque *out);

/*
 * merge_chains (mod.rs): merge two full chains to completion by ticking
 * __dtfl_chunk_merge until one empties, then appending the other's remainder.
 * The chains are consumed.
 */
void __dtfl_chunk_merge_chains(const __dtfl_chunk_ctx *x,
    __dtfl_chunk_deque *chain1, __dtfl_chunk_deque *chain2,
    __dtfl_chunk_deque *out);

/* ------------------------------------------------------------------ */
/* ChunkBatch: a chunk sequence plus a Description, refcounted.         */
/* ------------------------------------------------------------------ */

/*
 * A ChunkBatch (mod.rs): ordered, consolidated chunks whose concatenation is
 * the batch, plus the lower/upper/since frontiers.  Refcounted so the spine
 * shares it (Rc<ChunkBatch>).  Carries the layout context for its cursor and
 * for merges.
 */
typedef struct __dtfl_chunk_batch {
	__dtfl_arc ref;
	__dtfl_vec chunks;		/* __dtfl_chunk * (owned) */
	__dtfl_description description;
	__dtfl_chunk_ctx ctx;
} __dtfl_chunk_batch;

/* ChunkBatch::new: assemble from ordered chunks (takes the n refs and desc). */
__dtfl_chunk_batch *__dtfl_chunk_batch_new(const __dtfl_chunk_ctx *x,
    __dtfl_chunk **chunks, size_t n, __dtfl_description *desc);
/* Batch::empty: an empty batch over [lower, upper) with since = minimum. */
__dtfl_chunk_batch *__dtfl_chunk_batch_empty(const __dtfl_chunk_ctx *x,
    __dtfl_antichain *lower, __dtfl_antichain *upper);
__dtfl_chunk_batch *__dtfl_chunk_batch_retain(__dtfl_chunk_batch *b);
void __dtfl_chunk_batch_release(__dtfl_chunk_batch *b);

/* BatchReader::len: the summed update count over the chunks. */
size_t __dtfl_chunk_batch_len(const __dtfl_chunk_batch *b);
static inline bool
__dtfl_chunk_batch_is_empty(const __dtfl_chunk_batch *b)
{
	return __dtfl_chunk_batch_len(b) == 0;
}
const __dtfl_antichain *__dtfl_chunk_batch_lower(const __dtfl_chunk_batch *b);
const __dtfl_antichain *__dtfl_chunk_batch_upper(const __dtfl_chunk_batch *b);

/* ------------------------------------------------------------------ */
/* ChunkBatchCursor: the straddle cursor over a ChunkBatch.             */
/* ------------------------------------------------------------------ */

/*
 * A cursor over a ChunkBatch (mod.rs ChunkBatchCursor).  Chunk breakpoints are
 * unconstrained, so a key -- or a (key, val) -- may straddle consecutive
 * chunks; the chunks are one globally sorted sequence cut at arbitrary points,
 * so a straddle concatenates rather than merges.  The cursor holds the chunk
 * being read and a per-chunk cursor (over the batch's rows), galloping chunk
 * bounds from the current key's first chunk on a seek.
 */
typedef struct __dtfl_chunk_cursor {
	size_t key_chunk;	/* first chunk of the current key's run */
	size_t chunk;		/* chunk currently being read; >= key_chunk */
	size_t pos;		/* row index within `chunk` of the current key */
	size_t vpos;		/* row index within `chunk` of the (key, val) */
} __dtfl_chunk_cursor;

void __dtfl_chunk_cursor_init(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b);
bool __dtfl_chunk_cursor_key_valid(const __dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b);
bool __dtfl_chunk_cursor_val_valid(const __dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b);
/* Current key / value bytes; assert valid.  len receives the byte length. */
const void *__dtfl_chunk_cursor_key(const __dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b, size_t *len);
const void *__dtfl_chunk_cursor_val(const __dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b, size_t *len);
/* Visit each (time, diff) of the current (key, val), across straddles. */
void __dtfl_chunk_cursor_map_times(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b,
    void (*logic)(const uint64_t *time, const void *diff, void *ctx),
    void *ctx);
void __dtfl_chunk_cursor_step_key(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b);
void __dtfl_chunk_cursor_seek_key(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b, const void *key, size_t klen);
void __dtfl_chunk_cursor_step_val(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b);
void __dtfl_chunk_cursor_seek_val(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b, const void *val, size_t vlen);
void __dtfl_chunk_cursor_rewind_keys(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b);
void __dtfl_chunk_cursor_rewind_vals(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b);

/* ------------------------------------------------------------------ */
/* ChunkBatchBuilder and ChunkBatchMerger.                             */
/* ------------------------------------------------------------------ */

/*
 * ChunkBatchBuilder (mod.rs): collect pushed chunks into a graded ChunkBatch.
 * push settles each pushed chunk; done flushes the carry and seals.
 */
typedef struct __dtfl_chunk_builder {
	__dtfl_chunk_ctx ctx;
	__dtfl_chunk_deque input;	/* holds settle's sub-TARGET carry */
	__dtfl_chunk_deque output;	/* graded chunks emitted so far */
} __dtfl_chunk_builder;

void __dtfl_chunk_builder_init(__dtfl_chunk_builder *bld,
    const __dtfl_chunk_ctx *x);
/* push: settle a chunk into the output (takes ownership; empty is a no-op). */
void __dtfl_chunk_builder_push(__dtfl_chunk_builder *bld, __dtfl_chunk *chunk);
/* done: flush and seal into a ChunkBatch (moves desc; consumes bld). */
__dtfl_chunk_batch *__dtfl_chunk_builder_done(__dtfl_chunk_builder *bld,
    __dtfl_description *desc);
/*
 * Builder::seal over a chain of consolidated chunks (each a __dtfl_batch of
 * (key||val, time, diff) rows): settle them into a graded ChunkBatch.  The
 * chunks are borrowed (retained into the batch); kw splitting uses the ctx.
 */
__dtfl_chunk_batch *__dtfl_chunk_builder_seal(const __dtfl_chunk_ctx *x,
    __dtfl_chunk **chunks, size_t nchunks, __dtfl_description *desc);

/*
 * ChunkBatchMerger (mod.rs): the resumable Batch::Merger.  Pipelines merge ->
 * advance -> settle a fuel-bounded step at a time, reading the sources by
 * cloning (refcount bump) and settling at every yield so a suspended merge
 * holds only graded chunks.
 */
typedef struct __dtfl_chunk_merger {
	__dtfl_chunk_ctx ctx;
	__dtfl_antichain frontier;	/* compaction frontier */
	__dtfl_antichain lower;
	__dtfl_antichain upper;
	__dtfl_chunk_batch *src1;	/* retained */
	__dtfl_chunk_batch *src2;	/* retained */
	__dtfl_chunk_deque in1;
	__dtfl_chunk_deque in2;
	size_t idx1;
	size_t idx2;
	__dtfl_chunk_deque merged;
	__dtfl_chunk_deque advanced;
	__dtfl_chunk_deque settled;
	bool complete;
} __dtfl_chunk_merger;

/* Merger::new: begin a merge; asserts src1->upper == src2->lower. */
void __dtfl_chunk_merger_new(__dtfl_chunk_merger *m, __dtfl_chunk_batch *src1,
    __dtfl_chunk_batch *src2, const __dtfl_antichain *compaction_frontier);
/* Merger::work: apply up to *fuel effort (summed update count). */
void __dtfl_chunk_merger_work(__dtfl_chunk_merger *m, int64_t *fuel);
/* Merger::done: extract the merged batch (consumes m; releases the sources). */
__dtfl_chunk_batch *__dtfl_chunk_merger_done(__dtfl_chunk_merger *m);

/* ------------------------------------------------------------------ */
/* Spill: page committed chunks out to a byte store and back.           */
/* ------------------------------------------------------------------ */

/*
 * Cumulative spill counters (columnar/trace/spill.rs SpillStats).  Not atomic
 * here: libdtfl spill state is per worker and single threaded, so plain
 * counters stand in for the Arc<AtomicUsize> upstream shares across workers.
 */
typedef struct __dtfl_spill_stats {
	size_t spilled_chunks;		/* chunks paged out */
	size_t spilled_records;		/* records in paged-out chunks */
	size_t fetched_chunks;		/* chunks fetched back in */
	size_t bytes_written;		/* serialized bytes written */
} __dtfl_spill_stats;

/*
 * A pluggable byte store (BytesStore / BytesSource).  store() persists bytes
 * and returns an opaque source handle; load() reads them back into a fresh
 * buffer (caller frees with __dtfl_free); drop() releases a source handle.
 * The reference backend (file-backed) is __dtfl_spill_file_store below.
 */
typedef struct __dtfl_spill_store __dtfl_spill_store;
struct __dtfl_spill_store {
	void *(*store)(__dtfl_spill_store *s, const void *bytes, size_t len);
	unsigned char *(*load)(__dtfl_spill_store *s, void *source,
	    size_t *len);
	void (*drop)(__dtfl_spill_store *s, void *source);
	void *ctx;
};

/*
 * Per-worker spill state (columnar/trace/spill.rs SpillState): the record
 * budget, the resident high-water mark, the store, and the stats.  install
 * sets it on the chunk ctx, uninstall clears it.  With nothing installed no
 * chunk is paged and the ctx stays resident (zero overhead beyond a peek).
 */
typedef struct __dtfl_spill_state {
	size_t budget_records;		/* keep this many records resident */
	size_t resident_records;	/* monotone high-water mark */
	__dtfl_spill_store *store;	/* borrowed */
	__dtfl_spill_stats *stats;	/* borrowed */
} __dtfl_spill_state;

/*
 * The settle seal hook (mod.rs pack's `seal`): commit a chunk, perhaps paging
 * it out.  When no spiller is installed on the ctx this is the identity (the
 * chunk is kept resident); when one is and the resident high-water mark is
 * exceeded, the chunk's bytes are serialized to the store and a paged chunk
 * (resident length + a byte source) is returned in its place.  Reads fetch a
 * paged chunk's body back on demand (see __dtfl_batch and spill.c).
 */
__dtfl_chunk *__dtfl_spill_seal(const __dtfl_chunk_ctx *x, __dtfl_chunk *chunk,
    void *ctx);

/*
 * Install spill on `x`: page committed chunks once resident records exceed
 * `budget_records`, writing to `store`; `stats` receives the counters.  The
 * state is borrowed and must outlive any settle that uses `x`.
 */
void __dtfl_spill_install(__dtfl_chunk_ctx *x, __dtfl_spill_state *state,
    size_t budget_records, __dtfl_spill_store *store,
    __dtfl_spill_stats *stats);
void __dtfl_spill_uninstall(__dtfl_chunk_ctx *x);

/*
 * The file-backed reference store (examples/columnar_spill.rs FileStore):
 * each store() appends the length-prefixed blob to a temp file (created lazily,
 * unlinked immediately so it vanishes on close) and returns a (file-offset,
 * length) source; load() seeks and reads it back.  Not thread safe: one per
 * worker.  Uses the standard file API (storage, not forbidden config I/O).
 */
__dtfl_spill_store *__dtfl_spill_file_store_new(void);
void __dtfl_spill_file_store_free(__dtfl_spill_store *s);

#endif /* DTFL_TRACE_CHUNK_INT_H */
