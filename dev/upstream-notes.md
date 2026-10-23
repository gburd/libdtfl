# Upstream notes

Things at the pins that look odd, ported as they are
(`.kiro/steering/porting.md`, "When upstream does something odd"),
unless the entry says libdtfl diverges.  Each entry gives the pinned
path, what looks wrong, and a reproducer.

## ChangeBatch::compact keeps a lone zero update

- `timely-v0.31.0:timely/src/progress/change_batch.rs`, `compact`,
  the guard `self.clean < self.updates.len() && self.updates.len() > 1`.
- With exactly one update, the body (sort, merge, `retain(|x| x.1 != 0)`)
  is skipped and `clean` is set to 1.  `update(k, 0)` on an empty batch
  therefore leaves a batch whose `is_empty()` is false, whose `len()`
  is 1 and whose `iter()` yields `(k, 0)`.  The doc comment says
  compaction discards elements whose accumulation is zero.
- Reproducer (the `compact` and `is_empty` bodies copied verbatim onto
  a `Vec`):

  ```rust
  let mut b = CB { updates: vec![(17, 0)], clean: 0 };
  assert!(!b.is_empty());   // passes at the pin
  ```

- Harm at the pin: none found.  Callers drop zero deltas before
  `update` or consume entries additively.  `MutableAntichain::rebuild`
  filters on `x.1 > 0`, so a lone zero never enters a frontier.
- libdtfl keeps the behavior (`tests/unit/test_antichain.c`,
  `change_batch_basics`), and the ChangeBatch property allows it.

## PointStamp summaries: results_in is monotone, not strictly

- `differential-dataflow-v0.25.1:differential-dataflow/src/dynamic/pointstamp.rs`,
  `PointStampSummary::results_in` and `followed_by`.
- A `retain` truncation maps distinct times to one, which is allowed.
  Two consequences are visible through composition:
  - a tuple `(PointStamp, T)` with a PointStamp first is not monotone in
    the time under `results_in`, because the tuple order lets the
    second component decrease once the first is strictly less;
  - `a.followed_by(b)` can succeed where applying `a` then `b`
    overflows, when `a` overflows a coordinate that `b` truncates away.
    `timestamp.rs` documents only that the composed and sequential
    results agree when both exist.
- Neither combination occurs in a pinned program.  libdtfl matches
  upstream, and `tests/pbt/pbt_time.c` states both exceptions.

## Refines<()> for signed integers starts at zero

- `timely-v0.31.0:timely/src/progress/timestamp.rs`,
  `implement_refines_empty`: `to_inner(_: ())` is `Default::default()`.
- For `i8` to `isize` that is 0, not `MIN`, so a root dataflow over a
  signed timestamp starts at 0 while `Timestamp::minimum()` is `MIN`.
  Times below 0 are then never available as capabilities from the root.
- libdtfl matches upstream (`__dtfl_time_to_inner`).

## Counter drops a flush once its produced count is drained

- `timely-v0.31.0:timely/src/dataflow/channels/pushers/counter.rs:25-28`,
  `Counter::push`: a flush (`None`) goes downstream only "if dirty",
  that is, if the shared `produced` batch is non-empty.  That batch is
  not the Counter's own: the operator that owns the output drains it
  into its `SharedProgress` every time it is scheduled (for an input,
  `Operator::schedule` in `operators/core/input.rs`).
- An `Exchange` pusher behind the Counter buffers records per peer and
  sends them only when the time changes or a flush arrives.  So: send
  a record, `flush()` the handle (the Exchange buffers it), let the
  input operator run once (it drains `produced`), then close the epoch
  (`advance_to` or drop).  `close_epoch` calls `done()`, the Counter
  sees an empty batch and swallows the flush, the Exchange never sends
  its buffer, and the dataflow never completes: the produced count was
  reported, but the message it counts is never delivered.
- Reproducer against crates.io `timely = "=0.31.0"`, two workers; it
  hangs (it completes without the `worker.step()`):

  ```rust
  timely::execute_from_args(["-w", "2"].map(String::from).into_iter(), |worker| {
      let mut input: InputHandle<u64, CapacityContainerBuilder<Vec<(u64, u64)>>> =
          InputHandle::new();
      worker.dataflow::<u64, _, _>(|scope| {
          scope.input_from(&mut input).exchange(|x: &(u64, u64)| x.0).probe();
      });
      if worker.index() == 0 {
          input.advance_to(1);
          input.send((0, 0));
          input.flush();
          worker.step();
      }
  }).unwrap();   // never returns
  ```

- Harm at the pin: a dataflow that flushes an input feeding an exchange
  and is stepped before the epoch closes can hang.  It is still on
  timely master (3012f8f2), and is reported upstream as
  https://github.com/TimelyDataflow/timely-dataflow/issues/824.
- libdtfl diverges here (ADR-0012, 2026-10-23; PLAN D12 item 3): a hang
  reachable with one record is a bug to fix, not behavior to match.
  `src/timely/push_counter.c` also forwards a flush when it pushed data
  since the last flush it forwarded, the fix proposed in #824.  It
  forwards every flush the pin forwards.  `tests/unit/test_input.c`
  input__advance_flush_step_close_completes is the case above, and
  hangs without the fix.
- libdtfl used to reach the hang in more programs: its input operator
  asked for a first schedule when built (the shared op_core in
  `src/timely/builder_raw.c`), which upstream's does not, so `produced`
  was drained before the first epoch closed even without an
  `advance_to`.  The input no longer does
  (`tests/unit/test_input.c` input__flush_step_close_completes).
- Found by `make check-sim` (notify_multiworker, then behind
  `DTFL_PBT_FLUSH_STEPS=on`, now drawn by default); the pinned case is
  in `tests/corpus/seeds.txt`.
