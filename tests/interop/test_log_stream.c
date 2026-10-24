/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/interop/test_log_stream.c: libdtfl's timely log records against
 * real timely's bincode (PLAN D9, section 6 tier 7).
 *
 * Reads tests/interop/fixtures/log_stream.bin, which real timely at the
 * pin produced (dev/gen-fixtures.d/log_stream.rs): for a set of TimelyEvent
 * variants, bincode::serialize(&TimelyEvent::...).  This test registers a
 * capturing "timely" logger on a worker, calls the ACTUAL libdtfl log
 * functions (__dtfl_worker_log_operates and friends in src/timely/
 * timely_log.c) with the same field values, and asserts each emitted
 * record is byte-identical to the Rust bincode.
 *
 * WHAT IS PROVEN BYTE-IDENTICAL TO RUST
 *   The TimelyEvent record layout the C codec emits: the u32 LE variant
 *   tag in declaration order, usize as u64 LE, bool and the Option tag as
 *   one byte, String and Vec as a u64 LE length then elements, Duration as
 *   a u64 LE seconds then u32 LE nanoseconds.  Covered variants: Operates,
 *   Channels, Schedule, Shutdown, Messages, Park(Some(Duration)).
 *
 * WHAT IS NOT COVERED (and why)
 *   The outer BatchLogger stream framing Event<Duration, Vec<(Duration,
 *   TimelyEvent)>> is not emitted here: the C log functions produce a bare
 *   TimelyEvent record (one per __dtfl_logger_log call), and the public
 *   logging registration that would wrap them in the batch stream is not
 *   wired through a public API yet (PLAN D9).  So this proves the per-event
 *   record layout, not the batch wrapper.  Stated plainly rather than
 *   faked.
 */
#include "dtfl_test.h"

#include "dtfl/timely.h"

#include "timely/scope_int.h"	/* struct dtfl_worker { __dtfl_worker *w; } */
#include "timely/worker_int.h"	/* __dtfl_worker_log_* */
#include "base/log_int.h"	/* __dtfl_log_registry_insert, __dtfl_log_event */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE "tests/interop/fixtures/log_stream.bin"
#define MAX_REC 16
#define MAX_RECLEN 256

static uint32_t
rd_u32_le(const unsigned char *p)
{
	uint32_t v = 0;
	size_t i;

	for (i = 0; i < 4; i++)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}

static unsigned char *
slurp(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	long n;
	unsigned char *buf;

	if (f == NULL) {
		const char *base[2] = { getenv("top_srcdir"), getenv("srcdir") };
		size_t k;
		char alt[1024];

		for (k = 0; k < 2 && f == NULL; k++) {
			if (base[k] == NULL)
				continue;
			snprintf(alt, sizeof alt, "%s/%s", base[k], path);
			f = fopen(alt, "rb");
		}
	}
	munit_assert_not_null(f);
	munit_assert_int(fseek(f, 0, SEEK_END), ==, 0);
	n = ftell(f);
	munit_assert_long(n, >, 0);
	munit_assert_int(fseek(f, 0, SEEK_SET), ==, 0);
	buf = malloc((size_t)n);
	munit_assert_not_null(buf);
	munit_assert_size(fread(buf, 1, (size_t)n, f), ==, (size_t)n);
	fclose(f);
	*out_len = (size_t)n;
	return buf;
}

/* Each logged event's bytes are copied into this capture (one per log). */
typedef struct cap {
	unsigned char rec[MAX_REC][MAX_RECLEN];
	size_t len[MAX_REC];
	size_t n;
} cap;

static void
capture_action(void *ctx, uint64_t now_ns, const __dtfl_log_event *events,
    size_t n)
{
	cap *c = ctx;
	size_t i;

	(void)now_ns;
	for (i = 0; i < n; i++) {
		munit_assert_size(c->n, <, MAX_REC);
		munit_assert_size(events[i].len, <=, MAX_RECLEN);
		memcpy(c->rec[c->n], events[i].data, events[i].len);
		c->len[c->n] = events[i].len;
		c->n++;
	}
}

DTFL_TEST(log_stream_matches_rust)
{
	size_t flen, off, i, nrec;
	unsigned char *buf;
	dtfl_worker *pub = dtfl_worker_new_standalone();
	__dtfl_worker *w = ((struct dtfl_worker *)pub)->w;
	static cap c;
	const size_t op_addr[2] = { 0, 2 };
	const size_t ch_scope[1] = { 0 };

	(void)params; (void)data;
	memset(&c, 0, sizeof c);

	/* Register a capturing logger under "timely"; the log functions emit
	 * to it, so we see exactly the bytes timely_log.c produces. */
	munit_assert_not_null(w->log);
	__dtfl_log_registry_insert(w->log, "timely", capture_action, &c, NULL);

	/* Emit the SAME variants the Rust fixture holds, in order. */
	__dtfl_worker_log_operates(w, 7, op_addr, 2, "Map");
	__dtfl_worker_log_channel(w, 3, ch_scope, 1, 1, 0, 2, 1, "u64");
	__dtfl_worker_log_schedule(w, 7, true);		/* StartStop::Start */
	__dtfl_worker_log_shutdown(w, 7);
	__dtfl_worker_log_messages(w, true, 3, 0, 1, 5, 42);
	__dtfl_worker_log_park(w, true, 1500000000ull);	/* Park(Some(1.5s)) */
	__dtfl_log_registry_flush(w->log);

	buf = slurp(FIXTURE, &flen);
	munit_assert_size(flen, >=, 12);
	munit_assert_uint32(rd_u32_le(buf), ==, 0x4454464cu);	/* "DTFL" */
	munit_assert_uint32(rd_u32_le(buf + 4), ==, 1);		/* version */
	nrec = rd_u32_le(buf + 8);

	munit_assert_size(c.n, ==, nrec);
	munit_assert_size(nrec, ==, 6);

	off = 12;
	for (i = 0; i < nrec; i++) {
		size_t rlen;

		munit_assert_size(off + 4, <=, flen);
		rlen = rd_u32_le(buf + off);
		off += 4;
		munit_assert_size(off + rlen, <=, flen);
		/* The libdtfl-emitted record must equal the Rust bincode byte
		 * for byte, both length and bytes. */
		munit_assert_size(c.len[i], ==, rlen);
		munit_assert_memory_equal(rlen, c.rec[i], buf + off);
		off += rlen;
	}
	munit_assert_size(off, ==, flen);

	free(buf);
	dtfl_worker_free(pub);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	DTFL_T(log_stream_matches_rust),
	DTFL_T_END
};

DTFL_TEST_MAIN("interop", tests)
