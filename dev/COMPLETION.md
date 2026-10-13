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

Not started or partial -- MUST be finished before the project is done
---------------------------------------------------------------------
[x] P2/P3 TCP + cluster allocator, networking handshake, create_sockets,
        zero-copy initialize; the ProcessBinary and Cluster modes of
        initialize.  DONE (feat/tcp): test_tcp drives a loopback mesh;
        Cluster is wired into dtfl_execute; TSan clean.  (comm_hello the
        example and comm_log emission remain, tracked separately.)
[ ] Binary capture transport: dtfl_event_writer_* / dtfl_event_reader_*
        (declared in timely_ops.h, unimplemented).  Unblocks the
        capture_send/capture_recv timely examples.
[x] Public timely/differential logging subscriber API (dtfl/log.h).
        DONE (feat/log-subscriber): a subscriber receives decoded
        timely/differential/communication event batches; the P11
        prerequisite.  (The human-readable log sink hook is separate,
        under embedding.)
[~] P9  algorithms + dogsdogsdogs + remaining differential examples.
        DONE: bfs, scc (the ord_neu compaction fix landed), identifiers,
        the dogsdogsdogs delta-query library, and 11 differential/dogs
        examples.  REMAINING: a few blocked examples (pagerank,
        monoid-bfs, dynamic, graspan, progress, multitemporal, scc,
        ngo) each needing a distinct API, and half_join2.
[ ] P10 differential capture (1 row).  Exit: capture round trip under
        message duplication and reordering.
[x] P11 diagnostics (the dataflow-diag crate): the in-process WebSocket
        server and dtfl-diag.  DONE (feat/p11-diag): Frame JSON byte-
        compatible with the upstream consoles; graph/profile/
        arrangements; binds 127.0.0.1 (D12).  scc-bench example remains.
[~] P12 trace spill: graded chunks, chunk spill, the spines and
        chunk_bench benchmarks, the spill_stress / spill_compare
        examples.  IN PROGRESS (feat/p12-spill).  Exit: spill round
        trip; spill PBT.
[~] P13 DDIR: parser, lowering, interpreter, explain, dtfl-ddir, the
        plugin server and programs.  IN PROGRESS.  The parse/IR/lower
        core, the faithful backend (the whole program in one iterative
        dynamic scope, time literally Product<u64,PointStamp<u64>>, with
        enter_at and lift_iter), and the explain rewrite + reverse rules
        are DONE (feat/p13-explain): the explain suite passes for reach,
        transitive closure, count, flatmap, collect, demand-subset and
        clone_identity.  The scc sufficiency sub-tests now pass too (the
        trace batch-lifetime UAF below is fixed); remaining is the server
        + dtfl-ddir tool.  Exit: the explain suite; upstream .ddp/.ddir
        programs give the same results.
[x] trace batch-lifetime use-after-free: a reduce-built batch
        (reduce.c rd_build_batches -> ord_neu.c __dtfl_builder_ord_done)
        was freed by the spine's fuelled merge (spine_fueled.c ms_work ->
        cursor.c __dtfl_batch_ord_release) while ArrangeToCollection still
        cursored it (arrangement.c ascoll_logic -> trace_int.h
        __dtfl_slices_len read freed storage.vals).  FIXED on
        feat/trace-uaf: the batch-pointer stream record now owns a
        reference (the arena carries elem drop/retain hooks set per
        batch-pointer session; the producer retains one reference into the
        record and the stream message releases it on free), matching
        upstream's Vec<Rc<Batch>> message that keeps a batch alive for the
        message's lifetime whether or not a consumer reads it.  The 3
        explain scc sufficiency tests (scc_row_explanations_sufficient_small,
        two_simultaneous_queries_sufficient, demand_is_a_subset_of_the_input)
        are un-SKIPped and pass under ASan + LSan.
[ ] DDIR self-referential feedback hang: a program with
        var x = ARRANGE(CONCAT(x, NEGATE(...))) parks the worker (lost
        wake / non-convergence) in the backend/iterate rendering; kcore
        and stable DDIR programs SKIP on it.  Minimal repro in the
        p13-server report.  Distinct from the two bugs above; a backend
        convergence fix before 1.0.
[ ] reduce interesting-time assertion: rd_retire's output-tag match
        asserts idx != SIZE_MAX (reduce.c:1198) and fails on the
        DTFL_EXPLAIN_HEAVY fuzz sweep (explain__fuzz_explanations_sufficient).
        Distinct from the batch-lifetime UAF above (it was previously
        masked by that UAF crashing first); the heavy fuzz sweep stays
        SKIP until the reduce output-tag logic is fixed.  scc_100 remains
        a time-gated heavy sweep, not blocked on this.
[ ] P14 plugin server + its four example plugins.  Exit: the dd_server
        session scripts replay.
[ ] P15 release: the three experiments/ benchmarks, the performance
        parity table (within 1.5x of Rust on the agreed subset), the
        fuzz campaign, distcheck on Linux (glibc, musl), FreeBSD, macOS.

Examples
--------
[~] timely examples: 18/23 (5 blocked: capture_send/recv on the binary
        transport, 3 logging examples on the logging subscriber being
        wired into examples)
[~] differential + dogs examples: 11 ported (hello, degrees, bfs,
        projekt, compact, stackoverflow, accumulate, interpreted,
        itembased_cf, 2 dogs delta queries); ~16 remain (pagerank,
        monoid-bfs, cursors, arrange, dynamic, graspan, progress,
        multitemporal, scc, delta_query/2, ngo, the P12 spill examples)
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
[x] logging sink: dtfl_set_log_hook (human-readable; maps to elog)
[x] thread-spawn hook: dtfl_set_thread_hooks (maps to bgworkers), or the
        documented single-thread cooperative mode
[x] embedding contract man page dtfl_embedding(7)
[ ] a worked PostgreSQL embedding example or an extension skeleton
        (alloc->palloc, panic->ereport, log->elog, threads->bgworker)

Verification (fidelity against real Rust) -- a release requirement
------------------------------------------------------------------
[x] completeness ledger + ledger-diff (regenerates from the pins)
[x] ledger gate fails if a done file skips all its upstream tests
[x] interop fixture: dtfl_type_cmp reproduces Rust Ord (byte-level)
[x] interop: wire-format fixtures (capture framing, log streams)
[x] interop: operator-output oracle (join/count/reduce byte-equal to Rust)
[x] differential fuzz oracle (Hegel vs C model, anchored to Rust-seeded
        fixtures)
[ ] every upstream #[test] ported (test-ledger all done/subsumed/
        deferred): currently 52/167
[ ] the P15 fuzz campaign and the performance parity table

Deferred-with-reason items that MUST still be finished before 1.0
----------------------------------------------------------------
These are not dropped; each has a precise completion condition.  The
first three are port-ledger rows the completion gate already counts as
unfinished (wip/todo); the fourth is tracked here because a property
test is not an upstream #[test] and so has no ledger row.
[ ] comm logging (communication/src/logging.rs -> src/comm/comm_log.c,
        wip): the CommunicationEvent encoder and the "communication"
        subscriber stream exist and round-trip; the DATA-PLANE EMISSION
        from the network send/recv loops is not wired (the allocator log
        sink, upstream Hooks.log_fn, is unmodeled).  FINISH: plumb a log
        sink into the comm allocators so send/recv actually emit Message
        and State events; then flip to done.
[ ] differential logging emission (differential-dataflow/src/logging.rs
        -> src/dd/dd_log.c, wip): same shape as comm_log; the encoder
        and stream exist, the spine/batcher emission from the arrange
        layer is not wired.  FINISH: emit Batch/Merge/Drop/etc. events
        from the arrangement operators; flip to done.
[ ] half_join2 (dogsdogsdogs/src/operators/half_join2.rs ->
        src/dogs/half_join2.c, todo): the unified-blob half-join
        variant.  Observable behavior equals half_join, so it has no
        oracle until the ngo example exercises it.  FINISH: port the
        ngo example first (it needs arrange_by_self/by_key leapfrog),
        then port half_join2 with ngo as its test, flip both to done.
[ ] TCP property test (pbt_tcp.c): deferred because loopback TCP is not
        deterministic enough for Hegel's shrink/replay (the framing
        round-trip is covered by pbt_wire.c; the live mesh by
        test_tcp.c).  FINISH CONDITION: add a seeded, deterministic
        transport harness (e.g. an in-memory socketpair with a recorded
        schedule) so a property over the full send/recv/route path is
        replayable; then add pbt_tcp.c.  If that harness proves
        infeasible, this stays covered by pbt_wire + test_tcp and is
        closed with that justification recorded here.

Release gate (tests/dist/test_completion.sh)
---------------------------------------------
At 1.0 the completion gate must pass: no port-ledger or test-ledger row
at todo or wip.  Until then the gate runs in reporting mode (counts the
remaining rows, exit 0) so the normal build is not blocked, and prints
the remaining count so progress is visible on every make check.  Flip it
to enforcing (exit nonzero on any todo/wip) for the release branch.
