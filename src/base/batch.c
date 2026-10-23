/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   container/src/lib.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/base/batch.c: the record batch, libdtfl's only container (PLAN D2).
 *
 * Upstream's container crate is a set of traits (Accountable,
 * DrainContainer, SizableContainer, PushInto, ContainerBuilder) with
 * impls for Vec, Rc and Arc.  All of them collapse into this one type:
 * record_count is __dtfl_batch_len, push_into is __dtfl_batch_push,
 * the Rc/Arc impls are the batch's own atomic count with copy-on-write
 * (__dtfl_batch_make_mut), and BUFFER_SIZE_BYTES is
 * DTFL_BUFFER_SIZE_BYTES in batch_int.h.  The builders port with the
 * operators that use them.
 *
 * Invariants: offs.len == count + 1, offs[0] == 0, offs is
 * non-decreasing, offs[count] == arena.len, times.len == count * tw and
 * diffs.len == count * dw.  A batch with more than one reference is
 * immutable; only the holder of the sole reference writes to it.
 */
#include "base_int.h"

#include "batch_int.h"

/* An empty batch with one reference, owned by the caller. */
__dtfl_batch *
__dtfl_batch_new(unsigned time_words, unsigned diff_bytes)
{
	return __dtfl_batch_new_reserved(time_words, diff_bytes, 0, 0);
}

/*
 * __dtfl_batch_new, with room for `rows` records and `bytes` arena bytes
 * reserved as __dtfl_batch_reserve would, but with each column allocated
 * once at its final size: the offsets vector holds its leading zero and
 * `rows` more without a second allocation.
 */
__dtfl_batch *
__dtfl_batch_new_reserved(unsigned time_words, unsigned diff_bytes,
    size_t rows, size_t bytes)
{
	__dtfl_batch *b = __dtfl_alloc(sizeof *b);
	size_t zero = 0;

	__dtfl_arc_init(&b->ref);
	__dtfl_vec_init(&b->arena, 1);
	__dtfl_vec_init(&b->offs, sizeof(size_t));
	__dtfl_vec_init(&b->times, sizeof(uint64_t));
	__dtfl_vec_init(&b->diffs, 1);
	b->tw = time_words;
	b->dw = diff_bytes;
	b->elem_drop = NULL;
	b->elem_retain = NULL;
	__dtfl_vec_reserve(&b->offs, rows + 1);
	__dtfl_vec_push_size(&b->offs, zero);
	if (rows != 0 || bytes != 0)
		__dtfl_batch_reserve(b, rows, bytes);
	return b;
}

void
__dtfl_batch_set_elem_hooks(__dtfl_batch *b, void (*drop)(void *rec),
    void (*retain)(void *rec))
{
	b->elem_drop = drop;
	b->elem_retain = retain;
}

/* Runs `fn` on each record's bytes (for the owning-pointer arenas). */
static void
batch_each_elem(__dtfl_batch *b, void (*fn)(void *rec))
{
	size_t i, n = __dtfl_batch_len(b);
	const size_t *o = (const size_t *)(void *)b->offs.data;

	for (i = 0; i < n; i++)
		fn(b->arena.data + o[i]);
}

__dtfl_batch *
__dtfl_batch_retain(__dtfl_batch *b)
{
	__dtfl_arc_retain(&b->ref);
	return b;
}

/* Drops one reference; the last frees the batch.  NULL is a no-op. */
void
__dtfl_batch_release(__dtfl_batch *b)
{
	if (b == NULL || !__dtfl_arc_release(&b->ref))
		return;
	if (b->elem_drop != NULL)
		batch_each_elem(b, b->elem_drop);
	__dtfl_vec_free(&b->arena);
	__dtfl_vec_free(&b->offs);
	__dtfl_vec_free(&b->times);
	__dtfl_vec_free(&b->diffs);
	__dtfl_free(b);
}

/* A new, uniquely held batch with the same contents. */
__dtfl_batch *
__dtfl_batch_copy(const __dtfl_batch *b)
{
	__dtfl_batch *c = __dtfl_batch_new(b->tw, b->dw);

	__dtfl_vec_copy(&c->arena, &b->arena);
	__dtfl_vec_copy(&c->offs, &b->offs);
	__dtfl_vec_copy(&c->times, &b->times);
	__dtfl_vec_copy(&c->diffs, &b->diffs);
	c->elem_drop = b->elem_drop;
	c->elem_retain = b->elem_retain;
	/* The copy owns its own reference to each record's referent. */
	if (c->elem_retain != NULL)
		batch_each_elem(c, c->elem_retain);
	return c;
}

/*
 * Copy-on-write.  The acquire load in __dtfl_arc_count pairs with the
 * release of every other holder's reference, so their reads finish
 * before the caller writes.  Consumes the caller's reference to b.
 */
__dtfl_batch *
__dtfl_batch_make_mut(__dtfl_batch *b)
{
	__dtfl_batch *c;

	if (__dtfl_arc_count(&b->ref) == 1)
		return b;
	c = __dtfl_batch_copy(b);
	__dtfl_batch_release(b);
	return c;
}

/* Empties the batch, keeping its allocations. */
void
__dtfl_batch_clear(__dtfl_batch *b)
{
	if (b->elem_drop != NULL)
		batch_each_elem(b, b->elem_drop);
	__dtfl_vec_clear(&b->arena);
	__dtfl_vec_truncate(&b->offs, 1);
	__dtfl_vec_clear(&b->times);
	__dtfl_vec_clear(&b->diffs);
}

void
__dtfl_batch_drain_front(__dtfl_batch *b, size_t k)
{
	size_t n = __dtfl_batch_len(b), base, j;
	size_t *o;

	DTFL_ASSERT(k <= n && b->elem_drop == NULL);
	DTFL_DEBUG_ASSERT(__dtfl_arc_count(&b->ref) == 1);
	if (k == 0)
		return;
	o = (size_t *)(void *)b->offs.data;
	base = o[k];
	if (b->arena.len > base)
		memmove(b->arena.data, b->arena.data + base,
		    b->arena.len - base);
	b->arena.len -= base;
	for (j = 0; j <= n - k; j++)
		o[j] = o[k + j] - base;
	b->offs.len = n - k + 1;
	if (b->tw != 0) {
		if (n > k)
			memmove(b->times.data, __dtfl_batch_time(b, k),
			    (n - k) * b->tw * sizeof(uint64_t));
		b->times.len = (n - k) * b->tw;
	}
	if (b->dw != 0) {
		if (n > k)
			memmove(b->diffs.data, __dtfl_batch_diff(b, k),
			    (n - k) * b->dw);
		b->diffs.len = (n - k) * b->dw;
	}
}

/*
 * Reserves room for `rows` more records: the offsets, time and diff
 * columns exactly, and `bytes` more arena bytes.  A hint only; the batch
 * still grows if more is pushed.  Lets a caller that knows a chunk's
 * capacity fill it without per-record reallocation.
 */
void
__dtfl_batch_reserve(__dtfl_batch *b, size_t rows, size_t bytes)
{
	__dtfl_vec_reserve(&b->offs, rows);
	if (bytes != 0)
		__dtfl_vec_reserve(&b->arena, bytes);
	if (b->tw != 0)
		__dtfl_vec_reserve(&b->times, rows * b->tw);
	if (b->dw != 0)
		__dtfl_vec_reserve(&b->diffs, rows * b->dw);
}

/*
 * Appends record i of src.  Copies after the push so that b == src
 * works: the push may move src's storage.
 */
void
__dtfl_batch_push_from(__dtfl_batch *b, const __dtfl_batch *src, size_t i)
{
	const size_t *o;
	size_t off, len, n;
	unsigned char *p;

	DTFL_ASSERT(b->tw == src->tw && b->dw == src->dw);
	DTFL_ASSERT(i < __dtfl_batch_len(src));
	o = (const size_t *)(void *)src->offs.data;
	off = o[i];
	len = o[i + 1] - off;
	p = __dtfl_batch_push_uninit(b, len, NULL, NULL);
	n = __dtfl_batch_len(b) - 1;
	if (len != 0)
		memcpy(p, src->arena.data + off, len);
	if (b->tw != 0)
		memcpy(__dtfl_batch_time_mut(b, n), __dtfl_batch_time(src, i),
		    (size_t)b->tw * sizeof(uint64_t));
	if (b->dw != 0)
		memcpy(__dtfl_batch_diff_mut(b, n), __dtfl_batch_diff(src, i),
		    b->dw);
}

size_t
__dtfl_batch_bytes(const __dtfl_batch *b)
{
	return b->arena.len;
}

/*
 * Appends records [i, i + count) of src in bulk (b != src).  Equivalent
 * to count calls to __dtfl_batch_push_from but with one memcpy per column
 * instead of per record.  src's offsets are rebased onto b's arena.
 */
void
__dtfl_batch_push_run(__dtfl_batch *b, const __dtfl_batch *src, size_t i,
    size_t count)
{
	const size_t *o;
	size_t base, span, j, dst0, *doffs;
	unsigned char *p;

	DTFL_ASSERT(b->tw == src->tw && b->dw == src->dw);
	if (count == 0)
		return;
	DTFL_ASSERT(i + count <= __dtfl_batch_len(src));
	o = (const size_t *)(void *)src->offs.data;
	base = o[i];
	span = o[i + count] - base;

	/* Arena: one copy of the whole span. */
	p = __dtfl_vec_push_n_uninit(&b->arena, span);
	if (span != 0)
		memcpy(p, src->arena.data + base, span);

	/* Offsets: count new entries, each rebased to b's arena start. */
	dst0 = (size_t)(p - b->arena.data);
	doffs = __dtfl_vec_push_n_uninit(&b->offs, count);
	for (j = 0; j < count; j++)
		doffs[j] = dst0 + (o[i + j + 1] - base);

	/* Times and diffs: contiguous blocks copied whole. */
	if (b->tw != 0) {
		p = __dtfl_vec_push_n_uninit(&b->times, count * b->tw);
		memcpy(p, __dtfl_batch_time(src, i),
		    (size_t)count * b->tw * sizeof(uint64_t));
	}
	if (b->dw != 0) {
		p = __dtfl_vec_push_n_uninit(&b->diffs, count * b->dw);
		memcpy(p, __dtfl_batch_diff(src, i), (size_t)count * b->dw);
	}
}

/* ------------------------------------------------------------------ */
/* Serialization                                                       */
/* ------------------------------------------------------------------ */

static void
le_put(unsigned char *p, uint64_t v, size_t w)
{
	size_t i;

	for (i = 0; i < w; i++, v >>= 8)
		p[i] = (unsigned char)v;
}

static uint64_t
le_get(const unsigned char *p, size_t w)
{
	uint64_t v = 0;

	while (w-- > 0)
		v = v << 8 | p[w];
	return v;
}

#define PAD8(n) (((n) + 7) & ~(size_t)7)

size_t
__dtfl_batch_encoded_len(const __dtfl_batch *b)
{
	size_t n = __dtfl_batch_len(b), len;

	len = 16 + __dtfl_mul_size(n + 1, 8) + b->arena.len +
	    __dtfl_mul_size(b->times.len, 8) + b->diffs.len;
	if (len > SIZE_MAX - 7)
		__dtfl_panic("batch encoding overflow");
	return PAD8(len);
}

/* Writes exactly __dtfl_batch_encoded_len(b) bytes; padding is zero. */
void
__dtfl_batch_encode(const __dtfl_batch *b, unsigned char *out)
{
	const size_t *o = (const size_t *)(void *)b->offs.data;
	const uint64_t *t = (const uint64_t *)(void *)b->times.data;
	size_t n = __dtfl_batch_len(b), i, at;

	le_put(out, n, 8);
	le_put(out + 8, b->tw, 4);
	le_put(out + 12, b->dw, 4);
	at = 16;
	for (i = 0; i <= n; i++, at += 8)
		le_put(out + at, o[i], 8);
	if (b->arena.len != 0)
		memcpy(out + at, b->arena.data, b->arena.len);
	at += b->arena.len;
	for (i = 0; i < b->times.len; i++, at += 8)
		le_put(out + at, t[i], 8);
	if (b->diffs.len != 0)
		memcpy(out + at, b->diffs.data, b->diffs.len);
	at += b->diffs.len;
	while (at % 8 != 0)
		out[at++] = 0;
}

/*
 * Parses an encoding from an untrusted peer.  Every size is checked
 * against the bytes remaining before it is multiplied or allocated, so
 * allocation never exceeds a small multiple of len.  The input must be
 * exactly the canonical encoding: offs[0] == 0, offsets non-decreasing
 * and ending at the arena size, and zero padding to the 8-byte length.
 */
__dtfl_batch *
__dtfl_batch_decode(const unsigned char *p, size_t len)
{
	__dtfl_batch *b;
	uint64_t count, tw, dw, prev, off, arena;
	size_t rest, at, i, nt;

	if (len < 16 || len % 8 != 0)
		return NULL;
	count = le_get(p, 8);
	tw = le_get(p + 8, 4);
	dw = le_get(p + 12, 4);
	rest = len - 16;
	if (count >= rest / 8)
		return NULL;
	at = 16 + (size_t)(count + 1) * 8;
	rest = len - at;
	if (le_get(p + 16, 8) != 0)
		return NULL;
	prev = 0;
	for (i = 1; i <= count; i++) {
		off = le_get(p + 16 + i * 8, 8);
		if (off < prev)
			return NULL;
		prev = off;
	}
	arena = prev;
	if (arena > rest)
		return NULL;
	rest -= (size_t)arena;
	if (tw != 0 && count > rest / 8 / tw)
		return NULL;
	nt = (size_t)(count * tw);
	rest -= nt * 8;
	if (dw != 0 && count > rest / dw)
		return NULL;
	rest -= (size_t)(count * dw);
	if (rest >= 8)
		return NULL;
	for (i = len - rest; i < len; i++)
		if (p[i] != 0)
			return NULL;

	b = __dtfl_batch_new((unsigned)tw, (unsigned)dw);
	__dtfl_vec_reserve(&b->offs, (size_t)count);
	for (i = 1; i <= count; i++) {
		size_t o = (size_t)le_get(p + 16 + i * 8, 8);

		__dtfl_vec_push(&b->offs, &o);
	}
	__dtfl_vec_push_n(&b->arena, p + at, (size_t)arena);
	at += (size_t)arena;
	__dtfl_vec_reserve(&b->times, nt);
	for (i = 0; i < nt; i++, at += 8) {
		uint64_t w = le_get(p + at, 8);

		__dtfl_vec_push(&b->times, &w);
	}
	__dtfl_vec_push_n(&b->diffs, p + at, (size_t)(count * dw));
	return b;
}
