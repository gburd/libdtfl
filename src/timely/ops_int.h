/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_int.h: shared internals for the record operators.
 *
 * The record operators (map, filter, inspect, concat, exchange, ok_err,
 * partition, to_stream, count) are thin builder_rc operators.  A
 * dtfl_emit is the sink a producing callback writes into: it wraps an
 * open output session (handles.c) so a callback can give records at the
 * session's time.  Everything here belongs to one worker thread.
 */
#ifndef DTFL_OPS_INT_H
#define DTFL_OPS_INT_H

#include "timely/timely_int.h"
#include "dtfl/timely_ops.h"

/* A sink wrapping an open session; dtfl_emit_* write through it. */
struct dtfl_emit {
	dtfl_session *s;	/* not owned */
};

#endif /* DTFL_OPS_INT_H */
