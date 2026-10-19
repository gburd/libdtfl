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
