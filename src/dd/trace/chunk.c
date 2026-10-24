/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/chunk/mod.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/chunk.c: the chunk-sequence batch and its machinery.
 *
 * ChunkBatch is a vec of chunks plus a Description; its straddle cursor reads
 * the chunks as one globally sorted (key, val, time, diff) sequence cut at
 * arbitrary points, concatenating a key's vals (and a (key, val)'s times) when
 * a key straddles a chunk boundary.  ChunkBatchBuilder settles pushed chunks
 * into a graded batch; ChunkBatchMerger pipelines merge -> advance -> settle a
 * fuel-bounded step at a time, settling at every yield so a suspended merge
 * holds only graded (spillable) chunks.  The per-chunk operations live in
 * chunk_vec.c; this file is their harness, parallel to ord_neu.c's Builder and
 * Merger over the layered trie.
 */
#include "dd/trace/chunk_int.h"

/* ------------------------------------------------------------------ */
/* Record split: a chunk row's record is key||val.                     */
/* ------------------------------------------------------------------ */

/* The key and value spans of record `rec` of length `len`. */
static void
split_rec(const __dtfl_chunk_ctx *x, const void *rec, size_t len,
    const void **key, size_t *klen, const void **val, size_t *vlen)
{
	if (x->keyed) {
		*key = rec;
		*klen = len;
		*val = (const unsigned char *)rec;	/* zero-length unit */
		*vlen = 0;
		return;
	}
	*klen = dtfl_type_value_len(x->kt, rec, len);
	DTFL_ASSERT(*klen != SIZE_MAX && *klen <= len);
	*key = rec;
	*val = (const unsigned char *)rec + *klen;
	*vlen = len - *klen;
}

/* The key span of chunk c's row i. */
static const void *
row_key(const __dtfl_chunk_ctx *x, const __dtfl_chunk *c, size_t i, size_t *klen)
{
	size_t len;
	const void *rec = __dtfl_batch_rec(c, i, &len), *key, *val;
	size_t vlen;

	split_rec(x, rec, len, &key, klen, &val, &vlen);
	return key;
}

/* The value span of chunk c's row i. */
static const void *
row_val(const __dtfl_chunk_ctx *x, const __dtfl_chunk *c, size_t i, size_t *vlen)
{
	size_t len;
	const void *rec = __dtfl_batch_rec(c, i, &len), *key, *val;
	size_t klen;

	split_rec(x, rec, len, &key, &klen, &val, vlen);
	return val;
}

static int
cmp_key(const __dtfl_chunk_ctx *x, const void *a, size_t al, const void *b,
    size_t bl)
{
	return __dtfl_type_cmp(x->kt, a, al, b, bl);
}

static int
cmp_val(const __dtfl_chunk_ctx *x, const void *a, size_t al, const void *b,
    size_t bl)
{
	if (x->keyed)
		return 0;
	return __dtfl_type_cmp(x->vt, a, al, b, bl);
}

/* ------------------------------------------------------------------ */
/* ChunkBatch lifecycle (mod.rs).                                      */
/* ------------------------------------------------------------------ */

__dtfl_chunk_batch *
__dtfl_chunk_batch_new(const __dtfl_chunk_ctx *x, __dtfl_chunk **chunks,
    size_t n, __dtfl_description *desc)
{
	__dtfl_chunk_batch *b = __dtfl_alloc(sizeof *b);
	size_t i;

	__dtfl_arc_init(&b->ref);
	__dtfl_vec_init(&b->chunks, sizeof(__dtfl_chunk *));
	for (i = 0; i < n; i++) {
		DTFL_ASSERT(__dtfl_batch_len(chunks[i]) > 0);
		__dtfl_vec_push(&b->chunks, &chunks[i]);
	}
	b->description = *desc;			/* move */
	memset(desc, 0, sizeof *desc);
	b->ctx = *x;
	return b;
}

__dtfl_chunk_batch *
__dtfl_chunk_batch_empty(const __dtfl_chunk_ctx *x, __dtfl_antichain *lower,
    __dtfl_antichain *upper)
{
	__dtfl_antichain since;
	__dtfl_description desc;
	uint64_t min[DTFL_TIME_MAX];

	memset(min, 0, sizeof min);
	__dtfl_antichain_init_elem(&since, x->tc, min);
	__dtfl_description_init(&desc, lower, upper, &since);
	return __dtfl_chunk_batch_new(x, NULL, 0, &desc);
}

__dtfl_chunk_batch *
__dtfl_chunk_batch_retain(__dtfl_chunk_batch *b)
{
	__dtfl_arc_retain(&b->ref);
	return b;
}

void
__dtfl_chunk_batch_release(__dtfl_chunk_batch *b)
{
	size_t i;

	if (b == NULL || !__dtfl_arc_release(&b->ref))
		return;
	for (i = 0; i < b->chunks.len; i++)
		__dtfl_batch_release(*DTFL_VEC_AT(&b->chunks, __dtfl_chunk *, i));
	__dtfl_vec_free(&b->chunks);
	__dtfl_description_free(&b->description);
	__dtfl_free(b);
}

size_t
__dtfl_chunk_batch_len(const __dtfl_chunk_batch *b)
{
	size_t n = 0, i;

	for (i = 0; i < b->chunks.len; i++)
		n += __dtfl_batch_len(*DTFL_VEC_AT(&b->chunks,
		    __dtfl_chunk *, i));
	return n;
}

const __dtfl_antichain *
__dtfl_chunk_batch_lower(const __dtfl_chunk_batch *b)
{
	return __dtfl_description_lower(&b->description);
}

const __dtfl_antichain *
__dtfl_chunk_batch_upper(const __dtfl_chunk_batch *b)
{
	return __dtfl_description_upper(&b->description);
}

static __dtfl_chunk *
batch_chunk(const __dtfl_chunk_batch *b, size_t i)
{
	return *DTFL_VEC_AT(&b->chunks, __dtfl_chunk *, i);
}

bool
__dtfl_chunk_is_graded_batch(const __dtfl_chunk_batch *b)
{
	size_t n = b->chunks.len, i;

	for (i = 0; i < n; i++)
		if (__dtfl_batch_len(batch_chunk(b, i)) > DTFL_CHUNK_TARGET)
			return false;
	for (i = 0; i + 1 < n; i++)
		if (__dtfl_batch_len(batch_chunk(b, i)) +
		    __dtfl_batch_len(batch_chunk(b, i + 1)) <= DTFL_CHUNK_TARGET)
			return false;
	return true;
}

/* ------------------------------------------------------------------ */
/* ChunkBatchCursor (mod.rs).                                          */
/* ------------------------------------------------------------------ */

/*
 * The per-chunk inner cursor is the pair (pos, vpos): row indices into the
 * current chunk's rows.  pos is the first row of the current key; vpos the
 * first row of the current (key, val).  The chunk's rows are sorted by
 * (record, time), so a key is a contiguous run of rows and a (key, val) a
 * sub-run.  These mirror VecChunkCursor's key_pos / val_pos.
 */

void
__dtfl_chunk_cursor_init(__dtfl_chunk_cursor *c, const __dtfl_chunk_batch *b)
{
	c->key_chunk = 0;
	c->chunk = 0;
	c->pos = 0;
	c->vpos = 0;
	(void)b;
}

/* goto chunk `k`, opening a fresh inner cursor at its start. */
static void
cur_goto(__dtfl_chunk_cursor *c, size_t k)
{
	c->chunk = k;
	c->pos = 0;
	c->vpos = 0;
}

static bool
inner_key_valid(const __dtfl_chunk_cursor *c, const __dtfl_chunk_batch *b)
{
	return c->chunk < b->chunks.len &&
	    c->pos < __dtfl_batch_len(batch_chunk(b, c->chunk));
}

static bool
inner_val_valid(const __dtfl_chunk_cursor *c, const __dtfl_chunk_batch *b)
{
	const __dtfl_chunk *ch;
	size_t kl, vkl;
	const void *k, *vk;

	if (c->chunk >= b->chunks.len)
		return false;
	ch = batch_chunk(b, c->chunk);
	if (c->pos >= __dtfl_batch_len(ch) || c->vpos >= __dtfl_batch_len(ch))
		return false;
	/* val_valid: vpos's row shares the key at pos. */
	k = row_key(&b->ctx, ch, c->pos, &kl);
	vk = row_key(&b->ctx, ch, c->vpos, &vkl);
	return cmp_key(&b->ctx, k, kl, vk, vkl) == 0;
}

bool
__dtfl_chunk_cursor_key_valid(const __dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b)
{
	return c->chunk < b->chunks.len && inner_key_valid(c, b);
}

bool
__dtfl_chunk_cursor_val_valid(const __dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b)
{
	return c->chunk < b->chunks.len && inner_val_valid(c, b);
}

const void *
__dtfl_chunk_cursor_key(const __dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b, size_t *len)
{
	return row_key(&b->ctx, batch_chunk(b, c->chunk), c->pos, len);
}

const void *
__dtfl_chunk_cursor_val(const __dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b, size_t *len)
{
	return row_val(&b->ctx, batch_chunk(b, c->chunk), c->vpos, len);
}

/*
 * key_spills (mod.rs): does key `k` end chunk c and begin chunk c+1?  Reads the
 * last row's key of chunk c and the first row's key of chunk c+1 (the bounds).
 */
static bool
key_spills(const __dtfl_chunk_batch *b, size_t c, const void *k, size_t kl)
{
	const __dtfl_chunk *c0 = batch_chunk(b, c), *c1 = batch_chunk(b, c + 1);
	size_t l0, l1;
	const void *k0 = row_key(&b->ctx, c0, __dtfl_batch_len(c0) - 1, &l0);
	const void *k1 = row_key(&b->ctx, c1, 0, &l1);

	return cmp_key(&b->ctx, k0, l0, k, kl) == 0 &&
	    cmp_key(&b->ctx, k1, l1, k, kl) == 0;
}

/* val_spills (mod.rs): does (k, v) straddle the boundary between c and c+1? */
static bool
val_spills(const __dtfl_chunk_batch *b, size_t c, const void *k, size_t kl,
    const void *v, size_t vl)
{
	const __dtfl_chunk *c0, *c1;
	size_t l0, l1;
	const void *v0, *v1;

	if (!key_spills(b, c, k, kl))
		return false;
	c0 = batch_chunk(b, c);
	c1 = batch_chunk(b, c + 1);
	v0 = row_val(&b->ctx, c0, __dtfl_batch_len(c0) - 1, &l0);
	v1 = row_val(&b->ctx, c1, 0, &l1);
	return cmp_val(&b->ctx, v0, l0, v, vl) == 0 &&
	    cmp_val(&b->ctx, v1, l1, v, vl) == 0;
}

/*
 * locate_key (mod.rs): the first chunk at or after `hint` whose last key is
 * >= key.  Galloping from the hint, which is a valid lower bound for a forward
 * seek; a backward seek searches from the front.  "lt(i)" is chunk i's last
 * key < key, read from its resident bounds.
 */
static bool
chunk_last_key_lt(const __dtfl_chunk_batch *b, size_t i, const void *key,
    size_t kl)
{
	const __dtfl_chunk *ch = batch_chunk(b, i);
	size_t l;
	const void *lk = row_key(&b->ctx, ch, __dtfl_batch_len(ch) - 1, &l);

	return cmp_key(&b->ctx, lk, l, key, kl) < 0;
}

static size_t
locate_key(const __dtfl_chunk_batch *b, size_t hint, const void *key, size_t kl)
{
	size_t n = b->chunks.len, lo, prev, step, a, bb, m;

	if (hint > n)
		hint = n;
	/* The hint can skip the answer only on a backward seek. */
	lo = (hint == 0 || chunk_last_key_lt(b, hint - 1, key, kl)) ? hint : 0;
	if (lo >= n || !chunk_last_key_lt(b, lo, key, kl))
		return lo;
	/* Exponential search from lo, then binary within the final bracket. */
	prev = lo;
	step = 1;
	while (prev + step < n && chunk_last_key_lt(b, prev + step, key, kl)) {
		prev += step;
		step <<= 1;
	}
	a = prev + 1;
	bb = (prev + step < n) ? prev + step : n;
	while (a < bb) {
		m = a + (bb - a) / 2;
		if (chunk_last_key_lt(b, m, key, kl))
			a = m + 1;
		else
			bb = m;
	}
	return a;
}

/*
 * Within chunk `ch`, the first row >= start whose key is < `key` turns false:
 * the start of key's run.  A linear/galloping seek by key over the rows.
 */
static size_t
inner_seek_key(const __dtfl_chunk_ctx *x, const __dtfl_chunk *ch, size_t start,
    const void *key, size_t kl)
{
	size_t pos = start, step, n = __dtfl_batch_len(ch);

	if (pos < n) {
		size_t l;
		const void *rk = row_key(x, ch, pos, &l);

		if (cmp_key(x, rk, l, key, kl) < 0) {
			step = 1;
			for (;;) {
				size_t p = pos + step;

				if (p >= n)
					break;
				rk = row_key(x, ch, p, &l);
				if (cmp_key(x, rk, l, key, kl) >= 0)
					break;
				pos = p;
				step <<= 1;
			}
			step >>= 1;
			while (step > 0) {
				size_t p = pos + step;

				if (p < n) {
					rk = row_key(x, ch, p, &l);
					if (cmp_key(x, rk, l, key, kl) < 0)
						pos = p;
				}
				step >>= 1;
			}
			pos += 1;
		}
	}
	return pos;
}

/* Step past the current key within chunk ch: the next run start after pos. */
static size_t
inner_step_key(const __dtfl_chunk_ctx *x, const __dtfl_chunk *ch, size_t pos)
{
	size_t n = __dtfl_batch_len(ch), kl, l, i;
	const void *k, *rk;

	if (pos >= n)
		return pos;
	k = row_key(x, ch, pos, &kl);
	i = pos;
	while (i < n) {
		rk = row_key(x, ch, i, &l);
		if (cmp_key(x, rk, l, k, kl) != 0)
			break;
		i++;
	}
	return i;
}

/* Step past the current (key, val) within chunk ch: next (record) run start. */
static size_t
inner_step_val(const __dtfl_chunk_ctx *x, const __dtfl_chunk *ch, size_t vpos)
{
	size_t n = __dtfl_batch_len(ch), rl, l, i;
	const void *rec, *rc;

	if (vpos >= n)
		return vpos;
	rec = __dtfl_batch_rec(ch, vpos, &rl);
	i = vpos;
	while (i < n) {
		rc = __dtfl_batch_rec(ch, i, &l);
		if (__dtfl_type_cmp(x->rt, rc, l, rec, rl) != 0)
			break;
		i++;
	}
	return i;
}

void
__dtfl_chunk_cursor_map_times(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b,
    void (*logic)(const uint64_t *time, const void *diff, void *ctx),
    void *ctx)
{
	const __dtfl_chunk *ch;
	size_t n = b->chunks.len, rl, l, i, cc, kl, vl;
	const void *rec, *rc, *k, *v;

	if (!__dtfl_chunk_cursor_val_valid(c, b))
		return;
	ch = batch_chunk(b, c->chunk);
	rec = __dtfl_batch_rec(ch, c->vpos, &rl);
	k = row_key(&b->ctx, ch, c->vpos, &kl);
	v = row_val(&b->ctx, ch, c->vpos, &vl);
	/* Visit the (key, val)'s updates in this chunk. */
	for (i = c->vpos; i < __dtfl_batch_len(ch); i++) {
		rc = __dtfl_batch_rec(ch, i, &l);
		if (__dtfl_type_cmp(&b->ctx.rt[0], rc, l, rec, rl) != 0)
			break;
		logic(__dtfl_batch_time(ch, i), __dtfl_batch_diff(ch, i), ctx);
	}
	/* Follow the (key, val) forward across boundaries while it spills. */
	cc = c->chunk;
	while (cc + 1 < n && val_spills(b, cc, k, kl, v, vl)) {
		cc++;
		ch = batch_chunk(b, cc);
		for (i = 0; i < __dtfl_batch_len(ch); i++) {
			rc = __dtfl_batch_rec(ch, i, &l);
			if (__dtfl_type_cmp(b->ctx.rt, rc, l, rec, rl) != 0)
				break;
			logic(__dtfl_batch_time(ch, i),
			    __dtfl_batch_diff(ch, i), ctx);
		}
	}
}

void
__dtfl_chunk_cursor_step_key(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b)
{
	const __dtfl_chunk *ch;
	size_t n = b->chunks.len, kl;
	const void *k;

	if (!__dtfl_chunk_cursor_key_valid(c, b))
		return;
	ch = batch_chunk(b, c->chunk);
	k = row_key(&b->ctx, ch, c->pos, &kl);
	/* Advance to the last chunk the key spans. */
	while (c->chunk + 1 < n && key_spills(b, c->chunk, k, kl)) {
		cur_goto(c, c->chunk + 1);
		ch = batch_chunk(b, c->chunk);
	}
	/* Step past the key within its last chunk. */
	c->pos = inner_seek_key(&b->ctx, ch, c->pos, k, kl);
	c->pos = inner_step_key(&b->ctx, ch, c->pos);
	c->vpos = c->pos;
	/* If that exhausted the chunk, the next key starts the next chunk. */
	if (c->pos >= __dtfl_batch_len(ch) && c->chunk + 1 < n)
		cur_goto(c, c->chunk + 1);
}

void
__dtfl_chunk_cursor_seek_key(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b, const void *key, size_t klen)
{
	size_t n = b->chunks.len, lo;

	lo = locate_key(b, c->key_chunk, key, klen);
	cur_goto(c, lo);
	c->key_chunk = lo;
	if (lo < n) {
		const __dtfl_chunk *ch = batch_chunk(b, lo);

		c->pos = inner_seek_key(&b->ctx, ch, 0, key, klen);
		c->vpos = c->pos;
	}
}

void
__dtfl_chunk_cursor_step_val(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b)
{
	const __dtfl_chunk *ch;
	size_t n = b->chunks.len, kl, vl;
	const void *k, *v;

	if (!__dtfl_chunk_cursor_val_valid(c, b))
		return;
	ch = batch_chunk(b, c->chunk);
	k = row_key(&b->ctx, ch, c->pos, &kl);
	v = row_val(&b->ctx, ch, c->vpos, &vl);
	/* Advance to the last chunk the (key, val) spans. */
	while (c->chunk + 1 < n && val_spills(b, c->chunk, k, kl, v, vl)) {
		cur_goto(c, c->chunk + 1);
		ch = batch_chunk(b, c->chunk);
		/* pos tracks the key's start in the new chunk. */
		c->pos = inner_seek_key(&b->ctx, ch, 0, k, kl);
		c->vpos = c->pos;
	}
	/* Step past the (key, val) within that chunk. */
	c->vpos = inner_step_val(&b->ctx, ch, c->vpos);
	/* If the key's vals are exhausted here but the key spills, roll on. */
	if ((c->vpos >= __dtfl_batch_len(ch) || !inner_val_valid(c, b)) &&
	    c->chunk + 1 < n && key_spills(b, c->chunk, k, kl)) {
		cur_goto(c, c->chunk + 1);
		ch = batch_chunk(b, c->chunk);
		c->pos = inner_seek_key(&b->ctx, ch, 0, k, kl);
		c->vpos = c->pos;
	}
}

void
__dtfl_chunk_cursor_seek_val(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b, const void *val, size_t vlen)
{
	const __dtfl_chunk *ch;
	size_t n = b->chunks.len, kl;
	const void *k;

	if (!__dtfl_chunk_cursor_key_valid(c, b))
		return;
	k = row_key(&b->ctx, batch_chunk(b, c->chunk), c->pos, &kl);
	for (;;) {
		size_t i, l;

		ch = batch_chunk(b, c->chunk);
		/* seek_val: first row of this key whose val is >= val. */
		i = c->vpos;
		while (i < __dtfl_batch_len(ch)) {
			size_t rkl, rvl;
			const void *rk = row_key(&b->ctx, ch, i, &rkl);
			const void *rv;

			if (cmp_key(&b->ctx, rk, rkl, k, kl) != 0)
				break;
			rv = row_val(&b->ctx, ch, i, &rvl);
			if (cmp_val(&b->ctx, rv, rvl, val, vlen) >= 0)
				break;
			i++;
		}
		(void)l;
		c->vpos = i;
		if (inner_val_valid(c, b))
			return;
		/* Key's vals exhausted here; if it spills, retry next chunk. */
		if (c->chunk + 1 < n && key_spills(b, c->chunk, k, kl)) {
			cur_goto(c, c->chunk + 1);
			ch = batch_chunk(b, c->chunk);
			c->pos = inner_seek_key(&b->ctx, ch, 0, k, kl);
			c->vpos = c->pos;
		} else {
			return;
		}
	}
}

void
__dtfl_chunk_cursor_rewind_keys(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b)
{
	(void)b;
	c->key_chunk = 0;
	cur_goto(c, 0);
}

void
__dtfl_chunk_cursor_rewind_vals(__dtfl_chunk_cursor *c,
    const __dtfl_chunk_batch *b)
{
	const __dtfl_chunk *ch;
	size_t kl, kc;
	const void *k;

	if (!__dtfl_chunk_cursor_key_valid(c, b))
		return;
	k = row_key(&b->ctx, batch_chunk(b, c->chunk), c->pos, &kl);
	kc = c->key_chunk;
	cur_goto(c, kc);
	ch = batch_chunk(b, kc);
	c->pos = inner_seek_key(&b->ctx, ch, 0, k, kl);
	c->vpos = c->pos;
}

/* ------------------------------------------------------------------ */
/* ChunkBatchBuilder (mod.rs).                                         */
/* ------------------------------------------------------------------ */

void
__dtfl_chunk_builder_init(__dtfl_chunk_builder *bld, const __dtfl_chunk_ctx *x)
{
	bld->ctx = *x;
	__dtfl_chunk_deque_init(&bld->input);
	__dtfl_chunk_deque_init(&bld->output);
}

void
__dtfl_chunk_builder_push(__dtfl_chunk_builder *bld, __dtfl_chunk *chunk)
{
	if (__dtfl_batch_len(chunk) == 0) {
		__dtfl_batch_release(chunk);
		return;
	}
	__dtfl_chunk_deque_push_back(&bld->input, chunk);
	__dtfl_chunk_settle(&bld->ctx, &bld->input, false, &bld->output);
}

__dtfl_chunk_batch *
__dtfl_chunk_builder_done(__dtfl_chunk_builder *bld, __dtfl_description *desc)
{
	__dtfl_chunk_batch *b;
	__dtfl_chunk **arr;
	size_t n, i;

	__dtfl_chunk_settle(&bld->ctx, &bld->input, true, &bld->output);
	n = __dtfl_chunk_deque_len(&bld->output);
	arr = n != 0 ? __dtfl_alloc(__dtfl_mul_size(n, sizeof *arr)) : NULL;
	for (i = 0; i < n; i++)
		arr[i] = __dtfl_chunk_deque_at(&bld->output, i);
	b = __dtfl_chunk_batch_new(&bld->ctx, arr, n, desc);
	__dtfl_free(arr);
	/* The chunks moved into the batch; drop the deque bookkeeping only. */
	__dtfl_vec_free(&bld->output.v);
	__dtfl_chunk_deque_free(&bld->input);
	return b;
}

__dtfl_chunk_batch *
__dtfl_chunk_builder_seal(const __dtfl_chunk_ctx *x, __dtfl_chunk **chunks,
    size_t nchunks, __dtfl_description *desc)
{
	__dtfl_chunk_builder bld;
	size_t i;

	__dtfl_chunk_builder_init(&bld, x);
	for (i = 0; i < nchunks; i++)
		__dtfl_chunk_builder_push(&bld, __dtfl_batch_retain(chunks[i]));
	return __dtfl_chunk_builder_done(&bld, desc);
}

/* ------------------------------------------------------------------ */
/* ChunkBatchMerger (mod.rs).                                          */
/* ------------------------------------------------------------------ */

#define BURST 8

void
__dtfl_chunk_merger_new(__dtfl_chunk_merger *m, __dtfl_chunk_batch *src1,
    __dtfl_chunk_batch *src2, const __dtfl_antichain *compaction_frontier)
{
	const __dtfl_antichain *l1 = __dtfl_chunk_batch_lower(src1);
	const __dtfl_antichain *l2 = __dtfl_chunk_batch_lower(src2);
	const __dtfl_antichain *u1 = __dtfl_chunk_batch_upper(src1);
	const __dtfl_antichain *u2 = __dtfl_chunk_batch_upper(src2);

	m->ctx = src1->ctx;
	__dtfl_antichain_init(&m->frontier, src1->ctx.tc);
	__dtfl_antichain_copy(&m->frontier, compaction_frontier);
	/* lower = l1 meet l2; upper = u1 join u2. */
	__dtfl_antichain_init(&m->lower, src1->ctx.tc);
	__dtfl_antichain_meet_into(src1->ctx.tc, __dtfl_ac_base(l1),
	    __dtfl_antichain_len(l1), __dtfl_ac_base(l2),
	    __dtfl_antichain_len(l2), &m->lower);
	__dtfl_antichain_init(&m->upper, src1->ctx.tc);
	__dtfl_antichain_join_into(src1->ctx.tc, __dtfl_ac_base(u1),
	    __dtfl_antichain_len(u1), __dtfl_ac_base(u2),
	    __dtfl_antichain_len(u2), &m->upper);
	m->src1 = __dtfl_chunk_batch_retain(src1);
	m->src2 = __dtfl_chunk_batch_retain(src2);
	__dtfl_chunk_deque_init(&m->in1);
	__dtfl_chunk_deque_init(&m->in2);
	m->idx1 = 0;
	m->idx2 = 0;
	__dtfl_chunk_deque_init(&m->merged);
	__dtfl_chunk_deque_init(&m->advanced);
	__dtfl_chunk_deque_init(&m->settled);
	m->complete = false;
}

void
__dtfl_chunk_merger_work(__dtfl_chunk_merger *m, int64_t *fuel)
{
	__dtfl_chunk_batch *s1 = m->src1, *s2 = m->src2;

	if (m->complete)
		return;
	while (*fuel > 0) {
		__dtfl_chunk_deque produced;
		__dtfl_chunk *chunk;
		int64_t work = 0;
		size_t i, pn;

		/* Refill each input deque up to a burst of source chunks. */
		while (__dtfl_chunk_deque_len(&m->in1) < BURST &&
		    m->idx1 < s1->chunks.len) {
			__dtfl_chunk_deque_push_back(&m->in1,
			    __dtfl_batch_retain(batch_chunk(s1, m->idx1)));
			m->idx1++;
		}
		while (__dtfl_chunk_deque_len(&m->in2) < BURST &&
		    m->idx2 < s2->chunks.len) {
			__dtfl_chunk_deque_push_back(&m->in2,
			    __dtfl_batch_retain(batch_chunk(s2, m->idx2)));
			m->idx2++;
		}

		__dtfl_chunk_deque_init(&produced);
		if (!__dtfl_chunk_deque_empty(&m->in1) &&
		    !__dtfl_chunk_deque_empty(&m->in2)) {
			__dtfl_chunk_merge(&m->ctx, &m->in1, &m->in2,
			    &produced);
		} else if ((chunk = __dtfl_chunk_deque_pop_front(&m->in1)) !=
		    NULL || (chunk = __dtfl_chunk_deque_pop_front(&m->in2)) !=
		    NULL) {
			__dtfl_chunk_deque_push_back(&produced, chunk);
		} else {
			/* Both drained: final flush of advance and settle. */
			__dtfl_chunk_advance(&m->ctx, &m->merged, &m->frontier,
			    true, &m->advanced);
			__dtfl_chunk_settle(&m->ctx, &m->advanced, true,
			    &m->settled);
			m->complete = true;
			__dtfl_chunk_deque_free(&produced);
			break;
		}
		pn = __dtfl_chunk_deque_len(&produced);
		for (i = 0; i < pn; i++)
			work += (int64_t)__dtfl_batch_len(
			    __dtfl_chunk_deque_at(&produced, i));
		__dtfl_chunk_deque_extend(&m->merged, &produced);
		__dtfl_chunk_deque_free(&produced);
		__dtfl_chunk_advance(&m->ctx, &m->merged, &m->frontier, false,
		    &m->advanced);
		__dtfl_chunk_settle(&m->ctx, &m->advanced, false, &m->settled);
		*fuel -= work;
	}
}

__dtfl_chunk_batch *
__dtfl_chunk_merger_done(__dtfl_chunk_merger *m)
{
	__dtfl_antichain since;
	__dtfl_description desc;
	__dtfl_chunk_batch *b;
	__dtfl_chunk **arr;
	size_t n, i;

	DTFL_ASSERT(__dtfl_chunk_deque_empty(&m->merged) &&
	    __dtfl_chunk_deque_empty(&m->advanced));
	__dtfl_antichain_init(&since, m->ctx.tc);
	__dtfl_antichain_copy(&since, &m->frontier);
	__dtfl_description_init(&desc, &m->lower, &m->upper, &since);
	n = __dtfl_chunk_deque_len(&m->settled);
	arr = n != 0 ? __dtfl_alloc(__dtfl_mul_size(n, sizeof *arr)) : NULL;
	for (i = 0; i < n; i++)
		arr[i] = __dtfl_chunk_deque_at(&m->settled, i);
	b = __dtfl_chunk_batch_new(&m->ctx, arr, n, &desc);
	__dtfl_free(arr);
	__dtfl_vec_free(&m->settled.v);		/* chunks moved into the batch */
	__dtfl_chunk_deque_free(&m->in1);
	__dtfl_chunk_deque_free(&m->in2);
	__dtfl_chunk_deque_free(&m->merged);
	__dtfl_chunk_deque_free(&m->advanced);
	__dtfl_antichain_free(&m->frontier);
	/* lower/upper moved into desc; description owns them now. */
	__dtfl_chunk_batch_release(m->src1);
	__dtfl_chunk_batch_release(m->src2);
	return b;
}
