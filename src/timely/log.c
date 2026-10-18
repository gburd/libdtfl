/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/timely/log.c: the public structured-logging subscriber API and
 * the event decoder (dtfl/log.h).
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/logging.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * This is the C realization of enabling a log stream with upstream's
 * worker.log_register().insert_logger: where upstream hands the logger
 * a BatchLogger whose action serializes an Event stream, libdtfl hands
 * it an action that decodes each record and delivers a typed
 * dtfl_log_batch to the caller's subscriber.
 *
 * The internal registry and the timely/differential/communication event
 * encoders already produce the bincode-1 fixint little-endian records
 * that go on the wire (PLAN D9).  This file closes the loop for an
 * in-process consumer: it installs an internal __dtfl_log_action on the
 * worker's logger that decodes each record back into a typed
 * dtfl_log_batch and hands it to the caller's dtfl_log_subscriber_fn.
 * The decoder is the inverse of the writers in timely_log.c, dd_log.c
 * and comm_log.c; it reads the same field order so a round trip (encode
 * then decode) reproduces every field.
 *
 * A subscriber belongs to the worker thread that logs to it, like every
 * other worker-owned handle (PLAN section 5).  The decoded batch and
 * everything it points at live only for the duration of one subscriber
 * call; the decoder reuses a per-logger scratch buffer across calls.
 */
#include "timely/scope_int.h"
#include "timely/log_codec_int.h"
#include "comm/init_int.h"

/* The decoded form of one event; a union keyed by kind. */
typedef struct decoded_event {
	uint64_t time_ns;
	dtfl_log_event_kind kind;
	/* Shared integer fields; which are valid depends on kind. */
	size_t a, b, c, d, e, f;
	int64_t i0, i1, i2, i3;
	bool flag0, flag1;
	bool opt;		/* Option tag (Park timeout, Merge complete, Setup remote) */
	uint64_t dur_ns;
	/* Addresses and strings point into `arena`. */
	size_t addr_off;	/* offset into batch->addrs */
	size_t addr_len;
	size_t scope_off;
	size_t scope_len;
	const char *str;	/* into batch->strpool, NUL-terminated */
} decoded_event;

struct dtfl_log_batch {
	const char *stream;
	size_t worker;
	uint64_t time_ns;
	decoded_event *events;
	size_t n;
	__dtfl_vec addrs;	/* size_t pool for Operates addr / Channels scope */
	__dtfl_vec strpool;	/* char pool for names, typ, text */
	__dtfl_vec evbuf;	/* decoded_event backing store */
};

/* Context the internal action carries for one subscribed stream. */
typedef struct sub_ctx {
	/*
	 * The worker index, captured at install: a trace's logger reference
	 * can outlive the worker (a dtfl_trace handle freed after
	 * dtfl_worker_free still logs its DropEvents), so no worker pointer.
	 */
	size_t worker;
	char *stream;		/* owned */
	dtfl_log_subscriber_fn fn;
	void *user_ctx;
	void (*user_free)(void *);
	dtfl_log_batch batch;	/* reused scratch */
} sub_ctx;

/* ------------------------------------------------------------------ */
/* Decoding                                                            */
/* ------------------------------------------------------------------ */

/* Copies a borrowed slice into the string pool and returns an offset. */
static size_t
intern_str(dtfl_log_batch *b, const char *s, size_t len)
{
	size_t off = b->strpool.len;
	char nul = '\0';

	if (len > 0)
		__dtfl_vec_push_n(&b->strpool, s, len);
	__dtfl_vec_push(&b->strpool, &nul);
	return off;
}

/* Reads a Vec<usize> into the addr pool; returns offset, stores length. */
static size_t
read_usizes(dtfl_log_batch *b, __dtfl_bc_reader *r, size_t *out_len)
{
	size_t off = b->addrs.len;
	size_t n = __dtfl_bc_read_vec_len(r);
	size_t i;

	for (i = 0; i < n && !r->bad; i++) {
		size_t v = (size_t)__dtfl_bc_read_u64(r);

		__dtfl_vec_push(&b->addrs, &v);
	}
	*out_len = r->bad ? 0 : n;
	return off;
}

/*
 * Decodes one record into `ev`, resolving its enum tag against the
 * stream it came from (the three event enums share a tag space only
 * within a stream).  String and address fields are stashed as offsets
 * into the batch pools and resolved to pointers after the whole batch
 * is decoded (the pools may move while growing).
 */
static void
decode_timely(dtfl_log_batch *b, __dtfl_bc_reader *r, decoded_event *ev)
{
	uint32_t tag = __dtfl_bc_read_u32(r);
	size_t slen;
	const char *s;

	switch (tag) {
	case 0:	/* Operates { id, addr, name } */
		ev->kind = DTFL_LOG_OPERATES;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->addr_off = read_usizes(b, r, &ev->addr_len);
		s = __dtfl_bc_read_str(r, &slen);
		ev->str = (const char *)(uintptr_t)intern_str(b, s, slen);
		break;
	case 1:	/* Channels { id, scope, (src_n, src_p), (tgt_n, tgt_p), typ } */
		ev->kind = DTFL_LOG_CHANNELS;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->scope_off = read_usizes(b, r, &ev->scope_len);
		ev->b = (size_t)__dtfl_bc_read_u64(r);
		ev->c = (size_t)__dtfl_bc_read_u64(r);
		ev->d = (size_t)__dtfl_bc_read_u64(r);
		ev->e = (size_t)__dtfl_bc_read_u64(r);
		s = __dtfl_bc_read_str(r, &slen);
		ev->str = (const char *)(uintptr_t)intern_str(b, s, slen);
		break;
	case 2:	/* PushProgress { op_id } */
		ev->kind = DTFL_LOG_PUSH_PROGRESS;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		break;
	case 3:	/* Messages { is_send, channel, source, target, seq, count } */
		ev->kind = DTFL_LOG_MESSAGES;
		ev->flag0 = __dtfl_bc_read_u8(r) != 0;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->b = (size_t)__dtfl_bc_read_u64(r);
		ev->c = (size_t)__dtfl_bc_read_u64(r);
		ev->d = (size_t)__dtfl_bc_read_u64(r);
		ev->i0 = __dtfl_bc_read_i64(r);
		break;
	case 4:	/* Schedule { id, start_stop } (StartStop: Start=0, Stop=1) */
		ev->kind = DTFL_LOG_SCHEDULE;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->flag0 = __dtfl_bc_read_u32(r) == 0;
		break;
	case 5:	/* Shutdown { id } */
		ev->kind = DTFL_LOG_SHUTDOWN;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		break;
	case 6:	/* CommChannels { id, kind } (CommChannelKind: Progress=0, Data=1) */
		ev->kind = DTFL_LOG_COMM_CHANNELS;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->flag0 = __dtfl_bc_read_u32(r) == 0;
		break;
	case 7:	/* Park(ParkEvent): Park=0 (Option<Duration>) or Unpark=1 */
		ev->kind = DTFL_LOG_PARK;
		if (__dtfl_bc_read_u32(r) == 0) {
			ev->flag0 = true;	/* is a park */
			ev->opt = __dtfl_bc_read_u8(r) != 0;
			if (ev->opt)
				ev->dur_ns = __dtfl_bc_read_duration(r);
		} else {
			ev->flag0 = false;	/* unpark */
		}
		break;
	case 8:	/* Text(String) */
		ev->kind = DTFL_LOG_TEXT;
		s = __dtfl_bc_read_str(r, &slen);
		ev->str = (const char *)(uintptr_t)intern_str(b, s, slen);
		break;
	default:
		ev->kind = DTFL_LOG_UNKNOWN;
		break;
	}
}

static void
decode_differential(dtfl_log_batch *b, __dtfl_bc_reader *r, decoded_event *ev)
{
	uint32_t tag = __dtfl_bc_read_u32(r);

	(void)b;
	switch (tag) {
	case 0:	/* Batch { operator, length } */
		ev->kind = DTFL_LOG_DD_BATCH;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->b = (size_t)__dtfl_bc_read_u64(r);
		break;
	case 1:	/* Merge { operator, scale, length1, length2, complete } */
		ev->kind = DTFL_LOG_DD_MERGE;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->b = (size_t)__dtfl_bc_read_u64(r);
		ev->c = (size_t)__dtfl_bc_read_u64(r);
		ev->d = (size_t)__dtfl_bc_read_u64(r);
		ev->opt = __dtfl_bc_read_u8(r) != 0;	/* Option<usize> */
		if (ev->opt)
			ev->e = (size_t)__dtfl_bc_read_u64(r);
		break;
	case 2:	/* Drop { operator, length } */
		ev->kind = DTFL_LOG_DD_DROP;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->b = (size_t)__dtfl_bc_read_u64(r);
		break;
	case 3:	/* MergeShortfall { operator, scale, shortfall } */
		ev->kind = DTFL_LOG_DD_MERGE_SHORTFALL;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->b = (size_t)__dtfl_bc_read_u64(r);
		ev->c = (size_t)__dtfl_bc_read_u64(r);
		break;
	case 4:	/* TraceShare { operator, diff } (isize) */
		ev->kind = DTFL_LOG_DD_TRACE_SHARE;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->i0 = __dtfl_bc_read_i64(r);
		break;
	case 5:	/* Batcher { operator, 4 isize deltas } */
		ev->kind = DTFL_LOG_DD_BATCHER;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->i0 = __dtfl_bc_read_i64(r);
		ev->i1 = __dtfl_bc_read_i64(r);
		ev->i2 = __dtfl_bc_read_i64(r);
		ev->i3 = __dtfl_bc_read_i64(r);
		break;
	default:
		ev->kind = DTFL_LOG_UNKNOWN;
		break;
	}
}

static void
decode_communication(dtfl_log_batch *b, __dtfl_bc_reader *r, decoded_event *ev)
{
	uint32_t tag = __dtfl_bc_read_u32(r);

	(void)b;
	switch (tag) {
	case 0:	/* Message { is_send, header{channel,source,tl,tu,length,seqno} } */
		ev->kind = DTFL_LOG_COMM_MESSAGE;
		ev->flag0 = __dtfl_bc_read_u8(r) != 0;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->b = (size_t)__dtfl_bc_read_u64(r);
		ev->c = (size_t)__dtfl_bc_read_u64(r);
		ev->d = (size_t)__dtfl_bc_read_u64(r);
		ev->e = (size_t)__dtfl_bc_read_u64(r);
		ev->f = (size_t)__dtfl_bc_read_u64(r);
		break;
	case 1:	/* State { send, process, remote, start } */
		ev->kind = DTFL_LOG_COMM_STATE;
		ev->flag0 = __dtfl_bc_read_u8(r) != 0;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->b = (size_t)__dtfl_bc_read_u64(r);
		ev->flag1 = __dtfl_bc_read_u8(r) != 0;
		break;
	case 2:	/* Setup { sender, process, remote: Option<usize> } */
		ev->kind = DTFL_LOG_COMM_SETUP;
		ev->flag0 = __dtfl_bc_read_u8(r) != 0;
		ev->a = (size_t)__dtfl_bc_read_u64(r);
		ev->opt = __dtfl_bc_read_u8(r) != 0;
		if (ev->opt)
			ev->b = (size_t)__dtfl_bc_read_u64(r);
		break;
	default:
		ev->kind = DTFL_LOG_UNKNOWN;
		break;
	}
}

/*
 * Decodes `n` raw records into the batch `b` (whose three pools are
 * reused across calls) and resolves pooled offsets to pointers.  Shared
 * by the in-process subscriber action and the recorded-stream replay.
 */
static void
decode_into(dtfl_log_batch *b, const char *stream, size_t worker,
    uint64_t time_ns, const __dtfl_log_event *events, size_t n)
{
	size_t i;
	bool is_dd, is_comm;

	__dtfl_vec_clear(&b->addrs);
	__dtfl_vec_clear(&b->strpool);
	__dtfl_vec_clear(&b->evbuf);
	b->stream = stream;
	b->worker = worker;
	b->time_ns = time_ns;
	b->n = n;

	is_dd = strcmp(stream, "differential/arrange") == 0;
	is_comm = strcmp(stream, "communication") == 0;

	for (i = 0; i < n; i++) {
		decoded_event ev;
		__dtfl_bc_reader r;

		memset(&ev, 0, sizeof ev);
		ev.time_ns = events[i].time_ns;
		ev.str = (const char *)(uintptr_t)SIZE_MAX;	/* no string */
		__dtfl_bc_reader_init(&r, events[i].data, events[i].len);
		if (is_dd)
			decode_differential(b, &r, &ev);
		else if (is_comm)
			decode_communication(b, &r, &ev);
		else
			decode_timely(b, &r, &ev);
		if (r.bad)
			ev.kind = DTFL_LOG_UNKNOWN;
		__dtfl_vec_push(&b->evbuf, &ev);
	}
	b->events = (decoded_event *)(void *)b->evbuf.data;

	/* Resolve pooled offsets to pointers now the pools are final. */
	for (i = 0; i < n; i++) {
		decoded_event *ev = &b->events[i];

		if (ev->str != (const char *)(uintptr_t)SIZE_MAX)
			ev->str = (const char *)(void *)b->strpool.data +
			    (size_t)(uintptr_t)ev->str;
		else
			ev->str = NULL;
	}
}

/* The internal action: decode a batch of records, call the subscriber. */
static void
sub_action(void *ctx, uint64_t now_ns, const __dtfl_log_event *events, size_t n)
{
	sub_ctx *sc = ctx;
	decode_into(&sc->batch, sc->stream, sc->worker, now_ns, events, n);
	sc->fn(&sc->batch, sc->user_ctx);
}

/* ------------------------------------------------------------------ */
/* Recorded-stream replay entry points (used by src/diag)              */
/* ------------------------------------------------------------------ */

void *
__dtfl_log_scratch_new(void)
{
	dtfl_log_batch *b = __dtfl_calloc(1, sizeof *b);

	__dtfl_vec_init(&b->addrs, sizeof(size_t));
	__dtfl_vec_init(&b->strpool, 1);
	__dtfl_vec_init(&b->evbuf, sizeof(decoded_event));
	return b;
}

void
__dtfl_log_scratch_free(void *scratch)
{
	dtfl_log_batch *b = scratch;

	if (b == NULL)
		return;
	__dtfl_vec_free(&b->addrs);
	__dtfl_vec_free(&b->strpool);
	__dtfl_vec_free(&b->evbuf);
	__dtfl_free(b);
}

void
__dtfl_log_decode_and_deliver(void *scratch, const char *stream, size_t worker,
    uint64_t time_ns, const __dtfl_log_event *recs, size_t n,
    void (*fn)(const void *batch, void *ctx), void *ctx)
{
	dtfl_log_batch *b = scratch;

	decode_into(b, stream, worker, time_ns, recs, n);
	fn(b, ctx);
}

static void
sub_free(void *ctx)
{
	sub_ctx *sc = ctx;

	if (sc->user_free != NULL)
		sc->user_free(sc->user_ctx);
	__dtfl_free(sc->stream);
	__dtfl_vec_free(&sc->batch.addrs);
	__dtfl_vec_free(&sc->batch.strpool);
	__dtfl_vec_free(&sc->batch.evbuf);
	__dtfl_free(sc);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static sub_ctx *
sub_ctx_new(const char *stream, size_t worker, dtfl_log_subscriber_fn fn,
    void *ctx, void (*ctx_free)(void *))
{
	sub_ctx *sc = __dtfl_calloc(1, sizeof *sc);

	sc->worker = worker;
	sc->stream = __dtfl_strdup(stream);
	sc->fn = fn;
	sc->user_ctx = ctx;
	sc->user_free = ctx_free;
	__dtfl_vec_init(&sc->batch.addrs, sizeof(size_t));
	__dtfl_vec_init(&sc->batch.strpool, 1);
	__dtfl_vec_init(&sc->batch.evbuf, sizeof(decoded_event));
	return sc;
}

static int
install(__dtfl_worker *w, const char *stream, dtfl_log_subscriber_fn fn,
    void *ctx, void (*ctx_free)(void *))
{
	if (w->log == NULL)
		return __dtfl_err(DTFL_ESTATE, "worker has no log registry");
	__dtfl_log_registry_insert(w->log, stream, sub_action,
	    sub_ctx_new(stream, w->allocator != NULL ? w->allocator->index : 0,
	    fn, ctx, ctx_free), sub_free);
	return DTFL_OK;
}

/*
 * A standalone logger that decodes `stream` records for subscriber fn,
 * reporting `worker` as the batch's worker index.  ctx is not freed
 * (the configuration owns it).  The caller's thread owns the logger.
 */
__dtfl_logger *
__dtfl_log_subscriber_logger(const char *stream, size_t worker,
    dtfl_log_subscriber_fn fn, void *ctx)
{
	return __dtfl_logger_new(sub_action,
	    sub_ctx_new(stream, worker, fn, ctx, NULL), sub_free);
}

int
dtfl_worker_log_subscribe(dtfl_worker *w, const char *stream,
    dtfl_log_subscriber_fn fn, void *ctx, void (*ctx_free)(void *))
{
	if (w == NULL || stream == NULL || fn == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_worker_log_subscribe: "
		    "NULL argument");
	return install(w->w, stream, fn, ctx, ctx_free);
}

/*
 * Installs every config-level subscription on a worker as it starts.
 * The shared ctx is not freed per worker (ctx_free NULL to the
 * registry); the config frees it once.
 */
void
__dtfl_worker_install_log_subs(__dtfl_worker *w, const dtfl_config *cfg)
{
	const __dtfl_log_sub *subs;
	size_t n = __dtfl_config_log_subs(cfg, &subs);
	size_t i;

	if (w->log == NULL)
		return;
	for (i = 0; i < n; i++)
		(void)install(w, subs[i].stream, subs[i].fn, subs[i].ctx, NULL);
}

/* ------------------------------------------------------------------ */
/* Accessors                                                           */
/* ------------------------------------------------------------------ */

static const decoded_event *
at(const dtfl_log_batch *b, size_t i, dtfl_log_event_kind kind)
{
	if (i >= b->n)
		return NULL;
	if (kind != DTFL_LOG_UNKNOWN && b->events[i].kind != kind)
		return NULL;
	return &b->events[i];
}

size_t
dtfl_log_batch_len(const dtfl_log_batch *b)
{
	return b->n;
}

uint64_t
dtfl_log_batch_time(const dtfl_log_batch *b)
{
	return b->time_ns;
}

size_t
dtfl_log_batch_worker(const dtfl_log_batch *b)
{
	return b->worker;
}

const char *
dtfl_log_batch_stream(const dtfl_log_batch *b)
{
	return b->stream;
}

uint64_t
dtfl_log_event_time(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_UNKNOWN);

	return ev != NULL ? ev->time_ns : 0;
}

dtfl_log_event_kind
dtfl_log_event_kind_at(const dtfl_log_batch *b, size_t i)
{
	return i < b->n ? b->events[i].kind : DTFL_LOG_UNKNOWN;
}

size_t
dtfl_log_operates_id(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_OPERATES);

	return ev != NULL ? ev->a : 0;
}

const size_t *
dtfl_log_operates_addr(const dtfl_log_batch *b, size_t i, size_t *n)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_OPERATES);

	if (ev == NULL) {
		*n = 0;
		return NULL;
	}
	*n = ev->addr_len;
	return (const size_t *)(void *)b->addrs.data + ev->addr_off;
}

const char *
dtfl_log_operates_name(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_OPERATES);

	return ev != NULL ? ev->str : NULL;
}

size_t
dtfl_log_channels_id(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_CHANNELS);

	return ev != NULL ? ev->a : 0;
}

const size_t *
dtfl_log_channels_scope(const dtfl_log_batch *b, size_t i, size_t *n)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_CHANNELS);

	if (ev == NULL) {
		*n = 0;
		return NULL;
	}
	*n = ev->scope_len;
	return (const size_t *)(void *)b->addrs.data + ev->scope_off;
}

void
dtfl_log_channels_source(const dtfl_log_batch *b, size_t i, size_t *node,
    size_t *port)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_CHANNELS);

	*node = ev != NULL ? ev->b : 0;
	*port = ev != NULL ? ev->c : 0;
}

void
dtfl_log_channels_target(const dtfl_log_batch *b, size_t i, size_t *node,
    size_t *port)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_CHANNELS);

	*node = ev != NULL ? ev->d : 0;
	*port = ev != NULL ? ev->e : 0;
}

const char *
dtfl_log_channels_typ(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_CHANNELS);

	return ev != NULL ? ev->str : NULL;
}

size_t
dtfl_log_schedule_id(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_SCHEDULE);

	return ev != NULL ? ev->a : 0;
}

bool
dtfl_log_schedule_start(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_SCHEDULE);

	return ev != NULL && ev->flag0;
}

size_t
dtfl_log_shutdown_id(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_SHUTDOWN);

	return ev != NULL ? ev->a : 0;
}

size_t
dtfl_log_push_progress_op_id(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_PUSH_PROGRESS);

	return ev != NULL ? ev->a : 0;
}

bool
dtfl_log_messages_is_send(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_MESSAGES);

	return ev != NULL && ev->flag0;
}

size_t
dtfl_log_messages_channel(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_MESSAGES);

	return ev != NULL ? ev->a : 0;
}

size_t
dtfl_log_messages_source(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_MESSAGES);

	return ev != NULL ? ev->b : 0;
}

size_t
dtfl_log_messages_target(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_MESSAGES);

	return ev != NULL ? ev->c : 0;
}

size_t
dtfl_log_messages_seq_no(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_MESSAGES);

	return ev != NULL ? ev->d : 0;
}

int64_t
dtfl_log_messages_record_count(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_MESSAGES);

	return ev != NULL ? ev->i0 : 0;
}

size_t
dtfl_log_comm_channels_id(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_CHANNELS);

	return ev != NULL ? ev->a : 0;
}

bool
dtfl_log_comm_channels_progress(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_CHANNELS);

	return ev != NULL && ev->flag0;
}

bool
dtfl_log_park_is_park(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_PARK);

	return ev != NULL && ev->flag0;
}

bool
dtfl_log_park_timeout(const dtfl_log_batch *b, size_t i, uint64_t *timeout_ns)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_PARK);

	if (ev == NULL || !ev->flag0 || !ev->opt)
		return false;
	*timeout_ns = ev->dur_ns;
	return true;
}

const char *
dtfl_log_text(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_TEXT);

	return ev != NULL ? ev->str : NULL;
}

size_t
dtfl_log_dd_operator(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_UNKNOWN);

	if (ev == NULL || ev->kind < DTFL_LOG_DD_BATCH ||
	    ev->kind > DTFL_LOG_DD_BATCHER)
		return 0;
	return ev->a;
}

size_t
dtfl_log_dd_length(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_UNKNOWN);

	if (ev == NULL || (ev->kind != DTFL_LOG_DD_BATCH &&
	    ev->kind != DTFL_LOG_DD_DROP))
		return 0;
	return ev->b;
}

size_t
dtfl_log_dd_merge_scale(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_DD_MERGE);

	return ev != NULL ? ev->b : 0;
}

size_t
dtfl_log_dd_merge_length1(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_DD_MERGE);

	return ev != NULL ? ev->c : 0;
}

size_t
dtfl_log_dd_merge_length2(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_DD_MERGE);

	return ev != NULL ? ev->d : 0;
}

bool
dtfl_log_dd_merge_complete(const dtfl_log_batch *b, size_t i, size_t *complete)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_DD_MERGE);

	if (ev == NULL || !ev->opt)
		return false;
	*complete = ev->e;
	return true;
}

size_t
dtfl_log_dd_shortfall_scale(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_DD_MERGE_SHORTFALL);

	return ev != NULL ? ev->b : 0;
}

size_t
dtfl_log_dd_shortfall(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_DD_MERGE_SHORTFALL);

	return ev != NULL ? ev->c : 0;
}

int64_t
dtfl_log_dd_trace_share_diff(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_DD_TRACE_SHARE);

	return ev != NULL ? ev->i0 : 0;
}

void
dtfl_log_dd_batcher(const dtfl_log_batch *b, size_t i, int64_t *records_diff,
    int64_t *size_diff, int64_t *capacity_diff, int64_t *allocations_diff)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_DD_BATCHER);

	*records_diff = ev != NULL ? ev->i0 : 0;
	*size_diff = ev != NULL ? ev->i1 : 0;
	*capacity_diff = ev != NULL ? ev->i2 : 0;
	*allocations_diff = ev != NULL ? ev->i3 : 0;
}

bool
dtfl_log_comm_message_is_send(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_MESSAGE);

	return ev != NULL && ev->flag0;
}

void
dtfl_log_comm_message_header(const dtfl_log_batch *b, size_t i, size_t *channel,
    size_t *source, size_t *target_lower, size_t *target_upper, size_t *length,
    size_t *seqno)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_MESSAGE);

	*channel = ev != NULL ? ev->a : 0;
	*source = ev != NULL ? ev->b : 0;
	*target_lower = ev != NULL ? ev->c : 0;
	*target_upper = ev != NULL ? ev->d : 0;
	*length = ev != NULL ? ev->e : 0;
	*seqno = ev != NULL ? ev->f : 0;
}

bool
dtfl_log_comm_state_send(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_STATE);

	return ev != NULL && ev->flag0;
}

size_t
dtfl_log_comm_state_process(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_STATE);

	return ev != NULL ? ev->a : 0;
}

size_t
dtfl_log_comm_state_remote(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_STATE);

	return ev != NULL ? ev->b : 0;
}

bool
dtfl_log_comm_state_start(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_STATE);

	return ev != NULL && ev->flag1;
}

bool
dtfl_log_comm_setup_sender(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_SETUP);

	return ev != NULL && ev->flag0;
}

size_t
dtfl_log_comm_setup_process(const dtfl_log_batch *b, size_t i)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_SETUP);

	return ev != NULL ? ev->a : 0;
}

bool
dtfl_log_comm_setup_remote(const dtfl_log_batch *b, size_t i, size_t *remote)
{
	const decoded_event *ev = at(b, i, DTFL_LOG_COMM_SETUP);

	if (ev == NULL || !ev->opt)
		return false;
	*remote = ev->b;
	return true;
}
