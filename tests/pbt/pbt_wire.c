/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * tests/pbt/pbt_wire.c: properties of the zero-copy byte layer.
 *
 *   - header_round_trip: write_to then try_read returns the same header
 *     for any field values, and try_read accepts exactly the buffers
 *     that hold header + length bytes.
 *   - try_read_in_bounds: try_read on random bytes of random length
 *     reads only those bytes.  The buffer is an exact-size allocation,
 *     so ASan catches any overread; the property also checks the
 *     answer against a reference decode.
 *   - process_binary_schedule: two ProcessBinary allocators stepped by
 *     hand from one thread under a random schedule of send, release,
 *     receive and pull, against a multiset model of messages in flight.
 *     Every message sent is received exactly once, in order per
 *     (sender, channel).
 */
#include "pbt.h"
#include "comm/zc_int.h"

#include <pthread.h>

static void
prop_header_round_trip(pbt_tc *tc)
{
	__dtfl_message_header h, r;
	unsigned char buf[DTFL_HEADER_BYTES + 64];
	size_t extra, len;

	h.channel = (size_t)pbt_int(tc, 0, INT64_MAX);
	h.source = (size_t)pbt_int(tc, 0, INT64_MAX);
	h.target_lower = (size_t)pbt_int(tc, 0, INT64_MAX);
	h.target_upper = (size_t)pbt_int(tc, 0, INT64_MAX);
	h.length = (size_t)pbt_int(tc, 0, 64);
	h.seqno = (size_t)pbt_int(tc, 0, INT64_MAX);
	if (pbt_bool(tc))
		h.channel = SIZE_MAX - (size_t)pbt_int(tc, 0, 3);
	__dtfl_header_write_to(&h, buf);
	PBT_ASSERT(__dtfl_header_try_read(&r, buf, sizeof buf));
	PBT_ASSERT(memcmp(&h, &r, sizeof h) == 0);
	PBT_ASSERT(__dtfl_header_required_bytes(&r) == 48 + h.length);
	extra = (size_t)pbt_int(tc, 0, 64);
	len = DTFL_HEADER_BYTES + extra;
	PBT_ASSERT(__dtfl_header_try_read(&r, buf, len) == (extra >= h.length));
}

static void
prop_try_read_in_bounds(pbt_tc *tc)
{
	size_t len = (size_t)pbt_int(tc, 0, 128), i;
	unsigned char *buf = __dtfl_alloc(len);
	__dtfl_message_header r;
	uint64_t want_len = 0;
	bool ok;

	pbt_defer(tc, __dtfl_free, buf);
	for (i = 0; i < len; i++)
		buf[i] = (unsigned char)pbt_int(tc, 0, 255);
	/* Bias some cases toward small, plausible lengths. */
	if (len >= DTFL_HEADER_BYTES && pbt_bool(tc)) {
		memset(buf + 32, 0, 8);
		buf[39] = (unsigned char)pbt_int(tc, 0, 96);
	}
	ok = __dtfl_header_try_read(&r, buf, len);
	if (len >= DTFL_HEADER_BYTES) {
		for (i = 32; i < 40; i++)
			want_len = (want_len << 8) | buf[i];
		PBT_ASSERT(ok == (want_len <= len - DTFL_HEADER_BYTES));
		if (ok)
			PBT_ASSERT(r.length == want_len);
	} else {
		PBT_ASSERT(!ok);
	}
}

/* ------------------------------------------------------------------ */
/* ProcessBinary under a random schedule                               */
/* ------------------------------------------------------------------ */

static size_t
u64_len(const __dtfl_msg_class *mc, const void *msg)
{
	(void)mc; (void)msg;
	return 8;
}

static void
u64_encode(const __dtfl_msg_class *mc, const void *msg, unsigned char *out)
{
	(void)mc;
	memcpy(out, msg, 8);
}

static void *
u64_decode(const __dtfl_msg_class *mc, const __dtfl_bytes *b)
{
	(void)mc;
	return b->len == 8 ? __dtfl_memdup(b->ptr, 8) : NULL;
}

static void *
u64_clone(const __dtfl_msg_class *mc, const void *msg)
{
	(void)mc;
	return __dtfl_memdup(msg, 8);
}

static void
u64_free(const __dtfl_msg_class *mc, void *msg)
{
	(void)mc;
	__dtfl_free(msg);
}

static const __dtfl_msg_class u64_class = {
	"u64", u64_len, u64_encode, u64_decode, u64_clone, u64_free, NULL
};

#define PEERS 2
#define NCHAN 2
#define MAXMSG 256
#define MAXSTEPS 400

typedef struct step {
	unsigned char op, w, c, t;
} step;

typedef struct build_slot {
	__dtfl_allocator_builder *b;
	__dtfl_allocator *a;
} build_slot;

static void *
build_thread(void *arg)
{
	build_slot *s = arg;

	s->a = s->b->build(s->b);
	return NULL;
}

/*
 * The model: for each (receiver, channel, sender) the sequence numbers
 * sent and the next one expected.  A message value is
 * sender << 32 | channel << 24 | seq.
 */
static void
prop_process_binary_schedule(pbt_tc *tc)
{
	__dtfl_allocator_builder *b[PEERS];
	build_slot slot[PEERS];
	__dtfl_allocator *a[PEERS];
	__dtfl_push *push[PEERS][NCHAN][PEERS];
	__dtfl_pull *pull[PEERS][NCHAN];
	pthread_t th[PEERS];
	size_t sent[PEERS][NCHAN][PEERS];	/* [to][ch][from] */
	size_t recvd[PEERS][NCHAN][PEERS];
	size_t total_sent = 0, total_recvd = 0, w, c, t, f, i;
	size_t nsteps = (size_t)pbt_int(tc, 1, MAXSTEPS);
	step sched[MAXSTEPS];
	int64_t rounds;
	int bad = 0;

	/*
	 * Draw the whole schedule first: a draw can unwind (longjmp), and
	 * nothing may be allocated yet when it does.
	 */
	for (i = 0; i < nsteps; i++) {
		sched[i].op = (unsigned char)pbt_int(tc, 0, 3);
		sched[i].w = (unsigned char)pbt_int(tc, 0, PEERS - 1);
		sched[i].c = (unsigned char)pbt_int(tc, 0, NCHAN - 1);
		sched[i].t = (unsigned char)pbt_int(tc, 0, PEERS - 1);
	}
	memset(sent, 0, sizeof sent);
	memset(recvd, 0, sizeof recvd);
	__dtfl_process_binary_builders(PEERS, b);
	for (w = 0; w < PEERS; w++) {
		slot[w].b = b[w];
		if (pthread_create(&th[w], NULL, build_thread, &slot[w]) != 0)
			abort();
	}
	for (w = 0; w < PEERS; w++)
		(void)pthread_join(th[w], NULL);
	for (w = 0; w < PEERS; w++) {
		a[w] = slot[w].a;
		for (c = 0; c < NCHAN; c++)
			a[w]->ops->allocate(a[w], 4 + 4 * c, &u64_class,
			    push[w][c], &pull[w][c]);
	}

	for (i = 0; i < nsteps && !bad; i++) {
		int op = sched[i].op;

		w = sched[i].w;
		c = sched[i].c;
		t = sched[i].t;
		if (op == 0 && total_sent < MAXMSG) {
			uint64_t v;

			v = ((uint64_t)w << 32) | ((uint64_t)c << 24) |
			    sent[t][c][w];
			sent[t][c][w]++;
			total_sent++;
			__dtfl_push_send(push[w][c][t], &u64_class,
			    __dtfl_memdup(&v, 8));
		} else if (op == 1) {
			a[w]->ops->release(a[w]);
		} else if (op == 2) {
			a[w]->ops->receive(a[w]);
		} else if (op == 3) {
			uint64_t *m;

			m = pull[w][c]->ops->pull(pull[w][c]);
			if (m != NULL) {
				f = (size_t)(*m >> 32);
				bad = f >= PEERS || ((*m >> 24) & 0xff) != c ||
				    (*m & 0xffffff) != recvd[w][c][f];
				if (!bad)
					recvd[w][c][f]++;
				total_recvd++;
				__dtfl_free(m);
			}
		}
	}

	/* Drain: everything in flight arrives, nothing extra. */
	for (rounds = 0; rounds < 4 && !bad; rounds++) {
		for (w = 0; w < PEERS; w++) {
			a[w]->ops->release(a[w]);
			a[w]->ops->receive(a[w]);
			__dtfl_vec_clear(&a[w]->events.ids);
		}
		for (w = 0; w < PEERS && !bad; w++) {
			for (c = 0; c < NCHAN && !bad; c++) {
				uint64_t *m;

				while (!bad && (m = pull[w][c]->ops->pull(
				    pull[w][c])) != NULL) {
					f = (size_t)(*m >> 32);
					bad = f >= PEERS ||
					    ((*m >> 24) & 0xff) != c ||
					    (*m & 0xffffff) != recvd[w][c][f];
					if (!bad)
						recvd[w][c][f]++;
					total_recvd++;
					__dtfl_free(m);
				}
			}
		}
	}

	for (w = 0; w < PEERS; w++) {
		for (c = 0; c < NCHAN; c++) {
			for (t = 0; t < PEERS; t++)
				push[w][c][t]->ops->destroy(push[w][c][t]);
			pull[w][c]->ops->destroy(pull[w][c]);
		}
		a[w]->ops->receive(a[w]);
	}
	for (w = 0; w < PEERS; w++)
		a[w]->ops->destroy(a[w]);

	PBT_ASSERT(!bad);
	PBT_ASSERT(total_recvd == total_sent);
	PBT_ASSERT(memcmp(sent, recvd, sizeof sent) == 0);
}

static const pbt_prop props[] = {
	{ "header_round_trip", prop_header_round_trip, 500 },
	{ "try_read_in_bounds", prop_try_read_in_bounds, 500 },
	{ "process_binary_schedule", prop_process_binary_schedule, 200 },
};

PBT_MAIN("wire", props)
