/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dogs3-v0.25.1
 *   dogsdogsdogs/src/altneu.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dogs/altneu.c: the dogsdogsdogs-side AltNeu helpers.
 *
 * The AltNeu order, lattice and Refines are the ALTNEU time class in
 * src/progress/time.c (a time is one u64 word encoded 2t + neu, so alt(t)
 * is 2t and neu(t) is 2t + 1).  This file holds only the collection-level
 * helper the delta queries use: delay_neu, which moves every update of a
 * collection in an AltNeu scope from its alt time to its neu time, keeping
 * the base time (upstream `.delay(|t| AltNeu::neu(t.time))`).
 */
#include "dogs/dogs_int.h"
#include "dtfl/collection.h"
#include "dtfl/dogs.h"

/*
 * AltNeu::neu(t.time): the base time of the encoded word is w / 2, so the
 * neu encoding is 2 * (w / 2) + 1, i.e. the word with its low bit set.
 * delay must be monotone, and neu(t) >= alt(t) for the same base, which
 * holds because the low bit only ever rises here.
 */
static void
delay_to_neu(void *ctx, const uint64_t *time, uint64_t *new_time)
{
	(void)ctx;
	new_time[0] = (time[0] / 2) * 2 + 1;
}

dtfl_collection *
dtfl_collection_delay_neu(dtfl_collection *c)
{
	return dtfl_collection_delay(c, delay_to_neu, DTFL_NOCTX);
}
