/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/bytes_slab.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/zc_bytes_slab.c: a large binary allocation for writing and
 * sharing.
 *
 * A slab wraps a writable view and keeps a valid (written) length.  It
 * supports writing after the valid length and extracting read-only
 * views up to it.  When the working buffer is retired, the slab keeps
 * it in in_progress and checks it for uniqueness later, so it can be
 * recycled once every extracted view is released.
 *
 * Upstream's BytesRefill (the allocation function and the stash limit)
 * is not ported: buffers come from __dtfl_bytes_new and the stash is
 * unbounded, which is upstream's default Hooks.
 *
 * Owned by one worker thread.  Only the extracted views travel.
 */
#include "comm/zc_int.h"

/* Releases every view in v (a vector of __dtfl_bytes) and empties it. */
static void
release_all(__dtfl_vec *v)
{
	size_t i;

	for (i = 0; i < v->len; i++)
		__dtfl_bytes_release(DTFL_VEC_AT(v, __dtfl_bytes, i));
	__dtfl_vec_clear(v);
}

/* BytesSlab::new: an initial buffer of 1 << shift bytes. */
void
__dtfl_slab_init(__dtfl_bytes_slab *s, size_t shift)
{
	DTFL_ASSERT(shift < sizeof(size_t) * 8 - 1);
	__dtfl_bytes_new(&s->buffer, (size_t)1 << shift);
	__dtfl_vec_init(&s->in_progress, sizeof(__dtfl_bytes));
	__dtfl_vec_init(&s->stash, sizeof(__dtfl_bytes));
	s->shift = shift;
	s->valid = 0;
}

/* Drop: releases the slab's views; readers keep theirs. */
void
__dtfl_slab_free(__dtfl_bytes_slab *s)
{
	__dtfl_bytes_release(&s->buffer);
	release_all(&s->in_progress);
	release_all(&s->stash);
	__dtfl_vec_free(&s->in_progress);
	__dtfl_vec_free(&s->stash);
	s->valid = 0;
}

/* The empty region of the slab: buffer[valid ..]. */
unsigned char *
__dtfl_slab_empty(__dtfl_bytes_slab *s, size_t *len)
{
	*len = s->buffer.len - s->valid;
	return s->buffer.ptr + s->valid;
}

/* The valid region of the slab: buffer[.. valid]. */
unsigned char *
__dtfl_slab_valid(__dtfl_bytes_slab *s, size_t *len)
{
	*len = s->valid;
	return s->buffer.ptr;
}

/* Marks the next `bytes` bytes as valid. */
void
__dtfl_slab_make_valid(__dtfl_bytes_slab *s, size_t bytes)
{
	DTFL_DEBUG_ASSERT(bytes <= s->buffer.len - s->valid);
	s->valid += bytes;
}

/* Extracts the first `bytes` valid bytes as a read-only view. */
void
__dtfl_slab_extract(__dtfl_bytes_slab *s, size_t bytes, __dtfl_bytes *out)
{
	DTFL_DEBUG_ASSERT(bytes <= s->valid);
	s->valid -= bytes;
	__dtfl_bytes_extract_to(&s->buffer, bytes, out);
}

/*
 * Ensures that the empty region is at least `capacity` bytes.
 *
 * This may retire the current buffer if it does not have enough space,
 * in which case it copies any remaining valid contents into a new
 * buffer.  If that would not create enough free space, the shift grows
 * until it does.
 */
void
__dtfl_slab_ensure_capacity(__dtfl_bytes_slab *s, size_t capacity)
{
	__dtfl_bytes old, fresh;
	bool increased_shift = false;
	size_t i, keep;

	if (s->buffer.len - s->valid >= capacity)
		return;

	/* Increase allocation if copy would be insufficient. */
	while (s->valid + capacity > ((size_t)1 << s->shift)) {
		s->shift++;
		DTFL_ASSERT(s->shift < sizeof(size_t) * 8 - 1);
		/* Drop wrongly sized buffers. */
		release_all(&s->stash);
		release_all(&s->in_progress);
		increased_shift = true;
	}

	/* Attempt to reclaim shared slices. */
	if (s->stash.len == 0) {
		keep = 0;
		for (i = 0; i < s->in_progress.len; i++) {
			__dtfl_bytes *b = DTFL_VEC_AT(&s->in_progress,
			    __dtfl_bytes, i);

			if (__dtfl_bytes_try_regenerate(b)) {
				/* Should be redundant, but better safe. */
				if (b->len == ((size_t)1 << s->shift))
					__dtfl_vec_push(&s->stash, b);
				else
					__dtfl_bytes_release(b);
			} else {
				*DTFL_VEC_AT(&s->in_progress, __dtfl_bytes,
				    keep++) = *b;
			}
		}
		s->in_progress.len = keep;
	}

	if (s->stash.len > 0) {
		fresh = *DTFL_VEC_AT(&s->stash, __dtfl_bytes,
		    s->stash.len - 1);
		s->stash.len--;
	} else {
		__dtfl_bytes_new(&fresh, (size_t)1 << s->shift);
	}
	old = s->buffer;
	s->buffer = fresh;

	if (s->valid > 0)
		memcpy(s->buffer.ptr, old.ptr, s->valid);
	if (!increased_shift)
		__dtfl_vec_push(&s->in_progress, &old);
	else
		__dtfl_bytes_release(&old);
}
