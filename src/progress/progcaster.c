/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/progress/progcaster.c: broadcasts progress updates among workers.
 *
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/src/progress/broadcast.rs
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * A progress message is (source worker, sequence number, change batch of
 * (Location, T) updates).  Upstream bincode-encodes it; the cluster data
 * plane is libdtfl-only (PLAN D9), so the encoding here is libdtfl's:
 * u64 LE source, u64 LE seq, u64 LE count, u32 LE key words, then count
 * entries of key words and an i64 delta, all little-endian.  Typed
 * allocators move the message without encoding.
 */
#include "progress/operate_int.h"
#include "timely/worker_int.h"

struct __dtfl_progcaster {
	__dtfl_push *pusher;	/* broadcast pusher */
	__dtfl_pull *puller;
	size_t source;
	size_t counter;
	size_t identifier;
	size_t channel;
	unsigned kw;		/* key words: 2 + depth */
};

/* A progress message: a change batch tagged with source and sequence. */
typedef struct progress_msg {
	uint64_t source;
	uint64_t seq;
	__dtfl_change_batch changes;
} progress_msg;

static size_t
pm_encoded_len(const __dtfl_msg_class *mc, const void *m)
{
	const progress_msg *p = m;

	(void)mc;
	return 8 + 8 + 8 + 8 + p->changes.updates.len * (p->changes.kw + 1) * 8;
}

static void
put_le64(unsigned char *o, uint64_t v)
{
	int i;

	for (i = 0; i < 8; i++)
		o[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t
get_le64(const unsigned char *p)
{
	uint64_t v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static void
pm_encode(const __dtfl_msg_class *mc, const void *m, unsigned char *out)
{
	const progress_msg *p = m;
	size_t n = p->changes.updates.len, w = p->changes.kw + 1, i;
	const uint64_t *e = (const uint64_t *)p->changes.updates.data;

	(void)mc;
	put_le64(out, p->source);
	put_le64(out + 8, p->seq);
	put_le64(out + 16, n);
	put_le64(out + 24, p->changes.kw);
	for (i = 0; i < n * w; i++)
		put_le64(out + 32 + 8 * i, e[i]);
}

static void *
pm_decode(const __dtfl_msg_class *mc, const __dtfl_bytes *b)
{
	progress_msg *p;
	uint64_t n, kw, i;

	(void)mc;
	if (b->len < 32)
		return NULL;
	n = get_le64(b->ptr + 16);
	kw = get_le64(b->ptr + 24);
	if (kw < 2 || kw > 2 + DTFL_TIME_MAX || n > (b->len - 32) / ((kw + 1) * 8) ||
	    b->len - 32 < n * (kw + 1) * 8)
		return NULL;
	p = __dtfl_calloc(1, sizeof *p);
	p->source = get_le64(b->ptr);
	p->seq = get_le64(b->ptr + 8);
	__dtfl_cb_init(&p->changes, (unsigned)kw);
	for (i = 0; i < n; i++) {
		uint64_t key[2 + DTFL_TIME_MAX];
		const unsigned char *at = b->ptr + 32 + i * (kw + 1) * 8;
		uint64_t k;

		for (k = 0; k < kw; k++)
			key[k] = get_le64(at + 8 * k);
		__dtfl_cb_update(&p->changes, key, (int64_t)get_le64(at + 8 * kw));
	}
	return p;
}

static void *
pm_clone(const __dtfl_msg_class *mc, const void *m)
{
	const progress_msg *src = m;
	progress_msg *p = __dtfl_calloc(1, sizeof *p);

	(void)mc;
	p->source = src->source;
	p->seq = src->seq;
	__dtfl_cb_init(&p->changes, src->changes.kw);
	__dtfl_cb_extend(&p->changes, &src->changes);
	return p;
}

static void
pm_free(const __dtfl_msg_class *mc, void *m)
{
	progress_msg *p = m;

	(void)mc;
	if (p == NULL)
		return;
	__dtfl_cb_free(&p->changes);
	__dtfl_free(p);
}

static const __dtfl_msg_class progress_class = {
	"progress", pm_encoded_len, pm_encode, pm_decode, pm_clone, pm_free, NULL
};

/*
 * Progcaster::new: allocates a broadcast channel for the subgraph at
 * `path` and logs it as a progress channel.
 */
__dtfl_progcaster *
__dtfl_progcaster_new(__dtfl_worker *w, const size_t *path, size_t path_len,
    size_t identifier, const dtfl_time_class *tc)
{
	__dtfl_progcaster *pc = __dtfl_calloc(1, sizeof *pc);

	pc->channel = __dtfl_worker_new_identifier(w);
	__dtfl_worker_broadcast(w, pc->channel, path, path_len, &progress_class,
	    &pc->pusher, &pc->puller);
	pc->source = w->allocator->index;
	pc->identifier = identifier;
	pc->kw = 2 + tc->depth;
	__dtfl_worker_log_comm_channel(w, pc->channel, true);
	return pc;
}

void
__dtfl_progcaster_free(__dtfl_progcaster *pc)
{
	if (pc == NULL)
		return;
	__dtfl_push_destroy(pc->pusher);
	__dtfl_pull_destroy(pc->puller);
	__dtfl_free(pc);
}

/*
 * Progcaster::send: compacts `changes` and, if non-empty, broadcasts
 * them with this worker's next sequence number.  `changes` is left
 * empty.
 */
void
__dtfl_progcaster_send(__dtfl_progcaster *pc, __dtfl_change_batch *changes)
{
	progress_msg *m;
	void *msg;

	__dtfl_cb_compact(changes);
	if (__dtfl_cb_is_empty(changes))
		return;
	m = __dtfl_calloc(1, sizeof *m);
	m->source = pc->source;
	m->seq = pc->counter;
	__dtfl_cb_init(&m->changes, changes->kw);
	__dtfl_cb_drain_into(changes, &m->changes);
	msg = m;
	__dtfl_push_push(pc->pusher, &msg);
	if (msg != NULL)
		pm_free(&progress_class, msg);
	__dtfl_push_done(pc->pusher);
	pc->counter++;
}

/* Progcaster::recv: absorbs every received progress message into `changes`. */
void
__dtfl_progcaster_recv(__dtfl_progcaster *pc, __dtfl_change_batch *changes)
{
	progress_msg *m;

	while ((m = __dtfl_pull_recv(pc->puller)) != NULL) {
		DTFL_ASSERT(m->changes.kw == changes->kw);
		__dtfl_cb_extend(changes, &m->changes);
		pm_free(&progress_class, m);
	}
}
