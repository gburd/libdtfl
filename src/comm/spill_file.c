/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 * Ported from timely-dataflow timely-v0.31.0
 *   timely/examples/spill_compare.rs (mod file_spill, and the
 *   Hooks::spill factory its main installs)
 * Copyright (c) 2014 Frank McSherry and contributors.
 *
 * src/comm/spill_file.c: the file-backed BytesSpill strategy and the
 * SpillPolicyFn that dtfl_config_set_spill_file installs.
 *
 * The factory makes, per MergeQueue, a Threshold writer policy over a
 * fresh FileSpillStrategy and a PrefetchPolicy reader whose budget is
 * the head reserve, exactly as the example's closure does.  Each spill()
 * call writes its chunks back to back into one new temporary file and
 * returns one handle per chunk; the handles share the file, which closes
 * when the last of them is fetched or dropped.
 *
 * Differences from the example module, each deliberate:
 *   - The file is made with mkstemp(3) under `dir` and unlinked at once,
 *     so nothing is left behind if the process dies (tempfile_in does
 *     the same on Linux through O_TMPFILE).
 *   - A handle reads back only its own chunk with pread(2).  Upstream
 *     slurps the whole file into memory on the first fetch of any chunk
 *     and keeps it until every handle is gone, which can materialize
 *     more than the prefetch budget asked for.
 *   - A spill write error is reported through the diagnostic log hook
 *     and the chunks stay resident, as upstream (eprintln, no data
 *     taken).  A read error panics.  Upstream prints and returns the
 *     handle for a retry, but the file is our own unlinked temporary, so
 *     a failed read does not recover and the retry would spin the reader
 *     forever.
 *
 * Threads.  A strategy belongs to one queue's writer policy and runs
 * under that queue's mutex.  A handle runs under the same mutex on the
 * reader side, and may be destroyed by whichever handle drops the queue
 * last.  The file and the factory are shared across those threads
 * through atomic reference counts (__dtfl_arc); pread on a shared fd
 * keeps no file position, and each file is written once, before any of
 * its handles exist.  The counters are relaxed atomics: they order
 * nothing and are read after the threads that bump them are joined.
 */
#define _POSIX_C_SOURCE 200809L	/* mkstemp, pread, fcntl */

#include "comm/zc_int.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct spill_file_fn {
	__dtfl_spill_fn base;
	char *dir;			/* owned */
	size_t threshold_bytes;
	size_t head_reserve_bytes;
	atomic_uint_fast64_t files;
	atomic_uint_fast64_t spilled_chunks;
	atomic_uint_fast64_t spilled_bytes;
	atomic_uint_fast64_t fetched_chunks;
	atomic_uint_fast64_t failed_spills;
} spill_file_fn;

/* One temporary file, shared by the handles of one spill() call. */
typedef struct spill_file {
	__dtfl_arc ref;
	int fd;
	spill_file_fn *owner;		/* retained */
} spill_file;

/* ChunkHandle: one chunk at [off, off + len) of a file. */
typedef struct chunk_handle {
	__dtfl_bytes_fetch base;
	spill_file *file;		/* one reference */
	off_t off;
	size_t len;
} chunk_handle;

/* FileSpillStrategy, one per writer policy. */
typedef struct file_strategy {
	__dtfl_bytes_spill base;
	spill_file_fn *owner;		/* retained */
} file_strategy;

static void
count(atomic_uint_fast64_t *c, uint64_t n)
{
	atomic_fetch_add_explicit(c, n, memory_order_relaxed);
}

/*
 * A new unlinked temporary file under dir, close-on-exec.  Returns the
 * fd, or -1 with errno set.
 */
static int
temp_open(const char *dir)
{
	static const char suffix[] = "/dtfl-spill-XXXXXX";
	size_t n = strlen(dir);
	char *path = __dtfl_alloc(n + sizeof suffix);
	int fd, saved;

	memcpy(path, dir, n);
	memcpy(path + n, suffix, sizeof suffix);
	fd = mkstemp(path);
	if (fd >= 0) {
		(void)unlink(path);
		if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
			saved = errno;
			(void)close(fd);
			errno = saved;
			fd = -1;
		}
	}
	saved = errno;
	__dtfl_free(path);
	errno = saved;
	return fd;
}

static void
fn_release(spill_file_fn *f)
{
	__dtfl_spill_fn_release(&f->base);
}

static void
file_release(spill_file *f)
{
	if (!__dtfl_arc_release(&f->ref))
		return;
	(void)close(f->fd);
	fn_release(f->owner);
	__dtfl_free(f);
}

/* Writes all of [p, p + len) at offset off; false with errno on error. */
static bool
write_all(int fd, const unsigned char *p, size_t len, off_t off)
{
	while (len > 0) {
		ssize_t w = pwrite(fd, p, len, off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		p += w;
		len -= (size_t)w;
		off += w;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* BytesFetch for ChunkHandle                                          */
/* ------------------------------------------------------------------ */

static bool
handle_fetch(__dtfl_bytes_fetch *hh, __dtfl_vec *out)
{
	chunk_handle *h = (chunk_handle *)hh;
	__dtfl_bytes b;
	size_t got = 0;

	__dtfl_bytes_new(&b, h->len);
	while (got < h->len) {
		ssize_t r = pread(h->file->fd, b.ptr + got, h->len - got,
		    h->off + (off_t)got);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			__dtfl_panic("spill fetch: read of %zu bytes at %lld "
			    "failed: %s", h->len, (long long)h->off,
			    r < 0 ? strerror(errno) : "unexpected end of file");
		got += (size_t)r;
	}
	__dtfl_bytes_freeze(&b);
	__dtfl_vec_push(out, &b);
	count(&h->file->owner->fetched_chunks, 1);
	file_release(h->file);
	__dtfl_free(h);
	return true;
}

static void
handle_destroy(__dtfl_bytes_fetch *hh)
{
	chunk_handle *h = (chunk_handle *)hh;

	file_release(h->file);
	__dtfl_free(h);
}

static const __dtfl_bytes_fetch_ops handle_ops = {
	handle_fetch, handle_destroy
};

/* ------------------------------------------------------------------ */
/* BytesSpill for FileSpillStrategy                                    */
/* ------------------------------------------------------------------ */

/*
 * Writes every chunk to one new file, then takes them all: releases
 * each view and appends a handle for it.  On any error nothing is
 * taken.
 */
static void
strategy_spill(__dtfl_bytes_spill *ss, __dtfl_vec *chunks, __dtfl_vec *handles)
{
	file_strategy *s = (file_strategy *)ss;
	spill_file_fn *o = s->owner;
	spill_file *file;
	off_t off = 0;
	uint64_t bytes = 0;
	size_t i;
	int fd;

	if (chunks->len == 0)
		return;
	if ((fd = temp_open(o->dir)) < 0) {
		__dtfl_log(DTFL_LOG_WARN, "file spill failed: %s: %s", o->dir,
		    strerror(errno));
		count(&o->failed_spills, 1);
		return;
	}
	for (i = 0; i < chunks->len; i++) {
		__dtfl_bytes *c = DTFL_VEC_AT(chunks, __dtfl_bytes, i);

		if (!write_all(fd, c->ptr, c->len, off)) {
			__dtfl_log(DTFL_LOG_WARN, "file spill write failed: "
			    "%s", strerror(errno));
			(void)close(fd);
			count(&o->failed_spills, 1);
			return;
		}
		off += (off_t)c->len;
	}

	file = __dtfl_alloc(sizeof *file);
	__dtfl_arc_init(&file->ref);
	file->fd = fd;
	file->owner = (spill_file_fn *)__dtfl_spill_fn_retain(&o->base);
	off = 0;
	for (i = 0; i < chunks->len; i++) {
		__dtfl_bytes *c = DTFL_VEC_AT(chunks, __dtfl_bytes, i);
		chunk_handle *h = __dtfl_alloc(sizeof *h);

		h->base.ops = &handle_ops;
		h->file = file;
		if (i > 0)
			__dtfl_arc_retain(&file->ref);
		h->off = off;
		h->len = c->len;
		off += (off_t)c->len;
		bytes += c->len;
		__dtfl_vec_push(handles, &h);
		__dtfl_bytes_release(c);
	}
	count(&o->files, 1);
	count(&o->spilled_chunks, chunks->len);
	count(&o->spilled_bytes, bytes);
	__dtfl_vec_clear(chunks);
}

static void
strategy_destroy(__dtfl_bytes_spill *ss)
{
	file_strategy *s = (file_strategy *)ss;

	fn_release(s->owner);
	__dtfl_free(s);
}

static const __dtfl_bytes_spill_ops strategy_ops = {
	strategy_spill, strategy_destroy
};

/* ------------------------------------------------------------------ */
/* The SpillPolicyFn                                                   */
/* ------------------------------------------------------------------ */

static void
fn_make(__dtfl_spill_fn *ff, __dtfl_spill_policy **writer,
    __dtfl_spill_policy **reader)
{
	spill_file_fn *f = (spill_file_fn *)ff;
	file_strategy *s = __dtfl_alloc(sizeof *s);
	__dtfl_spill_threshold *tp;

	s->base.ops = &strategy_ops;
	s->owner = (spill_file_fn *)__dtfl_spill_fn_retain(&f->base);
	tp = __dtfl_spill_threshold_new(&s->base);
	tp->threshold_bytes = f->threshold_bytes;
	tp->head_reserve_bytes = f->head_reserve_bytes;
	*writer = &tp->base;
	*reader = &__dtfl_spill_prefetch_new(f->head_reserve_bytes)->base;
}

static void
fn_destroy(__dtfl_spill_fn *ff)
{
	spill_file_fn *f = (spill_file_fn *)ff;

	__dtfl_free(f->dir);
	__dtfl_free(f);
}

int
__dtfl_spill_file_fn_new(const char *dir, size_t threshold_bytes,
    size_t head_reserve_bytes, __dtfl_spill_fn **out)
{
	spill_file_fn *f;
	int fd;

	if (dir == NULL || *dir == '\0')
		return __dtfl_err(DTFL_EINVAL, "spill directory is empty");
	/* A zero prefetch budget never pages anything back in. */
	if (head_reserve_bytes == 0)
		return __dtfl_err(DTFL_EINVAL, "spill head reserve must be "
		    "at least 1 byte");
	if ((fd = temp_open(dir)) < 0)
		return __dtfl_err(DTFL_EIO, "spill directory %s: %s", dir,
		    strerror(errno));
	(void)close(fd);

	f = __dtfl_calloc(1, sizeof *f);
	__dtfl_arc_init(&f->base.ref);
	f->base.make = fn_make;
	f->base.destroy = fn_destroy;
	f->dir = __dtfl_strdup(dir);
	f->threshold_bytes = threshold_bytes;
	f->head_reserve_bytes = head_reserve_bytes;
	atomic_init(&f->files, 0);
	atomic_init(&f->spilled_chunks, 0);
	atomic_init(&f->spilled_bytes, 0);
	atomic_init(&f->fetched_chunks, 0);
	atomic_init(&f->failed_spills, 0);
	*out = &f->base;
	return 0;
}

void
__dtfl_spill_file_fn_stats(const __dtfl_spill_fn *ff,
    __dtfl_spill_file_stats *out)
{
	spill_file_fn *f = (spill_file_fn *)(uintptr_t)ff;

	out->files = atomic_load_explicit(&f->files, memory_order_relaxed);
	out->spilled_chunks = atomic_load_explicit(&f->spilled_chunks,
	    memory_order_relaxed);
	out->spilled_bytes = atomic_load_explicit(&f->spilled_bytes,
	    memory_order_relaxed);
	out->fetched_chunks = atomic_load_explicit(&f->fetched_chunks,
	    memory_order_relaxed);
	out->failed_spills = atomic_load_explicit(&f->failed_spills,
	    memory_order_relaxed);
}
