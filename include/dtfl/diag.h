/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/diag.h: the in-process diagnostics session and WebSocket server.
 *
 * This is the C port of the dataflow-diag "diagnostics" crate: a live
 * diagnostics console for timely and differential dataflow.  A session
 * subscribes to a worker's structured log streams (dtfl/log.h), keeps
 * the live set of operators, channels and statistics, and serves them
 * to browser clients over a WebSocket as a stream of JSON Frames.  The
 * upstream index.html and React console render the Frames unmodified
 * (PLAN D9).
 *
 * Typical use mirrors upstream's logging::register + Server::start:
 *
 *   dtfl_diag_session *s = dtfl_diag_session_new(worker);
 *   dtfl_diag_server *srv = NULL;
 *   if (dtfl_worker_index(worker) == 0)
 *           dtfl_diag_server_start(s, "127.0.0.1", 51371, &srv);
 *   // build and run the user dataflow; call dtfl_worker_step() as usual
 *   dtfl_diag_server_stop(srv);
 *   dtfl_diag_session_free(s);
 *
 * The session is worker-owned (PLAN section 5): create it, pump it and
 * free it on the worker thread.  The server runs its accept and send
 * loop on a background thread (dtfl_set_thread_hooks applies) and talks
 * to the session through an internal lock-free queue, so no session
 * handle crosses the thread boundary.
 *
 * The Frame JSON format (one Frame per WebSocket text message):
 *
 *   {"type":"Frame","ts_us":<u64>,"updates":[
 *     {"type":"Operator","id":<u>,"name":"...","addr":[...],"diff":<i64>},
 *     {"type":"Channel","id":<u>,"scope_addr":[...],
 *      "source":[<u>,<u>],"target":[<u>,<u>],"diff":<i64>},
 *     {"type":"Stat","kind":"Elapsed|Messages|...","id":<u>,"diff":<i64>}
 *   ]}
 *
 * The divergence from the pin (PLAN D12, ADR): the server binds
 * 127.0.0.1 by default where upstream binds 0.0.0.0, so operator names
 * and timings are not published to the network with no authentication.
 * Pass a different interface to dtfl_diag_server_start to restore the
 * upstream behavior.
 *
 * The manual page is dtfl_diag_session_new(3); the command-line tool
 * built on dtfl_diag_replay is dtfl-diag(1).
 */
#ifndef DTFL_DIAG_H
#define DTFL_DIAG_H

#include "dtfl/base.h"
#include "dtfl/log.h"
#include "dtfl/timely.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* In-process diagnostics session and server                           */
/* ------------------------------------------------------------------ */

/*
 * A diagnostics session bound to a worker.  Subscribes to the worker's
 * "timely" and "differential/arrange" log streams and maintains the
 * live diagnostic state.  Worker-owned.
 */
typedef struct dtfl_diag_session dtfl_diag_session;

/* A running WebSocket server; its accept loop runs on its own thread. */
typedef struct dtfl_diag_server dtfl_diag_server;

/*
 * Creates a diagnostics session on `w` and installs its log
 * subscribers.  Returns the session or NULL on a bad argument.  Free it
 * with dtfl_diag_session_free on the same worker thread, after stopping
 * any server started from it.
 */
DTFL_API dtfl_diag_session *dtfl_diag_session_new(dtfl_worker *w);
DTFL_API void dtfl_diag_session_free(dtfl_diag_session *s);

/*
 * Starts the WebSocket server for `s` on interface `iface` (for example
 * "127.0.0.1"; NULL means 127.0.0.1, the D12 default) and `port`.  On
 * success stores the server in *out and returns DTFL_OK; on failure
 * returns a DTFL_E* code with detail in dtfl_last_error_detail().  Call
 * on the session's worker thread; the server then runs its own thread.
 */
DTFL_API int dtfl_diag_server_start(dtfl_diag_session *s, const char *iface,
    uint16_t port, dtfl_diag_server **out);

/*
 * The port the server is listening on.  Equals the port passed to
 * dtfl_diag_server_start, or the kernel-chosen port if that was 0.
 */
DTFL_API uint16_t dtfl_diag_server_port(const dtfl_diag_server *srv);

/* Stops the server, joins its thread and frees it.  NULL is a no-op. */
DTFL_API void dtfl_diag_server_stop(dtfl_diag_server *srv);

/* ------------------------------------------------------------------ */
/* Recorded log-stream replay (the connect crate + ReplayWithShutdown)  */
/* ------------------------------------------------------------------ */

/*
 * Replays a recorded log stream and delivers decoded dtfl_log_batches to
 * a subscriber, the out-of-process counterpart of the in-process
 * subscriber API (dtfl/log.h).  The stream is the EventWriter framing of
 * Event<Duration, Vec<(Duration, Event)>> that a worker's log registry
 * produces (PLAN D9): a u64 LE length then one bincode-1 fixint
 * little-endian Event.  `stream` names the event enum to decode:
 * "timely", "differential/arrange" or "communication".
 *
 * dtfl_diag_replay_fd reads one file descriptor to end of stream (the
 * replay-with-shutdown stops cleanly at EOF).  dtfl_diag_replay_fds
 * reads `nfds` descriptors, one per source worker, round-robin, as
 * upstream's make_readers does.  The subscriber is called once per
 * decoded batch, with the batch and ctx; the batch is valid only for
 * the duration of the call.  Returns DTFL_OK at clean EOF, or a DTFL_E*
 * code (DTFL_EPROTO on malformed bytes, DTFL_EIO on a read error).
 */
DTFL_API int dtfl_diag_replay_fd(int fd, const char *stream,
    dtfl_log_subscriber_fn fn, void *ctx);
DTFL_API int dtfl_diag_replay_fds(int const *fds, size_t nfds,
    const char *stream, dtfl_log_subscriber_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_DIAG_H */
