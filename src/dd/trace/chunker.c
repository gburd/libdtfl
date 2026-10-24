/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/trace/implementations/chunker.rs
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/chunker.c: the container chunker feeding the merge batcher.
 * Upstream's ContainerChunker consolidates a stream of input updates into
 * sorted, consolidated chunks of about the default capacity and hands them
 * on.  libdtfl's ConsolidatingContainerBuilder (consolidate.c) already does
 * exactly this, so the chunker is a thin wrapper that presents the batcher's
 * expected interface over it.
 */
#include "dd/trace/trace_int.h"

void
__dtfl_chunker_init(__dtfl_chunker *ch, const dtfl_type *rt, unsigned tw,
    const __dtfl_diff_class *dc)
{
	__dtfl_consolidator_init(&ch->con, rt, tw, dc);
}

void
__dtfl_chunker_free(__dtfl_chunker *ch)
{
	__dtfl_consolidator_free(&ch->con);
}

void
__dtfl_chunker_push(__dtfl_chunker *ch, const void *rec, size_t len,
    const uint64_t *time, const void *diff)
{
	__dtfl_consolidator_push(&ch->con, rec, len, time, diff);
}

__dtfl_batch *
__dtfl_chunker_extract(__dtfl_chunker *ch)
{
	return __dtfl_consolidator_extract(&ch->con);
}

__dtfl_batch *
__dtfl_chunker_finish(__dtfl_chunker *ch)
{
	return __dtfl_consolidator_finish(&ch->con);
}

void
__dtfl_chunker_recycle(__dtfl_chunker *ch, __dtfl_batch *b)
{
	__dtfl_consolidator_recycle(&ch->con, b);
}
