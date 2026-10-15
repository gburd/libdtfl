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
