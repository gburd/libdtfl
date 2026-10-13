/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   differential-dataflow/src/columnar/trace/spill.rs
 *   differential-dataflow/examples/columnar_spill.rs (the FileStore backend)
 * Copyright (c) 2015 Frank McSherry and contributors.
 *
 * src/dd/trace/spill.c: the columnar spill, ported onto the one chunk layout.
 *
 * Chunk::settle is where committed chunks may be paged out: its seal hook
 * (__dtfl_spill_seal) consults the per-worker __dtfl_spill_state and, when the
 * resident high-water mark is exceeded, serializes the chunk's bytes and hands
 * them to a pluggable byte store (BytesStore), fetching them back on access.
 * With no spiller installed, seal is the identity and the chunk stays resident
 * (zero overhead beyond a NULL peek), exactly as upstream.
 *
 * Policy (spill.rs try_page): a per-worker high-water mark -- keep the first
 * `budget_records` worth of committed chunks resident, page the rest.  The
 * counter only rises, so paging is deterministic once the budget is reached.
 *
 * Transparency vs. footprint.  The whole-chunk operations here read a chunk's
 * length and rows directly (merge horizons, grading, the cursor), as upstream's
 * do through the resident bounds/len it keeps for a paged chunk.  libdtfl's
 * chunk is a __dtfl_batch with no split resident-metadata half, so a paged
 * chunk is fetched back (decoded from the store) at seal time rather than lazily
 * on first access.  That makes spilling a genuine serialize -> store -> load ->
 * decode round trip through secondary storage -- the correctness property the
 * spill tests assert (data intact after spill and reload) -- while keeping every
 * chunk operation working on resident rows unchanged.  Keeping a paged body on
 * disk until first touched (the memory-footprint win) is the deferred step; see
 * the module note in dev notes.  The store, counters, and policy are the full
 * upstream mechanism and are exercised end to end.
 *
 * The file store uses the standard file API (tmpfile/fwrite/fread): this is
 * secondary storage, not the configuration environment I/O the library forbids.
 * No getenv or signal is used.
 */
#include "dd/trace/chunk_int.h"

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* install / uninstall and the seal policy (spill.rs).                 */
/* ------------------------------------------------------------------ */

void
__dtfl_spill_install(__dtfl_chunk_ctx *x, __dtfl_spill_state *state,
    size_t budget_records, __dtfl_spill_store *store,
    __dtfl_spill_stats *stats)
{
	state->budget_records = budget_records;
	state->resident_records = 0;
	state->store = store;
	state->stats = stats;
	x->spill = state;
}

void
__dtfl_spill_uninstall(__dtfl_chunk_ctx *x)
{
	x->spill = NULL;
}

/*
 * __dtfl_spill_seal (spill.rs try_page + the fetch-back on access).  No spiller
 * or within budget: keep the chunk resident (identity).  Over the high-water
 * mark: serialize, store, count, then decode the stored bytes back into a fresh
 * chunk -- the full round trip through the store -- and return that in place of
 * the original.
 */
__dtfl_chunk *
__dtfl_spill_seal(const __dtfl_chunk_ctx *x, __dtfl_chunk *chunk, void *ctx)
{
	__dtfl_spill_state *st = x->spill;
	size_t records = __dtfl_batch_len(chunk);
	size_t enc_len, out_len;
	unsigned char *enc, *back;
	void *source;
	__dtfl_chunk *fetched;

	(void)ctx;
	if (st == NULL)
		return chunk;			/* resident: zero overhead */
	if (st->resident_records + records <= st->budget_records) {
		st->resident_records += records;
		return chunk;			/* within budget: keep resident */
	}

	/* Over the budget: serialize and page out. */
	enc_len = __dtfl_batch_encoded_len(chunk);
	enc = __dtfl_alloc(DTFL_MAX(enc_len, 1));
	__dtfl_batch_encode(chunk, enc);
	source = st->store->store(st->store, enc, enc_len);
	__dtfl_free(enc);
	__dtfl_batch_release(chunk);
	if (st->stats != NULL) {
		st->stats->spilled_chunks++;
		st->stats->spilled_records += records;
		st->stats->bytes_written += enc_len;
	}

	/* Fetch it back (decode) -- the inverse of the serialization. */
	back = st->store->load(st->store, source, &out_len);
	fetched = __dtfl_batch_decode(back, out_len);
	DTFL_ASSERT(fetched != NULL);
	__dtfl_free(back);
	st->store->drop(st->store, source);
	if (st->stats != NULL)
		st->stats->fetched_chunks++;
	return fetched;
}

/* ------------------------------------------------------------------ */
/* The file-backed reference store (columnar_spill.rs FileStore).      */
/* ------------------------------------------------------------------ */

/*
 * A source handle: the byte offset into the temp file and the blob length.
 * The file is created on the first store (tmpfile already unlinks it, so it
 * vanishes when the FILE closes), and the store appends length-prefixed blobs.
 */
typedef struct file_source {
	long offset;
	size_t len;
} file_source;

typedef struct file_store_ctx {
	FILE *fp;		/* NULL until the first store */
	long offset;		/* next append offset */
} file_store_ctx;

static void *
file_store(__dtfl_spill_store *s, const void *bytes, size_t len)
{
	file_store_ctx *fc = s->ctx;
	file_source *src;

	if (fc->fp == NULL) {
		fc->fp = tmpfile();
		if (fc->fp == NULL)
			__dtfl_panic("spill: tmpfile() failed");
		fc->offset = 0;
	}
	if (fseek(fc->fp, fc->offset, SEEK_SET) != 0)
		__dtfl_panic("spill: fseek failed");
	if (len != 0 && fwrite(bytes, 1, len, fc->fp) != len)
		__dtfl_panic("spill: fwrite short write");
	if (fflush(fc->fp) != 0)
		__dtfl_panic("spill: fflush failed");
	src = __dtfl_alloc(sizeof *src);
	src->offset = fc->offset;
	src->len = len;
	fc->offset += (long)len;
	return src;
}

static unsigned char *
file_load(__dtfl_spill_store *s, void *source, size_t *len)
{
	file_store_ctx *fc = s->ctx;
	file_source *src = source;
	unsigned char *buf = __dtfl_alloc(DTFL_MAX(src->len, 1));

	if (fseek(fc->fp, src->offset, SEEK_SET) != 0)
		__dtfl_panic("spill: fseek failed");
	if (src->len != 0 && fread(buf, 1, src->len, fc->fp) != src->len)
		__dtfl_panic("spill: fread short read");
	*len = src->len;
	return buf;
}

static void
file_drop(__dtfl_spill_store *s, void *source)
{
	(void)s;
	__dtfl_free(source);
}

__dtfl_spill_store *
__dtfl_spill_file_store_new(void)
{
	__dtfl_spill_store *s = __dtfl_alloc(sizeof *s);
	file_store_ctx *fc = __dtfl_alloc(sizeof *fc);

	fc->fp = NULL;
	fc->offset = 0;
	s->store = file_store;
	s->load = file_load;
	s->drop = file_drop;
	s->ctx = fc;
	return s;
}

void
__dtfl_spill_file_store_free(__dtfl_spill_store *s)
{
	file_store_ctx *fc;

	if (s == NULL)
		return;
	fc = s->ctx;
	if (fc->fp != NULL)
		fclose(fc->fp);		/* unlinked at create: frees the space */
	__dtfl_free(fc);
	__dtfl_free(s);
}
