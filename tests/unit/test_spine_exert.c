/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/unit/test_spine_exert.c: Trace::exert and the default exertion
 * logic (spine_fueled.rs exert / exert_effort, lib.rs configure).
 *
 * A spine fed batches of 64, 16, 4 and 1 updates holds four Single layers
 * (levels 6, 4, 2, 0): each insertion lands above nothing that needs rolling
 * up, and tidy_layers cannot draw the top layer down.  Without exert logic,
 * exert only tidies, so the four batches stay.  With the default logic,
 * repeated exert calls introduce virtual updates that roll the layers up
 * until one batch remains and the logic answers None.  The trace's
 * contents must not change on the way.
 */
#include "dtfl_test.h"
#include "dd/trace/trace_int.h"

#define I64 __dtfl_diff_builtin(DTFL_DIFF_I64)

static const size_t sizes[] = { 64, 16, 4, 1 };
#define NBATCH (sizeof sizes / sizeof sizes[0])

/* Batch e holds `n` distinct (key, key) updates at time e, over [e, e+1). */
static __dtfl_batch_ord *
mk_batch(dtfl_time_class *tc, const dtfl_type *rt, uint64_t e, size_t n,
    uint64_t key0)
{
	__dtfl_merge_batcher mb;
	__dtfl_chunker ch;
	__dtfl_batch *chunk;
	__dtfl_antichain lower, upper;
	__dtfl_description desc;
	__dtfl_batch **out;
	size_t nout, i;
	__dtfl_batch_ord *batch;
	uint64_t up = e + 1, min[1];
	int64_t one = 1;

	__dtfl_chunker_init(&ch, rt, 1, I64);
	for (i = 0; i < n; i++) {
		unsigned char rec[16];

		dtfl_put_u64(rec, key0 + i);
		dtfl_put_u64(rec + 8, key0 + i);
		__dtfl_chunker_push(&ch, rec, 16, &e, &one);
	}
	__dtfl_merge_batcher_init(&mb, rt, tc, I64);
	while ((chunk = __dtfl_chunker_finish(&ch)) != NULL)
		__dtfl_merge_batcher_push(&mb, chunk);
	__dtfl_chunker_free(&ch);
	__dtfl_antichain_init_elem(&upper, tc, &up);
	__dtfl_merge_batcher_seal(&mb, &upper, &out, &nout, &desc);
	__dtfl_description_free(&desc);
	/* The batcher's lower starts at the minimum; tile [e, e+1) instead. */
	{
		__dtfl_antichain since;

		__dtfl_antichain_init_elem(&lower, tc, &e);
		dtfl_time_minimum(tc, min);
		__dtfl_antichain_init_elem(&since, tc, min);
		__dtfl_description_init(&desc, &lower, &upper, &since);
	}
	batch = __dtfl_builder_ord_seal(false, dtfl_type_u64(),
	    dtfl_type_u64(), tc, I64, out, nout, &desc);
	for (i = 0; i < nout; i++)
		__dtfl_batch_release(out[i]);
	__dtfl_free(out);
	__dtfl_merge_batcher_free(&mb);
	return batch;
}

/* Total diff over every (key, val, time) in the trace. */
static void
sum_diff(const uint64_t *time, const void *diff, void *vp)
{
	int64_t r;

	(void)time;
	memcpy(&r, diff, 8);
	*(int64_t *)vp += r;
}

static int64_t
trace_total(__dtfl_trace *tr)
{
	__dtfl_cursor_list cl;
	int64_t total = 0;

	__dtfl_trace_cursor(tr, &cl);
	while (__dtfl_cursor_list_key_valid(&cl)) {
		while (__dtfl_cursor_list_val_valid(&cl)) {
			__dtfl_cursor_list_map_times(&cl, sum_diff, &total);
			__dtfl_cursor_list_step_val(&cl);
		}
		__dtfl_cursor_list_step_key(&cl);
	}
	__dtfl_cursor_list_free(&cl);
	return total;
}

/* A trace holding the four Single layers, all past physical compaction. */
static __dtfl_trace *
four_layer_trace(dtfl_time_class *tc, const dtfl_type *rt)
{
	__dtfl_trace *tr = __dtfl_trace_new(false, dtfl_type_u64(),
	    dtfl_type_u64(), tc, I64);
	__dtfl_antichain phys;
	uint64_t far = 100, key = 0;
	size_t e;

	__dtfl_antichain_init_elem(&phys, tc, &far);
	__dtfl_trace_set_physical_compaction(tr, &phys);
	__dtfl_antichain_free(&phys);
	for (e = 0; e < NBATCH; e++) {
		__dtfl_trace_insert(tr, mk_batch(tc, rt, e, sizes[e], key));
		key += sizes[e];
	}
	return tr;
}

DTFL_TEST(spine__exert_without_logic_only_tidies)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_trace *tr = four_layer_trace(tc, rt);
	int i;

	(void)params; (void)data;
	munit_assert_size(__dtfl_trace_batch_count(tr), ==, NBATCH);
	for (i = 0; i < 64; i++)
		__dtfl_trace_exert(tr);
	munit_assert_size(__dtfl_trace_batch_count(tr), ==, NBATCH);
	munit_assert_int64(trace_total(tr), ==, 85);
	__dtfl_trace_free(tr);
	dtfl_type_free((dtfl_type *)rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

DTFL_TEST(spine__exert_with_logic_reduces_to_one_batch)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_trace *tr = four_layer_trace(tc, rt);
	__dtfl_exert_logic probe = __dtfl_default_exert_logic(16);
	size_t effort = 0;
	int i;

	(void)params; (void)data;
	__dtfl_trace_set_exert_logic(tr, __dtfl_default_exert_logic(16));
	for (i = 0; i < 64 && __dtfl_trace_batch_count(tr) > 1; i++) {
		size_t before = __dtfl_trace_batch_count(tr);

		__dtfl_trace_exert(tr);
		/* exert never adds batches; the virtual ones carry no data. */
		munit_assert_size(__dtfl_trace_batch_count(tr), <=, before);
	}
	munit_logf(MUNIT_LOG_INFO, "one batch after %d exert calls", i);
	munit_assert_size(__dtfl_trace_batch_count(tr), ==, 1);
	munit_assert_int64(trace_total(tr), ==, 85);

	/* Reduced form: the logic now answers None. */
	{
		__dtfl_spine *s = (__dtfl_spine *)(void *)tr;
		__dtfl_exert_level lv[64];
		size_t n = 0, k;

		/* The trace's first member is its spine (trace.c). */
		for (k = s->merging.len; k-- > 0 && n < 64;) {
			const __dtfl_merge_state *m = DTFL_VEC_AT(&s->merging,
			    __dtfl_merge_state, k);

			lv[n].index = k;
			lv[n].count = m->kind == DTFL_MERGE_VACANT ? 0 :
			    m->kind == DTFL_MERGE_SINGLE ? 1 : 2;
			lv[n].length = m->kind == DTFL_MERGE_INPROGRESS ?
			    __dtfl_batch_ord_len(m->in1) +
			    __dtfl_batch_ord_len(m->in2) :
			    (m->kind != DTFL_MERGE_VACANT && m->present ?
			    __dtfl_batch_ord_len(m->batch) : 0);
			n++;
		}
		munit_assert_false(probe.fn(probe.ctx, lv, n, &effort));
	}
	probe.ctx_free(probe.ctx);
	__dtfl_trace_free(tr);
	dtfl_type_free((dtfl_type *)rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/* lib.rs configure: Some(effort) iff a level has count > 1, or two non-empty levels. */
DTFL_TEST(lib__default_exert_logic)
{
	__dtfl_exert_logic l = __dtfl_default_exert_logic(7);
	size_t effort;
	const __dtfl_exert_level empty[] = { { 2, 0, 0 }, { 1, 0, 0 },
	    { 0, 0, 0 } };
	const __dtfl_exert_level one[] = { { 2, 1, 5 }, { 1, 0, 0 },
	    { 0, 1, 0 } };
	const __dtfl_exert_level two[] = { { 2, 1, 5 }, { 1, 0, 0 },
	    { 0, 1, 1 } };
	const __dtfl_exert_level dbl[] = { { 1, 0, 0 }, { 0, 2, 0 } };

	(void)params; (void)data;
	munit_assert_false(l.fn(l.ctx, NULL, 0, &effort));
	munit_assert_false(l.fn(l.ctx, empty, 3, &effort));
	/* One non-empty level and a structurally empty single: reduced. */
	munit_assert_false(l.fn(l.ctx, one, 3, &effort));
	effort = 0;
	munit_assert_true(l.fn(l.ctx, two, 3, &effort));
	munit_assert_size(effort, ==, 7);
	/* A merge in progress asks for effort even with no updates. */
	effort = 0;
	munit_assert_true(l.fn(l.ctx, dbl, 2, &effort));
	munit_assert_size(effort, ==, 7);
	l.ctx_free(l.ctx);
	return MUNIT_OK;
}

/*
 * consider_merges fuses an empty batch into the lowest non-vacant layer
 * when that layer is an empty Single (spine_fueled.rs 410-421), instead
 * of introducing it with its fuel and roll-up.  A run of empty seals
 * after one empty batch therefore leaves a single empty layer, whose
 * bounds cover every seal, and no merge in progress.
 */
DTFL_TEST(spine__empty_batches_fuse)
{
	dtfl_time_class *tc = dtfl_time_u64();
	__dtfl_trace *tr = __dtfl_trace_new(false, dtfl_type_u64(),
	    dtfl_type_u64(), tc, I64);
	__dtfl_antichain phys;
	uint64_t far = 1000, e;
	size_t nonvacant = 0, i;
	const __dtfl_spine *sp;

	(void)params; (void)data;
	__dtfl_antichain_init_elem(&phys, tc, &far);
	__dtfl_trace_set_physical_compaction(tr, &phys);
	__dtfl_antichain_free(&phys);
	for (e = 0; e < 40; e++) {
		__dtfl_antichain lower, upper;
		uint64_t up = e + 1;

		__dtfl_antichain_init_elem(&lower, tc, &e);
		__dtfl_antichain_init_elem(&upper, tc, &up);
		__dtfl_trace_insert(tr, __dtfl_batch_ord_empty(false,
		    dtfl_type_u64(), dtfl_type_u64(), tc, I64, &lower, &upper));
	}
	sp = __dtfl_trace_spine(tr);
	for (i = 0; i < sp->merging.len; i++) {
		const __dtfl_merge_state *m = DTFL_VEC_AT(&sp->merging,
		    __dtfl_merge_state, i);

		assert_int(m->kind, !=, DTFL_MERGE_INPROGRESS);
		if (m->kind != DTFL_MERGE_VACANT)
			nonvacant++;
	}
	assert_size(nonvacant, ==, 1);
	assert_size(sp->merging.len, ==, 1);
	{
		const __dtfl_merge_state *m = DTFL_VEC_AT(&sp->merging,
		    __dtfl_merge_state, 0);

		assert_int(m->kind, ==, DTFL_MERGE_SINGLE);
		assert_true(m->present);
		assert_uint64(__dtfl_ac_base(__dtfl_batch_ord_lower(m->batch))[0],
		    ==, 0);
		assert_uint64(__dtfl_ac_base(__dtfl_batch_ord_upper(m->batch))[0],
		    ==, 40);
	}
	__dtfl_trace_free(tr);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

/*
 * __dtfl_trace_counts_get counts both inputs of a merge in progress, its
 * updates and their bytes.  Two batches of 300 updates land on level 9,
 * so the second begins a merge there, and its insertion fuel goes to the
 * merges below before this one exists: the merge is left in progress,
 * which the test asserts before checking the counts.
 */
DTFL_TEST(trace__counts_merge_inputs)
{
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *rt = dtfl_type_pair(dtfl_type_u64(), dtfl_type_u64());
	__dtfl_trace *tr = __dtfl_trace_new(false, dtfl_type_u64(),
	    dtfl_type_u64(), tc, I64);
	__dtfl_antichain phys;
	__dtfl_trace_counts c;
	const __dtfl_spine *sp;
	uint64_t far = 100;
	size_t i, inprogress = 0, upds = 0;

	(void)params; (void)data;
	__dtfl_antichain_init_elem(&phys, tc, &far);
	__dtfl_trace_set_physical_compaction(tr, &phys);
	__dtfl_antichain_free(&phys);
	__dtfl_trace_insert(tr, mk_batch(tc, rt, 0, 300, 0));
	__dtfl_trace_insert(tr, mk_batch(tc, rt, 1, 300, 300));
	sp = __dtfl_trace_spine(tr);
	for (i = 0; i < sp->merging.len; i++) {
		const __dtfl_merge_state *m = DTFL_VEC_AT(&sp->merging,
		    __dtfl_merge_state, i);

		if (m->kind == DTFL_MERGE_INPROGRESS) {
			inprogress++;
			upds += __dtfl_batch_ord_len(m->in1) +
			    __dtfl_batch_ord_len(m->in2);
		} else if (m->kind != DTFL_MERGE_VACANT && m->present) {
			upds += __dtfl_batch_ord_len(m->batch);
		}
	}
	/* The state this test needs: one merge left in progress. */
	munit_assert_size(inprogress, ==, 1);
	__dtfl_trace_counts_get(tr, &c);
	munit_assert_size(c.merges, ==, 1);
	munit_assert_size(c.batches, ==, __dtfl_trace_batch_count(tr));
	munit_assert_size(c.updates, ==, upds);
	munit_assert_size(c.updates, ==, 600);
	munit_assert_size(c.bytes, >=, 600 * 16);
	__dtfl_trace_free(tr);
	dtfl_type_free((dtfl_type *)rt);
	dtfl_time_class_free(tc);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(trace__counts_merge_inputs),
	DTFL_T(spine__empty_batches_fuse),
	DTFL_T(spine__exert_without_logic_only_tidies),
	DTFL_T(spine__exert_with_logic_reduces_to_one_batch),
	DTFL_T(lib__default_exert_logic),
	DTFL_T_END
};

DTFL_TEST_MAIN("spine_exert", tests)
