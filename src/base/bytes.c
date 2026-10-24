/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   bytes/src/lib.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/base/bytes.c: shared byte storage sliced into disjoint views.
 *
 * Divergence (PLAN D12, ADR-0012): this takes the aliasing and
 * regeneration hardening of upstream #821 (9efd0107), which landed after
 * the pin.  At the pin, try_regenerate read the allocation's pointer
 * and length through two separate deref calls on the backing object,
 * so a backing object that answered differently could install a view
 * whose length did not match its pointer; it also left the old view in
 * place while it re-borrowed the object, and the sequestered object was
 * not in an UnsafeCell, so a BytesMut wrote through bytes that the
 * Arc's shared reference still covered.  #821 takes one slice, clears
 * the view before touching the object, and wraps it in UnsafeCell.  The
 * C equivalents enforced here:
 *
 *   - the store records (data, cap) once, at creation, and
 *     regeneration restores exactly that range; nothing re-queries the
 *     owner;
 *   - regeneration happens only when the caller holds the store's only
 *     reference, observed with an acquire load that synchronizes with
 *     every other view's release, and otherwise changes nothing;
 *   - a writable view (BytesMut) cannot be cloned, extract_to always
 *     hands out a read-only view (as upstream returns Bytes), and a
 *     merge is writable only if both halves were, so no two live views
 *     can write overlapping bytes, and no view can write bytes another
 *     view reads;
 *   - the reference count lives in the store header, never in the data,
 *     so writes through a view cannot touch it.
 *
 * Threads: a writable view stays on its thread (BytesMut is !Send after
 * #821).  Read-only views may be cloned and released on any thread; the
 * count is atomic (Bytes is Send + Sync since 0.31, #800).
 */
#include "base_int.h"

#include "batch_int.h"

/* Takes ownership of (data, cap); the result is the store's only view. */
static void
bytes_init(__dtfl_bytes *b, unsigned char *data, size_t cap,
    void (*release)(unsigned char *, size_t, void *), void *ctx)
{
	__dtfl_bytes_store *s = __dtfl_alloc(sizeof *s);

	__dtfl_arc_init(&s->ref);
	s->data = data;
	s->cap = cap;
	s->release = release;
	s->ctx = ctx;
	b->store = s;
	b->ptr = data;
	b->len = cap;
	b->writable = true;
}

/* BytesMut::from(vec![0; cap]): the bytes are not zeroed. */
void
__dtfl_bytes_new(__dtfl_bytes *b, size_t cap)
{
	bytes_init(b, __dtfl_alloc(cap), cap, NULL, NULL);
}

/* BytesMut::from(bytes): release(data, cap, ctx) runs with the last view. */
void
__dtfl_bytes_wrap(__dtfl_bytes *b, unsigned char *data, size_t cap,
    void (*release)(unsigned char *, size_t, void *), void *ctx)
{
	DTFL_ASSERT(data != NULL);
	bytes_init(b, data, cap, release, ctx);
}

/*
 * Bytes::extract_to and BytesMut::extract_to.  Testing index against
 * len first ensures both that `out` covers valid memory and that `b`
 * can no longer reach it.
 */
void
__dtfl_bytes_extract_to(__dtfl_bytes *b, size_t index, __dtfl_bytes *out)
{
	if (index > b->len)
		__dtfl_panic("bytes extract_to: index %zu > len %zu", index,
		    b->len);
	__dtfl_arc_retain(&b->store->ref);
	out->store = b->store;
	out->ptr = b->ptr;
	out->len = index;
	out->writable = false;
	b->ptr += index;
	b->len -= index;
}

/*
 * Bytes::try_merge: succeeds when `other` is from the same store and
 * starts where `b` ends.  On success other's reference is dropped and
 * *other zeroed; on failure nothing changes (upstream's Err(other)).
 */
bool
__dtfl_bytes_try_merge(__dtfl_bytes *b, __dtfl_bytes *other)
{
	if (b == other || b->store == NULL || b->store != other->store ||
	    b->ptr + b->len != other->ptr)
		return false;
	b->len += other->len;
	b->writable = b->writable && other->writable;
	/* b still holds a reference, so this is never the last one. */
	(void)__dtfl_arc_release(&other->store->ref);
	memset(other, 0, sizeof *other);
	return true;
}

/*
 * BytesMut::try_regenerate, as hardened by #821.  The acquire load pairs
 * with the release in __dtfl_arc_release on every other view, so reads
 * through those views happen before the caller's next write.  A count of
 * 1 cannot rise concurrently: only a view holder can clone, and the
 * caller holds the only view.
 */
bool
__dtfl_bytes_try_regenerate(__dtfl_bytes *b)
{
	if (!b->writable)
		__dtfl_panic("bytes try_regenerate on a read-only view");
	if (__dtfl_arc_count(&b->store->ref) != 1)
		return false;
	b->ptr = b->store->data;
	b->len = b->store->cap;
	return true;
}

/* BytesMut::freeze. */
void
__dtfl_bytes_freeze(__dtfl_bytes *b)
{
	b->writable = false;
}

/* Bytes::clone.  BytesMut is not Clone: two writers would alias. */
void
__dtfl_bytes_clone(const __dtfl_bytes *b, __dtfl_bytes *out)
{
	if (b->writable)
		__dtfl_panic("bytes clone of a writable view");
	__dtfl_arc_retain(&b->store->ref);
	*out = *b;
}

/* Drops the view; the last view of a store frees it.  NULL store is a no-op. */
void
__dtfl_bytes_release(__dtfl_bytes *b)
{
	__dtfl_bytes_store *s = b->store;

	memset(b, 0, sizeof *b);
	if (s == NULL || !__dtfl_arc_release(&s->ref))
		return;
	if (s->release != NULL)
		s->release(s->data, s->cap, s->ctx);
	else
		__dtfl_free(s->data);
	__dtfl_free(s);
}
