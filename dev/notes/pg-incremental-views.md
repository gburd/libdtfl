# Incremental materialized views on an UNDO table AM, with libdtfl

This is the design handoff for building incremental view maintenance
(IVM) in PostgreSQL on the UNDO branch (`~/ws/postgres/undo`), using
libdtfl's differential dataflow engine to compute view deltas.  It is a
design, not shipped code.  Two runnable references ship with libdtfl and
are the starting point for the team:

- `examples/ivm/ivm_view.c`: a join + GROUP BY view kept current one
  commit at a time over open input sessions, reading each commit's view
  delta and the whole view as of that commit, checked every commit
  against a from-scratch recomputation.
- `examples/ivm/txn_harness.c`: a plain-C model of the PostgreSQL side:
  BEGIN, statements, OLD/NEW transition tables, COMMIT with a commit
  order number, ROLLBACK that feeds nothing, and applying each committed
  view delta to a toy view table, again checked every commit.
- `examples/ivm/sqlrow.h` with `sqlrow_check.c`: encodings of SQL column
  values (NULL, integers, dates and timestamps, float8 with PostgreSQL's
  NaN and -0 rules) whose byte order is the SQL order, so group and join
  keys compare and hash as SQL requires; the check program tests each
  rule against a reference comparator and through a GROUP BY dataflow.

Both build and run under `make check` (`tests/dist/test_examples_ivm.sh`)
with no server, using only the public `dtfl_*` API.

Differential dataflow is the right engine for *computing*
`dMV` from `dBase`, and an UNDO-based table AM is the right place to
*capture* `dBase` and to *apply* `dMV` with the transaction's own
atomicity, isolation and recovery.  Most of what made IVM hard in a
trigger-based design (snapshot-correct capture, commit ordering, crash
recovery of the derived state, cross-backend coordination) is either
already provided by the UNDO machinery or reduces to something small
once the view's storage is itself an UNDO table.  Section 9 is explicit
about the parts that do not fall out and still need design.


## 1. What true IVM requires

A materialized view `MV` is defined by a query `Q` over base relations.
`REFRESH MATERIALIZED VIEW` recomputes `Q`.  IVM instead applies only
`dMV = dQ(Base, dBase)` for the changes a transaction committed.  The
requirements that make this a database problem rather than a dataflow
problem:

1. **Atomicity.**  `dMV` is visible exactly when the triggering
   transaction commits, and never if it aborts.
2. **Visibility and isolation.**  `dMV` reflects the changes the
   transaction made, against the state it was entitled to see, and
   concurrent transactions produce consistent, non-interfering deltas.
3. **Multiset and aggregate semantics.**  Rows carry signed
   multiplicities; `COUNT`/`SUM` track running values; `MIN`/`MAX` must
   handle retraction of the current extreme.
4. **Cost proportional to the change**, not to the size of the base
   tables or of the view.
5. **Durability and recovery.**  After a crash, the view and its
   maintenance state agree with the base tables.

Requirements 3 and 4 are what differential dataflow does.  Requirements
1, 2 and 5 are what an UNDO table AM does.  The design joins them at
commit.


## 2. The engine: what libdtfl provides

Differential dataflow is incremental multiset algebra over partially
ordered time.  The SQL correspondence is direct:

- A **collection** is a multiset of records with signed `int64` weights
  (`dtfl_diff_i64`).  A row present `k` times has weight `k`; DELETE
  contributes `-k`; UPDATE is `remove(old)` + `insert(new)`.
- Every **operator** has its delta rule built in: `map` (projection and
  scalar expressions), `filter` (WHERE), `join_map`/`join_core`
  (equi-join over arrangements), `reduce`/`count`/`distinct`/`threshold`
  (GROUP BY and aggregates), `concat` (UNION ALL), `negate` + `concat`
  (EXCEPT ALL), `semijoin`/`antijoin` (EXISTS / NOT EXISTS), and `iterate`
  (WITH RECURSIVE).  `reduce` handles MIN/MAX retraction correctly: it
  recomputes a group's output from the group's remaining multiset.
- **Arrangements** are maintained, shared indexes.  One arrangement of a
  base relation by a join key serves every join and every view that needs
  it (`dtfl_arranged_trace` hands out readers; `dtfl_trace_import` uses
  one in another dataflow on the same worker).
- The **probe** is a completeness signal: once it passes commit `n`,
  every output change at times `<= n` has been produced.

The embedding surface (all public, all exercised by the examples and the
test suite):

| need | libdtfl |
|---|---|
| run in a backend's thread, no threads of its own | `dtfl_worker_new_standalone`, `dtfl_worker_step` |
| allocate in a PostgreSQL MemoryContext | `dtfl_set_alloc_hooks` |
| turn an engine abort into `ereport(ERROR)` | `dtfl_set_panic_hook` |
| route diagnostics to `elog` | `dtfl_set_log_hook` |
| feed a delta at a logical time | `dtfl_input_session_insert/remove/advance_to/flush` |
| know the delta for commit `n` is complete | `dtfl_probe_less_than` |
| read the view delta for commit `n` | `dtfl_collection_inspect` (filter to time `n`) |
| read the whole view as of commit `n` | `dtfl_trace_to_vec_through(upper = n+1)` |
| keep the maintained state bounded | `dtfl_trace_set_logical_compaction`, `dtfl_trace_set_physical_compaction` |

`examples/embed/pg_extension.c` shows the four hooks against stubbed
PostgreSQL symbols (alloc to palloc, panic to `ereport` through a
`sigsetjmp` region, log to `elog`, single-thread cooperative drive).


## 3. The substrate: what the UNDO branch provides

The facts below are from `src/backend/access/undo/README` and
`src/backend/access/flux/README` on the UNDO branch.

- **Two UNDO stores.**  The cluster-wide store embeds UNDO in the WAL
  (`XLOG_UNDO_BATCH`) and suits sparse, cross-object UNDO.  The
  per-backend store gives each backend its own append-only UNDO log
  (`access/undo/perbackend/`, segment files under `base/undo`, routed
  through `SMGR_UNDO` and buffered in `shared_buffers`), with no shared
  insertion point on the hot path; its pages are WAL-protected, and its
  records are folded onto the AM's forward WAL records.  A table AM
  chooses with `am_supports_undo` and `am_undo_engine =
  UNDO_ENGINE_PERBACKEND`.
- **FLUX**, the UNDO-based table AM, updates rows in place, keeps the
  old version in the per-backend UNDO log (`t_verptr`), reconstructs old
  versions for older snapshots by walking the chain
  (`FluxReconstructVisibleVersion`), and rolls back by applying UNDO in
  reverse.  Visibility is heap-shaped `xmin`/`xmax` against CLOG and the
  snapshot.
- **What is in a transaction's UNDO.**  FLUX writes UNDO records for
  UPDATE (`FLUX_UNDO_UPDATE`, or `FLUX_UNDO_UPDATE_DELTA`, a byte-diff
  of the before-image) and DELETE (`FLUX_UNDO_DELETE`, the deleted
  tuple).  It writes **no UNDO record for INSERT**; the sLog tracks a
  transaction's inserted tuples backend-locally
  (`SLogTupleCollectTrackedKeys`).  So the UNDO chain holds the OLD side
  of a transaction's changes, not the NEW side.
- **Rollback.**  `AtAbort_XactUndo` / `PbuAtAbort_ApplyUndo` apply the
  chain; a large chain (`undo_rollback_overflow_size`, default 16 MB) is
  handed to an undo apply worker.  Subtransaction abort applies the
  child's range.  CLRs make rollback idempotent across crashes.
- **Recovery.**  After redo, per-backend UNDO recovery registers loser
  transactions as rollback requests; an undo apply worker reverts them
  once the database opens.  Readers never see a loser's changes, because
  visibility is resolved through the writer's commit status.
- **Commit-time ordering in `CommitTransaction`.**  Deferred triggers
  fire first (`AfterTriggerFireDeferred`), then portals are closed, then
  `PbuAtPreCommit_Undo()` and the `XACT_EVENT_PRE_COMMIT` callbacks run,
  then `RecordTransactionCommit()`; locks are released after the commit
  record.  The transaction can still run statements and write tables in
  a deferred trigger; `PRE_COMMIT` callbacks run after portals are gone
  and are not the place for table writes.
- **Logical decoding of FLUX** exists (`flux_decode` in `decode.c`): the
  committed changes of FLUX tables can be decoded from WAL, including
  old images for UPDATE/DELETE when the table is logged with logical
  images.
- **UNDO resource managers** are registered by id (`RegisterUndoRmgr`,
  `UndoRmgrData { rm_name, rm_undo, rm_desc }`); FLUX is
  `UNDO_RMID_FLUX = 5`.


## 4. The design

### 4.1 The shape

```
  base tables (FLUX)                 maintained view (FLUX)
        |                                      ^
        | per-statement OLD/NEW images         | apply dMV in the
        v   (the transaction's own writes)     |  same transaction
   transaction-local delta buffer              |
        |                                      |
        +-- end of xact: commit number n ---->+
                       |                       |
                       v                       |
             libdtfl dataflow for Q  -- dMV(n) +
             (arrangements persist across commits)
```

1. **Capture in the AM, per statement.**  The base tables are FLUX
   tables.  For each statement that modifies a base table of a
   maintained view, the AM records the statement's OLD and NEW row
   images into a transaction-local delta buffer.  The old images are the
   before-images FLUX already builds for its UNDO record; the new
   images are the tuples it writes.  This is the same information an
   `AFTER STATEMENT` trigger's `REFERENCING OLD TABLE / NEW TABLE`
   transition tables carry, captured where it is produced.  An
   `AFTER STATEMENT` trigger with transition tables is a correct first
   implementation of this step and needs no AM change; the AM
   hook is an optimization that avoids materializing the transition
   tables.
2. **Rollback and savepoints need nothing new for the base tables.**  A
   ROLLBACK discards the transaction-local delta buffer with the
   transaction's memory, and FLUX's UNDO restores the base rows.  A
   `ROLLBACK TO SAVEPOINT` must drop the buffer's entries from the
   aborted subtransaction; tag each buffered row with its subtransaction
   and truncate on `SUBXACT_EVENT_ABORT_SUB`.  The engine never sees an
   uncommitted or rolled-back change.
3. **Feed and compute at end of transaction.**  A deferred constraint
   trigger (`CREATE CONSTRAINT TRIGGER ... DEFERRABLE INITIALLY DEFERRED`,
   one per maintained view, registered on its base tables) fires once
   per transaction inside `AfterTriggerFireDeferred`, while the
   transaction can still write tables.  `AfterTriggerFireDeferred` loops
   until no deferred events remain, so the trigger must fire once per
   view per transaction (use a transaction-local "already maintained"
   flag), and the view table itself must not be a base table of a view
   maintained this way, or its writes would queue more maintenance.
   (Views over views are the cascade case in section 9.)  There the
   transaction takes the next **maintenance commit number** `n` for each
   affected view (section 4.3), feeds its buffered delta to the view's
   dataflow at time `n`, advances the input sessions to `n + 1`, and
   steps until the probe passes `n`.  It then reads `dMV(n)` with an
   `inspect` callback filtered to time `n`.
4. **Apply in the same transaction.**  The transaction applies `dMV(n)`
   to the stored view, itself a FLUX table: delete `|d|` rows for each
   negative diff, insert `d` rows for each positive diff (or update a
   per-group row in place, which is what FLUX is good at).  The view's
   changes are ordinary FLUX writes of the committing transaction, so
   they are WAL-logged with the base changes, become visible atomically
   at commit, and are undone by UNDO if the transaction aborts after this
   point.
5. **Serve reads from the view table.**  Readers query the stored view
   under their own snapshot like any table.  FLUX reconstructs older
   versions from UNDO, so a `REPEATABLE READ` reader keeps seeing the
   view as of its snapshot while maintenance proceeds.  The dataflow's
   trace is the maintenance state, not the read path.

`examples/ivm/txn_harness.c` implements steps 1 to 4 in miniature, and
`examples/ivm/ivm_view.c` implements steps 3 and 4 for a two-table join
with GROUP BY.

### 4.2 Why the view should itself be an UNDO table

This is the decision that makes most of the hard problems fall out:

- **Atomicity is the transaction's.**  `dMV` is written by the
  committing transaction, so it commits or aborts with the base change.
  There is no separate "apply the view later" step to lose.
- **Abort after apply is free.**  If anything after step 4 fails (a
  constraint, `ereport`, a crash before the commit record), FLUX's UNDO
  reverts the view rows as it reverts the base rows; a crash is
  handled by per-backend UNDO recovery.
- **Readers are isolated by the AM, not by the engine.**  The engine has
  no notion of an MVCC snapshot and does not need one: snapshot reads of
  the view are FLUX reads, with old versions reconstructed from UNDO.
- **In-place group updates are cheap.**  A GROUP BY view changes a few
  group rows per commit; FLUX updates them in place without dead tuples
  or index churn when no indexed column changes.

### 4.3 Commit order

Differential dataflow needs each input's times to advance monotonically,
and `dMV(n)` is defined relative to the view after `n - 1`.  So for a
given view, maintenance must apply committed deltas in one total order,
and that order must be the order the base changes become visible.

The simplest correct rule: **a transaction that modifies a base table of
view `V` takes `V`'s maintenance lock in exclusive mode at
end of transaction (in the deferred trigger of 4.1 step 3), takes the
next maintenance time `n` (in placement 1, `last_n(V) + 1`; in
placement 2, the worker's next unused time), computes and applies
`dMV(n)`,
and holds the lock until its commit record is written** (it is released
with the transaction's other locks, after `RecordTransactionCommit()`).
Because the lock is held through the commit record, the order of `n` is
the commit order for those transactions, and no other transaction can
compute `dMV` against a state that does not yet include `n`.

This serializes commits of writers to the same view's base tables, which
is the cost of synchronous, in-transaction maintenance.  Writers
of unrelated tables, and readers of anything, are unaffected.  The lock
can be finer than the view: per arrangement key range is possible later,
but start with per view.  Deadlock with ordinary row locks cannot arise
from this lock alone if it is only ever taken in the deferred trigger,
after the transaction's own statements are done; the maintenance step's
writes touch only the view table and its `last_n` row, which only lock
holders write.  Two views sharing base tables must take their locks in a
fixed order (by view OID).

### 4.4 Where the dataflow lives

Two placements, in order of ambition:

1. **Per transaction, reseeded.**  In the deferred trigger, after taking
   the maintenance lock, build the dataflow, seed it from the base tables
   under a snapshot taken after the lock (section 6), feed the delta,
   read `dMV`, and drop everything.  The snapshot sees the committed
   state plus this transaction's own writes, which is the post-change
   state.  Seed time 0 with the pre-change state, which is those rows
   with this transaction's changes taken back out (drop its NEW images,
   add back its OLD images), then feed the delta at time 1; the view
   delta at time 1 is `dMV`.
   Correct, simple, single-backend, no shared state; cost is
   proportional to the seeded state, not the change.  Good for a first
   milestone and for small views.
2. **Persistent, in a maintenance worker.**  One background worker per
   database (or per view group) holds a standalone libdtfl worker and the
   arrangements for all maintained views.  A committing backend, holding
   the view's maintenance lock, sends its delta to the worker through a
   shared-memory queue (`shm_mq`).  The worker feeds it at the next
   unused dataflow time `n`, steps to the probe, and returns `dMV(n)`,
   which the backend applies to the view table in its own transaction.
   Maintenance cost is then proportional to the change.  The worker
   holds `n` as tentative until the backend reports the outcome: from an
   `XACT_EVENT_COMMIT` callback (the commit record is written, the lock
   not yet released) it confirms `n`; from `XACT_EVENT_ABORT` it asks the
   worker to discard `n` (section 9 explains how, since a fed time cannot
   be taken back).  Only confirmed times count toward the committed
   state.

The persistent placement needs the compaction calls: after commit `n` is
durable and no reader needs the engine's state before `n`, advance the
view trace's logical and physical compaction holds to `n`
(`dtfl_trace_set_logical_compaction` / `..._physical_compaction`) so the
arrangements stay bounded.  Without that, a long-lived reader handle pins
every historical update.

### 4.5 Recovery of the maintenance state

The stored view table is durable and authoritative; the dataflow's
arrangements are a cache.  On restart, the maintenance worker rebuilds
each view's arrangements by loading the base tables (or the view table)
at the last committed maintenance number, which is stored transactionally
in a small catalog row `(view, last_n)` updated in the same transaction
as each `dMV` apply.  Loser transactions need no special handling: their
base and view changes are reverted by UNDO recovery, their `last_n`
update never committed, and the rebuild starts from the committed state.
libdtfl's trace spill is a memory tool, not a durability mechanism, and
is not used for recovery.


## 5. How the requirements are met

| requirement | met by |
|---|---|
| atomicity | `dMV` applied by the committing transaction to a FLUX view table; aborted by UNDO |
| reader isolation | FLUX MVCC on the view table, old versions from UNDO |
| writer correctness | each transaction's delta is its own statements' OLD/NEW images; deltas applied in commit order under the view's maintenance lock |
| multiset semantics | `dtfl_diff_i64` weights; delete/insert by multiplicity at apply |
| aggregates incl. MIN/MAX retraction | `reduce` over the group's remaining multiset |
| cost proportional to change | persistent arrangements in the maintenance worker (placement 2) |
| durability and recovery | WAL and UNDO for base and view tables; arrangements rebuilt from the committed view state and `last_n` |
| bounded memory | compaction holds advanced as commits become durable |


## 6. Isolation, precisely

- The delta a transaction feeds is the set of changes *it* made, taken
  from its own statements.  It does not depend on its snapshot, so it is
  correct at every isolation level.
- Its OLD images are always the latest committed versions of those rows.
  A writer holds the row lock from its write until its commit, so no
  other transaction can change the row in between; under READ COMMITTED
  an UPDATE re-reads a concurrently updated row before writing it
  (EvalPlanQual), and under REPEATABLE READ or SERIALIZABLE it fails
  with a serialization error instead.  So feeding `remove(OLD)` against
  the committed state never retracts a row version that is not there.
- What the snapshot affects is the *base state the delta is joined
  against*.  In placement 2 the engine joins against the state after all
  previously committed maintenance numbers, which, because of the lock
  in 4.3, is the committed state including every earlier writer.  That
  is the right state for computing a view that must equal `Q` over the
  committed base after this commit, regardless of what the transaction's
  snapshot could see.  It is the same reason a trigger-based IVM must
  join the delta against the *current* committed state, not the
  snapshot, under `REPEATABLE READ`.
- In placement 1, seeding from the transaction's own snapshot is correct
  only if no concurrent writer of the same base tables committed after
  that snapshot; the maintenance lock plus a reseed under a fresh
  snapshot taken after acquiring it makes it correct.
- `SERIALIZABLE`: a writer that reads the view and writes a base table
  creates the usual rw-dependencies on the view table and base tables;
  SSI handles them as for any tables.  The maintenance step's own reads
  happen under the maintenance lock and should be done with a fresh
  snapshot, outside SSI predicate tracking, as internal catalog-like
  maintenance.
- `examples/ivm/txn_harness.c` models two concurrent transactions that
  commit out of begin order and shows deltas are fed in commit order; it
  does not model row-level write conflicts, which PostgreSQL resolves
  before commit by blocking.


## 7. The libdtfl side, concretely

Per maintained view, one dataflow on one standalone worker:

```c
dtfl_worker *w = dtfl_worker_new_standalone();
/* per base relation: an input session of (row, i64) over u64 time */
dtfl_input_session *orders = dtfl_input_session_new(order_type, i64);
/* build: to_collection -> map/filter -> join_map -> reduce -> inspect
 *        -> arrange (the view trace) -> probe */
dtfl_worker_dataflow(w, "region_totals", dtfl_time_u64(), build, &ctx);

/* end of transaction, commit n, holding the view's maintenance lock */
for (each OLD row r) dtfl_input_session_remove(orders, r, len);
for (each NEW row r) dtfl_input_session_insert(orders, r, len);
next = n + 1;
dtfl_input_session_advance_to(orders, &next);   /* every session */
dtfl_input_session_flush(orders);
while (dtfl_probe_less_than(probe, &next))
	dtfl_worker_step(w);
/* inspect collected dMV(n): apply it to the view table */
/* once n is durable and nothing reads the engine state before n: */
dtfl_trace_set_logical_compaction(view_trace, &n, 1);
dtfl_trace_set_physical_compaction(view_trace, &n, 1);
```

`examples/ivm/ivm_view.c` is the complete version of this, with the
self-check.  Things to copy from it: every input session advances every
commit, even one with no rows for that table; the probe is attached after
the view's arrangement, so passing `n` means the trace is complete
through `n`; and type descriptors passed to operators are bound to locals
and freed (an inline nested `dtfl_type_pair` leaks).

Record encoding (`examples/ivm/sqlrow.h` implements the fixed-width and
NULL cases and documents the rest): libdtfl records are byte strings with a type descriptor
(PLAN D1).  A PostgreSQL row maps to a tuple descriptor of fixed-width
fields (`int2/4/8`, `float8` by an order-preserving encoding, `date`,
`timestamp`) plus `bytes` for varlena values in their comparison-correct
form.  Equality and grouping on text with a nondeterministic collation
need a user type descriptor whose `cmp` calls the collation; joins on
such keys must hash consistently with that `cmp`.


## 8. Milestones

1. **Trigger prototype, placement 1.**  An extension with an
   `AFTER STATEMENT` trigger using transition tables on each base table,
   a transaction-local delta buffer, and a deferred constraint trigger
   that takes the view lock, reseeds a per-transaction dataflow, computes
   `dMV`, and applies it to a FLUX view table.  Prove: the view equals
   `SELECT Q` after every commit under READ COMMITTED and REPEATABLE
   READ, ROLLBACK and ROLLBACK TO SAVEPOINT leave both base and view
   unchanged, and a crash between apply and commit leaves both reverted.
   `examples/ivm/txn_harness.c` is the shape of this, outside the server.
2. **Persistent maintenance worker, placement 2.**  The worker, the
   `shm_mq` protocol, the `last_n` catalog row, rebuild on restart,
   compaction holds advanced on commit.  Prove the same properties under
   concurrent writers and restarts, and measure maintenance cost against
   delta size.
3. **AM-level capture.**  Replace the transition tables with OLD/NEW
   images captured in the FLUX write path, avoiding their
   materialization.
4. **Wider SQL.**  Outer joins (as union of an inner join and an
   antijoin), DISTINCT, HAVING, MIN/MAX, `WITH RECURSIVE` via `iterate`.


## 9. What does not fall out of UNDO

These are open design work, not solved by the substrate:

- **Commit ordering is a design choice, not a property of UNDO.**  The
  per-backend UNDO engine orders nothing across backends; the
  maintenance lock in 4.3 is what orders deltas.  It serializes writers
  of a view's base tables at commit.
- **The NEW side of a change is not in UNDO.**  FLUX's UNDO holds UPDATE
  and DELETE before-images; inserts have no UNDO record.  The delta
  buffer, or transition tables, or logical decoding must supply new
  images.
- **Discarding a tentative `n` in placement 2.**  Differential times
  only move forward, and computing `dMV(n)` already advanced the inputs
  to `n + 1`, so time `n` is sealed and cannot be un-fed or amended.  Two
  ways to discard an aborted `n`:
  - feed the negated delta at the next time, `n + 1`.  The inputs then
    net to zero across `n` and `n + 1`, so the arrangements after
    `n + 1` equal those after `n - 1`.  The view output at `n + 1` is
    `-dMV(n)`; the worker drops it, since the view table already rolled
    back with the transaction.  The next committing writer feeds at
    `n + 2`.
  - compute `dMV(n)` in a scratch dataflow that imports the persistent
    arrangements (`dtfl_trace_import`), and feed the persistent inputs
    only after the commit is confirmed.
  The first is simpler.  It needs the negation fed before any other
  transaction feeds, which the maintenance lock guarantees: no one else
  can take a time until the aborting transaction releases the lock.
  Either way the dataflow time is not the committed maintenance count, so
  the worker owns the dataflow time (it hands out the next unused time),
  and the `last_n` row records the dataflow time of the last committed
  step.  Gaps after an abort are harmless.  Compaction holds advance
  only to confirmed times.  Placement 1 has no such issue.
- **Deferred (asynchronous) IVM** is a different design: decode
  committed FLUX changes from WAL (`flux_decode`) in commit order and
  apply them in a background transaction.  It removes the commit-time
  serialization and gives up read-your-writes on the view.  The libdtfl
  side is identical; only the capture and apply points move.
- **Performance.**  libdtfl is correct but measured roughly 8x slower
  than Rust differential dataflow on its `arrange` benchmark and more on
  `count` (`dev/P15-measurements.md`).
  The comparison that matters for IVM is maintenance cost versus REFRESH
  and versus trigger-based IVM for realistic delta sizes; run that
  before drawing conclusions.
- **Views over views.**  Maintaining a view whose base is another
  maintained view needs the inner view's `dMV(n)` fed to the outer
  view's dataflow in the same step, ideally as one libdtfl dataflow over
  both, not by chained triggers.
- **Type mapping** (section 7) is real work for collations, numerics and
  `NULL` (encode `NULL` as an `option` tag so it sorts and joins the way
  SQL does, and filter `NULL` join keys before the join).
