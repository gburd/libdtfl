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
