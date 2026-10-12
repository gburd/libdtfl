/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/implementations/merge_batcher.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/merge_batcher.c: the MergeBatcher and its vector merger.
 * Callers feed sorted, consolidated chunks (each a __dtfl_batch of
 * (record, time, diff) rows ordered by (record, time)); the batcher keeps
 * geometrically sized chains, merging two chains whenever the smaller is at
 * least half the larger (weighed by summed update count), and on seal merges
 * everything, consolidates across chains, and extracts the updates not in
 * advance of `upper`.  The sealed frontier is taken over the kept survivors
 * AFTER consolidation, so a kept time whose updates cancel holds no
 * capability (the two ported frontier tests).  Records order by their
 * dtfl_type and times by Ord (word order).
 */
#include "dd/trace/trace_int.h"

static size_t
next_power_of_two(size_t n)
{
	size_t p = 1;

	while (p < n)
		p <<= 1;
	return p;
}

typedef struct chain_slot {
	size_t weight;
	__dtfl_chain chain;
} chain_slot;

void
__dtfl_merge_batcher_init(__dtfl_merge_batcher *mb, const dtfl_type *rt,
    const dtfl_time_class *tc, const __dtfl_diff_class *dc)
{
	size_t size;
	uint64_t min[DTFL_TIME_MAX];

	__dtfl_vec_init(&mb->chains, sizeof(chain_slot));
	mb->rt = rt;
	mb->dc = dc;
	mb->tc = tc;
	mb->tw = dtfl_time_depth(tc);
	mb->dw = (unsigned)dc->width;

	/* lower = [minimum]; frontier starts empty. */
	dtfl_time_minimum(tc, min);
	__dtfl_antichain_init_elem(&mb->lower, tc, min);
	__dtfl_antichain_init(&mb->frontier, tc);

	/*
	 * VecMerger::target_capacity: default_capacity rounded up to a power
	 * of two.  default_capacity is the rows that fit in BUFFER_SIZE_BYTES
	 * (a variable record counts as 24 bytes, as a Rust String or Vec).
	 */
	size = (rt->fixed_size != 0 ? rt->fixed_size : 24) +
	    (size_t)mb->tw * sizeof(uint64_t) + mb->dw;
	if (size == 0)
		mb->cap = DTFL_BUFFER_SIZE_BYTES;
	else if (size <= DTFL_BUFFER_SIZE_BYTES)
		mb->cap = DTFL_BUFFER_SIZE_BYTES / size;
	else
		mb->cap = 1;
	mb->cap = next_power_of_two(mb->cap);
}

static void
chain_free(__dtfl_chain *ch)
{
	size_t i;

	for (i = 0; i < ch->n; i++)
		__dtfl_batch_release(ch->chunks[i]);
	__dtfl_free(ch->chunks);
	ch->chunks = NULL;
	ch->n = 0;
}

void
__dtfl_merge_batcher_free(__dtfl_merge_batcher *mb)
{
	size_t i;

	for (i = 0; i < mb->chains.len; i++)
		chain_free(&DTFL_VEC_AT(&mb->chains, chain_slot, i)->chain);
	__dtfl_vec_free(&mb->chains);
	__dtfl_antichain_free(&mb->lower);
	__dtfl_antichain_free(&mb->frontier);
}

const __dtfl_antichain *
__dtfl_merge_batcher_frontier(const __dtfl_merge_batcher *mb)
{
	return &mb->frontier;
}

/* The summed update count of a chain (its merge weight). */
static size_t
chain_weight(const __dtfl_chain *ch)
{
	size_t w = 0, i;

	for (i = 0; i < ch->n; i++)
		w += __dtfl_batch_len(ch->chunks[i]);
	return w;
}

static void
chain_push(__dtfl_merge_batcher *mb, __dtfl_chain chain)
{
	chain_slot slot;

	slot.weight = chain_weight(&chain);
	slot.chain = chain;
	__dtfl_vec_push(&mb->chains, &slot);
}

static __dtfl_chain
chain_pop(__dtfl_merge_batcher *mb)
{
	chain_slot *s = DTFL_VEC_AT(&mb->chains, chain_slot, mb->chains.len - 1);
	__dtfl_chain ch = s->chain;

	__dtfl_vec_truncate(&mb->chains, mb->chains.len - 1);
	return ch;
}

/* Compare two chunk rows by (record, time), as VecMerger merges by (d, t). */
static int
row_cmp(const __dtfl_merge_batcher *mb, const __dtfl_batch *b1, size_t i1,
    const __dtfl_batch *b2, size_t i2)
{
	size_t l1, l2;
	const void *r1 = __dtfl_batch_rec(b1, i1, &l1);
	const void *r2 = __dtfl_batch_rec(b2, i2, &l2);
	int r = dtfl_type_cmp(mb->rt, r1, l1, r2, l2);

	if (r != 0)
		return r;
	return __dtfl_words_cmp(__dtfl_batch_time(b1, i1),
	    __dtfl_batch_time(b2, i2), mb->tw);
}

/*
 * A flat cursor over a chain's chunks, visiting rows in order.  A chain's
 * chunks are each internally sorted and the chunks are in sorted order, so
 * the cursor yields the chain's rows ascending.
 */
typedef struct chain_cursor {
	const __dtfl_chain *chain;
	size_t chunk;
	size_t row;
} chain_cursor;

static void
cc_init(chain_cursor *c, const __dtfl_chain *chain)
{
	c->chain = chain;
	c->chunk = 0;
	c->row = 0;
	while (c->chunk < chain->n &&
	    __dtfl_batch_len(chain->chunks[c->chunk]) == 0)
		c->chunk++;
}

static bool
cc_valid(const chain_cursor *c)
{
	return c->chunk < c->chain->n;
}

static const __dtfl_batch *
cc_batch(const chain_cursor *c)
{
	return c->chain->chunks[c->chunk];
}

static void
cc_step(chain_cursor *c)
{
	c->row++;
	if (c->row >= __dtfl_batch_len(c->chain->chunks[c->chunk])) {
		c->row = 0;
		c->chunk++;
		while (c->chunk < c->chain->n &&
		    __dtfl_batch_len(c->chain->chunks[c->chunk]) == 0)
			c->chunk++;
	}
}

/* Appends one chunk's worth to output, flushing full chunks of `cap` rows. */
typedef struct chunk_sink {
	__dtfl_vec chunks;	/* __dtfl_batch *, owned */
	__dtfl_batch *cur;
	unsigned tw;
	unsigned dw;
	size_t cap;
} chunk_sink;

static void
sink_init(chunk_sink *s, unsigned tw, unsigned dw, size_t cap)
{
	__dtfl_vec_init(&s->chunks, sizeof(__dtfl_batch *));
	s->cur = __dtfl_batch_new(tw, dw);
	s->tw = tw;
	s->dw = dw;
	s->cap = cap;
}

static void
sink_push(chunk_sink *s, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	__dtfl_batch_push(s->cur, rec, len, time, diff);
	if (__dtfl_batch_len(s->cur) >= s->cap) {
		__dtfl_vec_push(&s->chunks, &s->cur);
		s->cur = __dtfl_batch_new(s->tw, s->dw);
	}
}

/* Finish: emit the partial chunk if non-empty; return the chunk array. */
static void
sink_finish(chunk_sink *s, __dtfl_batch ***out, size_t *nout)
{
	if (__dtfl_batch_len(s->cur) != 0) {
		__dtfl_vec_push(&s->chunks, &s->cur);
		s->cur = NULL;
	} else {
		__dtfl_batch_release(s->cur);
		s->cur = NULL;
	}
	*nout = s->chunks.len;
	if (s->chunks.len == 0) {
		*out = NULL;
		__dtfl_vec_free(&s->chunks);
		return;
	}
	*out = __dtfl_alloc(s->chunks.len * sizeof(__dtfl_batch *));
	memcpy(*out, s->chunks.data, s->chunks.len * sizeof(__dtfl_batch *));
	__dtfl_vec_free(&s->chunks);
}

/*
 * VecMerger::merge: merge two chains into one, consolidating equal
 * (record, time) rows and dropping zero diffs.  The output is a single
 * chain (array of chunks of up to `cap` rows).
 */
static __dtfl_chain
merge_two(__dtfl_merge_batcher *mb, __dtfl_chain a, __dtfl_chain b)
{
	chain_cursor ca, cb;
	chunk_sink sink;
	__dtfl_chain out;
	unsigned char *accum = __dtfl_alloc(DTFL_MAX(mb->dw, 1u));

	cc_init(&ca, &a);
	cc_init(&cb, &b);
	sink_init(&sink, mb->tw, mb->dw, mb->cap);

	while (cc_valid(&ca) && cc_valid(&cb)) {
		int r = row_cmp(mb, cc_batch(&ca), ca.row, cc_batch(&cb),
		    cb.row);

		if (r < 0) {
			const __dtfl_batch *bb = cc_batch(&ca);
			size_t len;
			const void *rec = __dtfl_batch_rec(bb, ca.row, &len);

			sink_push(&sink, rec, len, __dtfl_batch_time(bb, ca.row),
			    __dtfl_batch_diff(bb, ca.row));
			cc_step(&ca);
		} else if (r > 0) {
			const __dtfl_batch *bb = cc_batch(&cb);
			size_t len;
			const void *rec = __dtfl_batch_rec(bb, cb.row, &len);

			sink_push(&sink, rec, len, __dtfl_batch_time(bb, cb.row),
			    __dtfl_batch_diff(bb, cb.row));
			cc_step(&cb);
		} else {
			const __dtfl_batch *b1 = cc_batch(&ca);
			const __dtfl_batch *b2 = cc_batch(&cb);
			size_t len;
			const void *rec = __dtfl_batch_rec(b1, ca.row, &len);

			if (mb->dw != 0)
				memcpy(accum, __dtfl_batch_diff(b1, ca.row),
				    mb->dw);
			mb->dc->plus_equals(mb->dc, accum,
			    __dtfl_batch_diff(b2, cb.row));
			if (!mb->dc->is_zero(mb->dc, accum))
				sink_push(&sink, rec, len,
				    __dtfl_batch_time(b1, ca.row), accum);
			cc_step(&ca);
			cc_step(&cb);
		}
	}
	while (cc_valid(&ca)) {
		const __dtfl_batch *bb = cc_batch(&ca);
		size_t len;
		const void *rec = __dtfl_batch_rec(bb, ca.row, &len);

		sink_push(&sink, rec, len, __dtfl_batch_time(bb, ca.row),
		    __dtfl_batch_diff(bb, ca.row));
		cc_step(&ca);
	}
	while (cc_valid(&cb)) {
		const __dtfl_batch *bb = cc_batch(&cb);
		size_t len;
		const void *rec = __dtfl_batch_rec(bb, cb.row, &len);

		sink_push(&sink, rec, len, __dtfl_batch_time(bb, cb.row),
		    __dtfl_batch_diff(bb, cb.row));
		cc_step(&cb);
	}
	sink_finish(&sink, &out.chunks, &out.n);
	__dtfl_free(accum);
	chain_free(&a);
	chain_free(&b);
	return out;
}

/* insert_chain: push a chain, fusing geometrically by weight. */
static void
insert_chain(__dtfl_merge_batcher *mb, __dtfl_chain chain)
{
	if (chain.n == 0 || chain_weight(&chain) == 0) {
		chain_free(&chain);
		return;
	}
	chain_push(mb, chain);
	while (mb->chains.len > 1) {
		chain_slot *top = DTFL_VEC_AT(&mb->chains, chain_slot,
		    mb->chains.len - 1);
		chain_slot *prev = DTFL_VEC_AT(&mb->chains, chain_slot,
		    mb->chains.len - 2);

		if (top->weight >= prev->weight / 2) {
			__dtfl_chain c1 = chain_pop(mb);
			__dtfl_chain c2 = chain_pop(mb);

			chain_push(mb, merge_two(mb, c1, c2));
		} else {
			break;
		}
	}
}

void
__dtfl_merge_batcher_chain_push(__dtfl_merge_batcher *mb, __dtfl_batch **chunks,
    size_t n)
{
	__dtfl_chain chain;
	size_t i;

	chain.chunks = __dtfl_alloc(DTFL_MAX(n, 1u) * sizeof(__dtfl_batch *));
	chain.n = n;
	for (i = 0; i < n; i++)
		chain.chunks[i] = chunks[i];	/* ownership moves in */
	/* chain_push without the geometric fusing (used by the tests). */
	chain_push(mb, chain);
}

void
__dtfl_merge_batcher_push(__dtfl_merge_batcher *mb, __dtfl_batch *chunk)
{
	__dtfl_chain chain;

	chain.chunks = __dtfl_alloc(sizeof(__dtfl_batch *));
	chain.chunks[0] = chunk;	/* ownership moves in */
	chain.n = 1;
	insert_chain(mb, chain);
}

/*
 * extract (VecMerger::extract): split the merged chain's rows into those
 * ready (not in advance of upper) and those kept (in advance of upper),
 * inserting each kept time into the frontier.  Because the merged chain is
 * already consolidated, a time is kept only if it has a surviving update, so
 * the frontier reflects post-consolidation survivors.
 */
static void
extract(__dtfl_merge_batcher *mb, __dtfl_chain merged,
    const __dtfl_antichain *upper, __dtfl_batch ***out, size_t *nout)
{
	chunk_sink ready;
	__dtfl_chain kept;
	chain_cursor cc;

	sink_init(&ready, mb->tw, mb->dw, mb->cap);
	{
		chunk_sink keep;

		sink_init(&keep, mb->tw, mb->dw, mb->cap);
		cc_init(&cc, &merged);
		while (cc_valid(&cc)) {
			const __dtfl_batch *bb = cc_batch(&cc);
			size_t len;
			const void *rec = __dtfl_batch_rec(bb, cc.row, &len);
			const uint64_t *t = __dtfl_batch_time(bb, cc.row);
			const void *d = __dtfl_batch_diff(bb, cc.row);

			if (__dtfl_frontier_less_equal(mb->tc,
			    __dtfl_ac_base(upper),
			    __dtfl_antichain_len(upper), t)) {
				__dtfl_antichain_insert(&mb->frontier, t);
				sink_push(&keep, rec, len, t, d);
			} else {
				sink_push(&ready, rec, len, t, d);
			}
			cc_step(&cc);
		}
		sink_finish(&keep, &kept.chunks, &kept.n);
	}
	sink_finish(&ready, out, nout);

	if (kept.n != 0)
		chain_push(mb, kept);
	else
		chain_free(&kept);
	chain_free(&merged);
}

void
__dtfl_merge_batcher_seal(__dtfl_merge_batcher *mb,
    const __dtfl_antichain *upper, __dtfl_batch ***out, size_t *nout,
    __dtfl_description *desc)
{
	__dtfl_chain merged;
	__dtfl_antichain lower, up, since;
	uint64_t min[DTFL_TIME_MAX];

	/* Merge all remaining chains into one. */
	while (mb->chains.len > 1) {
		__dtfl_chain c1 = chain_pop(mb);
		__dtfl_chain c2 = chain_pop(mb);

		insert_chain(mb, merge_two(mb, c1, c2));
	}
	if (mb->chains.len == 1)
		merged = chain_pop(mb);
	else {
		merged.chunks = NULL;
		merged.n = 0;
	}

	__dtfl_antichain_clear(&mb->frontier);
	extract(mb, merged, upper, out, nout);

	/* description = (lower, upper, [minimum]); lower = prior upper. */
	__dtfl_antichain_init(&lower, mb->tc);
	__dtfl_antichain_copy(&lower, &mb->lower);
	__dtfl_antichain_init(&up, mb->tc);
	__dtfl_antichain_copy(&up, upper);
	dtfl_time_minimum(mb->tc, min);
	__dtfl_antichain_init_elem(&since, mb->tc, min);
	__dtfl_description_init(desc, &lower, &up, &since);

	/* self.lower = upper (copy reuses lower's buffer). */
	__dtfl_antichain_copy(&mb->lower, upper);
}
