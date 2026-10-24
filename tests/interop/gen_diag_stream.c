/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/interop/gen_diag_stream.c: writes the dtfl-diag log-stream
 * fixture (tests/interop/fixtures/diag_graph_stream.bin).
 *
 * The fixture is one Messages event in the EventWriter batch-stream wire
 * format dtfl_diag_replay_fd consumes (PLAN D9): a u64 LE body length,
 * then the body (u32 LE Event tag = 1 Messages, Duration time, u64 LE
 * record count, then each record as Duration + u32 LE length + the
 * bincode TimelyEvent).  The TimelyEvent record bytes are produced by
 * the same internal encoders the in-process logger uses, so the fixture
 * is byte-faithful to the real stream.
 *
 * The graph it encodes is a root scope [0] containing a subscope [0, 2]
 * with two leaf operators inside, and channels that cross the subscope
 * boundary, so dtfl-diag graph exercises the ingress/egress re-wiring.
 * This generator is a build-time helper (not shipped); the resulting
 * .bin is committed and the Rust-equivalent is dev/gen-fixtures.d.
 */
#include "timely/log_codec_int.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TimelyEvent variant tags (declaration order), from timely_log.c. */
#define TE_OPERATES 0
#define TE_CHANNELS 1
#define TE_SCHEDULE 4
#define DE_BATCH 0

static void
enc_operates(__dtfl_vec *rec, size_t id, const size_t *addr, size_t naddr,
    const char *name)
{
	__dtfl_bc_u32(rec, TE_OPERATES);
	__dtfl_bc_u64(rec, id);
	__dtfl_bc_usizes(rec, addr, naddr);
	__dtfl_bc_str(rec, name);
}

static void
enc_channels(__dtfl_vec *rec, size_t id, const size_t *scope, size_t nscope,
    size_t sn, size_t sp, size_t tn, size_t tp, const char *typ)
{
	__dtfl_bc_u32(rec, TE_CHANNELS);
	__dtfl_bc_u64(rec, id);
	__dtfl_bc_usizes(rec, scope, nscope);
	__dtfl_bc_u64(rec, sn);
	__dtfl_bc_u64(rec, sp);
	__dtfl_bc_u64(rec, tn);
	__dtfl_bc_u64(rec, tp);
	__dtfl_bc_str(rec, typ);
}

/* Appends a (Duration event-time, u32 len, record bytes) record. */
static void
append_record_at(__dtfl_vec *body, uint64_t ns, const __dtfl_vec *rec)
{
	__dtfl_bc_duration(body, ns);
	__dtfl_bc_u32(body, (uint32_t)rec->len);
	if (rec->len > 0)
		__dtfl_vec_push_n(body, rec->data, rec->len);
}

/* Writes a single Messages batch frame (count records) to a file. */
static int
write_frame(const char *path, const __dtfl_vec *body)
{
	__dtfl_vec frame;
	unsigned char lenbuf[8];
	FILE *f;
	size_t i;

	__dtfl_vec_init(&frame, 1);
	for (i = 0; i < 8; i++)
		lenbuf[i] = (unsigned char)((uint64_t)body->len >> (8 * i));
	__dtfl_vec_push_n(&frame, lenbuf, 8);
	__dtfl_vec_push_n(&frame, body->data, body->len);
	f = fopen(path, "wb");
	if (f == NULL) {
		fprintf(stderr, "gen_diag_stream: cannot write %s\n", path);
		__dtfl_vec_free(&frame);
		return 1;
	}
	fwrite(frame.data, 1, frame.len, f);
	fclose(f);
	printf("wrote %zu bytes to %s\n", frame.len, path);
	__dtfl_vec_free(&frame);
	return 0;
}

static void
enc_schedule(__dtfl_vec *rec, size_t id, int start)
{
	__dtfl_bc_u32(rec, TE_SCHEDULE);
	__dtfl_bc_u64(rec, id);
	__dtfl_bc_u32(rec, start ? 0 : 1);	/* StartStop: Start=0, Stop=1 */
}

static void
enc_dd_batch(__dtfl_vec *rec, size_t op, size_t length)
{
	__dtfl_bc_u32(rec, DE_BATCH);
	__dtfl_bc_u64(rec, op);
	__dtfl_bc_u64(rec, length);
}

/*
 * Writes the profile fixture: Operates for two leaf operators, then
 * Schedule Start/Stop pairs with event-time gaps so each operator has a
 * known elapsed time (op 1: 100 ns, op 2: 300 ns).
 */
static int
write_profile(const char *path)
{
	__dtfl_vec body, rec;
	size_t a1[] = { 0, 1 };
	size_t a2[] = { 0, 2 };
	int rc;

	__dtfl_vec_init(&body, 1);
	__dtfl_bc_u32(&body, 1);		/* Event::Messages */
	__dtfl_bc_duration(&body, 0);		/* batch time */
	__dtfl_bc_u64(&body, 6);		/* record count */

	__dtfl_vec_init(&rec, 1);
	enc_operates(&rec, 1, a1, 2, "Map");
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_operates(&rec, 2, a2, 2, "Filter");
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_schedule(&rec, 1, 1);
	append_record_at(&body, 100, &rec);	/* op1 start @100 */
	__dtfl_vec_clear(&rec);
	enc_schedule(&rec, 1, 0);
	append_record_at(&body, 200, &rec);	/* op1 stop @200 -> 100 ns */
	__dtfl_vec_clear(&rec);
	enc_schedule(&rec, 2, 1);
	append_record_at(&body, 300, &rec);	/* op2 start @300 */
	__dtfl_vec_clear(&rec);
	enc_schedule(&rec, 2, 0);
	append_record_at(&body, 600, &rec);	/* op2 stop @600 -> 300 ns */
	__dtfl_vec_free(&rec);

	rc = write_frame(path, &body);
	__dtfl_vec_free(&body);
	return rc;
}

/* Writes the differential fixture: Batch events giving arrangement sizes. */
static int
write_differential(const char *path)
{
	__dtfl_vec body, rec;
	int rc;

	__dtfl_vec_init(&body, 1);
	__dtfl_bc_u32(&body, 1);		/* Event::Messages */
	__dtfl_bc_duration(&body, 0);
	__dtfl_bc_u64(&body, 3);		/* record count */

	__dtfl_vec_init(&rec, 1);
	enc_dd_batch(&rec, 1, 10);
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_dd_batch(&rec, 1, 5);
	append_record_at(&body, 0, &rec);	/* op1 total 15 */
	__dtfl_vec_clear(&rec);
	enc_dd_batch(&rec, 2, 7);
	append_record_at(&body, 0, &rec);	/* op2 total 7 */
	__dtfl_vec_free(&rec);

	rc = write_frame(path, &body);
	__dtfl_vec_free(&body);
	return rc;
}

int
main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/interop/fixtures";
	char path[4096];
	__dtfl_vec body, rec;
	int rc;

	/* The operator addresses: root [0], subscope [0,2], leaves. */
	size_t a_root[] = { 0 };
	size_t a_sub[] = { 0, 2 };
	size_t a_in[] = { 0, 1 };		/* an input operator in root */
	size_t a_sub_src[] = { 0, 2, 1 };	/* leaf inside subscope */
	size_t a_sub_dst[] = { 0, 2, 2 };	/* another leaf inside */
	size_t scope_root[] = { 0 };
	size_t scope_sub[] = { 0, 2 };

	__dtfl_vec_init(&body, 1);
	__dtfl_bc_u32(&body, 1);		/* Event::Messages */
	__dtfl_bc_duration(&body, 0);		/* batch time */
	__dtfl_bc_u64(&body, 10);		/* record count */

	/* Operates. */
	__dtfl_vec_init(&rec, 1);
	enc_operates(&rec, 0, a_root, 1, "Dataflow");
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_operates(&rec, 1, a_in, 2, "Input");
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_operates(&rec, 2, a_sub, 2, "Region");	/* the subscope */
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_operates(&rec, 3, a_sub_src, 3, "Map");
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_operates(&rec, 4, a_sub_dst, 3, "Filter");
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);

	/*
	 * Channels.  In the root scope [0]: from Input (node 1) to the
	 * subscope (node 2), port 0.  Inside the subscope [0,2]: the
	 * ingress channel from node 0 (the scope input) to Map (node 1),
	 * Map to Filter, and Filter to node 0 (the scope egress).  In the
	 * root: from the subscope (node 2) onward is not needed here.
	 */
	enc_channels(&rec, 10, scope_root, 1, 1, 0, 2, 0, "u64");	/* root: Input -> subscope */
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_channels(&rec, 11, scope_sub, 2, 0, 0, 1, 0, "u64");	/* sub ingress: in -> Map */
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_channels(&rec, 12, scope_sub, 2, 1, 0, 2, 0, "u64");	/* sub: Map -> Filter */
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_channels(&rec, 13, scope_sub, 2, 2, 0, 0, 0, "u64");	/* sub egress: Filter -> out */
	append_record_at(&body, 0, &rec);
	__dtfl_vec_clear(&rec);
	enc_channels(&rec, 14, scope_root, 1, 2, 0, 1, 0, "u64");	/* root: subscope -> Input (cycle) */
	append_record_at(&body, 0, &rec);
	__dtfl_vec_free(&rec);

	snprintf(path, sizeof path, "%s/diag_graph_stream.bin", dir);
	rc = write_frame(path, &body);
	__dtfl_vec_free(&body);
	if (rc != 0)
		return rc;

	snprintf(path, sizeof path, "%s/diag_profile_stream.bin", dir);
	if ((rc = write_profile(path)) != 0)
		return rc;
	snprintf(path, sizeof path, "%s/diag_differential_stream.bin", dir);
	if ((rc = write_differential(path)) != 0)
		return rc;
	return 0;
}
