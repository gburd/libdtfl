# Incremental materialized views in PostgreSQL with libdtfl

This is a design note, not shipped code.  It explains how libdtfl's
timely/differential dataflow engine could implement an incremental
materialized view (IVM) in PostgreSQL: a view that is kept current by
applying only the changes its base tables saw, never a full REFRESH,
while obeying PostgreSQL's visibility, isolation, and transaction
semantics.

The honest summary up front: the dataflow engine is an excellent fit for
the *computation* of an incremental view, and a poor fit for being driven
once per statement inside a backend if used naively.  The design below
spends most of its care on the second problem, because that is where IVM
attempts in PostgreSQL usually fail, and it names the places where
libdtfl helps and the places where PostgreSQL, not libdtfl, carries the
weight.


## 1. What "true incremental" has to mean in PostgreSQL

A materialized view `MV` is defined by a query `Q` over base relations.
A classic `REFRESH MATERIALIZED VIEW` recomputes `Q` from scratch.  True
IVM instead maintains a delta rule: given the committed changes to the
base relations in a transaction, `dMV = dQ(base, dbase)`, and applies
only `dMV` to the stored view.  The hard requirements, the ones that make
this a PostgreSQL problem and not just a dataflow problem, are:

1. **Transactional atomicity.**  `dMV` must become visible exactly when,
   and only when, the triggering transaction commits.  If the
   transaction rolls back, `dMV` must never have existed.
2. **Visibility and isolation.**  The delta computed for a transaction
   must be computed against the *snapshot that transaction sees*, not
   against whatever the heap holds at wall-clock time.  Under
   `REPEATABLE READ` or `SERIALIZABLE`, two concurrent transactions see
   different base states and must produce consistent, non-interfering
   view deltas.
3. **Correct multiset / aggregate semantics.**  SQL is multiset algebra
   with signed cardinalities: a row can be present `k` times, a `DELETE`
   subtracts, `COUNT`/`SUM` must track running totals, `MIN`/`MAX` must
   handle retraction of the current extreme (the genuinely hard case).
4. **Minimal cost.**  The maintenance work must be proportional to the
   size of the change, not the size of the base tables or the view.

Requirements 3 and 4 are exactly what differential dataflow was built
for.  Requirements 1 and 2 are exactly what it does not know about, and
what the extension must supply.


## 2. Why differential dataflow is the right computational core

Differential dataflow is, formally, incremental multiset algebra over a
partially ordered notion of time.  The correspondence to SQL IVM is close
to exact:

- A **collection** is a multiset of records, each with a signed integer
  weight (`dtfl_diff_i64`, the default diff algebra).  A SQL row present
  `k` times is a record with weight `k`; a delete is weight `-k`.  This
  is the correct SQL semantics for duplicates, not an approximation.
- Every **operator** has a delta rule already.  `dtfl_collection_map`
  (projection, scalar expressions), `filter` (WHERE), `join_core`
  (equi-join over arrangements), `reduce`/`count`/`distinct`
  (GROUP BY and aggregates), `concat` (UNION ALL), `negate` + `concat`
  (EXCEPT / set difference), and `iterate` (recursive CTEs / `WITH
  RECURSIVE`).  Feeding a base-table delta in produces exactly the view
  delta out, with no recomputation of unchanged rows.
- **Arrangements** are the shared, maintained indexes.  An
  `arrange_by_key` index built once is read by every join and reduce that
  needs it (`dtfl_arranged_trace` hands out shared readers), which is the
  same idea as a shared index backing several maintained views, and the
  same reason the maintenance cost is proportional to the change.
- The **frontier / probe** mechanism is a completeness signal: when the
  probe reports that time `t` is complete, every output change at times
  `<= t` has been produced.  That is the primitive the extension needs to
  know the view delta for a transaction is fully computed before commit.

A worked analogy already in the tree: `examples/dd/` builds exactly these
pipelines (map/filter/join/reduce/iterate), and the graph algorithms
(`bfs`, `scc`) are `WITH RECURSIVE` views maintained incrementally.  An
IVM for `SELECT a.x, count(*) FROM a JOIN b ON a.k = b.k GROUP BY a.x` is
`arrange_by_key(a)` and `arrange_by_key(b)`, `join_core`, then
`reduce`/`count`, probed, which is a transliteration of the SQL.


## 3. The embedding: run the engine inside the backend, synchronously

libdtfl was built to be embedded in a host like PostgreSQL.  Four facts
make it able to run inside a backend process, in the triggering
transaction's own thread, with no background worker and no shared state
leaking across transactions:

- **Host allocation.**  `dtfl_set_alloc_hooks` routes every allocation
  through callbacks.  Point them at `palloc`/`pfree` on a dedicated
  `MemoryContext` (per statement or per transaction).  When the context
  is reset at end of statement or on abort, the engine's memory is
  reclaimed with it; there is no separate free path to get wrong on
  rollback.  (`src/base/os_alloc.c` is the only place raw `malloc`
  appears, and the hooks replace it everywhere else; this is enforced by
  `tests/dist/test_api_discipline.sh`.)
- **Host panic.**  `dtfl_set_panic_hook` turns an engine abort into
  `ereport(ERROR, ...)` inside a `PG_TRY`/`sigsetjmp` region, so an
  engine failure unwinds as a normal PostgreSQL error and the transaction
  aborts cleanly rather than the backend crashing.  The skeleton in
  `examples/embed/pg_extension.c` already demonstrates all four hooks,
  including this panic-to-ereport path.
- **Host logging.**  `dtfl_set_log_hook` sends diagnostics to `elog`.
- **Cooperative, single-thread drive.**  `dtfl_worker_new_standalone()`
  returns a worker the caller steps by hand with `dtfl_worker_step()` on
  the calling thread.  There is no background thread, no `pthread_create`
  (confined to `src/base/os_thread.c` and simply never reached in this
  mode), and therefore nothing that violates the backend's one-thread
  model or its non-reentrant internals.  The engine runs only while the
  backend is executing the extension's code, synchronously, in the
  backend's transaction.

So the engine does not run "beside" PostgreSQL as a service.  It runs *as
a subroutine of the backend*, in the backend's thread, on the backend's
memory, inside the backend's transaction.  That is what lets it obey the
transactional context instead of racing it.


## 4. Mapping transaction time onto differential time

This is the crux, and where the design earns or loses correctness.

Differential dataflow's time is a lattice (`dtfl_time_*`).  For IVM the
natural choice is a **two-coordinate product time**
`Product<Xid-order, step>` (`dtfl_time_product`), or in the simplest
single-maintained-view case a single totally ordered coordinate
(`dtfl_time_u64`).  The coordinate is not wall-clock time; it is a
logical commit order.

The maintenance protocol per transaction `T`:

1. **Compute the base delta against T's snapshot.**  The extension
   collects the net changes `T` made to each base relation (section 5
   covers how), expressed against the snapshot `T` reads.  Isolation is
   satisfied here, by construction: the delta is derived from the rows
   `T` itself inserted/updated/deleted plus the base state visible in
   `T`'s snapshot, never from rows committed by concurrent transactions
   that `T` cannot see.
2. **Feed the delta at a logical time `t_T`.**
   `dtfl_input_session_insert`/`remove` buffer the `(row, +/-weight)`
   updates; `dtfl_input_session_advance_to(t_T + 1)` then `flush` seals
   time `t_T`.
3. **Step to completion.**  Call `dtfl_worker_step()` until
   `dtfl_op_frontier_less_than(probe, t_T + 1)` is false, i.e. the view
   output for time `t_T` is complete.  This is bounded work proportional
   to the delta, not the base size.
4. **Read the view delta out** at `t_T` (section 6) and stage it as the
   pending change to the stored view heap.
5. **Commit or abort atomically** (section 7).

Why a logical commit-order coordinate and not a snapshot id: differential
requires times to advance monotonically on an input, and it computes
*cumulative* collections at each time.  Serializing maintenance in commit
order gives a clean, monotone input frontier and makes "the view as of
commit `n`" a well-defined `dtfl_trace_to_vec_through(upper = n+1)` query.
The cost is that maintenance across transactions is serialized at the
point of applying deltas to the shared view state, discussed in section 8.


## 5. Capturing the base delta against the right snapshot

The extension needs, for each base relation and each maintained view, the
net multiset change the committing transaction made, as seen in that
transaction's snapshot.  PostgreSQL already offers the raw materials;
none of this is libdtfl's job:

- **AFTER ROW triggers** or the **transition tables** of an
  `AFTER STATEMENT` trigger (`REFERENCING OLD/NEW TABLE`) give the old and
  new images of every changed row.  Transition tables are the better
  primitive: they hand the whole statement's change set as two relations,
  which maps directly to a batch of `remove(old)` + `insert(new)` updates
  into the input session.
- An **UPDATE** becomes `remove(old_row, +1)` and `insert(new_row, +1)`,
  i.e. weights `-1` and `+1`, which is exactly how differential models a
  value change, and exactly the right thing for aggregates (a changed
  grouping key retracts from the old group and adds to the new).
- The changes are read from the transaction's own transition tables, so
  they are the changes *in that transaction's snapshot*.  Isolation is
  not something libdtfl enforces; it is preserved because the input is
  sourced from a snapshot-correct PostgreSQL mechanism.

For a first version, the view's dataflow is built fresh per transaction
from the committed base state the transaction sees plus its delta.  For
the maintained-across-transactions version (section 8), the arrangement
persists and only the delta is fed, which is where the cost truly becomes
proportional to the change.


## 6. Reading the view delta back out

Two libdtfl surfaces produce the output, and they correspond to the two
things an IVM needs:

- **The change stream** for applying to the stored heap.
  `dtfl_collection_inspect` delivers every `(record, time, diff)` the view
  produced.  Filtering to `time == t_T` yields exactly the rows to insert
  (`diff > 0`) and delete (`diff < 0`, `|diff|` copies) in the view's heap
  table.  This is the delta that commit will apply.
- **The point-in-time snapshot** for consistency and for serving reads.
  `dtfl_trace_to_vec_through(upper = t_T + 1)` enumerates the view's full
  contents as of logical commit `t_T`
  (`TraceReader::cursor_through`), the maintained equivalent of
  `SELECT * FROM Q`.  This is how a reader at a given isolation point can
  be served the view as of a specific commit, and how the stored heap can
  be validated against the engine's own notion of the view.

Signed weights come straight through: a reduce/count output is a value
with an `i64` diff, so `COUNT`/`SUM` maintenance is reading the running
value out, not recomputing it.


## 7. Making it transactional: commit, abort, crash

The stored materialized view is an ordinary PostgreSQL heap (plus its
indexes), so its physical changes are WAL-logged and MVCC-visible like any
table.  The engine computes *what* to change; PostgreSQL owns *when it
becomes visible*.

- **Apply inside the transaction.**  The view delta from section 6 is
  applied to the view heap with ordinary `heap_insert`/`heap_delete` (or
  SPI) *inside the triggering transaction*, typically from the
  `AFTER STATEMENT` trigger or a pre-commit hook.  Because it is the same
  transaction, the view change is WAL-logged with the base change and
  becomes visible atomically at commit.  A concurrent reader under MVCC
  sees either the old base and old view or the new base and new view,
  never a torn pair.
- **Abort is free for the engine's memory.**  The engine's state lives in
  a `palloc` context; on abort the context is reset and the engine's
  working memory vanishes with it.  The uncommitted view-heap changes roll
  back by normal MVCC.  Nothing in the engine needs an undo path, which is
  the point of routing allocation through the host.
- **Crash recovery** needs the stored view and the engine's persistent
  arrangement (section 8) to agree after restart.  The safe design keeps
  the authoritative state in the WAL-logged view heap and treats the
  in-memory arrangement as a rebuildable cache: on first use after a
  restart, the arrangement is reloaded from the view heap (or the base
  tables), the same way a REFRESH would seed it, after which maintenance
  is incremental again.  The engine's own trace spill (`src/dd/trace`,
  P12) is a within-process memory tool, not a crash-durable store, so
  durability stays PostgreSQL's job.

The invariant that keeps 1 and 2 honest: the view delta for `T` is
computed from `T`'s snapshot and applied in `T`, so it commits with `T`
or not at all, and it reflects only what `T` could see.


## 8. The shared, persistent arrangement: where cost becomes minimal

A per-transaction rebuild is correct but not cheap: it re-reads base
state.  The maintained version keeps the arrangement alive across
transactions, in shared memory owned by a background worker, and feeds it
only committed deltas in commit order:

- One background worker process holds the standalone worker and the
  arrangements for all maintained views, driven cooperatively (step on
  demand), not as a thread inside a backend.
- At commit, a backend hands its base delta and commit-order time to the
  worker (through a shared-memory queue), the worker steps the dataflow,
  and the resulting view delta is applied to the view heaps.
- Reads of a maintained view at a given commit point are served by
  `dtfl_trace_to_vec_through`.

This is where maintenance cost is finally proportional only to the change
size, and where arrangements are shared across many views (one indexed
base relation feeds every view that joins on it).  It is also where the
hard parts live: handing deltas across the backend/worker boundary without
losing isolation, ordering commits, and keeping the arrangement and the
WAL-durable heap consistent across crashes.  libdtfl supplies the engine
and the shared-trace registry primitive (P14, `dd_server`'s named
shared-trace registry is the in-process analogue); it does not supply the
PostgreSQL shared-memory, commit-ordering, or recovery machinery, which is
the extension's substantial remaining work.


## 9. Where the fit is clean, and where it is not

Clean, and the reason to use libdtfl at all:

- Multiset algebra with signed weights is SQL's semantics, not an
  approximation (`dtfl_diff_i64`).
- Every relational operator has a correct, already-implemented delta rule,
  including the genuinely hard ones: `reduce` handles retraction of a
  current `MIN`/`MAX` correctly (it recomputes the group's output from the
  group's remaining multiset, which is exactly what a correct IVM MIN must
  do), and `iterate` maintains recursive views incrementally.
- The embedding hooks let the engine run as a backend subroutine, in the
  transaction's thread and memory, so transactional atomicity falls out of
  running inside the transaction rather than being bolted on.

Not clean, and not libdtfl's to solve:

- **Isolation and visibility** are preserved only because the input is
  sourced from PostgreSQL's snapshot-correct transition tables and the
  output is applied in the same transaction.  libdtfl has no concept of an
  MVCC snapshot; feed it the wrong rows and it will faithfully compute the
  wrong delta.
- **Durability and crash recovery** are PostgreSQL's.  The in-memory
  arrangement is a cache to be rebuilt from the WAL-durable heap, not a
  system of record.
- **Commit ordering and cross-backend coordination** for the shared
  persistent arrangement are real distributed-systems work on the
  PostgreSQL side.
- **Performance.**  The port is correct but currently about an order of
  magnitude slower than hand-written Rust on its own benchmarks
  (`dev/P15-measurements.md`), and that is before the PostgreSQL
  marshalling cost of turning heap tuples into `dtfl` records and back.
  For small per-transaction deltas the absolute cost may still be far
  below a full REFRESH, which is the actual competitor; that is the
  benchmark to run before claiming a win, not the raw-throughput parity
  number.

## 10. A minimal first milestone

The smallest thing that proves the mechanism, before any shared-memory or
background-worker work:

1. A single maintained view defined by a map/filter/join/count query.
2. An `AFTER STATEMENT` trigger with transition tables on each base table.
3. Per statement: open a `palloc`-backed context, set the four hooks,
   build a standalone worker and the dataflow for `Q`, seed it from the
   base state the transaction sees, feed the transition-table delta at
   time `t`, step to the probe, read the output delta via `inspect`, and
   apply it to the view heap, all inside the triggering transaction.
4. Verify under `READ COMMITTED` and `REPEATABLE READ` that the
   maintained view equals `SELECT Q` at every commit, and that rollback
   leaves both base and view unchanged.

That milestone uses only the public `dtfl_*` API and the four embedding
hooks, is single-threaded and fully transactional, and is correct though
not yet cheap (it reseeds per statement).  Making it cheap is section 8,
which is mostly PostgreSQL engineering with libdtfl as the computational
core underneath.

Two runnable references live in `examples/ivm/`: `ivm_view.c` keeps a
join + GROUP BY view over persistent input sessions (the section 8
shape) and checks each `dtfl_trace_to_vec_through` read against a
recomputation, and `txn_harness.c` models BEGIN, statements, transition
tables, COMMIT and ROLLBACK in plain C, applying each committed view
delta to a toy view table.
