/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/diag/diag.c: the diagnostics session lifecycle.
 *
 * Ported from differential-dataflow differential-dataflow-v0.25.1
 *   diagnostics/src/lib.rs
 * Copyright (c) 2019 Timely Dataflow Diagnostics contributors.
 *
 * lib.rs ties logging::register to server::Server::start: register
 * subscribes to the log streams and maintains the diagnostics state,
 * and the server serves it over a WebSocket.  The C session does the
 * same: dtfl_diag_session_new installs the subscriber (diag_log.c) on
 * the worker's "timely" and "differential/arrange" streams, and the
 * shared state (diag_int.h) carries the live operators, channels and
 * statistics so a connecting client is replayed the full snapshot
 * before following the incremental Frames.
 */
#include <string.h>
#include <unistd.h>

#include "diag/diag_int.h"
#include "base/os_thread.h"

/* ------------------------------------------------------------------ */
/* Shared state                                                        */
/* ------------------------------------------------------------------ */

__dtfl_diag_shared *
__dtfl_diag_shared_new(void)
{
	__dtfl_diag_shared *sh = __dtfl_calloc(1, sizeof *sh);

	pthread_mutex_init(&sh->lock, NULL);
	__dtfl_vec_init(&sh->operators, sizeof(__dtfl_diag_operator));
	__dtfl_vec_init(&sh->channels, sizeof(__dtfl_diag_channel));
	__dtfl_vec_init(&sh->stats, sizeof(__dtfl_diag_stat));
	__dtfl_vec_init(&sh->sched_starts, sizeof(__dtfl_diag_sched_start));
	__dtfl_vec_init(&sh->queue, sizeof(char *));
	atomic_store(&sh->running, 1);
	atomic_store(&sh->refcount, 1);
	return sh;
}

void
__dtfl_diag_shared_retain(__dtfl_diag_shared *sh)
{
	atomic_fetch_add(&sh->refcount, 1);
}

void
__dtfl_diag_shared_release(__dtfl_diag_shared *sh)
{
	size_t i;

	if (atomic_fetch_sub(&sh->refcount, 1) != 1)
		return;

	for (i = 0; i < sh->operators.len; i++) {
		__dtfl_diag_operator *op = __dtfl_vec_at(&sh->operators, i);

		__dtfl_free(op->name);
		__dtfl_free(op->addr);
	}
	for (i = 0; i < sh->channels.len; i++) {
		__dtfl_diag_channel *ch = __dtfl_vec_at(&sh->channels, i);

		__dtfl_free(ch->scope_addr);
	}
	for (i = 0; i < sh->queue.len; i++) {
		char **p = __dtfl_vec_at(&sh->queue, i);

		__dtfl_free(*p);
	}
	__dtfl_vec_free(&sh->operators);
	__dtfl_vec_free(&sh->channels);
	__dtfl_vec_free(&sh->stats);
	__dtfl_vec_free(&sh->sched_starts);
	__dtfl_vec_free(&sh->queue);
	pthread_mutex_destroy(&sh->lock);
	__dtfl_free(sh);
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static void
shared_free_cb(void *ctx)
{
	__dtfl_diag_shared_release(ctx);
}

dtfl_diag_session *
dtfl_diag_session_new(dtfl_worker *w)
{
	dtfl_diag_session *s;

	if (w == NULL) {
		(void)__dtfl_err(DTFL_EINVAL, "dtfl_diag_session_new: NULL "
		    "worker");
		return NULL;
	}
	s = __dtfl_calloc(1, sizeof *s);
	s->worker = w;
	s->shared = __dtfl_diag_shared_new();

	/*
	 * The subscriber shares the one shared-state refcount with the
	 * session; the registry owns a reference that it drops (via the
	 * ctx_free hook) when the worker is torn down.  Two streams point
	 * at the same ctx, so retain once per extra registration.
	 */
	__dtfl_diag_shared_retain(s->shared);
	(void)dtfl_worker_log_subscribe(w, "timely", __dtfl_diag_on_batch,
	    s->shared, shared_free_cb);
	__dtfl_diag_shared_retain(s->shared);
	(void)dtfl_worker_log_subscribe(w, "differential/arrange",
	    __dtfl_diag_on_batch, s->shared, shared_free_cb);
	return s;
}

void
dtfl_diag_session_free(dtfl_diag_session *s)
{
	if (s == NULL)
		return;
	__dtfl_diag_shared_release(s->shared);
	__dtfl_free(s);
}

/* ------------------------------------------------------------------ */
/* Server lifecycle                                                    */
/* ------------------------------------------------------------------ */

int
dtfl_diag_server_start(dtfl_diag_session *s, const char *iface, uint16_t port,
    dtfl_diag_server **out)
{
	dtfl_diag_server *srv;
	int fd = -1, rc;
	uint16_t bound = 0;

	if (s == NULL || out == NULL)
		return __dtfl_err(DTFL_EINVAL, "dtfl_diag_server_start: NULL "
		    "argument");
	if (iface == NULL)
		iface = "127.0.0.1";	/* D12: bind loopback by default */

	rc = __dtfl_diag_listen(iface, port, &fd, &bound);
	if (rc != DTFL_OK)
		return rc;

	srv = __dtfl_calloc(1, sizeof *srv);
	srv->shared = s->shared;
	__dtfl_diag_shared_retain(srv->shared);
	srv->listen_fd = fd;
	srv->port = bound;
	atomic_store(&srv->shared->running, 1);

	rc = __dtfl_thread_spawn("dtfl:diag", __dtfl_diag_server_loop, srv,
	    &srv->thread);
	if (rc != 0) {
		(void)close(fd);
		__dtfl_diag_shared_release(srv->shared);
		__dtfl_free(srv);
		return rc;
	}
	*out = srv;
	return DTFL_OK;
}

uint16_t
dtfl_diag_server_port(const dtfl_diag_server *srv)
{
	return srv != NULL ? srv->port : 0;
}

void
dtfl_diag_server_stop(dtfl_diag_server *srv)
{
	if (srv == NULL)
		return;
	atomic_store(&srv->shared->running, 0);
	if (!srv->joined) {
		(void)__dtfl_thread_join(srv->thread);
		srv->joined = true;
	}
	__dtfl_diag_shared_release(srv->shared);
	__dtfl_free(srv);
}
