/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/ops_broadcast.c: broadcast a stream to every worker.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/dataflow/operators/vec/broadcast.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * broadcast tags each record with every peer index, exchanges by that
 * index so a copy lands on each worker, then strips the index.  This is
 * upstream's simplified broadcast, built from flat_map, exchange and map
 * (broadcast.rs); the intermediate record is a pair (peer index, value)
 * whose first field routes the exchange.  Public at the pin; the vec and
 * core variants collapse into one here (PLAN D2).
 */
#include "timely/ops_int.h"

#include "dtfl/record.h"

#include <string.h>

/* The tag/strip closures carry the peer count and the value's type. */
typedef struct bcast_ctx {
	size_t peers;
	const dtfl_type *val_type;	/* retained; the original record type */
} bcast_ctx;

/* flat_map: emit (i, value) for every peer index i, as an 8-byte-keyed pair. */
static void
bcast_tag(void *vctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	bcast_ctx *c = vctx;
	uint64_t i;

	(void)time;
	for (i = 0; i < c->peers; i++) {
		unsigned char *slot = dtfl_emit_reserve(out, 8 + len);

		dtfl_put_u64(slot, i);
		if (len > 0)
			memcpy(slot + 8, rec, len);
	}
}

/* Routes the (index, value) pair by its leading u64 index. */
static uint64_t
bcast_route(const void *rec, size_t len, void *vctx)
{
	(void)len; (void)vctx;
	return dtfl_get_u64(rec);
}

/* map: strip the leading index, leaving the original value. */
static void
bcast_strip(void *vctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	(void)vctx; (void)time;
	dtfl_emit_record(out, (const unsigned char *)rec + 8, len - 8);
}

static void
bcast_free(void *vctx)
{
	bcast_ctx *c = vctx;

	dtfl_type_free(c->val_type);
	__dtfl_free(c);
}

dtfl_stream *
dtfl_stream_broadcast(dtfl_stream *st)
{
	dtfl_scope *scope = dtfl_stream_scope(st);
	const dtfl_type *vt = dtfl_stream_type(st);
	/* The intermediate (index, value) pair; u64 index routes the exchange. */
	const dtfl_type *pair = dtfl_type_pair(dtfl_type_u64(), vt);
	bcast_ctx *tag = __dtfl_calloc(1, sizeof *tag);
	bcast_ctx *strip = __dtfl_calloc(1, sizeof *strip);
	dtfl_stream *s;

	tag->peers = dtfl_scope_peers(scope);
	tag->val_type = dtfl_type_retain(vt);
	strip->peers = tag->peers;
	strip->val_type = dtfl_type_retain(vt);

	s = dtfl_stream_flat_map(st, pair, bcast_tag, DTFL_CTX(tag, bcast_free));
	s = dtfl_stream_exchange(s, bcast_route, NULL);
	s = dtfl_stream_flat_map(s, vt, bcast_strip, DTFL_CTX(strip, bcast_free));
	dtfl_type_free(pair);
	return s;
}
