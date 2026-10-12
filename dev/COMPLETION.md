libdtfl completion plan
========================

This file is the binding definition of "done" for libdtfl.  The project
is a full and faithful C translation of three Rust crates at their pins
(timely-dataflow 0.31.0, differential-dataflow 0.25.1, and the
dataflow-diag diagnostics tooling).  The project is NOT done until every
row below is complete.  dev/PLAN.md section 10 holds the exit criteria
for each phase; this file tracks completion status and is checked by
tests/dist/test_completion.sh.

The source of truth for file-level completeness is dev/port-ledger.tsv
and dev/test-ledger.tsv (regenerated from the pins by dev/ledger-diff).
"Done" for the whole project means: every port-ledger row is done,
excluded, subsumed or deferred-with-reason; every test-ledger row is
done, subsumed or deferred-with-reason; and every phase exit criterion
in PLAN section 10 is met.  No row may sit at todo or wip at release.

Status legend: [x] done and merged to build; [~] in progress; [ ] not
started.  Update this file as phases land.

Engine (the dataflow core) -- DONE and tested
----------------------------------------------
[x] P0  skeleton: autotools, gates, CI, munit, hegel, hooks, ADRs
[x] P1  records, time, diff, change-batch, antichain
[~] P2  logging registry; comm (thread, process, binary allocators,
        the simulation allocator) DONE; TCP + cluster allocator,
        networking handshake, comm_hello on all allocators NOT done
[~] P3  progress core + worker DONE; a few progress-wiring rows remain
[~] P4  timely dataflow layer + operators + 18 examples DONE; remaining:
        5 capture/logging examples (blocked on binary EventWriter/Reader
        transport and a public logging subscriber API), 3 timely
        upstream tests, 7 include rows, the timely manual chapters
[x] P5  differential Collection + InputSession (linear methods)
[x] P6  traces: layout, ord_neu, merge batcher, spine, cursors
[x] P7  arrange, TraceAgent, writer, upsert, trace wrappers
[~] P8  join, reduce, count, threshold, iterate, dynamic DONE; remaining:
        the rest of collection.rs (a few methods), common.rs reduce-side
        helpers, 5 differential examples, 2 dd upstream tests, the
        differential manual chapters

Not started -- MUST be finished before the project is done
----------------------------------------------------------
[ ] P2/P3 TCP + cluster allocator, networking handshake, create_sockets,
        zero-copy initialize, comm_log, comm_hello; the ProcessBinary
        and Cluster modes of initialize; the remaining progress wiring.
        Exit: comm_hello on all five allocators; a hand-built operator
        runs on Cluster; TSan clean.
[ ] Binary capture transport: dtfl_event_writer_* / dtfl_event_reader_*
        (declared in timely_ops.h, unimplemented).  Unblocks the
        capture_send/capture_recv timely examples.
[ ] Public timely/differential logging subscriber API: worker
        log_register/insert, BatchLogger, the TimelyEvent / DifferentialEvent
        record types as public types.  Unblocks the logging examples and
        is a prerequisite for P11.  (The human-readable log sink hook is
        separate and tracked under embedding below.)
[ ] P9  algorithms + dogsdogsdogs + remaining differential examples:
        bfs, scc, pagerank and the rest as differential programs; the
        dogsdogsdogs delta-query library.  Exit: bfs/scc/dogsdogsdogs
        tests pass; examples diff-equal to Rust via fixtures.
[ ] P10 dogsdogsdogs / differential capture (1 row).  Exit: capture
        round trip under message duplication and reordering.
[ ] P11 diagnostics (the dataflow-diag crate): the in-process WebSocket
        server and the dtfl-diag tool.  Exit: upstream index.html
        renders a live libdtfl dataflow; dtfl-diag graph reads the Rust
        0.31 log fixture.
[ ] P12 DDIR: parser, lowering, interpreter, explain, dtfl-ddir; trace
        spill (graded chunks, chunk spill) and its benchmarks/examples.
        Exit: the explain suite; upstream .ddp/.ddir programs give the
        same results.
[ ] P13 DDIR server + tools (42 rows: server, programs, dtfl-ddir).
[ ] P14 plugin server + its four example plugins.  Exit: the dd_server
        session scripts replay.
[ ] P15 release: the three experiments/ benchmarks, the performance
        parity table (within 1.5x of Rust on the agreed subset), the
        fuzz campaign, distcheck on Linux (glibc, musl), FreeBSD, macOS.

Examples
--------
[~] timely examples: 18/23 (5 blocked on transport/logging API above)
[ ] differential examples: 0 ported
[ ] experiments/ benchmarks (3), deferred rows revisited at P15

Documentation
-------------
[ ] Texinfo manual doc/libdtfl.texi: NOT written (one attempt abandoned).
    Build it incrementally: minimal manual that builds under makeinfo and
    commits first, then one chapter per commit.
[ ] timely tutorial chapters doc/timely/*.texi (mdbook port): 0/7
[ ] differential chapters doc/differential/*.texi: 0

Embedding (host = PostgreSQL etc.) -- a release requirement
-----------------------------------------------------------
[x] memory: dtfl_set_alloc_hooks (all allocation; gate-enforced)
[x] panic: dtfl_set_panic_hook (host may longjmp / ereport)
[~] logging sink: dtfl_set_log_hook (human-readable; maps to elog) -- in
        progress on feat/embed
[~] thread-spawn hook: dtfl_set_thread_hooks (maps to bgworkers), or the
        documented single-thread cooperative mode -- in progress
[ ] embedding contract man page dtfl_embedding(7) -- in progress
[ ] a worked PostgreSQL embedding example or an extension skeleton
        (alloc->palloc, panic->ereport, log->elog, threads->bgworker)

Verification (fidelity against real Rust) -- a release requirement
------------------------------------------------------------------
[x] completeness ledger + ledger-diff (regenerates from the pins)
[x] ledger gate fails if a done file skips all its upstream tests
[x] interop fixture: dtfl_type_cmp reproduces Rust Ord (byte-level)
[~] interop: wire-format fixtures (capture framing, log streams) -- in
        progress on feat/fixtures
[~] interop: operator-output oracle (join/count/reduce byte-equal to
        Rust) -- in progress
[~] differential fuzz oracle (Hegel vs C model, anchored to Rust-seeded
        fixtures) -- in progress
[ ] every upstream #[test] ported (test-ledger all done/subsumed/
        deferred): currently 41/167
[ ] the P15 fuzz campaign and the performance parity table

Release gate (tests/dist/test_completion.sh)
---------------------------------------------
At 1.0 the completion gate must pass: no port-ledger or test-ledger row
at todo or wip.  Until then the gate runs in reporting mode (counts the
remaining rows, exit 0) so the normal build is not blocked, and prints
the remaining count so progress is visible on every make check.  Flip it
to enforcing (exit nonzero on any todo/wip) for the release branch.
