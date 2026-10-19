# ADR-0012 (D12): Divergences from the pin

- **Status:** Accepted, 2026-10-10.
- **Decision record:** dev/PLAN.md section 4, PLAN D12.  The plan holds the
  full context, the alternatives considered and the reasons they lost;
  this file records the decision's status and any later amendment.

## Amendments

### 2026-10-15: examples and emission points excluded or deferred with reason

Three groups of upstream items cannot be ported as literal examples
without reversing a decision this plan already made, or depend on a
feature with no consumer in libdtfl.  They are recorded here and marked
in dev/port-ledger.tsv as excluded (a decision forbids the literal
port) or deferred (ported where it has an effect; the rest waits for a
consumer), the same resolved-with-reason status the completion gate
already honours for the TCP property test.

Excluded, because the example demonstrates a layer the library
deliberately does not expose (PLAN D7, D9; AGENTS section 5.2: library
code has no getenv/getopt and communication is reached only through the
dataflow worker, dtfl_execute):

- communication/examples/comm_hello.rs and lgalloc.rs drive the raw
  communication allocator directly (allocate a typed channel, send/recv,
  count down peers; a Rust lgalloc refill hook).  The port keeps the
  allocator internal; the public entry is the worker.  Porting these
  would require a public raw-allocator surface that is a deliberate
  non-goal.  The allocators are exercised through the worker by
  test_comm and every execution-mode test instead.

- timely/examples/logging-recv.rs and logging_replay.rs replay a stream
  of (Duration, TimelySetup, TimelyEvent) log records as dataflow data.
  The port's logging is a structured subscriber API (dtfl/log.h decodes
  to dtfl_log_batch), not logging-as-data: there is no public TimelyEvent
  record type to flow through a dataflow, by the same decision that made
  logging a subscriber callback rather than a record stream.  The log
  streams are exercised by logging-send and the diagnostics console.

Deferred, because the code is ported and runs but an optional emit point
has no consumer yet:

- communication/src/logging.rs (comm_log.c) and
  differential-dataflow/src/logging.rs (dd_log.c): the event encoders are
  complete and byte-faithful (round-trip tested), but the data-plane emit
  calls in the network send/recv loops and the spine/batcher are not
  wired, because nothing in libdtfl consumes those streams on the data
  plane (the diagnostics console subscribes to the structured streams
  the subscriber API already fills).  They land when a consumer needs
  them.

- timely/src/progress/broadcast.rs (progcaster.c): the data plane is
  complete and runs every test; the optional per-message
  TimelyProgressEvent logging is a separate T-parameterized logger with
  no consumer (diagnostics subscribes only to the timely and
  differential/arrange streams).

- interactive/src/server.rs (ddir/server.c): the single-worker server is
  complete; the multi-worker Sequencer and trace compaction are deferred.

- differential-dataflow/tests/reduce_reference.rs: the oracle_* family is
  ported; the differential_*/bfs_*/scc_* families cross-check against
  reduce_trace_reference, a second model-derived reduce tactic not
  ported, and are deferred.

- differential-dataflow/examples/arrange.rs, graspan.rs and progress.rs
  each need a public surface that does not exist and is not a small add:
  a raw generic::source that downgrades its own capability (arrange), a
  recursively arranged Variable that imports its own not-yet-set trace
  within a scope (graspan), and timestamp path summaries carried as
  record data (progress).  Deferred with the exact surface named in each
  ledger note.

### 2026-10-16: arrange, graspan done; progress, spill examples re-scoped

- differential-dataflow/examples/arrange.rs and graspan.rs are ported
  on the existing public API.  The surfaces the 2026-10-15 entry named
  already existed: a zero-input dtfl_op_builder operator that keeps,
  downgrades and reactivates its capability is generic::source, and a
  dtfl_variable_new collection can be arranged and joined before
  dtfl_variable_set.  Porting arrange exposed and fixed a shared
  dtfl_probe bug (a handle attached to several streams did not hold
  the meet of their frontiers).
- differential-dataflow/examples/progress.rs needs no new surface (a u64
  summary is a record word applied with dtfl_summary_*), but stays
  deferred: the library answers it wrongly (frontier reduce under
  Product<u64, u64>; reproducer in the ledger note).
- communication/examples/spill_stress.rs joins comm_hello and lgalloc as
  excluded: it drives MergeQueue and the spill policies directly, the
  raw communication layer this port keeps internal (D7/D9).
- differential-dataflow/examples/columnar_spill.rs is subsumed by
  bench/trace_spill.c: its spill hangs off the columnar spine, which
  the public arrange does not use (D2).
- timely/examples/spill_compare.rs stays deferred until dtfl_config
  grows a spill-policy hook (upstream Hooks::spill).

### 2026-10-18: progress done

- differential-dataflow/examples/progress.rs is done and smoke-tested.
  The wrong answer was a trace-layer bug, not a reduce or iterate one:
  UpdsBuilder::seal (src/dd/trace/layout.c) compacted its stash in place
  while reading it through a sort permutation, so a merge whose
  advance_by reorders a value's times (any partially ordered time, here
  Product<u64, u64>) could read a slot it had already overwritten.  The
  reduce output trace then held a wrong history.  Fixed to sort the
  stash first, as consolidate_slice does.  The output now matches the
  pinned binary with 1, 2 and 4 workers.

### 2026-10-18: spill_compare done

- dtfl_config_set_spill_file(cfg, dir, threshold_bytes,
  head_reserve_bytes) is the public form of Hooks::spill.  It installs
  the factory spill_compare.rs builds by hand: per MergeQueue, a
  Threshold writer over a fresh file strategy and a PrefetchPolicy
  reader with the head reserve as its budget.  The file strategy is the
  example's file_spill module moved into the library
  (src/comm/spill_file.c).  The factory reaches every MergeQueue pair
  that ProcessBinary, the TcpBuilder and the TCP send loop create, as
  at the pin.  A general user BytesSpill callback is not exposed: no
  upstream example or test needs one, and it would put byte-buffer
  ownership on the public API.
- The strategy differs from the example module in three ways, all
  documented in dtfl_config(3): files come from mkstemp under the
  given directory and are unlinked at once; a handle preads only its
  own chunk instead of slurping the file on first fetch; a read error
  panics instead of returning the handle for an endless retry.
- Porting the example exposed an Exchange pusher divergence, now fixed
  rather than recorded: the C pusher held a whole epoch per peer, where
  upstream's CapacityContainerBuilder ships each full container.  With
  one unbounded message there was nothing for the policy to spill.
- timely/examples/spill_compare.rs is done.  The upstream example is
  unchanged, apart from its smaller defaults and the stdout/stderr split
  the smoke test needs.

An amendment adds a dated entry here and
edits PLAN D12 in the same commit.

### 2026-10-18: differential and communication logging emission implemented

The differential-dataflow/src/logging.rs and communication/src/logging.rs
deferrals above are closed.  The
spine, the agent and the merge batcher emit Batch, Merge, Drop,
TraceShare and Batcher events at the upstream points, through a logger
the operator acquires at construction (NULL, so free, without a
subscriber).  MergeShortfall stays unemitted because the pin never logs
it.

The communication stream follows the pin's Hooks.log_fn: each network
send and receive thread asks the hook built from a "communication"
dtfl_config_log_subscribe for its own logger and logs StateEvent and a
MessageEvent per header.  The subscriber therefore runs on the network
threads and reports the process index as the worker.  A per-worker
dtfl_worker_log_subscribe sees no communication events, because no
worker owns the network threads.

The logging-recv and logging_replay exclusions stand.  The emission adds
events to the subscriber streams but no public TimelyEvent record type
that a dataflow could carry.
