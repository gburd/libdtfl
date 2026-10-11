/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/description.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/description.c: the lower, upper and since frontiers of a
 * trace batch.  A batch holds updates at times in advance of `lower`
 * and not in advance of `upper`; times are compacted only to `since`,
 * so they are correct when accumulated at any time in advance of it.
 */
#include "dd/dd_int.h"

/*
 * Description::new: moves the three antichains into d; the caller's
 * structs are left empty and need no free.  Upstream asserts lower is
 * non-empty ("this should always be true").
 */
void
__dtfl_description_init(__dtfl_description *d, __dtfl_antichain *lower,
    __dtfl_antichain *upper, __dtfl_antichain *since)
{
	DTFL_ASSERT(__dtfl_antichain_len(lower) != 0);
	d->lower = *lower;
	d->upper = *upper;
	d->since = *since;
	memset(lower, 0, sizeof *lower);
	memset(upper, 0, sizeof *upper);
	memset(since, 0, sizeof *since);
}

void
__dtfl_description_free(__dtfl_description *d)
{
	__dtfl_antichain_free(&d->lower);
	__dtfl_antichain_free(&d->upper);
	__dtfl_antichain_free(&d->since);
}

const __dtfl_antichain *
__dtfl_description_lower(const __dtfl_description *d)
{
	return &d->lower;
}

const __dtfl_antichain *
__dtfl_description_upper(const __dtfl_description *d)
{
	return &d->upper;
}

const __dtfl_antichain *
__dtfl_description_since(const __dtfl_description *d)
{
	return &d->since;
}

/* PartialEq: all three frontiers equal as sets. */
bool
__dtfl_description_eq(const __dtfl_description *a, const __dtfl_description *b)
{
	return __dtfl_antichain_eq(&a->lower, &b->lower) &&
	    __dtfl_antichain_eq(&a->upper, &b->upper) &&
	    __dtfl_antichain_eq(&a->since, &b->since);
}
