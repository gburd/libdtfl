/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * src/diag/diag_int.h: internal types for the diagnostics session,
 * the WebSocket server and the Frame JSON builder.
 *
 * The session runs on the worker thread; the server runs on its own
 * thread.  They share a dtfl_diag_shared, guarded by one mutex: the
 * session appends Frame JSON payloads to a queue and keeps the live
 * state snapshot; the server drains the queue and, on a new client,
 * reads the snapshot to replay the full current state.  Only JSON byte
 * strings cross the boundary, so no worker-owned handle is shared
 * (PLAN section 5).
 */
#ifndef DTFL_DIAG_INT_H
#define DTFL_DIAG_INT_H

#include <pthread.h>
#include <stdatomic.h>

#include "base/base_int.h"
#include "base/os_thread.h"
#include "dtfl/diag.h"

/* ------------------------------------------------------------------ */
/* Diagnostic update kinds (logging.rs DiagnosticUpdate / StatKind)    */
/* ------------------------------------------------------------------ */

typedef enum __dtfl_diag_stat_kind {
	DIAG_STAT_ELAPSED = 0,
	DIAG_STAT_MESSAGES,
	DIAG_STAT_ARRANGEMENT_BATCHES,
	DIAG_STAT_ARRANGEMENT_RECORDS,
	DIAG_STAT_SHARING,
	DIAG_STAT_BATCHER_RECORDS,
	DIAG_STAT_BATCHER_SIZE,
	DIAG_STAT_BATCHER_CAPACITY,
	DIAG_STAT_BATCHER_ALLOCATIONS,
	DIAG_STAT__COUNT
} __dtfl_diag_stat_kind;

const char *__dtfl_diag_stat_kind_str(__dtfl_diag_stat_kind k);

/* A live operator: id -> (name, addr). */
typedef struct __dtfl_diag_operator {
	size_t id;
	char *name;		/* owned */
	size_t *addr;		/* owned */
	size_t addr_len;
} __dtfl_diag_operator;

/* A live channel: id -> (scope_addr, source, target). */
typedef struct __dtfl_diag_channel {
	size_t id;
	size_t *scope_addr;	/* owned */
	size_t scope_len;
	size_t src_node, src_port;
	size_t tgt_node, tgt_port;
} __dtfl_diag_channel;

/* A statistic value keyed by (kind, id); the running total. */
typedef struct __dtfl_diag_stat {
	__dtfl_diag_stat_kind kind;
	size_t id;
	int64_t value;
} __dtfl_diag_stat;

/* A pending Schedule Start time per operator (nanoseconds). */
typedef struct __dtfl_diag_sched_start {
	size_t id;
	uint64_t start_ns;
} __dtfl_diag_sched_start;

/* ------------------------------------------------------------------ */
/* Frame JSON builder                                                  */
/* ------------------------------------------------------------------ */

/*
 * Accumulates JsonUpdate objects for one Frame.  Build with
 * __dtfl_diag_frame_begin, add updates, then __dtfl_diag_frame_finish
 * returns the "{"type":"Frame",...}" payload as an owned C string (the
 * caller frees it with __dtfl_free), or NULL if no updates were added.
 */
typedef struct __dtfl_diag_frame {
	__dtfl_vec buf;		/* char: the "updates" array contents */
	size_t count;
} __dtfl_diag_frame;

void __dtfl_diag_frame_begin(__dtfl_diag_frame *f);
void __dtfl_diag_frame_add_operator(__dtfl_diag_frame *f, size_t id,
    const char *name, const size_t *addr, size_t addr_len, int64_t diff);
void __dtfl_diag_frame_add_channel(__dtfl_diag_frame *f, size_t id,
    const size_t *scope, size_t scope_len, size_t sn, size_t sp,
    size_t tn, size_t tp, int64_t diff);
void __dtfl_diag_frame_add_stat(__dtfl_diag_frame *f,
    __dtfl_diag_stat_kind kind, size_t id, int64_t diff);
char *__dtfl_diag_frame_finish(__dtfl_diag_frame *f, uint64_t ts_us);
void __dtfl_diag_frame_free(__dtfl_diag_frame *f);

/* Appends a JSON-escaped string (no surrounding quotes) to buf. */
void __dtfl_diag_json_escape(__dtfl_vec *buf, const char *s);

/* ------------------------------------------------------------------ */
/* WebSocket framing (RFC 6455)                                        */
/* ------------------------------------------------------------------ */

/* Fills out[20] with the SHA-1 of (data, len). */
void __dtfl_sha1(const void *data, size_t len, unsigned char out[20]);
/*
 * Base64-encodes (data, len) into out, NUL-terminated; out must hold at
 * least 4*((len+2)/3)+1 bytes.  Returns the string length.
 */
size_t __dtfl_base64_encode(const void *data, size_t len, char *out);

/*
 * Reads an HTTP Upgrade request on fd and replies with the RFC 6455
 * 101 handshake, computing Sec-WebSocket-Accept from the client key.
 * Returns DTFL_OK or a DTFL_E* code.  fd is a blocking socket with a
 * receive timeout set by the caller.
 */
int __dtfl_ws_handshake(int fd);
/*
 * Sends (data, len) as one unmasked WebSocket text frame on fd.
 * Returns DTFL_OK or DTFL_EIO.
 */
int __dtfl_ws_send_text(int fd, const char *data, size_t len);
/* Sends a close frame; ignores errors. */
void __dtfl_ws_send_close(int fd);

/* ------------------------------------------------------------------ */
/* Session and server                                                  */
/* ------------------------------------------------------------------ */

/*
 * Shared state between the worker-thread session and the server thread.
 * Guarded by `lock`.  The queue holds owned Frame JSON payloads the
 * session enqueued; the server drains and frees them.
 */
typedef struct __dtfl_diag_shared {
	pthread_mutex_t lock;

	/* Live state, for replaying the full snapshot to a new client. */
	__dtfl_vec operators;	/* __dtfl_diag_operator */
	__dtfl_vec channels;	/* __dtfl_diag_channel */
	__dtfl_vec stats;	/* __dtfl_diag_stat */
	__dtfl_vec sched_starts;	/* __dtfl_diag_sched_start */

	/* Pending Frame JSON payloads (char *), FIFO. */
	__dtfl_vec queue;	/* char * */

	atomic_int running;	/* server loop runs while nonzero */
	atomic_int refcount;	/* session + server each hold one */
} __dtfl_diag_shared;

struct dtfl_diag_session {
	dtfl_worker *worker;		/* not owned */
	__dtfl_diag_shared *shared;	/* refcounted, shared with server */
};

struct dtfl_diag_server {
	__dtfl_diag_shared *shared;	/* refcounted, shared with session */
	__dtfl_thread thread;
	int listen_fd;
	uint16_t port;
	bool joined;
};

/* Shared lifecycle. */
__dtfl_diag_shared *__dtfl_diag_shared_new(void);
void __dtfl_diag_shared_retain(__dtfl_diag_shared *sh);
void __dtfl_diag_shared_release(__dtfl_diag_shared *sh);

/* The subscriber installed on the worker's log streams. */
void __dtfl_diag_on_batch(const dtfl_log_batch *batch, void *ctx);

/* The server accept/serve loop (runs on its own thread). */
void *__dtfl_diag_server_loop(void *arg);

/*
 * Binds a listening socket on iface:port (port 0 lets the kernel pick),
 * leaves it non-blocking, and returns the bound port in *port_out.
 */
int __dtfl_diag_listen(const char *iface, uint16_t port, int *fd_out,
    uint16_t *port_out);

#endif /* DTFL_DIAG_INT_H */
