/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/dd_server.h: the shared-dataflow plugin server (dd_server).
 *
 * This is the differential-dataflow `server` crate: a long-running host for
 * several dataflows ("plugins") that share arrangements by name.  The point
 * of the crate is that one arranged (indexed) collection can feed many
 * queries: a plugin publishes a trace under a name, and other plugins import
 * that same trace into their own dataflows, so the cost of indexing the
 * collection is paid once and shared across every importer (the shared
 * TraceHandle of server/src/lib.rs).
 *
 * A dtfl_dd_server owns a single in-process worker, a registry of named
 * shared traces (the TraceHandler of lib.rs), one common probe, and the
 * plugins it has loaded.  A plugin is a named build callback; "load"ing it
 * builds one new dataflow on the shared worker, giving the callback an
 * Environment through which it imports shared traces, registers new ones,
 * and creates inputs the server then drives.
 *
 * Divergence from upstream: upstream loads plugins as dynamic libraries
 * through libloading (Library::new / Symbol), and dd_server.rs reads load
 * commands naming a shared-object path and a symbol.  Library code here may
 * not dlopen (AGENTS.md section 5, the api_discipline gate), so a plugin is
 * a statically registered dtfl_dataflow_plugin: a name plus a build
 * callback, linked into the server or the examples rather than opened at
 * run time.  Everything else (the shared TraceHandler, the per-load
 * dataflow, the common probe, the serialized command loop) is kept.
 *
 * The host (outer) time is a u64 epoch shared across all plugins, advanced
 * by one each "tick", exactly as the DDIR server's epoch (dtfl/server.h).
 * Commands run on the calling thread; a load builds a dataflow and runs the
 * worker to quiescence, so the server has no accept loop to hang on.
 */
#ifndef DTFL_DD_SERVER_H
#define DTFL_DD_SERVER_H

#include "dtfl/arrange.h"
#include "dtfl/base.h"
#include "dtfl/collection.h"
#include "dtfl/record.h"
#include "dtfl/timely.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* The trace handler and the plugin environment                        */
/* ------------------------------------------------------------------ */

/*
 * The shared trace registry (TraceHandler).  Maps a name to a shareable
 * trace handle.  A plugin's build callback reads it to import a trace
 * another plugin published and writes it to publish one of its own.  Owned
 * by the server; a plugin borrows it only for the duration of its build.
 */
typedef struct dtfl_trace_handler dtfl_trace_handler;

/*
 * Borrow the trace registered under `name`, or NULL if none is (reporting
 * DTFL_EINVAL through dtfl_last_error_detail).  The returned handle belongs
 * to the handler; a plugin imports it with dtfl_trace_import and must not
 * free it.
 */
DTFL_API dtfl_trace *dtfl_trace_handler_get(dtfl_trace_handler *h,
    const char *name);

/*
 * Publish `trace` under `name`, taking ownership of the handle (the server
 * frees it when the registry drops it).  `record_type` describes the full
 * record the trace indexes (key || value), retained by the handler so peek
 * can format and the caller can compare; pass NULL to leave it unknown.  A
 * second publish under a live name replaces the old handle.  This is
 * TraceHandler::set.
 */
DTFL_API void dtfl_trace_handler_set(dtfl_trace_handler *h, const char *name,
    dtfl_trace *trace, const dtfl_type *record_type);

/* Whether a trace is registered under `name` (TraceHandler::keys lookup). */
DTFL_API bool dtfl_trace_handler_has(const dtfl_trace_handler *h,
    const char *name);

/*
 * The environment a plugin's build callback receives (the Environment tuple
 * of lib.rs): the dataflow scope to build in, the shared trace handler, the
 * common probe to attach outputs to, the host timer in nanoseconds since the
 * server started, and the load's positional string arguments.  A plugin
 * that creates its own input (new_collection_from) registers it through
 * dtfl_dd_env_add_input so the server advances it each tick.
 */
typedef struct dtfl_dd_env dtfl_dd_env;

DTFL_API dtfl_scope *dtfl_dd_env_scope(dtfl_dd_env *e);
DTFL_API dtfl_trace_handler *dtfl_dd_env_handles(dtfl_dd_env *e);
DTFL_API dtfl_probe *dtfl_dd_env_probe(dtfl_dd_env *e);
DTFL_API uint64_t dtfl_dd_env_elapsed_ns(const dtfl_dd_env *e);
DTFL_API size_t dtfl_dd_env_argc(const dtfl_dd_env *e);
DTFL_API const char *dtfl_dd_env_arg(const dtfl_dd_env *e, size_t i);

/*
 * Hand an input session the plugin created to the server, which advances
 * and flushes it to the current epoch each tick.  The server takes
 * ownership and closes it when the plugin is dropped.  The plugin seeds its
 * constant data before registering (reachability's single root, for
 * example); the session must already be added to the environment's scope.
 */
DTFL_API void dtfl_dd_env_add_input(dtfl_dd_env *e, dtfl_input_session *in);

/*
 * A plugin's build callback.  It builds one dataflow in the environment's
 * scope, importing and publishing shared traces through the handler and
 * attaching its outputs to the common probe.  Returns 0 on success or a
 * DTFL_E* code with a message through dtfl_last_error_detail (the Rust
 * Result<(), String>); on error the server tears the half-built dataflow
 * down.
 */
typedef int (*dtfl_dd_build_fn)(dtfl_dd_env *env);

/* A named plugin: the static-registration stand-in for a shared object. */
typedef struct dtfl_dataflow_plugin {
	const char *name;	 /* the name "load" selects it by */
	dtfl_dd_build_fn build;  /* builds the dataflow (the `build` symbol) */
} dtfl_dataflow_plugin;

/* ------------------------------------------------------------------ */
/* The server                                                          */
/* ------------------------------------------------------------------ */

/* A plugin server.  Owned by the caller, released with dtfl_dd_server_free. */
typedef struct dtfl_dd_server dtfl_dd_server;

/*
 * Create a server with the host clock at epoch 0 and the elapsed timer
 * started now.  Status, list and output lines go to `out`; if `out` is NULL
 * they go to stdout.  Returns NULL only through the panic hook (allocation
 * failure).
 */
DTFL_API dtfl_dd_server *dtfl_dd_server_new(FILE *out);

DTFL_API void dtfl_dd_server_free(dtfl_dd_server *s);

/* The current open epoch (the host RootTime). */
DTFL_API uint64_t dtfl_dd_server_epoch(const dtfl_dd_server *s);

/* The shared trace handler, for a test to confirm a trace is shared. */
DTFL_API dtfl_trace_handler *dtfl_dd_server_handles(dtfl_dd_server *s);

/*
 * Register a plugin so "load" can select it by name.  The server keeps the
 * struct by value (name and callback), so the caller's storage need not
 * outlive the call.  A second registration under the same name replaces the
 * first.  This is the static-registration stand-in for Library::new; call
 * it for each plugin before loading.
 */
DTFL_API void dtfl_dd_server_register(dtfl_dd_server *s,
    const dtfl_dataflow_plugin *plugin);

/*
 * Load a registered plugin under instance name `instance`: build a new
 * dataflow on the shared worker, passing the plugin's build callback an
 * environment with `argc`/`argv` positional arguments.  Several loads of
 * one plugin under different instance names coexist, each its own dataflow
 * sharing the same imported traces.  Returns 0 on success, or a DTFL_E*
 * code with a message through dtfl_last_error_detail (unknown plugin,
 * instance already loaded, or the build callback's error).
 */
DTFL_API int dtfl_dd_server_load(dtfl_dd_server *s, const char *instance,
    const char *plugin, size_t argc, const char *const *argv);

/* Close the current epoch, advance every plugin input, and run to quiescence. */
DTFL_API void dtfl_dd_server_tick(dtfl_dd_server *s);

/*
 * Drop a loaded instance by name, releasing its dataflow and the inputs it
 * registered.  Traces it published stay in the handler for importers that
 * still reference them, matching dd_server.rs "drop" (which removes only the
 * named handler entries).  Returns 0 or a DTFL_E* code (no such instance).
 */
DTFL_API int dtfl_dd_server_drop(dtfl_dd_server *s, const char *instance);

/*
 * Snapshot a registered trace and print its consolidated (key, val, diff)
 * contents as of the current epoch to the server's output stream.  Returns
 * 0 or a DTFL_E* code (unknown trace).  This has no upstream command (there
 * a dataflow inspects its own output); it is the port's way to read a shared
 * arrangement for a test or a session transcript.
 */
DTFL_API int dtfl_dd_server_peek(dtfl_dd_server *s, const char *trace);

/* Print the registry: epoch, each registered trace name, each instance. */
DTFL_API void dtfl_dd_server_list(const dtfl_dd_server *s);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_DD_SERVER_H */
