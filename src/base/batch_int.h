/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/base/batch_int.h: shared byte storage and record batches (PLAN D2).
 *
 * __dtfl_bytes ports timely-dataflow bytes/src/lib.rs (Bytes, BytesMut)
 * with the post-pin aliasing hardening of upstream #821 (PLAN D12): a
 * refcounted allocation sliced into disjoint views.  Views share the
 * allocation through an atomic count, so slices may be handed to other
 * threads; a writable view (BytesMut) never aliases another writable
 * view.
 *
 * __dtfl_batch is the one container: an append-only arena of records
 * plus an offsets array, refcounted so fan-out shares it (D2).  A
 * differential batch keeps time and diff columns alongside (the `tw`
 * and `dw` widths).  Records are opaque bytes here.
 */
#ifndef DTFL_BATCH_INT_H
#define DTFL_BATCH_INT_H

#include "base/base_int.h"
#include "dtfl/record.h"

/* ------------------------------------------------------------------ */
/* Bytes                                                               */
/* ------------------------------------------------------------------ */

typedef struct __dtfl_bytes_store {
	__dtfl_arc ref;
	unsigned char *data;
	size_t cap;
	/* Releases data; NULL means __dtfl_free. */
	void (*release)(unsigned char *data, size_t cap, void *ctx);
	void *ctx;
} __dtfl_bytes_store;

/*
 * A view [ptr, ptr+len) into a store.  Holds one store reference.
 * `writable` is Rust's BytesMut: the holder is the only view of these
 * bytes and may write them; it stays on one thread.  A frozen view
 * (Rust's Bytes) is read-only and may be cloned and sent to other
 * threads.  Invariant: a writable range overlaps no other live view.
 */
typedef struct __dtfl_bytes {
	__dtfl_bytes_store *store;
	unsigned char *ptr;
	size_t len;
	bool writable;
} __dtfl_bytes;

/* BytesMut::from: a writable view of a new store of `cap` bytes. */
void __dtfl_bytes_new(__dtfl_bytes *b, size_t cap);
/* BytesMut::from over caller memory; `release` frees it at the end. */
void __dtfl_bytes_wrap(__dtfl_bytes *b, unsigned char *data, size_t cap,
    void (*release)(unsigned char *, size_t, void *), void *ctx);
/*
 * extract_to: splits off the first `index` bytes into `out`, a frozen
 * view (as both Rust extract_to methods return Bytes).  Panics if
 * index > b->len.
 */
void __dtfl_bytes_extract_to(__dtfl_bytes *b, size_t index, __dtfl_bytes *out);
/*
 * try_merge: appends `other` if it is the adjacent view of the same
 * store, consuming it (other is zeroed).  The result is writable only
 * if both views were.  On false neither view changes.
 */
bool __dtfl_bytes_try_merge(__dtfl_bytes *b, __dtfl_bytes *other);
/*
 * try_regenerate: if this writable view is the only view of its store,
 * resets it to the whole allocation and returns true.  Otherwise the
 * view is unchanged.  Panics on a frozen view.
 */
bool __dtfl_bytes_try_regenerate(__dtfl_bytes *b);
/* freeze: makes a writable view read-only (shareable).  Never undone. */
void __dtfl_bytes_freeze(__dtfl_bytes *b);
/* Clone: a second reference to the same range.  Panics if b is writable. */
void __dtfl_bytes_clone(const __dtfl_bytes *b, __dtfl_bytes *out);
void __dtfl_bytes_release(__dtfl_bytes *b);

/* ------------------------------------------------------------------ */
/* Record batch                                                        */
/* ------------------------------------------------------------------ */

/*
 * A batch of records.  Record i occupies arena bytes
 * [offs[i], offs[i+1]).  When tw > 0 each record also has a time of tw
 * words; when dw > 0 a diff of dw bytes.  Both are fixed width per
 * batch.  A batch is refcounted (atomic) and immutable once shared:
 * call __dtfl_batch_make_mut before writing to a batch others may hold.
 */
typedef struct __dtfl_batch {
	__dtfl_arc ref;
	__dtfl_vec arena;	/* bytes */
	__dtfl_vec offs;	/* size_t; len == count + 1 */
	__dtfl_vec times;	/* uint64_t; tw per record */
	__dtfl_vec diffs;	/* bytes; dw per record */
	unsigned tw;
	unsigned dw;
} __dtfl_batch;

__dtfl_batch *__dtfl_batch_new(unsigned time_words, unsigned diff_bytes);
__dtfl_batch *__dtfl_batch_retain(__dtfl_batch *b);
void __dtfl_batch_release(__dtfl_batch *b);
/* Returns b if uniquely held, otherwise releases b and returns a copy. */
__dtfl_batch *__dtfl_batch_make_mut(__dtfl_batch *b);
__dtfl_batch *__dtfl_batch_copy(const __dtfl_batch *b);

static inline size_t
__dtfl_batch_len(const __dtfl_batch *b)
{
	return b->offs.len - 1;
}

void __dtfl_batch_clear(__dtfl_batch *b);
/* Appends a record; time and diff may be NULL when tw/dw are 0. */
void __dtfl_batch_push(__dtfl_batch *b, const void *rec, size_t len,
    const uint64_t *time, const void *diff);
/* Reserves space and returns a pointer for a record of `len` bytes. */
void *__dtfl_batch_push_uninit(__dtfl_batch *b, size_t len,
    const uint64_t *time, const void *diff);
/* Appends record i of src. */
void __dtfl_batch_push_from(__dtfl_batch *b, const __dtfl_batch *src, size_t i);

static inline const void *
__dtfl_batch_rec(const __dtfl_batch *b, size_t i, size_t *len)
{
	const size_t *o = (const size_t *)b->offs.data;

	*len = o[i + 1] - o[i];
	return b->arena.data + o[i];
}

static inline const uint64_t *
__dtfl_batch_time(const __dtfl_batch *b, size_t i)
{
	return (const uint64_t *)b->times.data + (size_t)b->tw * i;
}

static inline uint64_t *
__dtfl_batch_time_mut(__dtfl_batch *b, size_t i)
{
	return (uint64_t *)b->times.data + (size_t)b->tw * i;
}

static inline const void *
__dtfl_batch_diff(const __dtfl_batch *b, size_t i)
{
	return b->diffs.data + (size_t)b->dw * i;
}

static inline void *
__dtfl_batch_diff_mut(__dtfl_batch *b, size_t i)
{
	return b->diffs.data + (size_t)b->dw * i;
}

/* Bytes the batch holds in its arena (record_count's sibling for sizing). */
size_t __dtfl_batch_bytes(const __dtfl_batch *b);

/*
 * Serialization of a batch for the byte-oriented allocators.  Layout
 * (all little-endian): u64 count, u32 tw, u32 dw, then count+1 u64
 * offsets, the arena, count*tw u64 times, count*dw diff bytes, padded
 * to 8 bytes.  decode returns NULL on malformed input.
 */
size_t __dtfl_batch_encoded_len(const __dtfl_batch *b);
void __dtfl_batch_encode(const __dtfl_batch *b, unsigned char *out);
__dtfl_batch *__dtfl_batch_decode(const unsigned char *p, size_t len);

/* Default buffer size, as timely container/src/lib.rs BUFFER_SIZE_BYTES. */
#define DTFL_BUFFER_SIZE_BYTES (1 << 13)

#endif /* DTFL_BATCH_INT_H */
