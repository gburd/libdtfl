/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/operators/int_proxy/mod.rs
 *   differential-dataflow/src/operators/int_proxy/history.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/int_proxy.c: the integer-proxy exchange medium.  A ProxyBridge is a
 * __dtfl_vec of __dtfl_bridge_upd rows, a consolidated collection of
 * [((hash, id), time, diff)] (see int_proxy_int.h for the two integers).
 * history.rs's IdHistory, a ValueHistory<u64, T, R>, is
 * __dtfl_value_history loaded with __dtfl_value_history_load_iter
 * (common.c); the reduce and join tactics are in int_proxy_reduce.c and
 * int_proxy_join.c.
 */
#include "dd/int_proxy_int.h"
#include "dd/trace/trace_int.h"
#include "dtfl/record.h"

/*
 * Debug check that a presented bridge is consolidated.  Operator harnesses
 * use the test to flag backend implementations that do not uphold it.
 */
void
__dtfl_bridge_assert_sorted(const __dtfl_vec *rows, unsigned tw,
    const char *who)
{
#ifdef DTFL_DEBUG
	const __dtfl_bridge_upd *r = (const __dtfl_bridge_upd *)(const void *)
	    rows->data;
	size_t i;

	for (i = 1; i < rows->len; i++)
		if (__dtfl_bridge_cmp(&r[i - 1], &r[i], tw) >= 0)
			__dtfl_panic("%s: a presented bridge must be sorted & "
			    "consolidated by ((key_hash, value_id), time)", who);
#else
	(void)rows; (void)tw; (void)who;
#endif
}

/* ------------------------------------------------------------------ */
/* Presenting u64 batches as bridge rows (the u64 backends).           */
/* ------------------------------------------------------------------ */

typedef struct push_ctx {
	__dtfl_vec *rows;
	uint64_t hash, id;
	const dtfl_time_class *tc;
	const uint64_t *advance_by;
	unsigned tw;
	size_t dw;
} push_ctx;

static void
push_time(const uint64_t *time, const void *diff, void *vctx)
{
	push_ctx *c = vctx;
	__dtfl_bridge_upd r;

	memset(&r, 0, sizeof r);
	r.hash = c->hash;
	r.id = c->id;
	if (c->tw != 0) {
		memcpy(r.time, time, (size_t)c->tw * sizeof(uint64_t));
		if (c->advance_by != NULL)
			dtfl_time_join(c->tc, r.time, c->advance_by, r.time);
	}
	if (c->dw != 0)
		memcpy(r.diff, diff, c->dw);
	__dtfl_vec_push(c->rows, &r);
}

void
__dtfl_proxy_present_u64(__dtfl_batch_ord *const *batches, size_t n,
    const __dtfl_diff_class *dc, unsigned tw, const uint64_t *keys,
    size_t nkeys, const uint64_t *advance_by, __dtfl_vec *rows)
{
	size_t i;

	for (i = 0; i < n; i++) {
		const __dtfl_batch_ord *bt = batches[i];
		__dtfl_cursor cu;
		size_t ki = 0;

		__dtfl_cursor_init(&cu, bt);
		while (__dtfl_cursor_key_valid(&cu, bt)) {
			size_t kl, vl;
			const void *k = __dtfl_cursor_key(&cu, bt, &kl);
			push_ctx pc;

			pc.rows = rows;
			pc.hash = dtfl_get_u64(k);
			pc.tc = bt->tc;
			pc.advance_by = advance_by;
			pc.tw = tw;
			pc.dw = dc->width;
			if (keys != NULL) {
				while (ki < nkeys && keys[ki] < pc.hash)
					ki++;
				if (ki == nkeys)
					break;
				if (keys[ki] != pc.hash) {
					unsigned char kb[8];

					dtfl_put_u64(kb, keys[ki]);
					__dtfl_cursor_seek_key(&cu, bt, kb,
					    sizeof kb);
					continue;
				}
			}
			while (__dtfl_cursor_val_valid(&cu, bt)) {
				pc.id = dtfl_get_u64(__dtfl_cursor_val(&cu, bt,
				    &vl));
				__dtfl_cursor_map_times(&cu, bt, push_time, &pc);
				__dtfl_cursor_step_val(&cu, bt);
			}
			__dtfl_cursor_step_key(&cu, bt);
		}
	}
}
