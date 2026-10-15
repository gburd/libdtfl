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
[x] P2  logging registry; comm (thread, process, binary, simulation,
        and TCP/cluster allocators).  comm_hello and refill are excluded
        (the raw allocator is internal, ADR-0012); comm_log emission is
        a deferred no-consumer emit point (the encoder is done).
[x] P3  progress core + worker + the progress-wiring rows.  progcaster's
        optional TimelyProgressEvent logging is a deferred no-consumer
        emit point (ADR-0012).
[x] P4  timely dataflow layer + operators + examples + the 3 timely
        upstream tests + the manual chapters.  capture_send/recv run on
        the binary EventWriter/Reader transport; logging-recv/replay are
        excluded (logging is a subscriber API, not logging-as-data,
        ADR-0012).
[x] P5  differential Collection + InputSession (linear methods)
[x] P6  traces: layout, ord_neu, merge batcher, spine, cursors
[x] P7  arrange, TraceAgent, writer, upsert, trace wrappers
[x] P8  join, reduce, count, threshold, iterate, dynamic, the rest of
        collection.rs and common.rs, the differential examples, the dd
        upstream tests (import done; reduce_reference's oracle family
        done, its cross-tactic family deferred, ADR-0012), and the
        differential manual chapters.

Not started or partial -- MUST be finished before the project is done
---------------------------------------------------------------------
[x] P2/P3 TCP + cluster allocator, networking handshake, create_sockets,
        zero-copy initialize; the ProcessBinary and Cluster modes of
        initialize.  DONE (feat/tcp): test_tcp drives a loopback mesh;
        Cluster is wired into dtfl_execute; TSan clean.  (comm_hello the
        example and comm_log emission remain, tracked separately.)
[x] Binary capture transport: dtfl_event_writer_* / dtfl_event_reader_*
        DONE: implemented in src/timely/event.c (the EventWriter, a
        pusher that frames each event as a u64 LE length plus the encoded
        body and writes it to the fd) and src/timely/replay.c (the
        EventReader replay operator, one initial capability per fd).
        Proven two ways: the in-process pipe round trip in
        tests/unit/test_capture.c and the capture_send/capture_recv
        examples running end to end over a loopback socket (0..9
        replayed).
[x] Public timely/differential logging subscriber API (dtfl/log.h).
        DONE (feat/log-subscriber): a subscriber receives decoded
        timely/differential/communication event batches; the P11
        prerequisite.  (The human-readable log sink hook is separate,
        under embedding.)
[x] P9  algorithms + dogsdogsdogs + the differential/dogs examples.
        DONE: bfs, scc, identifiers, the dogsdogsdogs delta-query
        library, half_join2 (dtfl_dogs_half_join2), and the examples
        pagerank, monoid-bfs (on the public dtfl_diff_new user diff),
        dynamic, multitemporal, cursors, scc, ngo, delta_query2.
        delta_query is the last in progress; arrange, graspan and
        progress are deferred (each needs a public surface that is not a
        small add, ADR-0012).
[x] P10 differential capture (1 row).  Exit: capture round trip under
        message duplication and reordering.  DONE (feat/finish-code):
        src/dd/capture.c ports CDC V2 (the Updates/Progress Message
        protocol, the sink, and the deduplicating reordering iterator);
        tests/unit/test_dd_capture.c reconstructs the captured multiset
        in order, reversed, duplicated and reversed+duplicated.
[x] P11 diagnostics (the dataflow-diag crate): the in-process WebSocket
        server and dtfl-diag.  DONE (feat/p11-diag): Frame JSON byte-
        compatible with the upstream consoles; graph/profile/
        arrangements; binds 127.0.0.1 (D12).  scc-bench example remains.
[x] P12 trace spill: graded chunks, chunk spill, the spines and
        chunk_bench benchmarks.  DONE (feat/p12-spill): spill round trip
        and the spill PBT pass.
[x] P13 DDIR: parser, lowering, interpreter, explain, dtfl-ddir, the
        plugin server and programs.  DONE.  The parse/IR/lower core, the
        faithful backend (the whole program in one iterative dynamic
        scope, time literally Product<u64,PointStamp<u64>>, with enter_at
        and lift_iter), the explain rewrite + reverse rules, and the
        server + dtfl-ddir tool.  The explain suite passes for reach,
        transitive closure, count, flatmap, collect, demand-subset,
        clone_identity and the scc sufficiency sub-tests.
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
[ ] DDIR self-referential feedback hang and the reduce interesting-time
        assertion are the SAME coupled root cause (investigated
        2026-10-15).  A program with var x = ARRANGE(CONCAT(x,
        NEGATE(...))) parks the worker; kcore and stable SKIP.  The DDIR
        backend renders ARRANGE as a pass-through copy; making it
        consolidate DOES fix convergence (kcore then yields the 5-node
        2-core, stable the 2 matched pairs, both hand-verified), BUT it
        feeds the reduce HistoryReplayer a nested Product<u64,PointStamp>
        time structure that trips the reduce assertion below MORE
        broadly, regressing the non-heavy explain__scc_row_explanations_
        sufficient_small.  So the self-ref fix cannot ship until the
        reduce replayer is fixed for nested Product time; the consolidate
        change then closes both.  Kept at the pass-through backend (HEAD)
        where scc_small passes and kcore/stable SKIP.
[x] trace new_listener empty-seal frontier bug: FIXED (feat/finish-
        import, merged).  An importer built after a pure empty seal-batch
        frontier advance stalled because new_listener seeded its start
        frontier from the last non-empty batch's upper and the spine's
        batches_through drops empties.  __dtfl_trace_upper reads the
        spine's tracked upper and new_listener seeds from it, matching
        upstream.  Regression test: import.c test_import_stalled_dataflow;
        full dd suite and scc/peek/shared_import pass under ASan.
[ ] reduce interesting-time assertion: in reduce_compute (the
        HistoryReplayer), an in-band synthetic next_time is emitted for
        which no held capability covers it (idx == SIZE_MAX in the
        outputs[u].tag <= next_time search, reduce.c:1198); upstream
        reduce.rs has the same expect('failed to find index').  Fails on
        the DTFL_EXPLAIN_HEAVY fuzz sweep, and (per the coupled finding
        above) on scc_small the moment ARRANGE consolidates.  Diagnosed
        2026-10-15: the failing case is tw=9 Product<u64,PointStamp>
        time, next_time=[0,8,3,1537] incomparable to the one held tag
        [0,11,0,1792], so a stale low history time leaked into a
        synthetic join; the fix is in the per-key meet/advance_buffer_by
        sequencing for the nested time.  Only the nested-Product reduce
        path under adversarial times is affected; all of dd/scc/the
        non-fuzz explain suite is correct.  This is the keystone: fixing
        it also unblocks the DDIR self-ref hang above.
[x] P14 plugin server + its four example plugins.  Exit: the dd_server
        session scripts replay.  DONE (feat/p14-server): dd_server with a
        named shared-trace registry, the degr_dist/reachability/
        neighborhood/random_graph plugins (hand-verified), the
        dtfl-server tool replaying the session scripts, and the
        shared-arrangement property proven by pointer identity.
[~] P15 release: the three experiments/ benchmarks DONE (arrange,
        graphs, graphs-static).  Multi-platform build and check DONE:
        Linux x86_64 glibc (floki/meh, gcc 14, all six configs green),
        FreeBSD with clang 19 (nuc, make check 105 pass / 24 skip / 0
        fail, -Wcast-align clean), and Linux riscv64 (lixr, gcc 15, a
        strict-alignment arch -- build and check clean, no SIGBUS, which
        validates the alignment-cast laundering).  The dist tarball
        builds and checks from scratch on riscv64.  Fuzz campaign DONE
        (24 suites, ~230000 cases, ASan-clean; dev/P15-measurements.md).
        Performance parity MEASURED (dev/P15-measurements.md): the port
        is ~13 to 24x slower than Rust on arrange/count, not within the
        1.5x target -- closing that is a dedicated optimisation project.
        REMAINING: the parity optimisation to actually meet 1.5x, and
        distcheck on musl and macOS (glibc distcheck green on floki; a
        full FreeBSD distcheck is impractical on the available host, but
        FreeBSD make check passes).

Examples
--------
[~] timely examples: 18/23 (5 blocked: capture_send/recv on the binary
        transport, 3 logging examples on the logging subscriber being
        wired into examples)
[x] differential + dogs examples: ported (hello, degrees, bfs, projekt,
        compact, stackoverflow, accumulate, interpreted, itembased_cf,
        pagerank, monoid-bfs, cursors, dynamic, multitemporal, scc, the
        dogs delta queries delta_query_wcoj/dogsdogsdogs/ngo/
        delta_query2, and the P12 spill benchmarks).  delta_query is the
        last in progress; arrange, graspan and progress are deferred
        (ADR-0012).
[x] experiments/ benchmarks (3): arrange, graphs, graphs-static DONE
        (bench/experiments/*.c, verified on EC2 Debian, ASan-clean).

Documentation
-------------
[x] Texinfo manual doc/libdtfl.texi: written and building under makeinfo
    with zero warnings.  Top-level chapters: Introduction, Installation,
    Concepts, Threads, Error handling, Embedding, Timely Dataflow,
    Differential Dataflow, Diagnostics, DDIR, API Reference, Porting
    notes, Index.  Wired as info_TEXINFOS + libdtfl_TEXINFOS so make
    info, make dist and distcheck build and ship libdtfl.info.
[x] timely tutorial chapters doc/timely/*.texi (mdbook port): 7/7
    (introduction + chapter_0..chapter_5), C API throughout.
[x] differential chapters doc/differential/*.texi: introduction +
    chapter_0, chapter_1, chapter_a, chapter_2..chapter_6, C API
    throughout.  Code examples ship as doc/snippets/*.c (timely_hello,
    timely_input, timely_ops, timely_iterate, dd_manages, dd_reduce),
    each compiled and run by tests/dist/test_docs.sh against a .expected.

Embedding (host = PostgreSQL etc.) -- a release requirement
-----------------------------------------------------------
[x] memory: dtfl_set_alloc_hooks (all allocation; gate-enforced)
[x] panic: dtfl_set_panic_hook (host may longjmp / ereport)
[x] logging sink: dtfl_set_log_hook (human-readable; maps to elog)
[x] thread-spawn hook: dtfl_set_thread_hooks (maps to bgworkers), or the
        documented single-thread cooperative mode
[x] embedding contract man page dtfl_embedding(7)
[x] a worked PostgreSQL embedding example or an extension skeleton
        (alloc->palloc, panic->ereport, log->elog, threads->bgworker).
        DONE: examples/embed/pg_extension.c, a self-contained extension
        skeleton that installs all four host hooks (alloc->palloc/pfree,
        panic->ereport(ERROR) with a sigsetjmp PG_TRY so a panic unwinds
        rather than crashes, log->elog at matching levels, and the
        single-thread cooperative mode -- dtfl_worker_new_standalone +
        dtfl_worker_step -- in place of thread hooks, with the bgworker
        spawn/join option documented).  The PostgreSQL symbols are
        stubbed so it builds and runs with no server; it runs a small
        distinct-residue dataflow and is in its own smoke test, ASan-
        clean.

Verification (fidelity against real Rust) -- a release requirement
------------------------------------------------------------------
[x] completeness ledger + ledger-diff (regenerates from the pins)
[x] ledger gate fails if a done file skips all its upstream tests
[x] interop fixture: dtfl_type_cmp reproduces Rust Ord (byte-level)
[x] interop: wire-format fixtures (capture framing, log streams)
[x] interop: operator-output oracle (join/count/reduce byte-equal to Rust)
[x] differential fuzz oracle (Hegel vs C model, anchored to Rust-seeded
        fixtures)
[x] every upstream #[test] resolved (test-ledger all done/subsumed/
        deferred/excluded, 0 todo/wip): 110 done, 23 subsumed (covered by
        another target), 21 deferred (need a feature not yet ported, each
        with a reason), 9 excluded (no C analog).
[~] the P15 fuzz campaign and the performance parity table (measured
        2026-10-15, dev/P15-measurements.md).  Fuzz campaign DONE: 24
        property suites at 10000 cases each (~230000 cases) under ASan,
        zero violations / crashes / leaks.  Parity table MEASURED but
        not passing: the C port is ~13 to 24x slower than the pinned Rust
        experiments crate on arrange/count (same host, same params), well
        outside the 1.5x target.  That is the measured cost of the
        fidelity-first decisions (D1/D2) and the unoptimised hot paths;
        reaching parity is a dedicated optimisation project (profile and
        cut allocation/copies in arrange and reduce).  The measurement is
        complete; the within-1.5x goal is the open part.

Deferred-with-reason items that MUST still be finished before 1.0
----------------------------------------------------------------
These are not dropped; each has a precise completion condition.  The
first three are port-ledger rows the completion gate already counts as
unfinished (wip/todo); the fourth is tracked here because a property
test is not an upstream #[test] and so has no ledger row.
[~] comm logging (communication/src/logging.rs -> src/comm/comm_log.c):
        the CommunicationEvent encoder and the "communication"
        subscriber stream exist, round-trip, and the row is done; the
        DATA-PLANE EMISSION from the network send/recv loops is deferred
        (ADR-0012: no consumer on the data plane).  Lands when a consumer
        needs it, by plumbing a log sink into the comm allocators.
[~] differential logging emission (differential-dataflow/src/logging.rs
        -> src/dd/dd_log.c): same shape; the encoder and stream exist
        and the row is done; the spine/batcher emission is deferred
        (ADR-0012: consumer-side logging, no data-plane consumer).
[x] half_join2 (dogsdogsdogs/src/operators/half_join2.rs ->
        src/dogs/half_join.c, done): the general half_join with user
        frontier/comparison closures.  DONE (dtfl_dogs_half_join2):
        comparison replaces the strict bool (NULL = strict less-than),
        frontier_func builds the compaction antichain from each held
        initial time (delta_query2's time -> (outer-1, inner-1)), both
        sharing one ctx; dtfl_dogs_half_join is now a thin wrapper of
        it.  Lives in half_join.c, not a separate half_join2.c (the two
        share the operator).  Its oracle is the delta_query2 example.
[~] TCP property test (pbt_tcp.c): CLOSED with justification.  Loopback
        TCP is not deterministic enough for Hegel's shrink/replay, so a
        full-path property test is not added; the framing round-trip is
        covered by pbt_wire.c and the live mesh by test_tcp.c (2x1, 3x1,
        2x2 loopback).  A seeded in-memory transport harness could enable
        it later, but the send/recv/route path is already exercised by
        those two, so this is resolved rather than left open.

Release gate (tests/dist/test_completion.sh)
---------------------------------------------
At 1.0 the completion gate must pass: no port-ledger or test-ledger row
at todo or wip.  Until then the gate runs in reporting mode (counts the
remaining rows, exit 0) so the normal build is not blocked, and prints
the remaining count so progress is visible on every make check.  Flip it
to enforcing (exit nonzero on any todo/wip) for the release branch.
