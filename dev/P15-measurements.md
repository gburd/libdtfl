# P15 measurements: fuzz campaign and performance parity

Recorded 2026-10-15 on `meh` (Linux x86_64, gcc 14.4.0, 24 cores).  These
are the P15 release measurements.  The fuzz result is a release gate; the
parity table is a measured, honest status, not yet a pass.

## Fuzz campaign

The property suites (hegel) run as the fuzz engine.  A campaign ran every
`pbt_*` suite at 10000 cases per property (the default is 200) under
AddressSanitizer, via `DTFL_PBT_CASES=10000`.

- 24 suites, roughly 230000 generated cases.
- Result: zero property violations, zero crashes, zero ASan errors or
  leaks.  23 suites completed within the per-suite 500 s cap; `pbt_dogs`
  (the heavy WCOJ triangle property) hit the cap at 10000 cases but
  passes cleanly at 2000, so it is slow, not failing.

Reproduce:

```
configure --enable-debug --enable-asan --with-hegel=PREFIX
DTFL_PBT_CASES=10000 ./tests/pbt/pbt_<suite>   # per suite, under setarch -R
```

## Performance parity

The three `experiments` benchmarks (`arrange`, `graphs`, `graphs-static`)
were built release (`-O2`) and run against the pinned Rust `experiments`
crate (differential-dataflow-v0.25.1) built `--release` on the same host,
same parameters.  The agreed subset is arrange / graphs / graphs-static
plus the graph algorithms; arrange is the representative data point.

`arrange`, keys = 100000, recs = 100000, rate = 1000, closed loop:

| metric            | C (rec/s) | Rust (rec/s) | C / Rust    |
|-------------------|-----------|--------------|-------------|
| arrange LOADING   |   271,055 |    3,460,956 | ~12.8x slow |
| arrange THROUGHPUT|   265,204 |    4,316,450 | ~16x slow   |
| count THROUGHPUT  |    27,469 |      673,888 | ~24x slow   |

Status: the port is correct but **not within the 1.5x parity target** --
it is roughly 13 to 24x slower than Rust on these workloads.  This is the
expected cost of the fidelity-first decisions (D1 byte-string records,
D2 the single refcounted-batch container with no columnar trie, and the
unoptimised allocation and copy paths), measured here rather than
assumed.  Reaching parity is a dedicated optimisation project: profile
the arrange and reduce hot paths, cut per-record allocation and copies,
and revisit the columnar layout.  It is tracked as the open P15 item;
the measurement itself is complete.

Reproduce the Rust baseline:

```
# in a checkout of differential-dataflow at the pin
cd experiments && cargo build --release --bin arrange
./target/release/arrange 100000 100000 1000 100000000 arrange
```

Note on the C invocation: the C `arrange` binary takes
`keys recs rate comp [seconds]` (no `work` positional), so the arrange
computation is selected by the 4th argument, e.g.
`arrange 100000 100000 1000 arrange 5`.  The Rust binary takes
`keys recs rate work comp`, with the computation in the 5th argument.

## Performance parity, after the first optimisation pass

A later pass profiled the arrange and count hot paths with `perf` and
landed three behaviour-preserving optimisations (each a separate commit
with a before/after measurement and the full `dev/check-all` matrix
green, including both hegel property-test configs and ASan/UBSan/TSan):

1. **memcmp fast path for composite types.**  A tuple or option whose
   byte encoding sorts identically to its logical order (built from unit,
   the big-endian / sign-flipped integers, with every tuple field but the
   last fixed width) now sets `cmp == NULL`, so `dtfl_type_cmp` takes the
   plain `cmp_raw` (memcmp) path instead of recursing field by field.
2. **Fewer allocations and copies on the batch build path.**  An
   uninitialised bulk vector push (no redundant memset of a span the
   caller overwrites), pre-reservation of the output batch columns in the
   merge batcher's sink and the consolidator, and a bulk run copy that
   moves a contiguous slice of records with one memcpy per column.
3. **Inlined raw comparison in the trace seek loop.**
   `__dtfl_slices_advance` (the key/value search behind every cursor
   seek, dominant in count) inlines the `cmp_raw` predicate for
   memcmp-ordered types, skipping the cross-TU `dtfl_type_cmp` dispatch.

Measured on `meh` the same way (release `-O2`, same host, median of five
runs with the first discarded, keys = recs = 100000, rate = 1000; the
Rust baseline rebuilt from the pin on the same host):

| metric            | C before | C after | Rust      | before | after |
|-------------------|----------|---------|-----------|--------|-------|
| arrange LOADING   |  273,813 | 425,641 | 3,542,655 | 12.9x  | 8.3x  |
| arrange THROUGHPUT|  276,321 | 366,774 | 2,948,381 | 10.7x  | 8.0x  |
| count   LOADING   |  300,939 | 434,900 | 3,177,577 | 10.6x  | 7.3x  |
| count   THROUGHPUT|   17,460 |  23,477 |   686,056 | 39.3x  | 29.2x |

(The absolute Rust and C numbers differ from the first table because
they were taken in a separate session; the host and parameters are the
same, so the ratios are the honest comparison.  The 1 M / 2 M-record
loading runs used while profiling show the same proportional gains.)

The pass cut the arrange gap from roughly 13x to about 8x and the count
throughput gap from roughly 39x to about 29x.  It did **not** reach the
1.5x target, and the remaining gap is dominated by the D1/D2 fidelity
decisions, which an optimisation is not allowed to undo:

- **Loading / arrange** (`perf`, after the pass): about 38% of the time
  is per-record allocation and copying in the merge batcher and the
  input buffer (`merge_two`, the `__dtfl_vec` push/reserve family,
  `memmove` on vector growth, `malloc`/`free`), and about 15% is record
  comparison (`dtfl_type_cmp`, `memcmp`, `cmp_rows`).  The copying is
  inherent to D2: every record is a byte span in an arena plus a `size_t`
  offset plus separate time and diff columns, so a single logical push
  touches four vectors, where Rust pushes one inline typed tuple.
- **Count throughput** (`perf`, after the pass): about 74% of the time is
  in the trace key search -- `__dtfl_slices_advance` plus the
  `dtfl_type_cmp`/`memcmp` it drives -- because the trace is a byte-arena
  trie (D2, no columnar layout), so each key seek is a memcmp binary
  search over length-prefixed byte keys rather than Rust's typed integer
  comparison over a columnar trie.

Closing the rest of the gap would require the columnar trie and inline
typed records that D1 and D2 deliberately rule out.  Within those
constraints the pass took the achievable wins; the residual is the
measured, structural cost of fidelity, not an un-optimised path.

Reproduce (C, on the same host, release build):

```
configure CFLAGS="-O2 -g"            # no --enable-debug for timing
make -j"$(nproc)"
bench/experiments/arrange 100000 100000 1000 arrange 5   # LOADING, THROUGHPUT
bench/experiments/arrange 100000 100000 1000 count 5
```

## Columnar key fast path (flag-gated experiment)

The profile above pins count throughput at about 74% in the trace key
seek (`__dtfl_slices_advance`).  To test how much of the gap a different
key representation can close, a deliberate non-fidelity experiment adds a
second container shape behind `--enable-columnar` (off by default,
`DTFL_COLUMNAR`): the trace's key container stores memcmp-ordered,
constant-width keys (u64, and the `(u64, unit)` benchmark key, recognised
via a constant-width check rather than `fixed_size`) as a flat stride
column and seeks it with a direct strided compare (a big-endian integer
load for width 8 and 4, else a fixed-width memcmp), dropping the offset
indirection and the per-record length bookkeeping.  It diverges from PLAN
D2 (one container shape); the faithful arena+offsets path stays the
default, and results are byte-identical.  Only the key container opts in;
the value container stays on the arena path because a value's stored
width is not guaranteed to equal its type's width.

Measured on `meh` the same way (release `-O2`, same host, median of
eleven pinned runs with the first discarded; the Rust baseline rebuilt
from the pin on the same host):

| metric            | C baseline | C columnar | Rust      | col/base | gap base | gap col |
|-------------------|------------|------------|-----------|----------|----------|---------|
| arrange LOADING   |    312,008 |    299,674 | 3,537,076 | 0.96x    | 11.3x    | 11.8x   |
| arrange THROUGHPUT|    356,338 |    347,619 | 2,987,692 | 0.98x    | 8.4x     | 8.6x    |
| count   THROUGHPUT|     31,134 |     39,878 |   689,832 | 1.28x    | 22.2x    | 17.3x   |

The columnar key seek gives a measured **1.28x** on count throughput
(31,134 -> 39,878 rec/s), cutting the count gap vs Rust from about 22x to
about 17x.  Arrange is unchanged (0.96 to 0.98x, within run-to-run noise)
because arrange's hot path is batch build (allocation and copy), not the
key seek; the arrange LOADING figure is a single-shot measurement per
process and is dominated by allocator and first-touch jitter.

This is below the 1.5x target, as expected and as Amdahl bounds it.  The
seek is about 74% of count, so even an infinitely fast seek caps count at
about 3.85x (gap 22x -> about 5.8x); the measured 1.28x is well under
that because the columnar seek is a representation change, not a free
seek.  The non-seek residual that the columnar key cannot touch is the
structural cost of D1/D2 that dominates once the key compare is cheaper:
the cursor_list merge and trie navigation per key (val-range bounds, the
singleton-encoded update walk, the per-key capability and frontier
bookkeeping in `count_logic`), and the diff accumulation.  Columnarizing
the key proves the micro-benchmark win is real inside the engine, but the
key compare was never the whole seek, and the seek was never the whole
count; closing the rest needs the inline typed records and columnar
update layout that D1 and D2 rule out.

Correctness: the full suite passes 105/0 with `--enable-debug
--enable-columnar`, and 129/0 with `--enable-debug --enable-columnar
--with-hegel`, identical to the faithful path, with the columnar seek
exercised by the dd, trace, arrange, count and reduce tests over u64 keys
and by the property suites.

Reproduce (release, both configs):

```
configure CFLAGS="-O2 -g"                 # baseline
configure --enable-columnar CFLAGS="-O2 -g"   # columnar, separate build dir
# median of eleven pinned runs, discard the first:
taskset -c 3 bench/experiments/arrange 100000 100000 1000 count 5
taskset -c 3 bench/experiments/arrange 100000 100000 1000 arrange 5
```

## After the compaction fixes (2026-10-18)

The numbers above were all taken on a build where no arranged trace ever
merged: `arrange_core` and reduce's `arrange_by_key` kept a reader that
nothing advanced, so the spine held every batch until shutdown and each
seek walked all of them (`dev/perf-investigation.md`).  Count throughput
therefore fell with run length, and the 5 s window hid how far.  The
fixes (merge `576519e`: the reader moved into the Arranged as upstream
does, advance_upper in count/threshold/join/reduce, the weak trace
writer) and the bench now matching upstream (probe the batch stream, run
25 s) change the picture.

Measured on `meh` (Xeon E5-2697 v2, schedutil, frequency not pinned),
release `-O2 -g`, `taskset -c 3`, 25 s closed loop, keys = recs = 100000,
rate = 1000, Rust rebuilt from the pin on the same host; medians:

| arm (25 s)         | C before fixes | C after fixes | Rust      | after: C / Rust |
|--------------------|----------------|---------------|-----------|-----------------|
| count THROUGHPUT   |     12,200     |   115,457     |   655,738 | 5.7x slower     |
| arrange THROUGHPUT |  (not equivalent; see below) | 421,977 | 3,066,834 | 7.3x slower |

Instructions per record (perf stat, user mode, whole run over throughput
records) agree with the ratios: count 22,900 for C against 4,400 for
Rust (5.2x); arrange 6,400 against 810 (8.0x), at similar IPC (2.2 for C,
2.0 to 2.1 for Rust).  So the residual is instruction count, not memory
behaviour.

"Before" for arrange is not comparable: the old arm probed an
as_collection re-emit and its trace grew to 3 GB at 25 s.

Peak RSS at 25 s, arrange arm: about 14 MB after the fixes, against 3.0
GB before (floki).

The same binaries are 4 to 5 times faster on a laptop-class Core Ultra 7
258V (floki: count about 600K, arrange about 2.2M rec/s), so only
same-host ratios are meaningful.

What this replaces: the "13 to 24x" and "8x arrange / 29x count" figures
and the claim that the residual is structural to D1/D2.  Most of that
gap was the port bugs.  The investigation's prototype also inlined two
comparisons and reached about 3.3x on count and 4.0x on arrange; those
changes are not merged yet, and it estimates that 1.5 to 2.5x of Rust is
reachable while keeping D1 and D2.
