/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/timely/wordcount.c: mirrors timely/examples/wordcount.rs.
 *
 * Each round sends one (text, diff) record; flat_map splits the text into
 * (word, diff) records; a WordCount operator (exchange by word length)
 * keeps a cumulative count per word and, once a time completes, emits
 * (word, running_count) for every word seen at that time.  inspect prints
 * "seen: (word, count)".  Upstream sends ("round", 1) for ten rounds, so
 * the output is ("round", 1), ("round", 2), ..., deterministic for a
 * check.
 */
#include "example_args.h"

#include "dtfl/record.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"
#include "dtfl/timely_ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* record type: pair(string, i64) for (text/word, diff/count). */
static const dtfl_type *
word_type(void)
{
	return dtfl_type_pair(dtfl_type_string(), dtfl_type_i64());
}

/* ------------------------------------------------------------------ */
/* flat_map: split text into (word, diff) records                      */
/* ------------------------------------------------------------------ */

static void
split_words(void *ctx, const uint64_t *time, const void *rec, size_t len,
    dtfl_emit *out)
{
	const dtfl_type *t = ctx;
	size_t toff, tlen, doff, dlen, i, start;
	const char *text;
	int64_t diff;

	toff = dtfl_type_tuple_offset(t, rec, len, 0, &tlen);
	doff = dtfl_type_tuple_offset(t, rec, len, 1, &dlen);
	(void)time;
	if (toff == SIZE_MAX || doff == SIZE_MAX)
		return;
	{
		size_t plen;
		const void *p = dtfl_get_bytes((const unsigned char *)rec + toff,
		    &plen);

		text = p;
		tlen = plen;
	}
	diff = dtfl_get_i64((const unsigned char *)rec + doff);

	for (i = 0, start = 0; i <= tlen; i++)
		if (i == tlen || text[i] == ' ') {
			if (i > start) {
				unsigned char buf[128];
				size_t w = dtfl_put_bytes(buf, text + start,
				    i - start);

				w += dtfl_put_i64(buf + w, diff);
				dtfl_emit_record(out, buf, w);
			}
			start = i + 1;
		}
}

/* ------------------------------------------------------------------ */
/* WordCount: cumulative count per word, emitted when a time completes */
/* ------------------------------------------------------------------ */

typedef struct wc_entry {
	char *word;
	size_t wlen;
	int64_t count;
} wc_entry;

/* A per-time queue of (word, diff) still to apply. */
typedef struct wc_slot {
	uint64_t time;
	wc_entry *words;	/* words observed at this time (word, diff) */
	size_t n, cap;
} wc_slot;

typedef struct wc_ctx {
	const dtfl_type *type;
	wc_entry *counts;	/* cumulative per word */
	size_t ncounts, ccap;
	wc_slot *slots;
	size_t nslots, scap;
} wc_ctx;

static int64_t *
count_for(wc_ctx *c, const char *word, size_t wlen)
{
	size_t i;

	for (i = 0; i < c->ncounts; i++)
		if (c->counts[i].wlen == wlen &&
		    memcmp(c->counts[i].word, word, wlen) == 0)
			return &c->counts[i].count;
	if (c->ncounts == c->ccap) {
		c->ccap = c->ccap ? c->ccap * 2 : 8;
		c->counts = realloc(c->counts, c->ccap * sizeof *c->counts);
	}
	c->counts[c->ncounts].word = malloc(wlen);
	memcpy(c->counts[c->ncounts].word, word, wlen);
	c->counts[c->ncounts].wlen = wlen;
	c->counts[c->ncounts].count = 0;
	return &c->counts[c->ncounts++].count;
}

static wc_slot *
slot_for(wc_ctx *c, dtfl_op *op, dtfl_notificator *note, const uint64_t *t)
{
	size_t i;

	for (i = 0; i < c->nslots; i++)
		if (c->slots[i].time == t[0])
			return &c->slots[i];
	if (c->nslots == c->scap) {
		c->scap = c->scap ? c->scap * 2 : 4;
		c->slots = realloc(c->slots, c->scap * sizeof *c->slots);
	}
	memset(&c->slots[c->nslots], 0, sizeof c->slots[c->nslots]);
	c->slots[c->nslots].time = t[0];
	dtfl_notificator_notify_at(note, dtfl_op_input_cap_delayed(op, 0, 0, t));
	return &c->slots[c->nslots++];
}

static uint64_t
route_by_wordlen(const void *rec, size_t len, void *vctx)
{
	wc_ctx *c = vctx;
	size_t wlen, woff = dtfl_type_tuple_offset(c->type, rec, len, 0, &wlen);
	size_t plen;

	if (woff == SIZE_MAX)
		return 0;
	(void)dtfl_get_bytes((const unsigned char *)rec + woff, &plen);
	return plen;
}

static void
wc_logic(void *vctx, dtfl_op *op, dtfl_notificator *note)
{
	wc_ctx *c = vctx;
	dtfl_message *m;
	dtfl_cap *cap;
	uint64_t count;

	while ((m = dtfl_op_input_next(op, 0)) != NULL) {
		const uint64_t *t = dtfl_message_time(m);
		wc_slot *slot = slot_for(c, op, note, t);
		size_t i, k = dtfl_message_len(m);

		for (i = 0; i < k; i++) {
			size_t len, woff, wlen, doff, dlen, plen;
			const void *rec = dtfl_message_record(m, i, &len);
			const char *word;
			int64_t diff;

			woff = dtfl_type_tuple_offset(c->type, rec, len, 0, &wlen);
			doff = dtfl_type_tuple_offset(c->type, rec, len, 1, &dlen);
			if (woff == SIZE_MAX || doff == SIZE_MAX)
				continue;
			word = dtfl_get_bytes((const unsigned char *)rec + woff,
			    &plen);
			diff = dtfl_get_i64((const unsigned char *)rec + doff);
			if (slot->n == slot->cap) {
				slot->cap = slot->cap ? slot->cap * 2 : 8;
				slot->words = realloc(slot->words,
				    slot->cap * sizeof *slot->words);
			}
			slot->words[slot->n].word = malloc(plen);
			memcpy(slot->words[slot->n].word, word, plen);
			slot->words[slot->n].wlen = plen;
			slot->words[slot->n].count = diff;
			slot->n++;
		}
	}

	while (dtfl_notificator_next(note, &cap, &count)) {
		const uint64_t *t = dtfl_cap_time(cap);
		size_t i;

		for (i = 0; i < c->nslots; i++) {
			wc_slot *slot = &c->slots[i];
			dtfl_session *s;
			size_t j;

			if (slot->time != t[0])
				continue;
			s = dtfl_op_session(op, 0, cap);
			for (j = 0; j < slot->n; j++) {
				int64_t *entry = count_for(c, slot->words[j].word,
				    slot->words[j].wlen);
				unsigned char buf[128];
				size_t w;

				*entry += slot->words[j].count;
				w = dtfl_put_bytes(buf, slot->words[j].word,
				    slot->words[j].wlen);
				w += dtfl_put_i64(buf + w, *entry);
				dtfl_session_give(s, buf, w);
				free(slot->words[j].word);
			}
			dtfl_session_close(s);
			free(slot->words);
			slot->words = NULL;
			slot->n = slot->cap = 0;
			break;
		}
		dtfl_cap_drop(cap);
	}
}

static void
wc_free(void *vctx)
{
	wc_ctx *c = vctx;
	size_t i, j;

	for (i = 0; i < c->ncounts; i++)
		free(c->counts[i].word);
	free(c->counts);
	for (i = 0; i < c->nslots; i++) {
		for (j = 0; j < c->slots[i].n; j++)
			free(c->slots[i].words[j].word);
		free(c->slots[i].words);
	}
	free(c->slots);
	free(c);
}

/* ------------------------------------------------------------------ */
/* inspect                                                             */
/* ------------------------------------------------------------------ */

static void
inspect_seen(void *ctx, const uint64_t *time, const void *rec, size_t len)
{
	const dtfl_type *t = ctx;
	size_t woff, wlen, doff, dlen, plen;
	const char *word;

	(void)time;
	woff = dtfl_type_tuple_offset(t, rec, len, 0, &wlen);
	doff = dtfl_type_tuple_offset(t, rec, len, 1, &dlen);
	if (woff == SIZE_MAX || doff == SIZE_MAX)
		return;
	word = dtfl_get_bytes((const unsigned char *)rec + woff, &plen);
	printf("seen: (%.*s, %lld)\n", (int)plen, word,
	    (long long)dtfl_get_i64((const unsigned char *)rec + doff));
}

typedef struct build_ctx {
	dtfl_input *input;
	dtfl_probe *probe;
} build_ctx;

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	const dtfl_type *wt = word_type();
	wc_ctx *wc = calloc(1, sizeof *wc);
	dtfl_stream *s = dtfl_input_to_stream(c->input, scope);

	wc->type = wt;
	s = dtfl_stream_flat_map(s, wt, split_words, DTFL_CTX((void *)wt, NULL));
	s = dtfl_stream_unary_notify(s, dtfl_pact_exchange(route_by_wordlen, wc),
	    "WordCount", NULL, 0, wt, wc_logic, DTFL_CTX(wc, wc_free));
	s = dtfl_stream_inspect(s, inspect_seen, DTFL_CTX((void *)wt, NULL));
	dtfl_stream_probe_with(s, c->probe);
	dtfl_type_free(wt);
}

typedef struct worker_ctx { uint64_t rounds; } worker_ctx;

static void
worker(dtfl_worker *w, void *ctx)
{
	worker_ctx *wc = ctx;
	dtfl_time_class *tc = dtfl_time_u64();
	const dtfl_type *wt = word_type();
	build_ctx c;
	uint64_t round;

	c.input = dtfl_input_new(wt);
	c.probe = dtfl_probe_new();
	dtfl_worker_dataflow(w, "wordcount", tc, build, &c);
	dtfl_type_free(wt);

	for (round = 0; round < wc->rounds; round++) {
		unsigned char buf[64];
		uint64_t next = round + 1;
		size_t n = dtfl_put_bytes(buf, "round", 5);

		n += dtfl_put_i64(buf + n, 1);
		dtfl_input_send(c.input, buf, n);
		dtfl_input_advance_to(c.input, &next);
		while (dtfl_probe_less_than(c.probe, dtfl_input_time(c.input)))
			dtfl_worker_step(w);
	}
	dtfl_input_close(c.input);
	while (dtfl_worker_step(w))
		;
	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
}

int
main(int argc, char **argv)
{
	worker_ctx wc;
	dtfl_config *cfg = ex_config(argc, argv, 0);
	int rc;

	wc.rounds = 10;	/* upstream wordcount runs ten rounds */
	rc = dtfl_execute(cfg, worker, &wc, NULL);
	dtfl_config_free(cfg);
	return rc;
}
