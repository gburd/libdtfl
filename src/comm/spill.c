/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   communication/src/allocator/zero_copy/spill.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/spill.c: spill strategies and policies for MergeQueue
 * entries under memory pressure.
 *
 * Three vtables compose (zc_int.h):
 *   - __dtfl_spill_policy decides whether and how a queue is reshaped.
 *     It gets the raw entry deque under the queue's mutex and may
 *     replace entries freely.
 *   - __dtfl_bytes_spill decides where bytes go when a policy spills
 *     (file, object store, in-memory mock for tests).
 *   - __dtfl_bytes_fetch is the handle a strategy returns; fetching
 *     reads the bytes back and consumes the handle.
 *
 * Threshold pairs a strategy with threshold and reserve knobs: "spill
 * the middle of the queue when resident bytes get too large".
 * PrefetchPolicy is its reader-side dual.  Upstream's SpillPolicyFn (a
 * factory of writer/reader policy pairs) arrives with the allocator
 * configuration; until then the allocators build queues without
 * policies, which is upstream's default.
 */
#include "comm/zc_int.h"

/* Drops a policy not handed to a queue.  NULL is a no-op. */
void
__dtfl_spill_policy_destroy(__dtfl_spill_policy *p)
{
	if (p != NULL)
		p->ops->destroy(p);
}

/* ------------------------------------------------------------------ */
/* threshold::Threshold                                                */
/* ------------------------------------------------------------------ */

/*
 * When the resident byte count exceeds head_reserve_bytes +
 * threshold_bytes, spill every resident entry that starts at or after
 * the head reserve, except the last entry, which stays as the try_merge
 * target.
 */
static void
threshold_apply(__dtfl_spill_policy *pp, __dtfl_queue_entries *queue)
{
	__dtfl_spill_threshold *p = (__dtfl_spill_threshold *)pp;
	__dtfl_vec target_indices, target_bytes, handles;
	size_t resident = 0, cumulative = 0, last_index, i, n;

	for (i = 0; i < queue->len; i++) {
		__dtfl_queue_entry *e = __dtfl_qe_at(queue, i);

		if (e->kind == DTFL_QE_BYTES)
			resident += e->u.bytes.len;
	}
	if (resident <= p->head_reserve_bytes + p->threshold_bytes)
		return;

	last_index = queue->len > 0 ? queue->len - 1 : 0;
	__dtfl_vec_init(&target_indices, sizeof(size_t));
	__dtfl_vec_init(&target_bytes, sizeof(__dtfl_bytes));
	for (i = 0; i < queue->len; i++) {
		__dtfl_queue_entry *e = __dtfl_qe_at(queue, i);
		__dtfl_bytes *clone;

		if (i == last_index)
			break;
		if (e->kind != DTFL_QE_BYTES)
			continue;
		if (cumulative >= p->head_reserve_bytes) {
			__dtfl_vec_push(&target_indices, &i);
			clone = __dtfl_vec_push(&target_bytes, NULL);
			__dtfl_bytes_clone(&e->u.bytes, clone);
		}
		cumulative += e->u.bytes.len;
	}

	if (target_bytes.len > 0) {
		__dtfl_vec_init(&handles, sizeof(__dtfl_bytes_fetch *));
		p->strategy->ops->spill(p->strategy, &target_bytes, &handles);
		/* Replace entries for however many chunks were spilled. */
		n = DTFL_MIN(target_indices.len, handles.len);
		for (i = 0; i < n; i++) {
			__dtfl_queue_entry *e = __dtfl_qe_at(queue,
			    *DTFL_VEC_AT(&target_indices, size_t, i));

			__dtfl_qe_entry_drop(e);
			e->kind = DTFL_QE_PAGED;
			e->u.paged = *DTFL_VEC_AT(&handles,
			    __dtfl_bytes_fetch *, i);
		}
		/* Surplus handles (a strategy bug) would leak; drop them. */
		for (; i < handles.len; i++) {
			__dtfl_bytes_fetch *h = *DTFL_VEC_AT(&handles,
			    __dtfl_bytes_fetch *, i);

			h->ops->destroy(h);
		}
		__dtfl_vec_free(&handles);
	}
	/* Clones the strategy did not take stay with us; release them. */
	for (i = 0; i < target_bytes.len; i++)
		__dtfl_bytes_release(DTFL_VEC_AT(&target_bytes, __dtfl_bytes,
		    i));
	__dtfl_vec_free(&target_bytes);
	__dtfl_vec_free(&target_indices);
}

/* Drops the policy and its strategy. */
static void
threshold_destroy(__dtfl_spill_policy *pp)
{
	__dtfl_spill_threshold *p = (__dtfl_spill_threshold *)pp;

	p->strategy->ops->destroy(p->strategy);
	__dtfl_free(p);
}

static const __dtfl_spill_policy_ops threshold_ops = {
	threshold_apply, threshold_destroy
};

/* Threshold::new: default knobs, takes ownership of `strategy`. */
__dtfl_spill_threshold *
__dtfl_spill_threshold_new(__dtfl_bytes_spill *strategy)
{
	__dtfl_spill_threshold *p = __dtfl_alloc(sizeof *p);

	p->base.ops = &threshold_ops;
	p->strategy = strategy;
	p->threshold_bytes = (size_t)256 << 20;		/* 256 MB */
	p->head_reserve_bytes = (size_t)64 << 20;	/*  64 MB */
	return p;
}

/* ------------------------------------------------------------------ */
/* prefetch::PrefetchPolicy                                            */
/* ------------------------------------------------------------------ */

/*
 * Materializes Paged entries near the front of the queue until the
 * resident head reaches the budget.  A failed fetch puts the handle
 * back and stops.
 */
static void
prefetch_apply(__dtfl_spill_policy *pp, __dtfl_queue_entries *queue)
{
	__dtfl_spill_prefetch *p = (__dtfl_spill_prefetch *)pp;
	size_t resident_head = 0, i = 0, j;
	__dtfl_vec fetched;

	__dtfl_vec_init(&fetched, sizeof(__dtfl_bytes));
	while (i < queue->len && resident_head < p->budget) {
		__dtfl_queue_entry *e = __dtfl_qe_at(queue, i);
		__dtfl_queue_entry paged;

		if (e->kind == DTFL_QE_BYTES) {
			resident_head += e->u.bytes.len;
			i++;
			continue;
		}
		paged = __dtfl_qe_remove(queue, i);
		__dtfl_vec_clear(&fetched);
		if (!paged.u.paged->ops->fetch(paged.u.paged, &fetched)) {
			/* Fetch failed; put the handle back and stop. */
			__dtfl_qe_insert(queue, i, paged);
			break;
		}
		for (j = 0; j < fetched.len; j++) {
			__dtfl_queue_entry b;

			b.kind = DTFL_QE_BYTES;
			b.u.bytes = *DTFL_VEC_AT(&fetched, __dtfl_bytes, j);
			resident_head += b.u.bytes.len;
			__dtfl_qe_insert(queue, i + j, b);
		}
		i += fetched.len;
	}
	__dtfl_vec_free(&fetched);
}

/* Drops the policy. */
static void
prefetch_destroy(__dtfl_spill_policy *pp)
{
	__dtfl_free(pp);
}

static const __dtfl_spill_policy_ops prefetch_ops = {
	prefetch_apply, prefetch_destroy
};

/* PrefetchPolicy::new: a reader policy with the given byte budget. */
__dtfl_spill_prefetch *
__dtfl_spill_prefetch_new(size_t budget)
{
	__dtfl_spill_prefetch *p = __dtfl_alloc(sizeof *p);

	p->base.ops = &prefetch_ops;
	p->budget = budget;
	return p;
}
