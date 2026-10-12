/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/implementations/spine_fueled.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/spine_fueled.c: the fuelled spine.  A geometric stack of
 * layers, each Vacant, a Single batch, or a Double pair being merged.  A
 * batch at level i is treated as 2^i updates for accounting even when
 * physically smaller.  Introducing a batch applies "8 << level" units of
 * fuel to in-progress merges, rolls up lower layers so the insertion lands
 * in an empty layer, inserts, and tidies the largest layers.  The fuel
 * constant and the accounting are kept exactly (PLAN D11): a wrong fuel
 * amount shows up as either a never-shrinking spine or a lost update.
 */
#include "dd/trace/trace_int.h"

/* ------------------------------------------------------------------ */
/* MergeState                                                          */
/* ------------------------------------------------------------------ */

static void
ms_vacant(__dtfl_merge_state *m)
{
	m->kind = DTFL_MERGE_VACANT;
	m->present = false;
	m->batch = NULL;
	m->in1 = NULL;
	m->in2 = NULL;
}

/* Releases any batches a merge state holds and leaves it Vacant. */
static void
ms_drop(__dtfl_merge_state *m)
{
	switch (m->kind) {
	case DTFL_MERGE_SINGLE:
	case DTFL_MERGE_COMPLETE:
		__dtfl_batch_ord_release(m->batch);
		break;
	case DTFL_MERGE_INPROGRESS:
		__dtfl_batch_ord_release(m->in1);
		__dtfl_batch_ord_release(m->in2);
		__dtfl_batch_ord_release(m->merger.src1);
		__dtfl_batch_ord_release(m->merger.src2);
		/* Free the merger's partial storage. */
		__dtfl_slices_free(&m->merger.result.keys);
		__dtfl_vals_free(&m->merger.result.vals);
		__dtfl_upds_free(&m->merger.result.upds);
		__dtfl_upds_builder_free(&m->merger.staging);
		__dtfl_description_free(&m->merger.description);
		break;
	default:
		break;
	}
	ms_vacant(m);
}

/* MergeState::len: actual updates in the level. */
static size_t
ms_len(const __dtfl_merge_state *m)
{
	switch (m->kind) {
	case DTFL_MERGE_SINGLE:
	case DTFL_MERGE_COMPLETE:
		return m->present ? __dtfl_batch_ord_len(m->batch) : 0;
	case DTFL_MERGE_INPROGRESS:
		return __dtfl_batch_ord_len(m->in1) +
		    __dtfl_batch_ord_len(m->in2);
	default:
		return 0;
	}
}

static bool
ms_is_vacant(const __dtfl_merge_state *m)
{
	return m->kind == DTFL_MERGE_VACANT;
}

static bool
ms_is_single(const __dtfl_merge_state *m)
{
	return m->kind == DTFL_MERGE_SINGLE;
}

static bool
ms_is_double(const __dtfl_merge_state *m)
{
	return m->kind == DTFL_MERGE_INPROGRESS ||
	    m->kind == DTFL_MERGE_COMPLETE;
}

static bool
ms_is_complete(const __dtfl_merge_state *m)
{
	return m->kind == DTFL_MERGE_COMPLETE;
}

/*
 * MergeVariant::work: apply fuel to an in-progress merge; on completion
 * (fuel still positive) extract the merged batch and become Complete.
 */
static void
ms_work(__dtfl_merge_state *m, int64_t *fuel)
{
	if (m->kind != DTFL_MERGE_INPROGRESS)
		return;
	__dtfl_merger_ord_work(&m->merger, fuel);
	if (*fuel > 0) {
		__dtfl_batch_ord *done = __dtfl_merger_ord_done(&m->merger);

		__dtfl_batch_ord_release(m->in1);
		__dtfl_batch_ord_release(m->in2);
		m->in1 = NULL;
		m->in2 = NULL;
		m->kind = DTFL_MERGE_COMPLETE;
		m->present = true;
		m->batch = done;
	}
}

/*
 * MergeState::complete: finish any merge and return the batch (present via
 * *out_present).  Leaves the state Vacant.
 */
static __dtfl_batch_ord *
ms_complete(__dtfl_merge_state *m, bool *out_present)
{
	int64_t fuel = INT64_MAX;
	__dtfl_batch_ord *b = NULL;

	*out_present = false;
	switch (m->kind) {
	case DTFL_MERGE_SINGLE:
		b = m->present ? m->batch : NULL;
		*out_present = m->present;
		m->batch = NULL;
		break;
	case DTFL_MERGE_COMPLETE:
		b = m->present ? m->batch : NULL;
		*out_present = m->present;
		m->batch = NULL;
		break;
	case DTFL_MERGE_INPROGRESS:
		ms_work(m, &fuel);
		DTFL_ASSERT(m->kind == DTFL_MERGE_COMPLETE);
		b = m->present ? m->batch : NULL;
		*out_present = m->present;
		m->batch = NULL;
		break;
	default:
		break;
	}
	ms_vacant(m);
	return b;
}

/*
 * MergeState::begin_merge: start merging old and new (either may be a
 * structurally empty "None").  Both present begins an in-progress merge;
 * otherwise the single present batch becomes a Complete state.
 */
static void
ms_begin_merge(__dtfl_merge_state *m, __dtfl_batch_ord *oldb, bool old_present,
    __dtfl_batch_ord *newb, bool new_present,
    const __dtfl_antichain *compaction)
{
	if (old_present && new_present) {
		DTFL_ASSERT(__dtfl_antichain_eq(__dtfl_batch_ord_upper(oldb),
		    __dtfl_batch_ord_lower(newb)));
		m->kind = DTFL_MERGE_INPROGRESS;
		m->in1 = oldb;
		m->in2 = newb;
		__dtfl_merger_ord_new(&m->merger, oldb, newb, compaction);
	} else if (new_present) {
		m->kind = DTFL_MERGE_COMPLETE;
		m->present = true;
		m->batch = newb;
	} else if (old_present) {
		m->kind = DTFL_MERGE_COMPLETE;
		m->present = true;
		m->batch = oldb;
	} else {
		m->kind = DTFL_MERGE_COMPLETE;
		m->present = false;
		m->batch = NULL;
	}
}

/* ------------------------------------------------------------------ */
/* Spine                                                               */
/* ------------------------------------------------------------------ */

void
__dtfl_spine_init(__dtfl_spine *s, bool keyed, const dtfl_type *kt,
    const dtfl_type *vt, const dtfl_time_class *tc, const __dtfl_diff_class *dc)
{
	uint64_t min[DTFL_TIME_MAX];

	dtfl_time_minimum(tc, min);
	__dtfl_antichain_init_elem(&s->logical_frontier, tc, min);
	__dtfl_antichain_init_elem(&s->physical_frontier, tc, min);
	__dtfl_vec_init(&s->merging, sizeof(__dtfl_merge_state));
	__dtfl_vec_init(&s->pending, sizeof(__dtfl_batch_ord *));
	__dtfl_antichain_init_elem(&s->upper, tc, min);
	s->effort = 1;
	s->keyed = keyed;
	s->kt = kt;
	s->vt = vt;
	s->tc = tc;
	s->dc = dc;
}

void
__dtfl_spine_free(__dtfl_spine *s)
{
	size_t i;

	for (i = 0; i < s->merging.len; i++)
		ms_drop(DTFL_VEC_AT(&s->merging, __dtfl_merge_state, i));
	__dtfl_vec_free(&s->merging);
	for (i = 0; i < s->pending.len; i++)
		__dtfl_batch_ord_release(*DTFL_VEC_AT(&s->pending,
		    __dtfl_batch_ord *, i));
	__dtfl_vec_free(&s->pending);
	__dtfl_antichain_free(&s->logical_frontier);
	__dtfl_antichain_free(&s->physical_frontier);
	__dtfl_antichain_free(&s->upper);
}

static __dtfl_merge_state *
layer(__dtfl_spine *s, size_t i)
{
	return DTFL_VEC_AT(&s->merging, __dtfl_merge_state, i);
}

static void
ensure_layers(__dtfl_spine *s, size_t index)
{
	__dtfl_merge_state v;

	ms_vacant(&v);
	while (s->merging.len <= index)
		__dtfl_vec_push(&s->merging, &v);
}

/* insert_at: install a batch (or a structurally empty None) at `index`. */
static void
insert_at(__dtfl_spine *s, __dtfl_batch_ord *batch, bool present, size_t index)
{
	__dtfl_merge_state *m;
	__dtfl_merge_state taken;

	ensure_layers(s, index);
	m = layer(s, index);
	taken = *m;
	ms_vacant(m);

	switch (taken.kind) {
	case DTFL_MERGE_VACANT:
		m->kind = DTFL_MERGE_SINGLE;
		m->present = present;
		m->batch = batch;
		break;
	case DTFL_MERGE_SINGLE:
		ms_begin_merge(m, taken.batch, taken.present, batch, present,
		    &s->logical_frontier);
		break;
	default:
		__dtfl_panic("spine: insert into incomplete merge");
	}
}

/* complete_at: finish and extract whatever is at `index`. */
static __dtfl_batch_ord *
complete_at(__dtfl_spine *s, size_t index, bool *present)
{
	return ms_complete(layer(s, index), present);
}

/*
 * apply_fuel: give each layer independent fuel; when a merge completes,
 * install the result at the next level.
 */
static void
apply_fuel(__dtfl_spine *s, int64_t *fuel)
{
	size_t index;

	for (index = 0; index < s->merging.len; index++) {
		int64_t f = *fuel;

		ms_work(layer(s, index), &f);
		if (ms_is_complete(layer(s, index))) {
			bool present;
			__dtfl_batch_ord *c = complete_at(s, index, &present);

			insert_at(s, c, present, index + 1);
		}
	}
}

/*
 * roll_up: merge all layers below `index` into one and install it at
 * `index`, so the following insert_at lands in an empty layer.
 */
static void
roll_up(__dtfl_spine *s, size_t index)
{
	size_t i;
	bool any = false;

	ensure_layers(s, index);
	for (i = 0; i < index; i++)
		if (!ms_is_vacant(layer(s, i)))
			any = true;
	if (!any)
		return;

	{
		__dtfl_batch_ord *merged = NULL;
		bool present = false;

		for (i = 0; i < index; i++) {
			insert_at(s, merged, present, i);
			merged = complete_at(s, i, &present);
		}
		insert_at(s, merged, present, index);
		if (ms_is_double(layer(s, index))) {
			merged = complete_at(s, index, &present);
			insert_at(s, merged, present, index + 1);
		}
	}
}

/* tidy_layers: draw the largest Single layer down if the invariant holds. */
static void
tidy_layers(__dtfl_spine *s)
{
	size_t length, appropriate;

	if (s->merging.len == 0)
		return;
	length = s->merging.len;
	if (!ms_is_single(layer(s, length - 1)))
		return;

	appropriate = 0;
	{
		size_t n = __dtfl_level_of_len(ms_len(layer(s, length - 1)));

		appropriate = n;
	}
	while (appropriate < length - 1) {
		__dtfl_merge_state *below = layer(s, length - 2);

		if (ms_is_vacant(below) ||
		    (ms_is_single(below) && !below->present)) {
			ms_drop(below);
			__dtfl_vec_remove(&s->merging, length - 2);
			length = s->merging.len;
		} else if (ms_is_single(below)) {
			__dtfl_batch_ord *batch = below->batch;
			bool present = below->present;
			size_t smaller = 0, index;

			below->batch = NULL;
			ms_vacant(below);
			for (index = 0; index < length - 2; index++) {
				__dtfl_merge_state *b = layer(s, index);

				if (ms_is_single(b))
					smaller += (size_t)1 << index;
				else if (ms_is_double(b))
					smaller += (size_t)2 << index;
			}
			if (smaller <= ((size_t)1 << length) / 8) {
				__dtfl_vec_remove(&s->merging, length - 2);
				insert_at(s, batch, present, length - 2);
			} else {
				__dtfl_merge_state *b2 = layer(s, length - 2);

				b2->kind = DTFL_MERGE_SINGLE;
				b2->present = present;
				b2->batch = batch;
			}
			return;
		} else {
			return;	/* Double: a merge is in progress */
		}
	}
}

/*
 * introduce_batch: the fuelled insertion.  Fuel is "8 << batch_index"
 * (times effort), applied to in-progress merges, then roll up and insert
 * into an empty layer, then tidy.  The 8 << level constant is kept exactly.
 */
void
__dtfl_spine_introduce_batch(__dtfl_spine *s, __dtfl_batch_ord *batch,
    size_t batch_index)
{
	int64_t fuel;
	bool present = batch != NULL;

	/*
	 * Eight units of fuel per introduced record: four for the record and
	 * four for a virtual record from promoting smaller batches.
	 */
	fuel = (int64_t)((size_t)8 << batch_index);
	fuel *= (int64_t)s->effort;

	apply_fuel(s, &fuel);
	roll_up(s, batch_index);
	insert_at(s, batch, present, batch_index);
	tidy_layers(s);
}

/* consider_merges: migrate pending batches whose upper is <= physical. */
static void
consider_merges(__dtfl_spine *s)
{
	while (s->pending.len != 0) {
		__dtfl_batch_ord *first = *DTFL_VEC_AT(&s->pending,
		    __dtfl_batch_ord *, 0);
		const __dtfl_antichain *up = __dtfl_batch_ord_upper(first);

		if (!__dtfl_frontier_le(up, &s->physical_frontier))
			break;

		__dtfl_vec_remove(&s->pending, 0);
		{
			size_t len = __dtfl_batch_ord_len(first);
			size_t index = __dtfl_level_of_len(len);

			__dtfl_spine_introduce_batch(s, first, index);
		}
	}
}

void
__dtfl_spine_insert(__dtfl_spine *s, __dtfl_batch_ord *batch)
{
	DTFL_ASSERT(!__dtfl_antichain_eq(__dtfl_batch_ord_lower(batch),
	    __dtfl_batch_ord_upper(batch)));
	DTFL_ASSERT(__dtfl_antichain_eq(__dtfl_batch_ord_lower(batch),
	    &s->upper));

	__dtfl_antichain_free(&s->upper);
	__dtfl_antichain_copy(&s->upper, __dtfl_batch_ord_upper(batch));
	__dtfl_vec_push(&s->pending, &batch);
	consider_merges(s);
}

void
__dtfl_spine_close(__dtfl_spine *s)
{
	if (__dtfl_antichain_len(&s->upper) != 0) {
		__dtfl_antichain lower, upper;
		__dtfl_batch_ord *empty;

		__dtfl_antichain_init(&lower, s->tc);
		__dtfl_antichain_copy(&lower, &s->upper);
		__dtfl_antichain_init(&upper, s->tc);
		empty = __dtfl_batch_ord_empty(s->keyed, s->kt, s->vt, s->tc,
		    s->dc, &lower, &upper);
		__dtfl_spine_insert(s, empty);
	}
}

void
__dtfl_spine_set_logical_compaction(__dtfl_spine *s,
    const __dtfl_antichain *frontier)
{
	__dtfl_antichain_free(&s->logical_frontier);
	__dtfl_antichain_copy(&s->logical_frontier, frontier);
}

void
__dtfl_spine_set_physical_compaction(__dtfl_spine *s,
    const __dtfl_antichain *frontier)
{
	DTFL_ASSERT(__dtfl_frontier_le(&s->physical_frontier, frontier));
	__dtfl_antichain_free(&s->physical_frontier);
	__dtfl_antichain_copy(&s->physical_frontier, frontier);
	consider_merges(s);
}

/*
 * batches_through: the non-empty batches whose updates lie at times not in
 * advance of `upper`.  Returns a new owned (retained) array; *nout is the
 * count, or SIZE_MAX if `upper` straddles a non-empty pending batch.  A NULL
 * or empty `upper` returns every batch (the full cursor).
 */
__dtfl_batch_ord **
__dtfl_spine_batches_through(__dtfl_spine *s, const __dtfl_antichain *upper,
    size_t *nout)
{
	__dtfl_vec storage;
	size_t i, mi;
	__dtfl_batch_ord **out;
	uint64_t min[DTFL_TIME_MAX];
	bool upper_is_min;

	__dtfl_vec_init(&storage, sizeof(__dtfl_batch_ord *));

	/* If `upper` <= minimum, an empty cursor. */
	dtfl_time_minimum(s->tc, min);
	upper_is_min = upper != NULL && __dtfl_antichain_len(upper) != 0 &&
	    __dtfl_frontier_less_equal(s->tc, __dtfl_ac_base(upper),
	    __dtfl_antichain_len(upper), min);
	if (upper_is_min) {
		*nout = 0;
		__dtfl_vec_free(&storage);
		return NULL;
	}

#define PUSH_RETAIN(b)							\
	do {								\
		__dtfl_batch_ord *bb = __dtfl_batch_ord_retain(b);	\
		__dtfl_vec_push(&storage, &bb);				\
	} while (0)

	/* Walk merging layers from largest to smallest. */
	for (mi = s->merging.len; mi-- > 0;) {
		__dtfl_merge_state *m = layer(s, mi);

		switch (m->kind) {
		case DTFL_MERGE_INPROGRESS:
			if (!__dtfl_batch_ord_is_empty(m->in1))
				PUSH_RETAIN(m->in1);
			if (!__dtfl_batch_ord_is_empty(m->in2))
				PUSH_RETAIN(m->in2);
			break;
		case DTFL_MERGE_COMPLETE:
			if (m->present && !__dtfl_batch_ord_is_empty(m->batch))
				PUSH_RETAIN(m->batch);
			break;
		case DTFL_MERGE_SINGLE:
			if (m->present && !__dtfl_batch_ord_is_empty(m->batch))
				PUSH_RETAIN(m->batch);
			break;
		default:
			break;
		}
	}

	for (i = 0; i < s->pending.len; i++) {
		__dtfl_batch_ord *b = *DTFL_VEC_AT(&s->pending,
		    __dtfl_batch_ord *, i);
		bool include_lower, include_upper;

		if (__dtfl_batch_ord_is_empty(b))
			continue;
		if (upper == NULL || __dtfl_antichain_len(upper) == 0) {
			/* Empty upper: include every pending batch. */
			PUSH_RETAIN(b);
			continue;
		}
		include_lower = __dtfl_frontier_le(__dtfl_batch_ord_lower(b),
		    upper);
		include_upper = __dtfl_frontier_le(__dtfl_batch_ord_upper(b),
		    upper);
		if (include_lower != include_upper &&
		    !__dtfl_antichain_eq(upper, __dtfl_batch_ord_lower(b))) {
			/* `upper` straddles the batch: catastrophic. */
			size_t j;

			for (j = 0; j < storage.len; j++)
				__dtfl_batch_ord_release(*DTFL_VEC_AT(&storage,
				    __dtfl_batch_ord *, j));
			__dtfl_vec_free(&storage);
			*nout = SIZE_MAX;
			return NULL;
		}
		if (include_upper)
			PUSH_RETAIN(b);
	}
#undef PUSH_RETAIN

	*nout = storage.len;
	if (storage.len == 0) {
		out = NULL;
	} else {
		out = __dtfl_alloc(storage.len * sizeof(__dtfl_batch_ord *));
		memcpy(out, storage.data,
		    storage.len * sizeof(__dtfl_batch_ord *));
	}
	__dtfl_vec_free(&storage);
	return out;
}
