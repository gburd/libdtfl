/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_smoke.c: proves the hegel harness runs properties and
 * reports failures.  The vector property checks __dtfl_vec against a
 * plain array model.
 */
#include "pbt.h"
#include "base/base_int.h"

static void
prop_vec_model(pbt_tc *tc)
{
	__dtfl_vec v;
	int64_t model[256];
	size_t n = 0, i;
	int64_t steps = pbt_int(tc, 0, 200);

	__dtfl_vec_init(&v, sizeof(int64_t));
	while (steps-- > 0) {
		int64_t op = pbt_int(tc, 0, 2);

		if (op == 0 && n < 256) {
			int64_t x = pbt_int(tc, INT64_MIN, INT64_MAX);
			__dtfl_vec_push(&v, &x);
			model[n++] = x;
		} else if (op == 1 && n > 0) {
			size_t at = (size_t)pbt_int(tc, 0, (int64_t)n - 1);
			__dtfl_vec_remove(&v, at);
			memmove(&model[at], &model[at + 1], (n - at - 1) * sizeof model[0]);
			n--;
		} else if (op == 2 && n > 0) {
			size_t at = (size_t)pbt_int(tc, 0, (int64_t)n - 1);
			__dtfl_vec_swap_remove(&v, at);
			model[at] = model[n - 1];
			n--;
		}
		PBT_ASSERT(v.len == n);
	}
	for (i = 0; i < n; i++)
		PBT_ASSERT(*DTFL_VEC_AT(&v, int64_t, i) == model[i]);
	__dtfl_vec_free(&v);
}

static const pbt_prop props[] = {
	{ "vec_model", prop_vec_model, 300 },
};

PBT_MAIN("smoke", props)
