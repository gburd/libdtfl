# Performance investigation: C port vs Rust on the arrange benchmark

Recorded 2026-10-18 on `meh` (Xeon E5-2697 v2, 12 cores/24 threads, one
NUMA node, glibc malloc on both sides, schedutil governor with turbo
on; no sudo, so frequency was not pinned).  C: build-branch HEAD
`0c1aa64`, gcc 14, `-O2 -g -fno-omit-frame-pointer`, no
`--enable-debug`.  Rust: `differential-dataflow-v0.25.1` experiments
crate, rustc 1.95 nightly, `--release` with debuginfo and frame
pointers.  Every number below comes from a run in this session unless it
is marked *estimate*.  The raw data, scripts, profiles and prototype
patches are in `meh:/scratch/gburd/dtfl-perf` (see the last section).

Status: this report measured build `0c1aa64`, before the fixes it
recommends.  The leaked arrange reader (B1), the per-key `rewind_keys`
(B2) and the writer's strong trace reference (B3) are being fixed on
branch `feat/longrun-growth`; `dev/P15-measurements.md` will carry the
post-fix numbers.

Question: why is the port about 8x slower on `arrange` and 22-29x slower
on `count` throughput, and what would close the gap?  The earlier
answer (P15-measurements.md, columnar-experiment.md) was "the residual is
structural D1/D2 cost".  **For the most part, that answer is wrong.**
Most of the count gap is two port bugs, a fifth to a third of the
remaining arrange gap is two engineering fixes, and part of the arrange
"gap" is a benchmark that does different work on each side.

## (a) Benchmark equivalence

I read `bench/experiments/arrange.c` and the pinned
`experiments/src/bin/arrange.rs` side by side.

| aspect | C | Rust | verdict |
|---|---|---|---|
| workers | 1 (`dtfl_execute_directly`) | 1 (`execute_from_args(args.skip(4))`, no `-w`) | same |
| key distribution | uniform `[0, keys)`, xorshift | uniform `[0, keys)`, StdRng | same |
| removals | independent random keys | `rng2`, seeded like `rng1`, so it retracts earlier inserts | differs; **measured no effect** (below) |
| timestamps | `t = wave*rate + round`, advance every round, flush once per wave | same (peers = 1) | same |
| updates per wave | 2 * rate = 2000 | same | same |
| worker steps per wave | 3.00 (measured) | 3.00 (measured) | same |
| count arm | `count_total` (arranges, then counts), probe the output | `arrange_by_self().count_total()`, probe the output | same |
| arrange arm | arrange, **then `as_collection`** (re-emits every record) and probe the collection | `arrange_by_self().stream.probe()`: probes the **batch stream** | **not equivalent** |
| trace maintenance in the arrange arm | the trace is kept and grows (see bug B1) | `Arranged` is dropped, the writer's `Weak<TraceBox>` fails to upgrade, **no batch ever enters a spine** | **not equivalent** |
| closed-loop duration | 5 s default | 25 s, hard-coded | **differs**, and C count degrades with duration |
| constants | `DTFL_BUFFER_SIZE_BYTES = 1<<13`; merge batcher cap = default_capacity rounded to a power of two (512 rows for 24-byte rows); spine fuel `8 << level`, effort 1 | timely `container/src/lib.rs:280` `1<<13`; `VecMerger::target_capacity`; `spine_fueled.rs` `8 << batch_index` | same (read at the pins) |

The removal-RNG check: I gave the C bench a second, identically seeded
RNG for removals (`EXP_RNG2`, prototype W, 4 runs each, 5 s).  Count went
from 210-247K to 216-220K rec/s, astream from 727-792K to 727-778K.  No
effect, so I left it out of the comparison.

**Making the arrange arm equivalent** needs two things.

1. A public accessor for the batch stream, so the bench can probe it as
   Rust does:
   `DTFL_API dtfl_stream *dtfl_arranged_stream(const dtfl_arranged *a);`
   in `dtfl/arrange.h`, forwarding the existing internal
   `__dtfl_arranged_stream()` (`src/dd/arrange_int.h:344`).  Per
   AGENTS rule 5 it also needs a `src/dd/exports.sym` line and a man
   page.  The stream is scope-owned, so it stays valid after
   `dtfl_arranged_free()`.  Then the bench uses
   `dtfl_stream_probe_with(dtfl_arranged_stream(a), probe)`.  My
   prototype added this as an `astream` arm.
2. Writer `Weak` semantics.  Upstream's `writer.insert` drops the
   batch when `self.trace.upgrade()` fails.  Without that, freeing the
   `Arranged` still leaves the C writer maintaining a trace nobody can
   read.  Measured, single 5 s runs: with the reader moved but without
   the `Weak` emulation, astream ran 184K and arrange 167K rec/s.  With
   the emulation they ran 460K and 362K.  So the accessor alone makes
   things worse.

`as_collection` costs 1.25-1.33x on the C side.  Measured at 5 s, arrange
arm vs astream arm in the same build: X 363K vs 454K, Y 508K vs 678K,
W 547K vs 722K.

## (b) Side-by-side measurements

Configurations:

- **HEAD**: build branch as committed.
- **X**: HEAD plus the three semantic fixes (B1 reader moved into the
  `Arranged`, B2 no per-key `rewind_keys`, B3 writer `Weak`
  emulation), plus the `astream` arm.
- **Y**: X plus `static inline` vector push and reserve fast paths
  (`base_int.h`), plus stack buffers instead of two heap allocations
  per key in `__dtfl_upds_builder_seal`.
- **W**: Y plus an inline memcmp-then-length compare with an 8-byte
  bswap fast path at every `cmp == NULL` call site (identical order).
- **Z**: X plus only the upds_builder_seal stack buffers.  Used to
  isolate the malloc effect.

Throughput is the median of 7 runs, first discarded.  At 5 s, runs were
serial on cpu 3.  At 25 s, 8 lanes ran in parallel on distinct physical
cores (cpus 3, 5, ..., 17).  The Rust lanes are the interference
control: they match the serial Rust runs (count 669K vs 669K, arrange
2.96M vs 2.92M).

### Throughput (rec/s) and ratio to Rust

Rust baselines (25 s, its only duration): count **669,304**, arrange
**2,963,459**.

| config | count 5 s | count 25 s | arrange arm 5 s | astream 5 s | astream 25 s |
|---|---|---|---|---|---|
| HEAD | 30,244 (22.1x) | 12,013 (**55.7x**) | 360,386 (8.2x) | n/a | n/a; arrange arm at 25 s: 377,476 (**7.85x**, n=6) |
| X (bugs fixed) | 146,990 (4.55x) | 135,891 (**4.93x**) | 362,914 | 453,672 (6.53x) | 459,897 (**6.44x**) |
| Y (+inline vec) | 186,677 (3.59x) | n/m | 508,208 | 678,461 (4.37x) | n/m |
| W (+inline cmp) | 207,436 (3.23x) | 205,125 (**3.26x**) | 546,802 (5.42x) | 721,692 (4.11x) | 739,999 (**4.00x**) |
| Z (seal buffers only) | ~147K (5 runs) | n/m | n/m | ~470K (5 runs) | n/m |

At 25 s, HEAD count is 2.5x slower than at 5 s.  X and W are flat.  The
"22-29x" count figure was an artifact of the 5 s window.  At Rust's
duration the gap was 55.7x.

Loading (rate 0, 1M keys and records, single-shot, 7 runs): HEAD
arrange arm 261K, Y astream 401K, W astream 446K, Rust arrange 1,606K
rec/s.  That is 6.15x for HEAD and 3.60x for W.  The arms differ, so
read it as indicative only.

### Work per record (steady state)

Instructions and IPC come from `perf stat -I 1000` user-mode counters,
summed over the steady throughput window (load and warm-up intervals
dropped) and divided by records in that window.

| config | instr/record | IPC | L1D misses/s | mallocs/record | reallocs/record |
|---|---|---|---|---|---|
| Rust count | 3,618 | 2.01 | 1.39e7 | 0.036 | 0.0024 |
| Rust arrange | 843 | 2.04 | 1.99e7 | 0.021 | 0.0014 |
| HEAD count | 78,900 | **1.46** | 5.55e7 | 1.21 | 0.33 |
| HEAD arrange arm | 7,204 | 2.09 | 6.9e6 | 2.93 | 2.32 |
| X count | 17,643 | 2.13 | 7.9e6 | **12.8** | 0.59 |
| X astream | 5,362 | 2.23 | 6.9e6 | 1.94 | 0.31 |
| Y count | 11,983 | 2.07 | 1.13e7 | 0.11 | 0.60 |
| Y astream | 3,590 | 2.15 | 9.7e6 | 0.068 | 0.34 |
| Y arrange arm | 4,868 | 2.22 | 9.4e6 | n/m | n/m |
| W count | 10,973 | 1.99 | 1.18e7 | n/m | n/m |
| W astream | 3,145 | 2.07 | 1.21e7 | n/m | n/m |

Malloc counts come from an `LD_PRELOAD` counter
(`tools/malloccount.so`) that samples once a second; the column is the
delta between t = 4 s and t = 7 s divided by the records in that
window.

Reading the table:

- HEAD count does 21.8x Rust's instructions at 0.73x its IPC (L1 misses
  are 4x Rust's).  That is extra work, not layout.
- HEAD arrange does 8.5x the instructions at equal IPC.
- After the fixes (W), the residual is **3.0x the instructions for count
  and 3.7x for arrange, at equal IPC (1.99-2.07 vs 2.01-2.04)**.  Memory
  behaviour is not the problem.  Instruction count is.
- Loading at 100K (whole process, rate 0): HEAD 8,274 (count) and
  8,630 (arrange) instr/rec at IPC 2.27, against Rust 1,292 and 1,091
  at IPC 1.9.  Y astream: 5,200.  At 1M: HEAD 9,610 and 9,967, Rust
  1,532 and 1,331, Y 6,028.  Loading allocations at 100K: HEAD 1.36
  mallocs + 0.46 reallocs/rec (count) and 1.99 + 1.70 (arrange); Rust
  0.022 + 0.0024; Y astream 0.082 + 0.37.

### Batches, merges, activations

These are C counters (`DBG` instrumentation in the prototype).  I did not
instrument the Rust side.  It runs the same spine algorithm with the
same constants, and in the arrange arm it inserts no batches (see a).

| config | trace inserts | max pending | merges | merged records | batches per count cursor |
|---|---|---|---|---|---|
| HEAD count, 5 s | 79 | **79** (never introduced) | 0 in window; 73 at teardown | 0.76M at teardown | avg **38.5**, max 77 |
| HEAD arrange arm, 5 s | 1,091 | **1,091** | 0 in window; 1,084 at teardown | 19.1M at teardown | n/a |
| X count, 25 s | 1,759 | 1 | 1,752 | 22.2M (6.3 per input record) | avg 5.4, max 8 |
| X astream | 0 (dropped, as upstream) | 0 | 0 | 0 | n/a |

Peak RSS at 25 s: HEAD arrange arm **506 MB**, Rust arrange 8 MB, X/W
astream 13 MB, HEAD count 22 MB, X/W count 25 MB, Rust count 12 MB.
HEAD's arrange-arm memory grows linearly with run time.

Worker steps per wave are 3.00 on both sides.  Progress tracking is
under 1% of samples on both sides (C: `propagate_pointstamps` 0.27%,
`__dtfl_tracker_propagate_all` 0.18%; Rust: 0.23-0.62%).  Scheduling
and progress are not a cause.

### Profiles: how the hot functions map across

HEAD count (perf, cycles): `__dtfl_slices_advance` 40% self,
`__memcmp_sse2` 28%, `minimize_keys` 3.5%.  Inclusive:
`__dtfl_cursor_list_seek_key` 69%.

W and Y count (perf, instructions:u):

| C | Rust counterpart | C share | Rust share |
|---|---|---|---|
| `agent_set` -> `consider_merges` -> `__dtfl_merger_ord_work` | `MergeVariant::work` / `OrdKeyMerger` (spine merges) | 33% | 40% |
| `__dtfl_cursor_list_seek_key` | `CursorList::seek_key` -> `BatchContainer::advance` | 22% | 22% |
| `arrange_logic` | `arrange_core` | 25% | 33% |

Per-phase instructions per record, C Y count vs Rust: merges 2.8x, seek
3.4x, arrange 2.5x.

W astream vs Rust arrange (instructions:u, inclusive):

| C | Rust counterpart | C share (instr/rec) | Rust share (instr/rec) | ratio |
|---|---|---|---|---|
| `flush_through` (chunker consolidate: `msort`, `cmp_rows`) | `ContainerChunker` / `consolidate_into` (pdqsort) | 28% (880) | 28% (237) | 3.7x |
| `merge_two` (batcher merges) | `MergeBatcher::merge_by` | 19% (610) | 25% (209) | 2.9x |
| `__dtfl_merge_batcher_seal` (extract) | `MergeBatcher::seal` | 14% (440) | 13% (113) | 3.9x |
| `__dtfl_builder_ord_seal` | `OrdKeyBuilder` / `UpdsBuilder::seal` | 13% (421) | 22% (184) | 2.3x |

Top W astream self symbols: `__dtfl_batch_push_uninit` 17%,
`cmp_rows` 12%, `memmove` 10%, `msort` 8%, `merge_two` 7%, `cc_step`
6.5%, `__dtfl_upds_builder_seal` 5%.

The two sides have the same phases in about the same proportions.  The
C version runs 2.3-3.9x more instructions in each phase.

Call counts per throughput record (callgrind, Y build, throughput run
minus a loading-only run):

- **astream:** 22.3 `memcpy` (out of line through glibc), 13.4
  `dtfl_type_cmp` -> 13.4 `memcmp`, 9.1 `cmp_rows`, 8.7
  `__dtfl_batch_push`, 4.7 `cc_step`/`sink_push`, 2.0 `int_is_zero`
  (diff-class function pointer), 1.0 each of `int_one`,
  `__dtfl_upds_builder_push` and `__dtfl_upds_builder_seal`.
- **count:** 51 `memcpy`, 51 `memcmp`, 18 `dtfl_type_cmp`, 6.2
  `int_is_zero`, 3.2 `__dtfl_slices_advance`, 2.6 each of
  `__dtfl_time_advance_by`, `time_lattice` and
  `__dtfl_description_since`.
- **Rust:** all of the equivalents are inlined.  Total Ir per record:
  C Y astream 3,975 vs Rust arrange 841; C Y count 9,330 vs Rust
  count 3,380.

## (c) Causes, ranked by measured share of the gap

Shares are of the log gap at 25 s: a cause that is a factor `f` of a
total gap `G` gets `ln f / ln G`.

**count** (HEAD 55.7x):

| # | cause | measured factor | share |
|---|---|---|---|
| B1+B2 | leaked arrange reader (no compaction, pending batches grow) plus per-key `rewind_keys` | 11.3x (HEAD -> X, 25 s) | **60%** |
| E1 | out-of-line vector push/reserve (4 columns per row, each a call plus memcpy) | 1.27x at 5 s (X -> Y); X -> Z shows the seal-malloc part is about 1.00x | 6% |
| E2 | `dtfl_type_cmp` -> `memcmp` call chain for memcmp-ordered types | 1.11x at 5 s (Y -> W) | 3% |
| R | residual: 3.0x instructions, spread uniformly over phases | 3.26x | 29% |

At 5 s, B1 and B2 measured separately: reader move only, 101.9K (median
of 7); rewind removal only, 52.4K (one run); both, 147.0K; HEAD,
30.2K.  B1 is the larger part, and the two compound.

**arrange** (HEAD arrange arm 7.85x):

| # | cause | measured factor | share |
|---|---|---|---|
| A | benchmark non-equivalence (`as_collection` plus kept trace) | 1.22x (HEAD arrange arm -> X astream, 25 s) | 10% |
| E1+E2 | as above | 1.61x (X -> W astream, 25 s) | 23% |
| R | residual: 3.7x instructions, uniform over the four phases | 4.00x | 67% |

Allocation is not a cause in itself.  Z removes the 12.7 mallocs per
record that `__dtfl_upds_builder_seal` adds during merges (X count does
12.8/rec, Y 0.11) and gains roughly nothing: count 147K -> 147K,
astream 454K -> 470K.  glibc's tcache fast path is cheap.  The cost is
the instructions around each push.

## (d) Structural or fixable?

- **B1, leaked reader: FIXABLE, a port bug.**  `arrange_core` keeps
  `ac->reader` and gives the `Arranged` a clone.  Upstream moves its only
  reader out (`*reader_ref = Some(reader_local)`).  The kept reader never
  advances, so physical compaction stays at the minimum,
  `consider_merges` never introduces a batch, and every batch stays in
  `pending` until teardown.  `src/dd/reduce.c` `arrange_by_key` (about
  line 2996) has the same clone-and-keep pattern.  `upsert.c` keeps its
  reader too, but advances it, which is what upstream does there.
  Owned by feat/longrun-growth.
- **B2, rewind per key: FIXABLE, a port divergence.**  `count.c:231` and
  `threshold.c:214` call `__dtfl_cursor_list_rewind_keys()` after every
  key, so each seek restarts every batch cursor at 0.  Upstream
  `count.rs` and `threshold.rs` only `seek_key` forward.  Owned by
  feat/longrun-growth.
- **B3, writer holds a strong box: FIXABLE.**  `writer.rs` holds a
  `Weak` and skips the spine insert when it cannot upgrade.  The C
  writer holds a strong `__dtfl_trace_box` reference.  Prototype: if
  the box refcount is 1, release the batch and return.  This needs
  `arrange_logic` to retain the batch before `writer_insert`, as
  upstream's `batch.clone()` does.  It matters as soon as B1 is fixed
  (see a).
- **A, benchmark: FIXABLE.**  Add `dtfl_arranged_stream()` and the
  astream-style arm, and run the closed loop for 25 s (or report the
  duration).
- **E1, out-of-line pushes: FIXABLE within D1/D2.**  Make
  `__dtfl_vec_push*`, `__dtfl_batch_push*` and `__dtfl_batch_rec`
  `static inline` with constant-size copies when the width is 8 (and
  usually the time and diff widths are).  Measured 1.27x (count) and
  1.50x (astream).  The prototype's gcc `-Warray-bounds` /
  `-Wstringop-overread` false positives need a cleaner formulation
  before it can land under `-Werror`.
- **E2, compare dispatch: FIXABLE within D1.**  The order stays
  memcmp-then-length, so D1 is untouched.  Measured 1.06-1.11x.  Types
  with a user `cmp` (variable-width tuples, vec, string) keep one
  indirect call per compare.  That part is structural to D1, but it is
  not exercised here.
- **R, residual 3-3.7x instructions.**  The shape of the residual says
  it is mostly per-row plumbing, not the data model: the same phases,
  equal IPC, and every phase 2.3-3.9x.  Concrete changes, with
  *estimated* gains:
  - **R1, consolidator: FIXABLE.**  It index-merge-sorts (`msort` over
    `size_t` indices, each compare through `cmp_rows` ->
    `dtfl_type_cmp`), then copies into a fresh batch and swaps storage.
    Upstream sorts the tuples in place.  Sort a packed (row, index)
    array for fixed-width rows, or sort in place when `fixed_size != 0`.
    *Estimate 1.1-1.2x on arrange* (it is 28% of W astream at 3.7x
    Rust's instructions).
  - **R2, `merge_two` and `extract`: FIXABLE.**  They push row by row
    through `sink_push` and `cc_step` (a non-inline chain cursor with
    chunk-boundary checks per row).  Inline the cursor and copy runs
    with `__dtfl_batch_push_run`.  *Estimate 1.1-1.15x.*
  - **R3, chunk recycling: FIXABLE, small.**  Upstream's `VecMerger`
    stash reuses chunks; C allocates new batches and `realloc`s columns
    (0.34-0.60 reallocs/rec vs Rust 0.002).  Given the Z result, expect
    less than 1.05x (*estimate*).
  - **R4, merges: FIXABLE.**  `stash_updates` calls
    `__dtfl_time_advance_by` -> `dtfl_time_join` -> `time_lattice` (a
    switch on the time shape), plus `dtfl_time_depth` and
    `__dtfl_description_since`, per update.  Specialise for totally
    ordered depth-1 times and hoist the `since` lookup.  *Estimate 1.05x
    on count.*
  - **R5, count operator: FIXABLE, small.**  Diff-class callbacks go
    through function pointers per update (`is_zero` 6.2/rec, `one`,
    `negate`), plus `diff_value_bytes` per emit.  `int_is_zero` is
    1.2% self.  *Estimate under 1.05x.*
  - **R6, the 4-column row (arena + offsets + times + diffs):
    STRUCTURAL (D2).**  Every row push touches four vectors, and every
    read subtracts two offsets.  Once the pushes are inline, the direct
    cost is a few instructions per column per push.  At 8.7 pushes per
    record that is roughly 100-150 of the remaining ~2,300 extra
    instructions per record, *about 5%* (*estimate*).  Removing the
    offsets for fixed-width records is the columnar experiment's
    "second container shape", a D2 divergence.
  - **R7, columnar trace keys: re-scope.**  The columnar experiment's
    1.28x and its Amdahl ceiling were measured on HEAD.  There, 74% of
    count was the B1/B2-inflated seek over 38-77 batches.  After the
    fixes the whole seek is 22% of count, so columnar keys are now worth
    *at most about 1.15-1.2x on count* (*estimate*; I did not re-run
    `--enable-columnar` on the fixed tree).

**The prior P15 profiles were skewed by teardown.**  In the HEAD
arrange profile, 53% of all samples are in `arrange_free` ->
`__dtfl_trace_agent_free` -> `__dtfl_trace_box_adjust_physical` ->
`consider_merges`.  That is the pending backlog (1,084 merges, 19.1M
records) merged when the dataflow shuts down, outside the timed window.
In the count profile the share is 6%.  P15's "38% alloc/copy + 15%
compare" shares for arrange are percentages of a profile half made of
untimed teardown.  Its "count 74% in `__dtfl_slices_advance`" is the B1
plus B2 bug signature.  After the fixes, `__dtfl_slices_advance` is
7-11% self.

## (e) Bottom line

- **Measured:** without abandoning D1 or D2, three bug fixes and two
  inlining changes take count from **55.7x to 3.26x** and the equivalent
  arrange arm from **7.85x (non-equivalent arm) to 4.00x**, at Rust's
  25 s duration.  The bug fixes are owned elsewhere; the inlining
  changes are prototypes.  Memory at 25 s drops from 506 MB to 13 MB on
  the arrange arm.
- **Plausible without abandoning D1/D2 (*estimate*):** about **1.5-2.5x**
  of Rust on these arms.  The residual is 3-3.7x the instructions at
  equal IPC, spread evenly over phases dominated by out-of-line
  per-row plumbing (R1-R5).  The part that is the D2 row layout itself
  (R6) is about 5%.
- **Needs D1/D2 relaxed:** the 1.5x target on every row probably needs a
  fixed-width specialisation of the batch (no offsets, in-place sort)
  when `fixed_size != 0`.  That bends D2 (a second container shape) and
  needs an ADR.  Parity near 1.0x would need typed, monomorphised rows,
  which means abandoning D1.  Neither is needed to get the first 17x on
  count.

## What was not checked

- Prototypes Y, Z and W were not run through `make check`.  X was: 94
  PASS, 24 SKIP (every `pbt_*`: hegel not configured), and 3 FAIL.  All
  three failures are dist gates tripped by my instrumentation (`getenv`
  in library code, unexported `dbg_` symbols, `DBG` lines on stderr),
  not functional failures.  `test_arrange_lifetime`, `test_agent`,
  `import` and `trace_compaction` passed.  No ASan, UBSan or TSan run.
  The count output values of the prototypes were not compared against
  HEAD.
- Rust batch and merge counts were not instrumented.  Rust at 5 s cannot
  be measured without editing the binary.  C `-O3` (Rust release is
  `opt-level=3`) was not tried.  The CPU frequency was not pinned:
  instructions per record are frequency-independent, but absolute rec/s
  carry turbo noise of roughly 5-10% run to run.
- Only the arrange and count arms were investigated.  `graphs`,
  `graphs-static` and `distinct` were not.

## Artifacts (`meh:/scratch/gburd/dtfl-perf`)

- `c/src` is HEAD.  `c/exp`, `c/exp2`, `c/exp3` and `c/exp4` are
  prototypes X, Y, Z and W, each with a `c/build-*` tree.  Runtime
  toggles in X: `EXP_NO_MOVE`, `EXP_REWIND`, `EXP_STRONG`.  In W:
  `EXP_RNG2`.
- `patches/*-vs-head.diff` and the incremental `exp2-vs-exp.diff` and
  `exp4-vs-exp2.diff`.  These are measurement prototypes, not fix
  candidates.
- `rust/` is the pinned crate.  `arrange_steps` is `arrange.rs` with a
  step counter and an 8 s window.
- `timing*.tsv`, `final/` (25 s lanes), `steady/` (`perf stat -I` and
  malloc ticks), `pstat.out` (whole-run counters), `prof/*.data` (perf
  record: `c-*` HEAD, `x-*`, `y-*`, `yi-*`, `wi-*`, `r-*`, `ri-*`),
  `cg/*.cg` (callgrind), and `tools/` (`malloccount.so`, `cgcalls.py`,
  `cgdiff.py`).
