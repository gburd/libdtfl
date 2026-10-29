/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/fuzz/gen_seeds.c: writes the seed corpus tests/corpus/fuzz/.
 *
 *   gen_seeds TOP_SRCDIR
 *
 * Each seed is a well-formed input made by the library's own encoders
 * (the ones the unit tests check against the Rust fixtures), or a
 * committed fixture or example program, so every target starts from
 * the accepted path of its decoder.  The first one or two bytes of a
 * seed are the target's selector bytes (tests/fuzz/fuzz_*.c).  Run with
 * `make fuzz-seeds` after a wire format changes and commit the output.
 */
#include "fuzz.h"

#include "comm/comm_int.h"
#include "comm/zc_int.h"
#include "base/base_int.h"
#include "ddir/ddir_int.h"
#include "dtfl/ddir.h"
#include "progress/operate_int.h"
#include "timely/capture_int.h"
#include "timely/log_codec_int.h"
#include "timely/timely_int.h"

#include <errno.h>
#include <sys/stat.h>

static const char *top;

static void
put(const char *target, const char *name, unsigned char sel0, int nsel,
    unsigned char sel1, const void *p, size_t len)
{
	char path[4096];
	FILE *f;

	snprintf(path, sizeof path, "%s/tests/corpus/fuzz/%s", top, target);
	FUZZ_ASSERT(mkdir(path, 0755) == 0 || errno == EEXIST);
	snprintf(path, sizeof path, "%s/tests/corpus/fuzz/%s/%s", top, target,
	    name);
	f = fopen(path, "wb");
	FUZZ_ASSERT(f != NULL);
	if (nsel > 0)
		fputc(sel0, f);
	if (nsel > 1)
		fputc(sel1, f);
	if (len > 0)
		FUZZ_ASSERT(fwrite(p, 1, len, f) == len);
	fclose(f);
}

static unsigned char *
slurp(const char *rel, size_t *len)
{
	char path[4096];
	FILE *f;
	long n;
	unsigned char *buf;

	snprintf(path, sizeof path, "%s/%s", top, rel);
	f = fopen(path, "rb");
	FUZZ_ASSERT(f != NULL);
	FUZZ_ASSERT(fseek(f, 0, SEEK_END) == 0);
	n = ftell(f);
	FUZZ_ASSERT(n >= 0 && fseek(f, 0, SEEK_SET) == 0);
	buf = malloc((size_t)n + 1);
	FUZZ_ASSERT(buf != NULL);
	FUZZ_ASSERT(fread(buf, 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	*len = (size_t)n;
	return buf;
}

static void
copy_file(const char *target, const char *name, int nsel, unsigned char sel,
    const char *rel)
{
	size_t len;
	unsigned char *b = slurp(rel, &len);

	put(target, name, sel, nsel, 0, b, len);
	free(b);
}

static __dtfl_batch *
sample_batch(void)
{
	__dtfl_batch *b = __dtfl_batch_new(1, 8);
	uint64_t t = 4;
	int64_t d = -2;

	__dtfl_batch_push(b, "abc", 3, &t, &d);
	__dtfl_batch_push(b, "", 0, &t, &d);
	t = 9;
	d = 1;
	__dtfl_batch_push(b, "\x01\x02\x03\x04\x05", 5, &t, &d);
	return b;
}

static void
frames(void)
{
	__dtfl_message_header h = { 3, 1, 0, 2, 5, 7 };
	unsigned char buf[2 * DTFL_HEADER_BYTES + 5];

	__dtfl_header_write_to(&h, buf);
	memcpy(buf + DTFL_HEADER_BYTES, "hello", 5);
	h.length = 0;
	__dtfl_header_write_to(&h, buf + DTFL_HEADER_BYTES + 5);
	put("fuzz_frames", "two_frames", 255, 1, 0, buf, sizeof buf);
	put("fuzz_frames", "two_frames_bytewise", 0, 1, 0, buf, sizeof buf);
}

static void
message(void)
{
	const __dtfl_msg_class *mc = __dtfl_message_class();
	dtfl_time_class *tc = dtfl_time_u64();
	uint64_t t = 11;
	dtfl_message *m = __dtfl_message_new(tc, &t, sample_batch());
	size_t n;
	unsigned char *out;

	m->from = 1;
	m->seq = 2;
	n = mc->encoded_len(mc, m);
	out = malloc(n);
	mc->encode(mc, m, out);
	put("fuzz_message", "u64_three_records", 0, 0, 0, out, n);
	free(out);
	__dtfl_message_free(m);
	dtfl_time_class_free(tc);
}

static void
progress(void)
{
	const __dtfl_msg_class *mc = __dtfl_progress_class();
	__dtfl_progress_msg m;
	uint64_t k[3] = { 1, 0, 5 };
	size_t n;
	unsigned char *out;

	m.source = 0;
	m.seq = 3;
	__dtfl_cb_init(&m.changes, 3);
	__dtfl_cb_update(&m.changes, k, 1);
	k[2] = 7;
	__dtfl_cb_update(&m.changes, k, -1);
	n = mc->encoded_len(mc, &m);
	out = malloc(n);
	mc->encode(mc, &m, out);
	put("fuzz_progress", "two_updates", 0, 0, 0, out, n);
	free(out);
	__dtfl_cb_free(&m.changes);
}

static void
event(void)
{
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_event e;
	uint64_t t0 = 0, t3 = 3;
	__dtfl_vec enc;

	__dtfl_vec_init(&enc, 1);
	e.tag = DTFL_EVENT_PROGRESS;
	e.depth = 1;
	__dtfl_cb_init(&e.u.progress, 1);
	__dtfl_cb_update(&e.u.progress, &t0, 1);
	__dtfl_cb_update(&e.u.progress, &t3, -1);
	__dtfl_event_encode(tc, &e, &enc);
	__dtfl_event_clear(&e);
	e.tag = DTFL_EVENT_MESSAGES;
	e.depth = 1;
	e.u.messages.time[0] = 3;
	e.u.messages.data = sample_batch();
	__dtfl_event_encode(tc, &e, &enc);
	__dtfl_event_clear(&e);
	put("fuzz_event", "u64_progress_then_messages", 1, 1, 0, enc.data,
	    enc.len);
	__dtfl_vec_free(&enc);
	dtfl_time_class_free(tc);
	/* The Rust EventWriter fixture: the Messages payload is Rust's. */
	copy_file("fuzz_event", "rust_capture_wire", 1, 1,
	    "tests/interop/fixtures/capture_wire.bin");
}

static void
times(void)
{
	const dtfl_coord u64 = { DTFL_COORD_UNSIGNED, 64 };
	dtfl_time_class *tc;
	uint64_t t[4] = { 42, 0, 0, 0 };
	__dtfl_vec enc;

	__dtfl_vec_init(&enc, 1);
	tc = dtfl_time_u64();
	__dtfl_time_encode(tc, t, &enc);
	put("fuzz_time", "u64_42", 1, 2, 0, enc.data, enc.len);
	__dtfl_vec_clear(&enc);
	__dtfl_summary_encode(tc, t, &enc);
	put("fuzz_time", "u64_summary", 1, 2, 1, enc.data, enc.len);
	dtfl_time_class_free(tc);
	tc = dtfl_time_pointstamp(u64, 3);
	t[0] = 1;
	t[1] = 2;
	__dtfl_vec_clear(&enc);
	__dtfl_time_encode(tc, t, &enc);
	put("fuzz_time", "pointstamp_1_2", 7, 2, 0, enc.data, enc.len);
	t[0] = 5;	/* retain Some(5), actions [1, 2] */
	t[1] = 1;
	t[2] = 2;
	__dtfl_vec_clear(&enc);
	__dtfl_summary_encode(tc, t, &enc);
	put("fuzz_time", "pointstamp_summary", 7, 2, 1, enc.data, enc.len);
	dtfl_time_class_free(tc);
	__dtfl_vec_free(&enc);
}

static void
rec_frame(__dtfl_vec *out, __dtfl_vec *rec)
{
	__dtfl_bc_u32(out, (uint32_t)rec->len);
	__dtfl_vec_push_n(out, rec->data, rec->len);
	__dtfl_vec_clear(rec);
}

static void
logs(void)
{
	size_t len;
	unsigned char *b = slurp("tests/interop/fixtures/log_stream.bin", &len);
	__dtfl_vec out, rec;

	/* log_stream.bin: "DTFL", version, count, then u32-framed records. */
	FUZZ_ASSERT(len > 12);
	put("fuzz_log", "rust_timely_records", 0, 1, 0, b + 12, len - 12);
	free(b);

	__dtfl_vec_init(&out, 1);
	__dtfl_vec_init(&rec, 1);
	__dtfl_bc_u32(&rec, 1);	/* Merge, complete Some(9) */
	__dtfl_bc_u64(&rec, 4);
	__dtfl_bc_u64(&rec, 2);
	__dtfl_bc_u64(&rec, 5);
	__dtfl_bc_u64(&rec, 6);
	__dtfl_bc_u8(&rec, 1);
	__dtfl_bc_u64(&rec, 9);
	rec_frame(&out, &rec);
	__dtfl_bc_u32(&rec, 5);	/* Batcher */
	__dtfl_bc_u64(&rec, 4);
	__dtfl_bc_u64(&rec, 1);
	__dtfl_bc_u64(&rec, (uint64_t)-1);
	__dtfl_bc_u64(&rec, 2);
	__dtfl_bc_u64(&rec, 3);
	rec_frame(&out, &rec);
	put("fuzz_log", "differential_records", 1, 1, 0, out.data, out.len);
	__dtfl_vec_clear(&out);
	__dtfl_bc_u32(&rec, 0);	/* Message header */
	__dtfl_bc_u8(&rec, 1);
	__dtfl_bc_u64(&rec, 3);
	__dtfl_bc_u64(&rec, 0);
	__dtfl_bc_u64(&rec, 1);
	__dtfl_bc_u64(&rec, 2);
	__dtfl_bc_u64(&rec, 64);
	__dtfl_bc_u64(&rec, 8);
	rec_frame(&out, &rec);
	__dtfl_bc_u32(&rec, 2);	/* Setup, remote Some(1) */
	__dtfl_bc_u8(&rec, 0);
	__dtfl_bc_u64(&rec, 0);
	__dtfl_bc_u8(&rec, 1);
	__dtfl_bc_u64(&rec, 1);
	rec_frame(&out, &rec);
	put("fuzz_log", "communication_records", 2, 1, 0, out.data, out.len);
	__dtfl_vec_free(&rec);
	__dtfl_vec_free(&out);
}

static void
ddir(void)
{
	static const char *const progs[] = {
		"adt.ddp", "binders.ddp", "kcore.ddir", "reach.ddir",
		"reach.ddp", "scc.ddir", "scc.ddp", "stable.ddir",
		"stable.ddp", "unnest.ddp"
	};
	static const char *const terms[] = {
		"inject(2,tuple(1,2))", "list(7,8,9)", "tuple(1,list(2,3))",
		"tuple()"
	};
	size_t i;

	for (i = 0; i < sizeof progs / sizeof progs[0]; i++) {
		char rel[256];
		size_t n = strlen(progs[i]);

		snprintf(rel, sizeof rel, "tools/dtfl-ddir/programs/%s",
		    progs[i]);
		copy_file("fuzz_ddir_parse", progs[i], 1,
		    strcmp(progs[i] + n - 4, ".ddp") == 0 ? 0 : 1, rel);
	}
	for (i = 0; i < sizeof terms / sizeof terms[0]; i++) {
		dtfl_ddir_value *v = dtfl_ddir_value_parse(terms[i]);
		uint8_t *buf = NULL;
		size_t len = 0, cap = 0;
		char name[32];

		FUZZ_ASSERT(v != NULL);
		snprintf(name, sizeof name, "term%zu", i);
		put("fuzz_ddir_value", name, 0, 1, 0, terms[i],
		    strlen(terms[i]));
		dd_value_encode(v, &buf, &len, &cap);
		snprintf(name, sizeof name, "record%zu", i);
		put("fuzz_ddir_value", name, 1, 1, 0, buf, len);
		__dtfl_free(buf);
		dtfl_ddir_value_free(v);
	}
}

static void
records(void)
{
	unsigned char b[64];
	size_t n = 0;
	__dtfl_batch *bt = sample_batch();
	size_t elen = __dtfl_batch_encoded_len(bt);
	unsigned char *enc = malloc(elen);

	/* pair(u64, string): u64 BE, then u64 BE length and the bytes. */
	n += dtfl_put_u64(b + n, 7);
	n += dtfl_put_bytes(b + n, "abc", 3);
	put("fuzz_record", "pair_u64_string", 4, 2, 8, b, n);
	/* vec(string) of two. */
	n = dtfl_put_u64(b, 2);
	n += dtfl_put_bytes(b + n, "x", 1);
	n += dtfl_put_bytes(b + n, "yz", 2);
	put("fuzz_record", "vec_string", 7, 2, 0, b, n);
	/* option(i64): Some(-1). */
	b[0] = 1;
	n = 1 + dtfl_put_i64(b + 1, -1);
	put("fuzz_record", "option_i64", 5, 2, 3, b, n);
	/* A batch encoding (the record half sees it as raw bytes). */
	__dtfl_batch_encode(bt, enc);
	put("fuzz_record", "batch", 0, 2, 0, enc, elen);
	free(enc);
	__dtfl_batch_release(bt);
}

int
main(int argc, char **argv)
{
	char path[4096];

	if (argc != 2) {
		fprintf(stderr, "usage: gen_seeds TOP_SRCDIR\n");
		return 2;
	}
	top = argv[1];
	snprintf(path, sizeof path, "%s/tests/corpus/fuzz", top);
	FUZZ_ASSERT(mkdir(path, 0755) == 0 || errno == EEXIST);
	frames();
	message();
	progress();
	event();
	times();
	logs();
	copy_file("fuzz_diag_replay", "graph_stream", 0, 0,
	    "tests/interop/fixtures/diag_graph_stream.bin");
	copy_file("fuzz_diag_replay", "profile_stream", 0, 0,
	    "tests/interop/fixtures/diag_profile_stream.bin");
	copy_file("fuzz_diag_replay", "differential_stream", 0, 0,
	    "tests/interop/fixtures/diag_differential_stream.bin");
	{
		static const char req[] =
		    "GET / HTTP/1.1\r\nHost: localhost\r\n"
		    "Upgrade: websocket\r\nConnection: Upgrade\r\n"
		    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		    "Sec-WebSocket-Version: 13\r\n\r\n";

		put("fuzz_ws_handshake", "rfc6455_request", 0, 0, 0, req,
		    sizeof req - 1);
	}
	ddir();
	records();
	return 0;
}
