/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * examples/embed/pg_extension.c: a worked PostgreSQL embedding skeleton.
 *
 * This shows how a host embeds libdtfl through the four host hooks
 * documented in dtfl_embedding(7): allocation, panic, the diagnostic
 * log, and thread creation.  It is a skeleton, not a loadable module:
 * the PostgreSQL symbols (MemoryContextAllocHuge, repalloc_huge,
 * pfree, ereport, elog, sigsetjmp/PG_exception_stack) are
 * stubbed here so the example compiles and runs with no PostgreSQL
 * installed, while the mapping to the real symbols is spelled out in
 * each hook.  To turn this into a real extension, delete the stubs,
 * include "postgres.h" and the headers each stub names, and build it
 * against the server (PGXS), keeping the hook bodies.
 *
 * The computation is a trivial differential dataflow (count distinct
 * residues of a small integer collection) so the example stays about
 * the embedding, not the dataflow.
 */
#define _POSIX_C_SOURCE 200809L	/* sigsetjmp, siglongjmp, sigjmp_buf */

#include "dtfl/base.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/reduce.h"
#include "dtfl/time.h"
#include "dtfl/timely.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* PostgreSQL stand-ins.  In a real extension these come from the      */
/* server headers; here they are the smallest stubs that let the       */
/* example build and run standalone.                                   */
/* ------------------------------------------------------------------ */

/* elog/ereport levels.  The real values come from utils/elog.h. */
enum { PG_DEBUG1, PG_LOG, PG_WARNING, PG_ERROR };

/* MaxAllocHugeSize from utils/memutils.h.  (MaxAllocSize, the
 * limit of plain palloc and repalloc, is 1 GB - 1: too small.) */
#define PG_MAX_ALLOC_HUGE_SIZE	(SIZE_MAX / 2)

static void pg_stub_fatal(const char *what);

/*
 * MemoryContextAllocHuge, repalloc_huge and pfree, in a dedicated
 * long-lived context (dtfl_cxt below).  The stubs use libc so the
 * example runs standalone, but they enforce the server's contract, so
 * a hook that breaks it fails here as it would crash a backend:
 * repalloc and pfree need a pointer palloc returned (NULL crashes), and
 * plain palloc and repalloc refuse more than MaxAllocSize (1 GB - 1),
 * which a large arrangement's columns exceed.  The _huge forms allow up
 * to MaxAllocHugeSize.
 */
static void *
pg_alloc_huge(size_t n)
{
	if (n > PG_MAX_ALLOC_HUGE_SIZE)
		pg_stub_fatal("invalid memory alloc request size");
	return malloc(n);
}

static void *
pg_repalloc_huge(void *p, size_t n)
{
	if (p == NULL)
		pg_stub_fatal("repalloc of a NULL pointer");
	if (n > PG_MAX_ALLOC_HUGE_SIZE)
		pg_stub_fatal("invalid memory alloc request size");
	return realloc(p, n);
}

static void
pg_pfree(void *p)
{
	if (p == NULL)
		pg_stub_fatal("pfree of a NULL pointer");
	free(p);
}

/* A real ereport(ERROR, ...) does not return: it longjmps to the
 * nearest PG_TRY via PG_exception_stack.  The stub models that with a
 * sigjmp_buf the worker sets up, so the panic hook's longjmp path is
 * actually exercised. */
static sigjmp_buf pg_error_jmp;
static int pg_in_try;
static char pg_error_msg[256];

static void
pg_elog(int level, const char *msg)
{
	static const char *tag[] = { "DEBUG1", "LOG", "WARNING", "ERROR" };

	fprintf(stderr, "elog(%s): %s\n", tag[level], msg);
	if (level == PG_ERROR) {
		/* ereport(ERROR) unwinds to the PG_TRY site. */
		snprintf(pg_error_msg, sizeof pg_error_msg, "%s", msg);
		if (pg_in_try)
			siglongjmp(pg_error_jmp, 1);
		abort();
	}
}

/* A crash in a real backend; here, a message and abort(). */
static void
pg_stub_fatal(const char *what)
{
	fprintf(stderr, "pg stub: %s\n", what);
	abort();
}

/* ------------------------------------------------------------------ */
/* The four libdtfl host hooks, mapped to PostgreSQL.                  */
/* ------------------------------------------------------------------ */

/*
 * 1. Allocation -> MemoryContextAllocHuge / repalloc_huge / pfree in
 * a context made for the dataflow (in a real extension,
 * AllocSetContextCreate(TopMemoryContext, "dtfl", ...)).  The hooks
 * are set once per process, so ctx is the address of the variable
 * naming the current context, not a context: a host with several
 * workers points it at a worker's context before calling into that
 * worker.  Every byte lives in that context, so discarding the worker
 * after an error is MemoryContextReset, which keeps the context valid
 * for the rebuild (see the panic hook).  The _huge forms matter: plain
 * palloc stops at 1 GB.  libdtfl never passes NULL to realloc_fn or
 * free_fn, but the realloc hook checks anyway, so it is also safe with
 * an older libdtfl.  palloc ereport(ERROR)s on OOM rather than
 * returning NULL, which unwinds like a panic.
 */
/*
 * The current context (a MemoryContext in a real extension; the stub
 * has one context, so a placeholder).  The hooks receive its address.
 */
typedef struct pg_context { int unused; } pg_context;
static pg_context the_context;
static pg_context *dtfl_cxt = &the_context;

static void *
hook_malloc(size_t size, void *ctx)
{
	/* MemoryContextAllocHuge(*(MemoryContext *)ctx, size) */
	(void)*(pg_context **)ctx;
	return pg_alloc_huge(size);
}

static void *
hook_realloc(void *p, size_t size, void *ctx)
{
	(void)*(pg_context **)ctx;
	return p != NULL ? pg_repalloc_huge(p, size) : pg_alloc_huge(size);
}

static void
hook_free(void *p, void *ctx)
{
	(void)ctx;
	pg_pfree(p);
}

/*
 * 2. Panic -> ereport(ERROR).  A contract violation or allocation
 * failure calls this; it must not return.  Mapping it to
 * ereport(ERROR) turns a libdtfl panic into a normal PostgreSQL error
 * that unwinds the current query rather than crashing the backend.
 *
 * The longjmp leaves the worker in an unknown state: a structure half
 * updated, a mutex held.  Nothing reached from it may be used again,
 * not even freed with dtfl_worker_free.  The catch site discards the
 * worker by resetting its memory context, and rebuilds from durable
 * state if it needs the dataflow again.  The same holds for an
 * ereport(ERROR) raised inside a callback libdtfl calls (an operator
 * function, a sink, the log hook): it unwinds through libdtfl's frames,
 * which is why the log hook below never raises ERROR.
 */
static void
hook_panic(const char *msg, void *ctx)
{
	(void)ctx;
	pg_elog(PG_ERROR, msg);
	/* pg_elog(PG_ERROR) does not return; this abort is the "if the
	 * hook returns, libdtfl calls abort()" backstop. */
	abort();
}

/* 3. Diagnostic log -> elog.  libdtfl's human-readable diagnostics map
 * to the server log at matching levels.  (This is separate from the
 * structured event logging reached through dtfl/log.h.) */
static void
hook_log(dtfl_log_level level, const char *msg, void *ctx)
{
	(void)ctx;
	switch (level) {
	case DTFL_LOG_DEBUG: pg_elog(PG_DEBUG1, msg); break;
	case DTFL_LOG_INFO:  pg_elog(PG_LOG, msg);    break;
	case DTFL_LOG_WARN:  pg_elog(PG_WARNING, msg); break;
	case DTFL_LOG_ERROR: pg_elog(PG_LOG, msg);    break;  /* not fatal here */
	}
}

/*
 * 4. Threads.  A PostgreSQL backend is single-threaded, and palloc,
 * ereport and the rest of the server are not thread-safe.  So: do NOT
 * set thread hooks and do NOT call dtfl_execute.  Drive one worker by
 * hand with dtfl_worker_new_standalone() and dtfl_worker_step(), so the
 * dataflow runs entirely on the backend's (or background worker's) own
 * thread, interleaved with its other work.  This is what run_dataflow()
 * below does.
 *
 * Thread hooks cannot be backed by background workers: a background
 * worker is a separate process, while the workers of one dtfl_execute
 * share one address space (their channels, batches and reference
 * counts are plain memory).  A spawn hook must start a thread.
 */

/* ------------------------------------------------------------------ */
/* The dataflow: count distinct residues mod 5 of 0..19.              */
/* ------------------------------------------------------------------ */

typedef struct result { int64_t rows; } result;

static void
sink(void *ctx, const void *rec, size_t len, const uint64_t *time,
    const void *diff)
{
	result *r = ctx;
	int64_t d;

	(void)len; (void)time;
	memcpy(&d, diff, sizeof d);		/* diff is native i64 */
	r->rows += d;
	pg_elog(PG_LOG, "dataflow produced a row");
}

typedef struct build_ctx {
	dtfl_input_session *in;
	dtfl_probe *probe;
	result *out;
} build_ctx;

static void
to_residue(void *ctx, const void *rec, size_t len, dtfl_collection_emit *out)
{
	unsigned char buf[8];

	(void)ctx; (void)len;
	dtfl_put_u64(buf, dtfl_get_u64(rec) % 5);
	dtfl_collection_emit_record(out, buf, 8);
}

static void
build(dtfl_scope *scope, void *arg)
{
	build_ctx *c = arg;
	dtfl_diff *i64 = dtfl_diff_i64();
	dtfl_collection *data = dtfl_input_session_to_collection(c->in, scope);
	dtfl_collection *res = dtfl_collection_map(data, dtfl_type_u64(),
	    to_residue, DTFL_NOCTX);
	dtfl_collection *dist = dtfl_collection_distinct(res, i64);
	dtfl_collection *ins = dtfl_collection_inspect(dist, sink,
	    DTFL_CTX(c->out, NULL));
	dtfl_collection *probed = dtfl_collection_probe_with(ins, c->probe);

	dtfl_collection_free(probed);
	dtfl_collection_free(ins);
	dtfl_collection_free(dist);
	dtfl_collection_free(res);
	dtfl_collection_free(data);
	dtfl_diff_free(i64);
}

/* Single-thread cooperative execution: build one standalone worker,
 * feed it, and step it by hand.  No dtfl_execute, no threads. */
static int64_t
run_dataflow(void)
{
	dtfl_worker *w = dtfl_worker_new_standalone();
	dtfl_time_class *tc = dtfl_time_u64();
	dtfl_diff *i64 = dtfl_diff_i64();
	build_ctx c;
	result out = { 0 };
	uint64_t t;
	int i;

	c.in = dtfl_input_session_new(dtfl_type_u64(), i64);
	c.probe = dtfl_probe_new();
	c.out = &out;
	dtfl_worker_dataflow(w, "pg-residues", tc, build, &c);

	for (i = 0; i < 20; i++) {
		unsigned char buf[8];

		dtfl_put_u64(buf, (uint64_t)i);
		dtfl_input_session_insert(c.in, buf, sizeof buf);
	}
	t = 1;
	dtfl_input_session_advance_to(c.in, &t);
	dtfl_input_session_flush(c.in);
	dtfl_input_session_close(c.in);
	while (dtfl_worker_step(w))
		;

	dtfl_probe_free(c.probe);
	dtfl_time_class_free(tc);
	dtfl_diff_free(i64);
	dtfl_worker_free(w);
	return out.rows;
}

/*
 * The extension entry.  In a real module this is the body of a
 * PG_FUNCTION_INFO_V1 SQL function or a bgworker main; here it is main()
 * so the skeleton runs standalone.  It installs the hooks once, wraps
 * the computation in a PG_TRY-equivalent so a libdtfl panic becomes a
 * caught error rather than a crash, and reports the result.
 */
int
main(void)
{
	dtfl_alloc_hooks alloc = { hook_malloc, hook_realloc, hook_free,
	    &dtfl_cxt };
	dtfl_thread_hooks threads;
	int64_t rows;

	/* Install the hooks before any libdtfl allocation.  Thread hooks
	 * are left unset: this backend drives one worker cooperatively. */
	if (dtfl_set_alloc_hooks(&alloc) != DTFL_OK) {
		fprintf(stderr, "dtfl_set_alloc_hooks failed\n");
		return 1;
	}
	dtfl_set_panic_hook(hook_panic, NULL);
	dtfl_set_log_hook(hook_log, NULL);
	(void)threads;	/* no thread hooks: see 4 above */

	/* PG_TRY: a libdtfl panic longjmps here via the panic hook, so the
	 * query errors out cleanly instead of crashing the backend.  The
	 * worker is then unusable: PG_CATCH resets the dtfl memory context
	 * (MemoryContextReset), which frees what the worker held on Linux,
	 * and never calls into it again.  The context, and so the hooks,
	 * stay valid for a rebuild. */
	pg_in_try = 1;
	if (sigsetjmp(pg_error_jmp, 1) != 0) {
		pg_in_try = 0;
		fprintf(stderr, "caught a dataflow error: %s\n", pg_error_msg);
		return 1;
	}
	rows = run_dataflow();
	pg_in_try = 0;

	printf("distinct residues: %lld\n", (long long)rows);
	return 0;
}
