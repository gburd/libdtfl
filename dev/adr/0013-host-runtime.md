# ADR-0013 (D13): Host runtime: sync hooks, byte transports, process-hosted workers

- **Status:** Accepted, 2026-10-23.
- **Decision record:** this file.  dev/PLAN.md section 4 carries a short
  D13 summary that points here, and section 5 lists the hook globals
  this decision adds, once the code that defines them lands.

## Context

libdtfl already lets a host replace four process-wide facilities:
allocation, panic, the diagnostic log, and thread spawning (PLAN D6,
section 5).  Two kinds of host still cannot use it.

1. **A host whose concurrency is not pthreads.**  A fiber runtime such
   as libxtc multiplexes many tasks on a few carrier threads.  If
   libdtfl blocks a carrier in `pthread_cond_wait`, `futex(2)`,
   `nanosleep` or a `sched_yield` spin, every other task on that
   carrier stops, and a spin that waits for another task on the same
   carrier never ends.  The thread hook gives such a host the spawn,
   but every wait after the spawn still goes to the kernel.
2. **A host whose workers are processes.**  PostgreSQL runs each
   backend, each background worker and (with `io_method=worker`) each
   I/O worker as a separate process forked by the postmaster.  The
   processes share memory only through DSM segments and talk through
   `shm_mq` rings and latches.  libdtfl's only multi-process mode,
   `DTFL_COMM_CLUSTER`, needs a TCP address per process and starts a
   connect thread, an accept thread, and a send and a receive thread
   per peer.  A backend must not get threads at all: `palloc` and
   `ereport` are not thread-safe.

What libdtfl blocks or locks on today (inventory at 9f956dd, every
`src/` call site):

| Primitive | Call sites |
|---|---|
| `pthread_mutex_*` | `comm/alloc_process.c` (channel table), `comm/zc_bytes_exchange.c` (MergeQueue), `comm/zc_allocator_process.c` (rendezvous), `comm/buzzer.c` (non-futex parker), `timely/activate.c` (SyncActivations), `timely/event.c` (link_sync), `diag/diag.c`, `diag/diag_log.c`, `diag/ws_server.c` |
| `pthread_cond_*` | `comm/zc_allocator_process.c` (rendezvous), `comm/buzzer.c` (non-futex parker) |
| `futex(2)` | `comm/buzzer.c` (the Linux parker) |
| `sched_yield` | `comm/queue.c` (list-channel backoff), `comm/zc_allocator.c` and `comm/zc_tcp.c` (handoff spins) |
| `nanosleep` | `comm/zc_initialize.c` (connect retry), `diag/ws_server.c` (accept poll) |
| `clock_gettime` | `base/util.c` (`__dtfl_clock_ns`), `dd/dd_server.c` |
| `pthread_self` | `timely/worker.c`, `timely/worker_int.h` (debug owner check) |
| blocking sockets | `comm/zc_initialize.c`, `comm/zc_tcp.c`, `diag/ws_frame.c`, `diag/ws_server.c` |

The C11 atomics (the MPSC list channel, refcounts, the futex word) need
no hook: they never block.

## Decision

### 1. Synchronization hooks

A fifth process-wide hook, of the same kind as the other four:

```c
typedef struct dtfl_sync_hooks {
	int (*mutex_init)(void **mutex, void *ctx);
	void (*mutex_destroy)(void *mutex, void *ctx);
	void (*mutex_lock)(void *mutex, void *ctx);
	void (*mutex_unlock)(void *mutex, void *ctx);
	int (*cond_init)(void **cond, void *ctx);
	void (*cond_destroy)(void *cond, void *ctx);
	void (*cond_wait)(void *cond, void *mutex, uint64_t timeout_ns, void *ctx);
	void (*cond_signal)(void *cond, void *ctx);
	void (*cond_broadcast)(void *cond, void *ctx);
	void (*wait)(const uint32_t *word, uint32_t expect, uint64_t timeout_ns, void *ctx);
	void (*wake)(const uint32_t *word, void *ctx);
	void (*yield)(void *ctx);
	void (*sleep)(uint64_t ns, void *ctx);
	uint64_t (*clock)(void *ctx);
	uintptr_t (*self)(void *ctx);
	void *ctx;
} dtfl_sync_hooks;

int dtfl_set_sync_hooks(const dtfl_sync_hooks *hooks);
```

- Groups are all-or-nothing: the four mutex functions; the five
  condition functions, which also need the mutex group (a condition
  waits on a hooked mutex); `wait` with `wake`.  `yield`, `sleep`,
  `clock` and `self` are each optional.  A NULL member of an absent
  group keeps the default for that primitive.
- Installable only before first use, like the allocation hook: the
  first mutex, condition or parker libdtfl creates latches the hooks,
  and a later call returns `DTFL_ESTATE`.  A primitive created before
  the hooks and released after them would otherwise cross families.
- Every call site in the table above routes through one internal
  layer, `src/base/os_sync.{h,c}`.  It is the only file in `src/` that
  may name `pthread_mutex_*`, `pthread_cond_*`, `futex`, `sched_yield`,
  `nanosleep`, `clock_gettime` or `pthread_self`, and
  `tests/dist/test_api_discipline.sh` enforces that, the way it keeps
  `pthread_create` in `os_thread.c`.
- The parker keeps the Rust std state machine (EMPTY, NOTIFIED,
  PARKED in one atomic word) on every path, and chooses how to sleep:
  the host's `wait`/`wake` on the word if set; otherwise the host's
  mutex and condition if set; otherwise `futex(2)` on Linux and a
  pthread condition elsewhere.  A host that supplies only mutexes and
  conditions therefore gets a parker that never enters the kernel
  behind its back.
- `self` names the calling thread of execution for the debug check
  that a worker is used only by its owner (PLAN section 5 rule 1).  A
  fiber that migrates between carriers keeps its identity, so the
  check does not fire falsely.
- Socket I/O is not hooked here.  A host that needs every byte to move
  through its own I/O uses a transport (decision 2) and
  `dtfl_execute_worker`, which starts no thread and opens no socket.

### 2. Byte transports

Upstream generalizes the network loops over a `Stream` trait
(`communication/src/allocator/zero_copy/stream.rs`), implemented for
`TcpStream` and `UnixStream`.  libdtfl folded that trait into raw file
descriptors.  It now ports the trait as a public vtable, so the host can
supply the stream:

```c
typedef struct dtfl_transport {
	int (*send)(void *ctx, size_t peer, const void *buf, size_t len, size_t *sent);
	int (*recv)(void *ctx, size_t peer, void *buf, size_t cap, size_t *got);
	void (*wait)(void *ctx, uint64_t timeout_ns);
	void (*wake)(void *ctx);
	int (*shutdown)(void *ctx, size_t peer);	/* optional */
	void (*destroy)(void *ctx);			/* optional */
	void *ctx;
} dtfl_transport;

int dtfl_transport_socket(const int *fds, size_t nfds, size_t self,
    dtfl_transport *out);
```

- One ordered byte stream per peer, as a socket.  `send` takes up to
  `len` bytes and reports how many in `*sent`; zero means "not now".
  After a short send libdtfl offers exactly the unsent remainder of the
  same buffer before anything else to that peer, which is the retry
  contract of `shm_mq_send(..., nowait = true)`.  `recv` copies up to
  `cap` bytes; zero means none now, `DTFL_EOF` that the peer will send
  no more.  Either returns `DTFL_EIO` on failure, after setting the
  reason with `dtfl_set_error_detail`, and the driver turns that into
  upstream's "timely communication error" panic carrying the reason.
- With `shutdown` set, a peer's stream ends in `DTFL_EOF` after its
  zero-length header, and the driver reads on to that `DTFL_EOF` and
  panics if bytes follow the header, as `recv_loop` does.  Without
  `shutdown` the driver stops reading a peer at its header.
- `wait` blocks until a peer may have bytes, a peer whose last send was
  short may take more, `wake` was called, or the timeout passes.
  Spurious returns are allowed.  `wake` may be called from any thread
  and makes the current or next `wait` return, as `SetLatch` does.
- The framing on the stream is the cluster's (PLAN D9): 48-byte
  `MessageHeader`s and bodies, ended by a zero-length header.  No
  handshake: the host already knows which peer is which.
- The host owns the transport.  libdtfl borrows it for the call and
  never calls `destroy`; the host does, after the call returns.
- `dtfl_transport_socket` is the built-in transport over connected
  stream sockets (`socketpair(2)` after `fork`, or TCP connected by
  the host).  It sets the descriptors non-blocking, waits with
  `poll(2)` on the sockets and a self-pipe, and takes ownership of the
  descriptors (its `destroy` closes them).
- The cluster's send and receive loops (`zc_tcp.c`) are ported onto
  the vtable.  The TCP mesh uses an internal blocking variant of the
  socket transport, so its system calls, panics and shutdown sequence
  are unchanged.  Those loops run only over the built-in socket
  transport: a host transport serves one inline worker (decision 3),
  because `wait` and `wake` are per transport, not per thread.

### 3. Process-hosted workers and inline I/O

```c
int dtfl_execute_worker(const dtfl_config *cfg, size_t index, size_t peers,
    const dtfl_transport *transport, dtfl_worker_fn fn, void *ctx);
```

- Runs worker `index` of `peers` on the calling thread, reaching every
  other worker through `transport`, and returns once `fn` has returned,
  the worker's dataflows are complete, and every peer has sent its
  end-of-stream.  It starts no thread.  `cfg` (may be NULL) supplies
  the progress mode, log subscriptions and idle merge effort; its comm
  mode and thread count are ignored.  With `peers == 1` the transport
  may be NULL and the worker is a Thread worker.
- The allocator is the cluster's TcpAllocator with one worker per
  process, unchanged.  What changes is who moves the bytes.  Instead
  of a send and a receive thread per peer, an inline pump owned by the
  worker moves them: the allocator's `receive` reads whatever the
  transport has and frames it into the receive queues, `release`
  writes what the send queues hold as far as the transport takes it,
  and `await_events` parks on a parker whose sleep is the transport's
  `wait` and whose unpark is its `wake`.  A `dtfl_sync_activator` from
  another thread of the host therefore wakes the worker through the
  transport.  After the worker is freed the pump finishes the
  shutdown the network threads would have done: it flushes, writes
  the end-of-stream header to every peer, and reads until every peer's
  end-of-stream, discarding what arrives.
- The MergeQueues between the allocator and the pump keep their
  mutexes (now uncontended).  The pump is a different driver for the
  same queues, not a second allocator, so the progress and channel
  code paths are the cluster's.

### 4. Threads or processes, chosen in the configuration

```c
typedef enum dtfl_workers {
	DTFL_WORKERS_THREADS = 0,
	DTFL_WORKERS_PROCESSES = 1
} dtfl_workers;
int dtfl_config_set_workers(dtfl_config *cfg, dtfl_workers how);
dtfl_workers dtfl_config_workers(const dtfl_config *cfg);
```

- `DTFL_WORKERS_THREADS` (the default) is today's behavior.
- `DTFL_WORKERS_PROCESSES`, with mode `DTFL_COMM_PROCESS` or
  `DTFL_COMM_PROCESS_BINARY` and `threads` workers, makes
  `dtfl_execute` build a `socketpair(2)` mesh, `fork(2)` one child per
  worker, and run `dtfl_execute_worker` in each child over
  `dtfl_transport_socket`.  The parent waits for every child and
  returns `DTFL_EIO` with detail if any exits non-zero or is killed.
  Other modes return `DTFL_EINVAL`; a platform without `fork` returns
  `DTFL_ENOTSUP`.  `fork` and `waitpid` live in `src/base/os_thread.c`
  beside `pthread_create`.  Each child also writes its result to a
  pipe, which the parent reads only when `waitpid` fails with `ECHILD`
  (the caller ignores `SIGCHLD`).  The mesh and pipes are
  close-on-exec.
- Host-created processes (PostgreSQL backends and background workers)
  call `dtfl_execute_worker` directly with their own transport.

## Alternatives considered

- **Per-primitive setters** (`dtfl_set_mutex_hooks`,
  `dtfl_set_cond_hooks`, ...).  More symbols and man pages, and the
  mutex/condition pairing would cross setters.  One struct says which
  primitives come as a family.
- **A parker hook instead of futex-style wait/wake** (park, unpark,
  new, free).  It maps one-to-one onto libxtc's `xtc_notify`, but it
  moves the std state machine into every host.  wait/wake on a word
  keeps the machine in libdtfl, and a host with only a condition
  variable can skip both and still get a correct parker.
- **Host-allocated vs in-place mutex storage.**  The hooks hand back an
  opaque pointer.  An in-place size negotiated at run time would save
  an allocation per mutex, but libdtfl creates mutexes per queue and
  per channel table, not per message, so the cost is noise.
- **Threaded drivers over host transports.**  The send and receive
  loops would call one transport's `wait` from 2 x (n - 1) threads at
  once; what `wake` wakes would then be the host's problem.  Not
  needed by any host in view: a process-hosted worker wants no threads,
  and a threaded host has `DTFL_COMM_PROCESS` in one address space.
- **An inline driver for `DTFL_COMM_CLUSTER` over TCP addresses.**
  The socket mesh is built by a connect and an accept thread
  (`networking.rs`).  A threadless mesh would need non-blocking
  connect and accept under `poll`.  A host that wants TCP without
  threads connects the sockets itself and passes them to
  `dtfl_transport_socket`; that covers the case without a second mesh
  builder.  Revisit if a user asks for `-n` without threads.
- **A new comm mode `DTFL_COMM_FORK`.**  The comm mode says how
  workers talk; forking says where they live.  A separate setter keeps
  the two orthogonal and leaves room for a threads-and-processes mix.
- **Shared-memory queues instead of a byte stream.**  libdtfl would
  allocate the rings itself in a `MAP_SHARED` mapping.  PostgreSQL
  already owns DSM and `shm_mq`, libxtc owns its own channels, and
  `fork` has sockets for free; the byte stream is the interface all
  three can implement.

## Consequences

- Five process-wide hooks instead of four.  `os_sync.c` adds the
  writable globals `sync_hooks`, `sync_hooks_set` and `sync_used`;
  PLAN section 5 rule 5 and `tests/dist/test_symbols.sh` list them.
- Every lock and wait costs one predictable branch on a latched flag.
- PostgreSQL, process per backend: the leader registers N - 1 dynamic
  background workers, creates N x (N - 1) `shm_mq` rings in a DSM
  segment, and every process calls `dtfl_execute_worker` with a
  transport whose `send`/`recv` are `shm_mq_send`/`shm_mq_receive`
  with `nowait = true`, whose `wait` is `WaitLatch(MyLatch, ...)` then
  `ResetLatch`, and whose `wake` is `SetLatch(MyLatch)`.
- PostgreSQL, `io_method=worker`: I/O workers are postmaster children
  with a fixed main loop, so they host no libdtfl worker; their shared
  submission ring woken by `SetLatch` is the same pattern a DSM-ring
  transport uses.  libdtfl submits no AIO and its spill files use
  `pread`/`pwrite` in the calling process.
- PostgreSQL, threaded backends, and libxtc applications: thread hooks
  spawn tasks, sync hooks map mutexes, conditions, yield, sleep and
  clock onto the runtime, and `dtfl_execute` runs as today, or each
  task calls `dtfl_execute_worker` with an in-memory transport.
- The built-in socket transport makes `fork` a first-class way to run
  workers, so a dataflow can be isolated per process without TCP.
