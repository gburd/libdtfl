/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/stream.c: a handle to an operator output.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/stream.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A stream names its source operator output (a Source location), the Tee
 * its producer pushes into, the record type, and the containing scope.
 * connect_to records the dataflow edge for progress tracking and adds a
 * pusher to the Tee so the producer reaches a new consumer.  Streams are
 * owned by the scope (builder.c pools them), so a build callback need
 * not free them (PLAN D6).
 */
#include "timely/scope_int.h"
#include "timely/timely_int.h"

dtfl_stream *
__dtfl_stream_new(dtfl_scope *s, __dtfl_location source, __dtfl_tee *ports,
    const dtfl_type *type)
{
	dtfl_stream *st = __dtfl_alloc(sizeof *st);

	st->source = source;
	st->ports = __dtfl_tee_retain(ports);
	st->scope = s;
	st->type = dtfl_type_retain(type);
	return st;
}

void
__dtfl_stream_free(dtfl_stream *st)
{
	if (st == NULL)
		return;
	__dtfl_tee_release(st->ports);
	dtfl_type_free(st->type);
	__dtfl_free(st);
}

dtfl_scope *
dtfl_stream_scope(const dtfl_stream *st)
{
	return st->scope;
}

const dtfl_type *
dtfl_stream_type(const dtfl_stream *st)
{
	return st->type;
}

void
__dtfl_stream_connect_to(dtfl_stream *st, __dtfl_location target,
    __dtfl_push *pusher, size_t identifier)
{
	dtfl_scope *s = st->scope;
	size_t slen;
	const size_t *addr = dtfl_scope_addr(s, &slen);

	__dtfl_worker_log_channel(s->worker, identifier, addr, slen,
	    st->source.node, st->source.port, target.node, target.port,
	    st->type->name);
	__dtfl_scope_add_edge(s, st->source, target);
	__dtfl_tee_add_pusher(st->ports, pusher);
}
