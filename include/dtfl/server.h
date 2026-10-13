/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/server.h: the live DDIR server.
 *
 * A server hosts interpreted DDIR programs in one running computation and
 * lets them share results by name.  Programs are installed one at a time;
 * each may import traces that other programs have exported, so a consumer
 * maintains a computation incrementally over a producer's output.  This is
 * the interpreter-driven form of the old dd_server: "install" renders a
 * DDIR program against a live registry, not a loaded shared object.
 *
 * The server owns a single in-process worker.  Commands run on the calling
 * thread:
 *
 *   install  builds a program's dataflow over imported traces and
 *            positional inputs, publishing each export's trace by name.
 *   feed     stages an input update at a chosen time (default: the current
 *            epoch), so inputs can be scheduled into the future.
 *   tick     closes the current epoch, advances every input, and runs to
 *            quiescence.
 *   drop     evicts a program, gated on its published traces having no live
 *            importer, and removes its dataflow outright.
 *   peek     snapshots a registered trace's consolidated contents.
 *   list     reports the registry (epoch, traces, programs).
 *
 * The host (outer) time is a u64 epoch shared across all programs.  A
 * program body renders in one iterative dynamic scope, so iterate pushes a
 * coordinate without changing the host time.
 *
 * Generated sources are installed on demand when first imported, addressed
 * by content so two importers of the same recipe share one source:
 *   "random:nodes=N,edges=E[,arity=A][,seed=S]"  a deterministic graph;
 *   "iota:N"                                      the rows (0) .. (N-1);
 *   "clock"                                       one row holding the epoch,
 *                                                 advanced by one each tick.
 */
#ifndef DTFL_SERVER_H
#define DTFL_SERVER_H

#include "dtfl/base.h"
#include "dtfl/ddir.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A live DDIR server.  Owned by the caller, released with
 * dtfl_ddir_server_free. */
typedef struct dtfl_ddir_server dtfl_ddir_server;

/*
 * Create a server with the host clock at epoch 0.  peek/list and the
 * command status lines are written to `out`; if `out` is NULL they go to
 * stdout.  Returns NULL only through the panic hook (allocation failure).
 */
DTFL_API dtfl_ddir_server *dtfl_ddir_server_new(FILE *out);

DTFL_API void dtfl_ddir_server_free(dtfl_ddir_server *s);

/* The current open epoch. */
DTFL_API uint64_t dtfl_ddir_server_epoch(const dtfl_ddir_server *s);

/* Whether a trace is registered under `name`. */
DTFL_API bool dtfl_ddir_server_has_trace(const dtfl_ddir_server *s,
    const char *name);

/*
 * Install `program` under `name`: build its dataflow, wiring each root
 * trace import to a registered trace (installing a generated source on
 * demand) and registering each export's trace for later imports.  The
 * server does not take ownership of `program`; it keeps a private copy.
 * Returns 0 on success, or a DTFL_E* code with a message through
 * dtfl_last_error_detail (name already installed, unknown import, export
 * name already published).
 */
DTFL_API int dtfl_ddir_server_install(dtfl_ddir_server *s, const char *name,
    const dtfl_ddir_program *program);

/*
 * Stage an update to positional input `input` of installed program `prog`:
 * add (key, val) with multiplicity `diff` at `time` (when `has_time`), else
 * at the current epoch.  The time must be at or after the current epoch.
 * The server copies `key` and `val`.  Returns 0 or a DTFL_E* code.
 */
DTFL_API int dtfl_ddir_server_feed(dtfl_ddir_server *s, const char *prog,
    size_t input, const dtfl_ddir_value *key, const dtfl_ddir_value *val,
    bool has_time, uint64_t time, int64_t diff);

/* Close the current epoch, advance every input, and run to quiescence. */
DTFL_API void dtfl_ddir_server_tick(dtfl_ddir_server *s);

/*
 * Drop installed program `name`, releasing its dataflow.  Refuses (changing
 * nothing) if any trace it publishes still has a live importer.  Returns 0
 * or a DTFL_E* code.
 */
DTFL_API int dtfl_ddir_server_drop(dtfl_ddir_server *s, const char *name);

/*
 * Snapshot a registered trace and print its consolidated contents (as of
 * the current epoch) to the server's output stream.  When `key` is not
 * NULL, restricts to that key.  Returns 0 or a DTFL_E* code (unknown
 * trace).
 */
DTFL_API int dtfl_ddir_server_peek(dtfl_ddir_server *s, const char *trace,
    const dtfl_ddir_value *key);

/* Print the registry (epoch, traces with importer counts, programs). */
DTFL_API void dtfl_ddir_server_list(const dtfl_ddir_server *s);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_SERVER_H */
