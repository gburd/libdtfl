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

/*
 * The constant encoded width of a type if every value is the same number
 * of bytes, else SIZE_MAX.  Unlike dtfl_type.fixed_size this counts a
 * zero-width type (unit) as width 0 and sums constant-width tuple fields,
 * so (u64, unit) reports 8.  Defined in base/record.c; used by the
 * columnar trace fast path to size its fixed-stride key column.
 */
size_t __dtfl_type_const_width(const dtfl_type *t);

/*
 * The memcmp-ordered default compare (dtfl_type.cmp == NULL): memcmp over
 * the common prefix, then length, normalised to -1/0/1.  Two 8-byte
 * values compare as big-endian words, which is the same order as memcmp.
 */
static inline int
__dtfl_cmp_bytes(const void *a, size_t alen, const void *b, size_t blen)
{
	size_t m;
	int c;

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && \
    (defined(__GNUC__) || defined(__clang__))
	if (alen == 8 && blen == 8) {
		uint64_t x, y;

		memcpy(&x, a, sizeof x);
		memcpy(&y, b, sizeof y);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		x = __builtin_bswap64(x);
		y = __builtin_bswap64(y);
#endif
		return (x > y) - (x < y);
	}
#endif
	m = alen < blen ? alen : blen;
	c = m != 0 ? memcmp(a, b, m) : 0;
	if (c != 0)
		return c < 0 ? -1 : 1;
	return (alen > blen) - (alen < blen);
}

/* dtfl_type_cmp with the memcmp-ordered case inline, for hot loops. */
static inline int
__dtfl_type_cmp(const dtfl_type *t, const void *a, size_t alen,
    const void *b, size_t blen)
{
	if (DTFL_LIKELY(t->cmp == NULL))
		return __dtfl_cmp_bytes(a, alen, b, blen);
	return dtfl_type_cmp(t, a, alen, b, blen);
}

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
	/*
	 * Optional per-record ownership hooks for arenas whose records are
	 * owning pointers (the batch-pointer streams, where each record is a
	 * retained __dtfl_batch_ord *).  drop releases one reference to the
	 * record's referent; retain adds one.  NULL for plain record arenas,
	 * where records are inert bytes.  Set once at arena creation; a copy
	 * (copy-on-write) carries them and retains each record so both arenas
	 * own a reference.  release runs drop on every record before freeing,
	 * so an arena that is never consumed still frees what its records own
	 * (upstream's message is Vec<Rc<Batch>>, dropped with the message).
	 */
	void (*elem_drop)(void *rec);
	void (*elem_retain)(void *rec);
} __dtfl_batch;

__dtfl_batch *__dtfl_batch_new(unsigned time_words, unsigned diff_bytes);
/*
 * A new batch with room for `rows` records and `bytes` arena bytes, in
 * one allocation: the struct and the reserved columns share a block, and
 * freeing it is one free.  Pushing past the reservation still works (the
 * column that outgrows its share moves to its own allocation, once).
 */
__dtfl_batch *__dtfl_batch_new_reserved(unsigned time_words,
    unsigned diff_bytes, size_t rows, size_t bytes);
/*
 * Replaces dst's rows with src's and leaves src empty, both uniquely held
 * with the same tw and dw and no element hooks.  dst's old storage passes
 * to src where it can (separately allocated columns trade places); a
 * column in a one-block batch's block stays there and is copied into.
 */
void __dtfl_batch_move_rows(__dtfl_batch *dst, __dtfl_batch *src);
/*
 * Marks an arena as holding owning record pointers: release runs `drop`
 * on each record (and clear/make_mut keep the invariant), `retain` is run
 * on each record of a copy.  Both take a pointer to the record bytes.
 */
void __dtfl_batch_set_elem_hooks(__dtfl_batch *b, void (*drop)(void *rec),
    void (*retain)(void *rec));
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
/*
 * Vec::drain(..k): removes the first k records and moves the rest to the
 * front in place, keeping the storage.  b is uniquely held and has no
 * element hooks.
 */
void __dtfl_batch_drain_front(__dtfl_batch *b, size_t k);

/*
 * Appends a record of len uninitialized bytes and returns them; the
 * pointer is valid until the next push.  A NULL time or diff stores
 * zeros.  Inline because it is the per-row cost of every container
 * fill; the column pushes copy 8-byte times and diffs inline.
 */
static inline void *
__dtfl_batch_push_uninit(__dtfl_batch *b, size_t len, const uint64_t *time,
    const void *diff)
{
	size_t start = b->arena.len, end = start + len;

	DTFL_DEBUG_ASSERT(__dtfl_arc_count(&b->ref) == 1);
	if (DTFL_UNLIKELY(end < start))
		__dtfl_panic("batch arena overflow");
	/* The caller fully overwrites these len bytes, so leave them uninit. */
	__dtfl_vec_push_n_uninit(&b->arena, len);
	__dtfl_vec_push_size(&b->offs, end);
	if (b->tw == 1 && time != NULL)
		__dtfl_vec_push_u64(&b->times, time[0]);
	else
		__dtfl_vec_push_n(&b->times, time, b->tw);
	if (b->dw == sizeof(uint64_t) && diff != NULL) {
		uint64_t d;

		/* Common case: an 8-byte diff (i64), copied as one word. */
		memcpy(&d, diff, sizeof d);
		__dtfl_vec_push_8bytes(&b->diffs, d);
	} else
		__dtfl_vec_push_n(&b->diffs, diff, b->dw);
	/* A zero-length first record leaves the arena without a buffer. */
	return b->arena.data == NULL ? NULL : b->arena.data + start;
}

/* Appends a record; time and diff may be NULL when tw/dw are 0. */
static inline void
__dtfl_batch_push(__dtfl_batch *b, const void *rec, size_t len,
    const uint64_t *time, const void *diff)
{
	unsigned char *p = __dtfl_batch_push_uninit(b, len, time, diff);

	if (len == 8)
		memcpy(p, rec, 8);
	else if (len != 0)
		memcpy(p, rec, len);
}
/* Reserves room for `rows` more records and `bytes` more arena bytes. */
void __dtfl_batch_reserve(__dtfl_batch *b, size_t rows, size_t bytes);
/* Appends record i of src. */
void __dtfl_batch_push_from(__dtfl_batch *b, const __dtfl_batch *src, size_t i);
/* Appends records [i, i + count) of src in bulk (b != src). */
void __dtfl_batch_push_run(__dtfl_batch *b, const __dtfl_batch *src, size_t i,
    size_t count);

static inline const void *
__dtfl_batch_rec(const __dtfl_batch *b, size_t i, size_t *len)
{
	const size_t *o = (const size_t *)(void *)b->offs.data;

	*len = o[i + 1] - o[i];
	/* Zero-length records only: the arena has no buffer yet. */
	return b->arena.data == NULL ? NULL : b->arena.data + o[i];
}

static inline const uint64_t *
__dtfl_batch_time(const __dtfl_batch *b, size_t i)
{
	/* tw == 0 (unit time) leaves times without a buffer. */
	if (b->times.data == NULL)
		return NULL;
	return (const uint64_t *)(void *)b->times.data + (size_t)b->tw * i;
}

static inline uint64_t *
__dtfl_batch_time_mut(__dtfl_batch *b, size_t i)
{
	return (uint64_t *)(void *)b->times.data + (size_t)b->tw * i;
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
