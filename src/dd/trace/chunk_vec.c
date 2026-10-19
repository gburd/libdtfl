/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/chunk/vec.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/chunk_vec.c: the one worked Chunk layout.  A chunk is a
 * __dtfl_batch of consolidated (record = key||val, time, diff) rows sorted by
 * (record, time); Clone is a __dtfl_batch_retain.  This file ports VecChunk's
 * four whole-chunk operations -- merge, extract, advance, settle -- over that
 * flat row layout.  Upstream's Vec<((K,V),T,R)> becomes the batch's rows; a
 * (key, val) group is a run of rows with equal records, and (record, time)
 * ordering is the batch's row order.  The columnar layout is subsumed (D2), so
 * the generic Chunk trait collapses to these concrete functions.
 */
#include "dd/trace/chunk_int.h"

/* ------------------------------------------------------------------ */
/* Chunk deque and layout context.                                     */
/* ------------------------------------------------------------------ */

void
__dtfl_chunk_deque_init(__dtfl_chunk_deque *d)
{
	__dtfl_vec_init(&d->v, sizeof(__dtfl_chunk *));
	d->head = 0;
}

void
__dtfl_chunk_deque_free(__dtfl_chunk_deque *d)
{
	size_t i;

	for (i = d->head; i < d->v.len; i++)
		__dtfl_batch_release(*DTFL_VEC_AT(&d->v, __dtfl_chunk *, i));
	__dtfl_vec_free(&d->v);
	d->head = 0;
}

void
__dtfl_chunk_deque_push_back(__dtfl_chunk_deque *d, __dtfl_chunk *c)
{
	__dtfl_vec_push(&d->v, &c);
}

void
__dtfl_chunk_deque_push_front(__dtfl_chunk_deque *d, __dtfl_chunk *c)
{
	size_t live;

	if (d->head > 0) {
		d->head--;
		*DTFL_VEC_AT(&d->v, __dtfl_chunk *, d->head) = c;
		return;
	}
	/* No room ahead: grow by one slot and shift the live window right. */
	live = d->v.len;			/* head is 0 here */
	__dtfl_vec_push(&d->v, &c);		/* append (c is a placeholder) */
	if (live > 0)
		memmove(DTFL_VEC_AT(&d->v, __dtfl_chunk *, 1),
		    DTFL_VEC_AT(&d->v, __dtfl_chunk *, 0),
		    live * sizeof(__dtfl_chunk *));
	*DTFL_VEC_AT(&d->v, __dtfl_chunk *, 0) = c;
}

__dtfl_chunk *
__dtfl_chunk_deque_pop_front(__dtfl_chunk_deque *d)
{
	__dtfl_chunk *c;

	if (d->v.len == d->head)
		return NULL;
	c = *DTFL_VEC_AT(&d->v, __dtfl_chunk *, d->head);
	d->head++;
	if (d->head == d->v.len) {		/* emptied: reset to the start */
		__dtfl_vec_clear(&d->v);
		d->head = 0;
	}
	return c;
}

void
__dtfl_chunk_deque_extend(__dtfl_chunk_deque *d, __dtfl_chunk_deque *src)
{
	__dtfl_chunk *c;

	while ((c = __dtfl_chunk_deque_pop_front(src)) != NULL)
		__dtfl_chunk_deque_push_back(d, c);
}

void
__dtfl_chunk_ctx_init(__dtfl_chunk_ctx *x, bool keyed, const dtfl_type *rt,
    const dtfl_type *kt, const dtfl_type *vt, const dtfl_time_class *tc,
    const __dtfl_diff_class *dc)
{
	x->rt = rt;
	x->kt = kt;
	x->vt = vt;
	x->tc = tc;
	x->dc = dc;
	x->keyed = keyed;
	x->tw = dtfl_time_depth(tc);
	x->dw = (unsigned)dc->width;
	x->spill = NULL;
}

/* ------------------------------------------------------------------ */
/* Row helpers over a chunk (__dtfl_batch).                            */
/* ------------------------------------------------------------------ */

/* Compare rows i and j of chunk c by (record, time). */
static int
cmp_row(const __dtfl_chunk_ctx *x, const __dtfl_chunk *c, size_t i,
    const __dtfl_chunk *c2, size_t j)
{
	const void *a, *b;
	size_t alen, blen;
	int r;

	a = __dtfl_batch_rec(c, i, &alen);
	b = __dtfl_batch_rec(c2, j, &blen);
	r = __dtfl_type_cmp(x->rt, a, alen, b, blen);
	if (r != 0)
		return r;
	return __dtfl_words_cmp(__dtfl_batch_time(c, i),
	    __dtfl_batch_time(c2, j), x->tw);
}

/* Compare only the records (the (key, val) part) of rows i and j. */
static int
cmp_rec(const __dtfl_chunk_ctx *x, const __dtfl_chunk *c, size_t i,
    const __dtfl_chunk *c2, size_t j)
{
	const void *a, *b;
	size_t alen, blen;

	a = __dtfl_batch_rec(c, i, &alen);
	b = __dtfl_batch_rec(c2, j, &blen);
	return __dtfl_type_cmp(x->rt, a, alen, b, blen);
}

/*
 * gallop (vec.rs): first index >= start in [0, c->len) at which "row < bound"
 * turns false, by exponential search.  `less` tests row i < the bound.
 */
static size_t
gallop(const __dtfl_chunk *c, size_t start,
    bool (*less)(size_t, void *), void *ctx)
{
	size_t pos = start, step, n = __dtfl_batch_len(c);

	if (pos < n && less(pos, ctx)) {
		step = 1;
		while (pos + step < n && less(pos + step, ctx)) {
			pos += step;
			step <<= 1;
		}
		step >>= 1;
		while (step > 0) {
			if (pos + step < n && less(pos + step, ctx))
				pos += step;
			step >>= 1;
		}
		pos += 1;
	}
	return pos;
}

/* "row < (record, time) of (bchunk, bi)" for gallop in merge. */
struct lt_rt {
	const __dtfl_chunk_ctx *x;
	const __dtfl_chunk *c;
	const __dtfl_chunk *bchunk;
	size_t bi;
};
static bool
lt_rt(size_t i, void *ctx)
{
	struct lt_rt *p = ctx;

	return cmp_row(p->x, p->c, i, p->bchunk, p->bi) < 0;
}

/* Append rows [lo, hi) of src to dst. */
static void
push_rows(__dtfl_chunk *dst, const __dtfl_chunk *src, size_t lo, size_t hi)
{
	size_t i;

	for (i = lo; i < hi; i++)
		__dtfl_batch_push_from(dst, src, i);
}

/* A fresh empty chunk shaped like the context (tw, dw). */
static __dtfl_chunk *
chunk_new(const __dtfl_chunk_ctx *x)
{
	return __dtfl_batch_new(x->tw, x->dw);
}

/* Flush: emit result when full (or forced non-empty), replacing it fresh. */
static void
flush(const __dtfl_chunk_ctx *x, __dtfl_chunk **result, __dtfl_chunk_deque *out,
    bool force)
{
	size_t n = __dtfl_batch_len(*result);

	if (n >= DTFL_CHUNK_TARGET || (force && n != 0)) {
		__dtfl_chunk_deque_push_back(out, *result);
		*result = chunk_new(x);
	}
}

/* ------------------------------------------------------------------ */
/* merge (vec.rs Chunk::merge).                                        */
/* ------------------------------------------------------------------ */

void
__dtfl_chunk_merge(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *in1,
    __dtfl_chunk_deque *in2, __dtfl_chunk_deque *out)
{
	__dtfl_chunk *result = chunk_new(x);
	__dtfl_chunk *c1 = __dtfl_chunk_deque_pop_front(in1);
	__dtfl_chunk *c2 = __dtfl_chunk_deque_pop_front(in2);
	size_t p1 = 0, p2 = 0, n1, n2;

	n1 = __dtfl_batch_len(c1);
	n2 = __dtfl_batch_len(c2);
	while (p1 < n1 && p2 < n2) {
		int r = cmp_row(x, c1, p1, c2, p2);

		if (r < 0) {
			struct lt_rt lt = { x, c1, c2, p2 };
			size_t run = gallop(c1, p1 + 1, lt_rt, &lt);
			size_t i;

			for (i = p1; i < run; i += DTFL_CHUNK_TARGET) {
				size_t end = i + DTFL_CHUNK_TARGET;

				if (end > run)
					end = run;
				push_rows(result, c1, i, end);
				flush(x, &result, out, false);
			}
			p1 = run;
		} else if (r > 0) {
			struct lt_rt lt = { x, c2, c1, p1 };
			size_t run = gallop(c2, p2 + 1, lt_rt, &lt);
			size_t i;

			for (i = p2; i < run; i += DTFL_CHUNK_TARGET) {
				size_t end = i + DTFL_CHUNK_TARGET;

				if (end > run)
					end = run;
				push_rows(result, c2, i, end);
				flush(x, &result, out, false);
			}
			p2 = run;
		} else {
			/* Equal (record, time): sum the diffs, keep if non-zero. */
			unsigned char accum[64];
			unsigned char *acc = accum;
			size_t rlen;
			const void *rec;

			if (x->dw > sizeof accum)
				acc = __dtfl_alloc(x->dw);
			if (x->dw != 0) {
				memcpy(acc, __dtfl_batch_diff(c1, p1), x->dw);
				x->dc->plus_equals(x->dc, acc,
				    __dtfl_batch_diff(c2, p2));
			}
			if (x->dw == 0 || !x->dc->is_zero(x->dc, acc)) {
				rec = __dtfl_batch_rec(c1, p1, &rlen);
				__dtfl_batch_push(result, rec, rlen,
				    __dtfl_batch_time(c1, p1), acc);
			}
			if (acc != accum)
				__dtfl_free(acc);
			p1++;
			p2++;
			flush(x, &result, out, false);
		}
		if (p1 == n1) {
			__dtfl_batch_release(c1);
			c1 = __dtfl_chunk_deque_pop_front(in1);
			if (c1 == NULL)
				break;
			p1 = 0;
			n1 = __dtfl_batch_len(c1);
		}
		if (p2 == n2) {
			__dtfl_batch_release(c2);
			c2 = __dtfl_chunk_deque_pop_front(in2);
			if (c2 == NULL)
				break;
			p2 = 0;
			n2 = __dtfl_batch_len(c2);
		}
	}
	flush(x, &result, out, true);
	__dtfl_batch_release(result);

	/* Push back the survivor's unconsumed suffix (one copy). */
	if (c1 != NULL && p1 < n1) {
		__dtfl_chunk *tail = chunk_new(x);

		push_rows(tail, c1, p1, n1);
		__dtfl_chunk_deque_push_front(in1, tail);
	}
	if (c1 != NULL)
		__dtfl_batch_release(c1);
	if (c2 != NULL && p2 < n2) {
		__dtfl_chunk *tail = chunk_new(x);

		push_rows(tail, c2, p2, n2);
		__dtfl_chunk_deque_push_front(in2, tail);
	}
	if (c2 != NULL)
		__dtfl_batch_release(c2);
}

/* ------------------------------------------------------------------ */
/* extract (vec.rs Chunk::extract).                                    */
/* ------------------------------------------------------------------ */

void
__dtfl_chunk_extract(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *input,
    const __dtfl_antichain *frontier, __dtfl_antichain *residual,
    __dtfl_chunk_deque *keep, __dtfl_chunk_deque *ship)
{
	__dtfl_chunk *chunk = __dtfl_chunk_deque_pop_front(input);
	__dtfl_chunk *k, *s;
	size_t i, n;
	bool have_k = false, have_s = false;

	if (chunk == NULL)
		return;
	k = chunk_new(x);
	s = chunk_new(x);
	n = __dtfl_batch_len(chunk);
	for (i = 0; i < n; i++) {
		const uint64_t *t = __dtfl_batch_time(chunk, i);

		/* frontier.less_equal(t): keep rows at or above the frontier. */
		if (__dtfl_frontier_less_equal(x->tc, __dtfl_ac_base(frontier),
		    __dtfl_antichain_len(frontier), t)) {
			__dtfl_antichain_insert(residual, t);
			__dtfl_batch_push_from(k, chunk, i);
			have_k = true;
		} else {
			__dtfl_batch_push_from(s, chunk, i);
			have_s = true;
		}
	}
	if (have_k)
		__dtfl_chunk_deque_push_back(keep, k);
	else
		__dtfl_batch_release(k);
	if (have_s)
		__dtfl_chunk_deque_push_back(ship, s);
	else
		__dtfl_batch_release(s);
	__dtfl_batch_release(chunk);
}

/* ------------------------------------------------------------------ */
/* advance (vec.rs Chunk::advance).                                    */
/* ------------------------------------------------------------------ */

/*
 * Sort rows [lo, hi) of buf, all one (key, val) group, by time (advancing is
 * lattice-monotone but not total-order-monotone, so the group must be re-sorted
 * after advance_by).  Stable insertion sort of a small group's row order; the
 * group is one key's updates, bounded by the per-value update count.
 */
static void
sort_group_by_time(const __dtfl_chunk_ctx *x, __dtfl_chunk *buf, size_t lo,
    size_t hi, size_t *idx)
{
	size_t i, j;

	for (i = lo; i < hi; i++)
		idx[i] = i;
	for (i = lo + 1; i < hi; i++) {
		size_t key = idx[i];

		for (j = i; j > lo &&
		    __dtfl_words_cmp(__dtfl_batch_time(buf, idx[j - 1]),
		    __dtfl_batch_time(buf, key), x->tw) > 0; j--)
			idx[j] = idx[j - 1];
		idx[j] = key;
	}
}

void
__dtfl_chunk_advance(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *input,
    const __dtfl_antichain *frontier, bool done, __dtfl_chunk_deque *out)
{
	__dtfl_chunk *buf, *chunk, *result;
	size_t n, end, i, *idx;
	const uint64_t *fbase = __dtfl_ac_base(frontier);
	size_t flen = __dtfl_antichain_len(frontier);

	/*
	 * Reuse the front chunk as the working buffer (last call's carry) and
	 * append the rest, so a withheld group accumulates in place.
	 */
	buf = __dtfl_chunk_deque_pop_front(input);
	if (buf == NULL)
		return;
	buf = __dtfl_batch_make_mut(buf);
	while ((chunk = __dtfl_chunk_deque_pop_front(input)) != NULL) {
		size_t m = __dtfl_batch_len(chunk), r;

		for (r = 0; r < m; r++)
			__dtfl_batch_push_from(buf, chunk, r);
		__dtfl_batch_release(chunk);
	}
	n = __dtfl_batch_len(buf);
	if (n == 0) {
		__dtfl_batch_release(buf);
		return;
	}

	/*
	 * Giant-key case: if the whole buffer is one (key, val), no group is
	 * provably complete, so unless done withhold it all.
	 */
	if (!done && cmp_rec(x, buf, 0, buf, n - 1) == 0) {
		__dtfl_chunk_deque_push_front(input, buf);
		return;
	}

	/* Withhold the last group as the carry unless done. */
	if (done) {
		end = n;
	} else {
		end = n;
		while (end > 0 && cmp_rec(x, buf, end - 1, buf, n - 1) == 0)
			end--;
	}
	if (end < n) {
		__dtfl_chunk *carry = chunk_new(x);

		push_rows(carry, buf, end, n);
		__dtfl_chunk_deque_push_front(input, carry);
	}

	/* Advance and consolidate each complete group into TARGET-sized chunks. */
	idx = __dtfl_alloc(__dtfl_mul_size(DTFL_MAX(end, 1), sizeof *idx));
	result = chunk_new(x);
	i = 0;
	while (i < end) {
		size_t j = i, k;
		unsigned char accum[64];
		unsigned char *acc = accum;

		if (x->dw > sizeof accum)
			acc = __dtfl_alloc(x->dw);
		while (j < end && cmp_rec(x, buf, j, buf, i) == 0)
			j++;
		/* advance_by every time in [i, j), then re-sort by time. */
		for (k = i; k < j; k++)
			__dtfl_time_advance_by(x->tc,
			    __dtfl_batch_time_mut(buf, k), fbase, flen);
		sort_group_by_time(x, buf, i, j, idx);
		/* Consolidate equal times along the sorted order. */
		k = i;
		while (k < j) {
			size_t rk = idx[k], rlen;
			const void *rec;
			const uint64_t *t = __dtfl_batch_time(buf, rk);

			if (x->dw != 0)
				memcpy(acc, __dtfl_batch_diff(buf, rk), x->dw);
			k++;
			while (k < j && __dtfl_words_cmp(
			    __dtfl_batch_time(buf, idx[k]), t, x->tw) == 0) {
				if (x->dw != 0)
					x->dc->plus_equals(x->dc, acc,
					    __dtfl_batch_diff(buf, idx[k]));
				k++;
			}
			if (x->dw == 0 || !x->dc->is_zero(x->dc, acc)) {
				rec = __dtfl_batch_rec(buf, rk, &rlen);
				__dtfl_batch_push(result, rec, rlen, t, acc);
				if (__dtfl_batch_len(result) >=
				    DTFL_CHUNK_TARGET) {
					__dtfl_chunk_deque_push_back(out,
					    result);
					result = chunk_new(x);
				}
			}
		}
		if (acc != accum)
			__dtfl_free(acc);
		i = j;
	}
	if (__dtfl_batch_len(result) != 0)
		__dtfl_chunk_deque_push_back(out, result);
	else
		__dtfl_batch_release(result);
	__dtfl_free(idx);
	__dtfl_batch_release(buf);
}

/* ------------------------------------------------------------------ */
/* pack / settle (mod.rs pack + vec.rs Chunk::settle).                 */
/* ------------------------------------------------------------------ */

/* pack_absorb (mod.rs): absorb `chunk` into an empty carry. */
static void
pack_absorb(const __dtfl_chunk_ctx *x, __dtfl_chunk *chunk,
    __dtfl_chunk **carry, __dtfl_chunk_deque *out,
    void (*split)(const __dtfl_chunk_ctx *, __dtfl_chunk *, size_t,
	__dtfl_chunk **, __dtfl_chunk **, void *),
    __dtfl_chunk *(*seal)(const __dtfl_chunk_ctx *, __dtfl_chunk *, void *),
    void *ctx)
{
	size_t len = __dtfl_batch_len(chunk);

	if (len == DTFL_CHUNK_TARGET) {
		__dtfl_chunk_deque_push_back(out, seal(x, chunk, ctx));
	} else if (len < DTFL_CHUNK_TARGET) {
		/* Carry it: make it uniquely held so combine can extend it. */
		*carry = __dtfl_batch_make_mut(chunk);
	} else {
		__dtfl_chunk *rest = chunk;

		for (;;) {
			__dtfl_chunk *head, *tail;

			split(x, rest, DTFL_CHUNK_TARGET, &head, &tail, ctx);
			__dtfl_chunk_deque_push_back(out, seal(x, head, ctx));
			if (__dtfl_batch_len(tail) >= DTFL_CHUNK_TARGET) {
				rest = tail;
			} else {
				if (__dtfl_batch_len(tail) > 0)
					*carry = tail;
				else
					__dtfl_batch_release(tail);
				break;
			}
		}
	}
}

void
__dtfl_chunk_pack(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *input,
    bool done, __dtfl_chunk_deque *out,
    void (*combine)(const __dtfl_chunk_ctx *, __dtfl_chunk *, __dtfl_chunk *,
	void *),
    void (*split)(const __dtfl_chunk_ctx *, __dtfl_chunk *, size_t,
	__dtfl_chunk **, __dtfl_chunk **, void *),
    __dtfl_chunk *(*seal)(const __dtfl_chunk_ctx *, __dtfl_chunk *, void *),
    void *ctx)
{
	__dtfl_chunk *carry = NULL, *chunk;

	while ((chunk = __dtfl_chunk_deque_pop_front(input)) != NULL) {
		if (carry == NULL) {
			pack_absorb(x, chunk, &carry, out, split, seal, ctx);
		} else if (__dtfl_batch_len(carry) + __dtfl_batch_len(chunk) <=
		    DTFL_CHUNK_TARGET) {
			combine(x, carry, chunk, ctx);
			if (__dtfl_batch_len(carry) == DTFL_CHUNK_TARGET) {
				__dtfl_chunk_deque_push_back(out,
				    seal(x, carry, ctx));
				carry = NULL;
			}
		} else {
			__dtfl_chunk_deque_push_back(out, seal(x, carry, ctx));
			carry = NULL;
			pack_absorb(x, chunk, &carry, out, split, seal, ctx);
		}
	}
	if (carry != NULL) {
		if (done)
			__dtfl_chunk_deque_push_back(out, seal(x, carry, ctx));
		else
			__dtfl_chunk_deque_push_front(input, carry);
	}
}

/* combine: extend acc's rows with next's, then release next (vec.rs settle). */
static void
settle_combine(const __dtfl_chunk_ctx *x, __dtfl_chunk *acc, __dtfl_chunk *next,
    void *ctx)
{
	size_t m = __dtfl_batch_len(next), i;

	(void)x;
	(void)ctx;
	for (i = 0; i < m; i++)
		__dtfl_batch_push_from(acc, next, i);
	__dtfl_batch_release(next);
}

/* split: the first n rows and the remaining len - n (vec.rs settle). */
static void
settle_split(const __dtfl_chunk_ctx *x, __dtfl_chunk *chunk, size_t n,
    __dtfl_chunk **head, __dtfl_chunk **tail, void *ctx)
{
	size_t len = __dtfl_batch_len(chunk);

	(void)ctx;
	*head = chunk_new(x);
	*tail = chunk_new(x);
	push_rows(*head, chunk, 0, n);
	push_rows(*tail, chunk, n, len);
	__dtfl_batch_release(chunk);
}

void
__dtfl_chunk_settle(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *input,
    bool done, __dtfl_chunk_deque *out)
{
	__dtfl_chunk_pack(x, input, done, out, settle_combine, settle_split,
	    __dtfl_spill_seal, (void *)x);
}

/* ------------------------------------------------------------------ */
/* is_graded, settle_all, merge_chains (mod.rs).                       */
/* ------------------------------------------------------------------ */

bool
__dtfl_chunk_is_graded(const __dtfl_chunk_deque *chunks)
{
	size_t n = __dtfl_chunk_deque_len(chunks), i;

	for (i = 0; i < n; i++)
		if (__dtfl_batch_len(__dtfl_chunk_deque_at(chunks, i)) >
		    DTFL_CHUNK_TARGET)
			return false;
	for (i = 0; i + 1 < n; i++)
		if (__dtfl_batch_len(__dtfl_chunk_deque_at(chunks, i)) +
		    __dtfl_batch_len(__dtfl_chunk_deque_at(chunks, i + 1)) <=
		    DTFL_CHUNK_TARGET)
			return false;
	return true;
}

void
__dtfl_chunk_settle_all(const __dtfl_chunk_ctx *x, __dtfl_chunk_deque *input,
    __dtfl_chunk_deque *out)
{
	__dtfl_chunk_settle(x, input, true, out);
}

void
__dtfl_chunk_merge_chains(const __dtfl_chunk_ctx *x,
    __dtfl_chunk_deque *chain1, __dtfl_chunk_deque *chain2,
    __dtfl_chunk_deque *out)
{
	while (!__dtfl_chunk_deque_empty(chain1) &&
	    !__dtfl_chunk_deque_empty(chain2))
		__dtfl_chunk_merge(x, chain1, chain2, out);
	__dtfl_chunk_deque_extend(out, chain1);
	__dtfl_chunk_deque_extend(out, chain2);
}
