# Upstream notes

Things at the pins that look odd, ported as they are
(`.kiro/steering/porting.md`, "When upstream does something odd").
Each entry gives the pinned path, what looks wrong, and a reproducer.

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
