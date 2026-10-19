# Columnar layout experiment: can it reach 1.5x parity?

**Correction (2026-10-18).**  The measurements and the Amdahl argument
below were taken on a build in which no arranged trace ever merged
(`arrange_core` kept a reader that nothing advanced; see
`dev/perf-investigation.md`).  Every key seek therefore walked tens of
unmerged batches, which is why the seek looked like 74% of count.  With
that fixed the seek is about a fifth of count, so the columnar key is
worth roughly 1.15 to 1.2x (an estimate, not re-measured), and the
conclusion that 1.5x is unreachable without undoing D1/D2 does not hold.
The flag and its code are unchanged; read the numbers below as history.

Recorded 2026-10-15 on `meh` (Linux x86_64, gcc 14.4.0, 24 cores).

This document answers a specific question: the port is about 8x slower
than Rust on `arrange` and about 29x on `count` throughput
(`dev/P15-measurements.md`); the profile attributes most of the count
cost to the trace key seek over a byte-arena-plus-offsets key
representation.  A columnar key representation (a flat typed column, the
way Rust's columnar `BatchContainer` works) is the obvious lever.  Would
it close the gap to the 1.5x target, and what would it cost in fidelity?

## What "columnar" means here, concretely

The faithful trace stores keys as a `SliceContainer`: one byte arena of
concatenated record bytes plus a `size_t` offsets array (PLAN D2, "one
container").  A key seek (`__dtfl_slices_advance`) binary-searches the
sorted keys; each comparison reads `offs[i]` and `offs[i+1]`, subtracts
to get the length, indexes the arena, and `memcmp`s the common prefix
then compares lengths.  This holds any record type, fixed or variable
width, which is exactly what D1 (records are opaque byte strings) and D2
(one container) buy: no code generation, no per-type container.

A columnar representation specialises the common case.  When the key
type is fixed width and memcmp-ordered (`u8`..`u64`, `i8`..`i64`, and
fixed-width tuples of them), store the keys in a flat stride column: one
contiguous buffer of `n * stride` bytes, no offsets array, no per-record
length.  A seek then indexes the column directly (`col[i]`) and compares
with a single integer load for width 8 or 4, or a fixed-stride `memcmp`.
This is what Rust gets for free from monomorphising the trace over a
`Vec<u64>`-style container.

## The ceiling: an isolated micro-benchmark

Before touching the engine, a standalone micro-benchmark
(`columnar_ceiling.c`) ran the exact `__dtfl_slices_advance` algorithm
over the same sorted `u64` keys in both representations, timing only the
seek:

| keys      | faithful (arena+offs+memcmp) | columnar (flat u64+intcmp) | seek speedup |
|-----------|------------------------------|----------------------------|--------------|
| 100,000   | 1321 ns/seek                 |  402 ns/seek               | 3.3x         |
| 1,000,000 | 1654 ns/seek                 |  751 ns/seek               | 2.2x         |

So the columnar key makes the seek itself 2 to 3x faster: it removes the
offsets indirection, the length subtraction, and replaces `memcmp` with
an integer compare.  Real, but bounded.

## Amdahl's law caps the win well short of 1.5x

The count-throughput path is about 74% key seek and 26% other work
(cursor and trie walk, output batch construction, the diff accumulation).
With the seek sped up by a factor `s`, overall count improves by
`1 / (0.26 + 0.74/s)`:

| seek speedup `s` | count overall | gap 29.2x becomes |
|------------------|---------------|-------------------|
| 2.2x (measured)  | 1.68x         | 17.4x             |
| 3.3x (measured)  | 2.07x         | 14.1x             |
| infinite         | 3.85x         | **7.6x**          |

Even an infinitely fast seek leaves count at 7.6x slower than Rust,
because the other 26% is itself several times slower than Rust's
equivalent.  To reach 1.5x, count would have to be 19.5x faster overall,
which the key representation alone cannot deliver: `0.26 + 0.74/s = 1/19.5`
has no solution for any `s > 0` (the floor `0.26 > 0.051`).

`arrange` loading has even less seek in it (about 15% comparison, 38%
allocation and copying), so the columnar key moves it less.

## The end-to-end measurement, behind a flag

The flag-gated columnar key path was built (`--enable-columnar`, off by
default; see `dev/P15-measurements.md` for the full table and reproduce
steps) and measured on `meh`:

| metric             | C baseline | C columnar | col/base | gap vs Rust |
|--------------------|------------|------------|----------|-------------|
| arrange LOADING    |    312,008 |    299,674 | 0.96x    | 11.3 -> 11.8x |
| arrange THROUGHPUT |    356,338 |    347,619 | 0.98x    | 8.4 -> 8.6x  |
| count THROUGHPUT   |     31,134 |     39,878 | 1.28x    | 22.2 -> 17.3x |

The columnar key gives a measured **1.28x** on count throughput, cutting
the count gap from about 22x to about 17x.  Arrange is unchanged (within
noise): its hot path is batch build (allocation and copy), not the key
seek.  The full suite passes 105/0 and the hegel config 129/0 with the
flag on, identical to the faithful path.  1.5x is not reached, exactly as
the Amdahl ceiling predicted: the measured 1.28x is below even the
seek-only ceiling because the columnar path changes the key
representation, not the whole seek, and the seek was never the whole
count.

## How the columnar layout makes the port less faithful

The port's fidelity rests on three decisions:

- **D1: records are opaque byte strings described by a `dtfl_type`.**
  The library never inspects record bytes; it only calls the
  descriptor's `cmp` and `hash`.  This is how C avoids Rust's
  monomorphisation and still holds strings, tuples, and user types.
- **D2: one container, the refcounted byte-arena batch.**  Rust is
  generic over containers (`Vec`, columnar, `Rc<C>`); libdtfl has a
  single `dtfl_batch` (arena plus offsets, plus parallel time and diff
  columns).  The ledger marks the whole columnar subsystem `subsumed`
  because the flat arena gives what Rust's columnar gives, through one
  code path.
- **The trace layer is ported literally** (AGENTS section 2): `ord_neu`,
  the cursors, `__dtfl_slices_advance`, the merge batcher.  Upstream's
  names, comments, and the exponential-search constants are kept.

A columnar key column breaks the second decision and bends the third:

1. **A second container shape.**  D2's promise is one container.  The
   columnar path adds a parallel representation (flat stride column) that
   the init, push, get, and advance paths must branch on.  Every place
   that reads a key now has two cases.  That is precisely the generic-
   over-containers complexity D2 chose to avoid, reintroduced for one
   type class.
2. **It only helps the fixed-width case.**  Variable-width keys (bytes,
   string, vec) and user types keep the arena path, so the fast path is a
   special case, not a replacement.  The code carries both forever.
3. **It diverges from the literal port.**  `__dtfl_slices_advance` is a
   faithful translation of upstream's `advance`.  The columnar
   specialisation is not in upstream's SliceContainer; it is upstream's
   *columnar* container, which the port deliberately did not take as a
   separate type.  Keeping both is a divergence that a reviewer can no
   longer check line-for-line against one upstream file.

## The trade-off

The columnar key representation buys a measured 2 to 3x on the seek, and
by Amdahl a smaller end-to-end gain (count from 29x toward the mid-teens,
arrange less), for the cost of a second container shape that must be
maintained, branched on at every key access, and justified against D2.
It does not reach 1.5x, and it cannot: the structural gap is spread
across the whole pipeline (allocation, copying, cursor and trie walk,
output construction), not concentrated where one representation change
can remove it.

Closing the gap to 1.5x would require not one columnar container but the
whole Rust strategy: inline typed records (undoing D1), a family of
columnar containers chosen per type (undoing D2), and abandoning the
literal trace port for a typed reimplementation.  That is a different
library, not an optimisation of this one.

## Recommendation

Keep the columnar path as a measured, flag-gated experiment
(`--enable-columnar`, default off), not a shipped default:

- It documents, with numbers, exactly how much speed the fidelity
  decisions cost and where, which is useful for anyone weighing the port
  against a from-scratch typed implementation.
- It stays off by default, so the shipped library remains the faithful,
  single-container, literally-ported port the project set out to be.
- If a future consumer has a hot, fixed-width-keyed workload and accepts
  the divergence, the flag is there and proven correct (identical test
  results).

The honest status for 1.0 is unchanged: the port is correct and about an
order of magnitude slower than Rust, by deliberate design, and the
1.5x target is not reachable without becoming a different library.  That
is a decision to record, not a bug to fix.
