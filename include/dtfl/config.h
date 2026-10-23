/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Greg Burd
 *
 * dtfl/config.h: execution configuration (timely Config,
 * communication Config, worker Config).
 *
 * A dtfl_config says how many workers to run and how they communicate:
 *   DTFL_COMM_THREAD          one worker on the calling thread's worker
 *   DTFL_COMM_PROCESS         `threads` workers in this process, typed
 *                             queues between them
 *   DTFL_COMM_PROCESS_BINARY  `threads` workers, serialized zero-copy
 *                             byte queues (the -z flag)
 *   DTFL_COMM_CLUSTER         `threads` workers in each of
 *                             `naddresses` processes over TCP
 *   DTFL_COMM_SIM             `threads` workers on one OS thread with a
 *                             seeded scheduler (PLAN D8)
 *
 * The manual page is dtfl_config_new(3).
 */
#ifndef DTFL_CONFIG_H
#define DTFL_CONFIG_H

#include "dtfl/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dtfl_comm_mode {
	DTFL_COMM_THREAD = 0,
	DTFL_COMM_PROCESS = 1,
	DTFL_COMM_PROCESS_BINARY = 2,
	DTFL_COMM_CLUSTER = 3,
	DTFL_COMM_SIM = 4
} dtfl_comm_mode;

typedef enum dtfl_progress_mode {
	DTFL_PROGRESS_DEMAND = 0,	/* upstream default */
	DTFL_PROGRESS_EAGER = 1
} dtfl_progress_mode;

typedef struct dtfl_config dtfl_config;

/* A new configuration: one thread, demand progress, no logging. */
DTFL_API dtfl_config *dtfl_config_new(void);
DTFL_API void dtfl_config_free(dtfl_config *cfg);

DTFL_API int dtfl_config_set_mode(dtfl_config *cfg, dtfl_comm_mode mode);
/* Worker threads per process (Process, ProcessBinary, Cluster, Sim). */
DTFL_API int dtfl_config_set_threads(dtfl_config *cfg, size_t threads);
/*
 * Cluster membership: this process's index and every process's
 * "host:port" listening address.  Copies the strings.  Does not change
 * the mode.
 */
DTFL_API int dtfl_config_set_cluster(dtfl_config *cfg, size_t process,
    const char *const *addresses, size_t naddresses);
/* Print connection progress (the -r flag). */
DTFL_API void dtfl_config_set_report(dtfl_config *cfg, bool report);
DTFL_API void dtfl_config_set_progress_mode(dtfl_config *cfg,
    dtfl_progress_mode mode);
/* Seed for the simulation scheduler (DTFL_COMM_SIM only). */
DTFL_API void dtfl_config_set_seed(dtfl_config *cfg, uint64_t seed);
/*
 * Hooks::spill with a file-backed strategy.  Every zero-copy byte queue
 * (DTFL_COMM_PROCESS_BINARY between workers, DTFL_COMM_CLUSTER between
 * a worker and its network threads) gets its own policy pair: once more
 * than head_reserve_bytes + threshold_bytes are queued, the middle of
 * the queue is written to an unlinked temporary file under `dir`, and
 * the reader pages it back in head_reserve_bytes at a time.  `dir` is
 * copied; NULL removes the hook.  Fails with DTFL_EIO if a file cannot
 * be created under `dir`.
 */
DTFL_API int dtfl_config_set_spill_file(dtfl_config *cfg, const char *dir,
    size_t threshold_bytes, size_t head_reserve_bytes);

/*
 * differential Config::idle_merge_effort: with effort > 0, every
 * arrangement, reduce and upsert operator reschedules itself until its
 * trace is compact, doing as much merge work each time as if `effort`
 * updates had arrived.  0 (the default) turns it off.  Fails with
 * DTFL_EINVAL for a negative effort.
 */
DTFL_API int dtfl_config_set_idle_merge_effort(dtfl_config *cfg,
    int64_t effort);
DTFL_API int64_t dtfl_config_idle_merge_effort(const dtfl_config *cfg);

DTFL_API dtfl_comm_mode dtfl_config_mode(const dtfl_config *cfg);
DTFL_API size_t dtfl_config_threads(const dtfl_config *cfg);
DTFL_API size_t dtfl_config_process(const dtfl_config *cfg);
DTFL_API size_t dtfl_config_processes(const dtfl_config *cfg);
DTFL_API const char *dtfl_config_address(const dtfl_config *cfg, size_t i);
DTFL_API bool dtfl_config_report(const dtfl_config *cfg);
DTFL_API dtfl_progress_mode dtfl_config_progress_mode(const dtfl_config *cfg);
DTFL_API uint64_t dtfl_config_seed(const dtfl_config *cfg);

/*
 * A byte transport between the workers of one execution (ADR-0013): the
 * port of upstream's Stream trait (zero_copy/stream.rs), which the
 * network loops are generic over.  One ordered byte stream per peer,
 * peers numbered 0 .. n-1 as the workers are.  The framing on it is the
 * cluster's (48-byte MessageHeaders, a zero-length header at the end);
 * the transport only moves bytes.
 *
 *   send      takes up to len bytes for `peer`, storing how many in
 *             *sent; 0 means "not now".  After a short send, libdtfl next
 *             offers that peer exactly the unsent rest of the same buffer.
 *   recv      copies up to cap bytes from `peer` into buf, storing how
 *             many in *got; 0 means none now.  Returns DTFL_EOF once the
 *             peer will send nothing more.
 *   wait      blocks until a peer may have bytes, a peer whose last send
 *             was short may take more, wake was called, or timeout_ns
 *             passes (UINT64_MAX: no limit).  Spurious returns are fine.
 *   wake      makes the current or next wait return.  Any thread may
 *             call it, at any time until the call using the transport
 *             returns.
 *   shutdown  (optional) the end-of-stream header has gone to `peer`.
 *   destroy   (optional) frees ctx; libdtfl never calls it, the owner
 *             does.
 *
 * send, recv and shutdown return 0 or a DTFL_E* code (DTFL_EIO with
 * dtfl_last_error_detail() for a failure); a failure panics the worker
 * with upstream's "timely communication error" message.  See
 * dtfl_transport(3).
 */
typedef struct dtfl_transport {
	int (*send)(void *ctx, size_t peer, const void *buf, size_t len,
	    size_t *sent);
	int (*recv)(void *ctx, size_t peer, void *buf, size_t cap,
	    size_t *got);
	void (*wait)(void *ctx, uint64_t timeout_ns);
	void (*wake)(void *ctx);
	int (*shutdown)(void *ctx, size_t peer);
	void (*destroy)(void *ctx);
	void *ctx;
} dtfl_transport;

/*
 * The built-in transport over connected stream sockets: fds[p] reaches
 * peer p, fds[self] is ignored (pass -1).  socketpair(2) before fork(2)
 * and TCP sockets the host connected both work.  The descriptors are
 * made non-blocking and wait uses poll(2) with a self-pipe; the
 * transport owns the descriptors from now on and its destroy closes
 * them.  Fails with DTFL_EINVAL or DTFL_EIO, leaving the descriptors
 * open.
 */
DTFL_API int dtfl_transport_socket(const int *fds, size_t nfds, size_t self,
    dtfl_transport *out);

/*
 * Where dtfl_execute() runs the workers of a Process or ProcessBinary
 * configuration: threads of this process (the default), or one child
 * process each, forked by libdtfl and joined over a socketpair mesh.
 * Fails with DTFL_EINVAL for an unknown value.  See dtfl_execute_worker(3)
 * for processes the host creates itself.
 */
typedef enum dtfl_workers {
	DTFL_WORKERS_THREADS = 0,
	DTFL_WORKERS_PROCESSES = 1
} dtfl_workers;
DTFL_API int dtfl_config_set_workers(dtfl_config *cfg, dtfl_workers how);
DTFL_API dtfl_workers dtfl_config_workers(const dtfl_config *cfg);

/*
 * Parses timely's command-line flags at the pin:
 *   -w, --threads NUM     worker threads per process (default 1)
 *   -p, --process IDX     this process's index (default 0)
 *   -n, --processes NUM   number of processes (default 1)
 *   -h, --hostfile FILE   process addresses, one per line
 *                         (default localhost:2101+i)
 *   -r, --report          report connection progress
 *   -z, --zerocopy        zero-copy intra-process channels
 *   --progress-mode MODE  "eager" or "demand"
 * Both "-w 4" and "-w4" forms, and "--threads=4", are accepted, as
 * getopts does.  Mode follows upstream: processes > 1 gives Cluster,
 * else threads > 1 gives Process (ProcessBinary with -z), else Thread.
 *
 * Arguments that are not timely flags are kept, in order, in
 * rest[0..*nrest): free arguments, options timely does not know (where
 * getopts would fail), and everything after "--" (which is dropped).
 * `rest` must have room for argc entries (it may be argv itself;
 * argv[0] is passed through as rest[0]).  On error cfg, rest and
 * *nrest are unchanged.  getopt(3) is not used and no global state is
 * touched.
 */
DTFL_API int dtfl_config_from_args(dtfl_config *cfg, int argc,
    char *const *argv, char **rest, int *nrest);

/*
 * Applies the opt-in environment settings (PLAN D9): TIMELY_WORKER_LOG_ADDR
 * and DIFFERENTIAL_LOG_ADDR.  `lookup` is called with each name and
 * returns the value or NULL; pass a wrapper around getenv(3).  The
 * library never reads the environment itself.
 */
DTFL_API int dtfl_config_from_env(dtfl_config *cfg,
    const char *(*lookup)(const char *name, void *ctx), void *ctx);
DTFL_API const char *dtfl_config_timely_log_addr(const dtfl_config *cfg);
DTFL_API const char *dtfl_config_differential_log_addr(const dtfl_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DTFL_CONFIG_H */
