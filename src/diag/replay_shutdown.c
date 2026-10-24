/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/diag/replay_shutdown.c: replay a recorded log stream.
 *
 * Ported from dataflow-diag (HEAD 0fa322c9)
 *   connect/src/receive/replaywithshutdown.rs
 *   connect/src/receive/connect.rs
 *   connect/src/receive/mod.rs
 * Copyright (c) 2017 the timely-dataflow diagnostics contributors.
 *
 * The connect crate opens EventReaders over the inspected computation's
 * log sockets and ReplayWithShutdown replays them into a dataflow,
 * stopping cleanly at EOF.  This file is the C counterpart that the
 * dtfl-diag tool uses through the public dtfl_diag_replay_* API: it
 * reads the EventWriter framing of the log stream
 * (Event<Duration, Vec<(Duration, Event)>>), decodes each inner event
 * record with the shared log decoder (src/timely/log.c), and hands each
 * batch to the caller's subscriber.  "Replay with shutdown" here is the
 * read-to-EOF loop: when the socket or file reaches end of stream the
 * replay returns, as upstream's is_running flip to false drains the
 * frontier and the operator closes.
 *
 * Wire format (PLAN D9), the bincode-1 fixint little-endian form the
 * worker's BatchLogger(EventWriter) produces:
 *   per event: u64 LE body length, then the body
 *   body: u32 LE Event tag (Progress = 0, Messages = 1)
 *     Progress: u64 LE count, then count * (Duration, i64)
 *     Messages: Duration time, u64 LE count,
 *               then count * (Duration, u32 LE record length, record bytes)
 *   Duration: u64 LE seconds, u32 LE nanoseconds
 * The event record bytes are exactly the per-event encoding the in-
 * process encoders emit and the shared decoder inverts.  libdtfl frames
 * each inner record with a u32 LE length so the stream is self-
 * delimiting without a bincode cursor; the writer that produces this
 * framing is dtfl_diag_log_writer (src/diag/diag_log_writer.c).
 */
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "base/base_int.h"
#include "base/log_int.h"
#include "timely/log_codec_int.h"
#include "dtfl/diag.h"

#define EVENT_PROGRESS 0
#define EVENT_MESSAGES 1

/* Reads exactly len bytes into buf; returns 1 on success, 0 at clean EOF
 * (nothing read), -1 on a short read or error. */
static int
read_exact(int fd, void *buf, size_t len)
{
	unsigned char *p = buf;
	size_t off = 0;

	while (off < len) {
		ssize_t n = read(fd, p + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return off == 0 ? 0 : -1;
		off += (size_t)n;
	}
	return 1;
}

static uint64_t
rd_u64(const unsigned char *p)
{
	uint64_t v = 0;
	size_t i;

	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

static uint32_t
rd_u32(const unsigned char *p)
{
	uint32_t v = 0;
	size_t i;

	for (i = 0; i < 4; i++)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}

/* A Duration is u64 LE seconds then u32 LE nanoseconds -> nanoseconds. */
static uint64_t
rd_duration(const unsigned char *p)
{
	return rd_u64(p) * 1000000000ULL + rd_u32(p + 8);
}

/* The subscriber callback has type void(const dtfl_log_batch *, void *);
 * the shared decoder uses void(const void *, void *).  They are
 * compatible (the first argument is a pointer); the cast is confined
 * here. */
static void
deliver(const void *batch, void *ctx)
{
	struct { dtfl_log_subscriber_fn fn; void *ctx; } *d = ctx;

	d->fn((const dtfl_log_batch *)batch, d->ctx);
}

/*
 * Decodes one Event body and, if it is a Messages batch, delivers the
 * decoded records.  Returns DTFL_OK or DTFL_EPROTO on malformed bytes.
 */
static int
handle_body(void *scratch, const char *stream, const unsigned char *body,
    size_t blen, dtfl_log_subscriber_fn fn, void *user)
{
	struct { dtfl_log_subscriber_fn fn; void *ctx; } d = { fn, user };
	uint32_t tag;
	size_t off;
	uint64_t time_ns, count, i;
	__dtfl_vec recs;	/* __dtfl_log_event */
	int rc = DTFL_OK;

	if (blen < 4)
		return __dtfl_err(DTFL_EPROTO, "diag replay: short body");
	tag = rd_u32(body);
	off = 4;

	if (tag == EVENT_PROGRESS)
		return DTFL_OK;	/* frontier updates carry no event records */
	if (tag != EVENT_MESSAGES)
		return __dtfl_err(DTFL_EPROTO, "diag replay: bad event tag %u",
		    (unsigned)tag);

	if (blen < off + 12)
		return __dtfl_err(DTFL_EPROTO, "diag replay: short time");
	time_ns = rd_duration(body + off);
	off += 12;
	if (blen < off + 8)
		return __dtfl_err(DTFL_EPROTO, "diag replay: short count");
	count = rd_u64(body + off);
	off += 8;

	__dtfl_vec_init(&recs, sizeof(__dtfl_log_event));
	for (i = 0; i < count; i++) {
		__dtfl_log_event ev;
		uint64_t ev_time;
		uint32_t rec_len;

		if (blen < off + 12 + 4) {
			rc = __dtfl_err(DTFL_EPROTO,
			    "diag replay: short record header");
			goto done;
		}
		ev_time = rd_duration(body + off);
		off += 12;
		rec_len = rd_u32(body + off);
		off += 4;
		if (blen < off + rec_len) {
			rc = __dtfl_err(DTFL_EPROTO,
			    "diag replay: short record body");
			goto done;
		}
		ev.time_ns = ev_time;
		ev.data = body + off;
		ev.len = rec_len;
		off += rec_len;
		__dtfl_vec_push(&recs, &ev);
	}
done:
	if (rc == DTFL_OK && recs.len > 0)
		__dtfl_log_decode_and_deliver(scratch, stream, 0, time_ns,
		    (const __dtfl_log_event *)(void *)recs.data, recs.len, deliver, &d);
	__dtfl_vec_free(&recs);
	return rc;
}

int
dtfl_diag_replay_fd(int fd, const char *stream, dtfl_log_subscriber_fn fn,
    void *ctx)
{
	void *scratch;
	int rc = DTFL_OK;

	if (fd < 0 || stream == NULL || fn == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_diag_replay_fd: bad "
		    "argument");
	scratch = __dtfl_log_scratch_new();

	for (;;) {
		unsigned char lenbuf[8];
		unsigned char *body;
		uint64_t blen;
		int r = read_exact(fd, lenbuf, sizeof lenbuf);

		if (r == 0)
			break;	/* clean EOF */
		if (r < 0) {
			rc = __dtfl_err(DTFL_EIO, "diag replay: read error");
			break;
		}
		blen = rd_u64(lenbuf);
		if (blen == 0 || blen > (1u << 28)) {
			rc = __dtfl_err(DTFL_EPROTO,
			    "diag replay: body length %llu out of range",
			    (unsigned long long)blen);
			break;
		}
		body = __dtfl_calloc(1, (size_t)blen);
		r = read_exact(fd, body, (size_t)blen);
		if (r != 1) {
			__dtfl_free(body);
			rc = __dtfl_err(DTFL_EPROTO, "diag replay: truncated "
			    "body");
			break;
		}
		rc = handle_body(scratch, stream, body, (size_t)blen, fn, ctx);
		__dtfl_free(body);
		if (rc != DTFL_OK)
			break;
	}

	__dtfl_log_scratch_free(scratch);
	return rc;
}

int
dtfl_diag_replay_fds(int const *fds, size_t nfds, const char *stream,
    dtfl_log_subscriber_fn fn, void *ctx)
{
	size_t i;

	if (fds == NULL || nfds == 0)
		return __dtfl_err(DTFL_EINVAL, "dtfl_diag_replay_fds: no fds");
	/*
	 * The sources are drained in order; a real deployment interleaves
	 * them per worker, but the graph and profile commands fold every
	 * event into one state regardless of order, so a sequential drain
	 * reconstructs the same graph (upstream filters to worker 0).
	 */
	for (i = 0; i < nfds; i++) {
		int rc = dtfl_diag_replay_fd(fds[i], stream, fn, ctx);

		if (rc != DTFL_OK)
			return rc;
	}
	return DTFL_OK;
}
