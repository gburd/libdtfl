# libdtfl

Timely and differential dataflow for C.

[![CI](https://github.com/gburd/libdtfl/actions/workflows/ci.yml/badge.svg)](https://github.com/gburd/libdtfl/actions/workflows/ci.yml)
[![Docs](https://github.com/gburd/libdtfl/actions/workflows/pages.yml/badge.svg)](https://gburd.github.io/libdtfl/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](COPYING)
[![C11](https://img.shields.io/badge/C-11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Sanitizers](https://img.shields.io/badge/tested-ASan%20%2B%20UBSan%20%2B%20TSan-brightgreen)](.github/workflows/ci.yml)

libdtfl is a C11 library that brings three Rust projects to C programmers,
ported function by function at a pinned upstream revision:

* **timely dataflow**, a data-parallel runtime for cyclic dataflows with
  partially ordered timestamps and progress tracking, on one thread, many
  threads, or many processes
  ([upstream](https://github.com/TimelyDataflow/timely-dataflow), pinned at
  `timely-v0.31.0`);
* **differential dataflow**, incrementally maintained collections (map,
  join, reduce, iterate, shared arrangements), its graph algorithms, and the
  dogsdogsdogs worst-case-optimal joins
  ([upstream](https://github.com/TimelyDataflow/differential-dataflow),
  pinned at `differential-dataflow-v0.25.1`);
* **the timely diagnostics tools**, as an in-process WebSocket server and the
  `dtfl-diag` program
  ([upstream](https://github.com/TimelyDataflow/diagnostics)).

The port is thread-safe, GNU-style, and MIT-licensed.  It keeps upstream's
names, comments, and algorithm constants in the correctness core (progress
tracking, the trace layer, join and reduce), and translates to idiomatic C
elsewhere.  Records are opaque byte strings described by a type descriptor,
so no code generation or templating is involved; see `doc/libdtfl.texi` and
`dev/adr/` for the design decisions.

## A 30-second taste

A differential dataflow that keeps a running count of distinct values and
reports it as the input changes:

```c
#include <dtfl/collection.h>
#include <dtfl/reduce.h>
#include <dtfl/record.h>
#include <dtfl/time.h>
#include <dtfl/timely.h>

static void build(dtfl_scope *scope, void *ctx) {
    build_ctx *c = ctx;
    dtfl_collection *in   = dtfl_input_session_to_collection(c->input, scope);
    dtfl_collection *dist = dtfl_collection_distinct(in, c->diff);
    dtfl_collection *out  = dtfl_collection_probe_with(dist, c->probe);
    dtfl_collection_free(out);
    dtfl_collection_free(dist);
    dtfl_collection_free(in);
}
```

The manual's Concepts chapter and the 18 programs under `examples/` build
this up from first principles; every snippet in the manual is compiled and
run by `make check`.

## Building

```sh
./bootstrap          # generates configure (from a git checkout)
./configure
make
make check           # runs the unit, property, and dist-gate test tiers
sudo make install
```

Requirements: a C11 compiler (gcc or clang), autotools (for `./bootstrap`
from git; the release tarball ships `configure`), and `pkg-config`.
`mandoc` and `makeinfo` are used for the man-page and manual checks if
present.  Property tests use the hegel engine when configured with
`--with-hegel=PREFIX`, and report SKIP otherwise.

Link against it with `pkg-config`:

```sh
cc myprog.c $(pkg-config --cflags --libs dtfl)
```

## Status

Functionally complete.  Every upstream file is ported or resolved with a
recorded reason, and every upstream `#[test]` is ported, subsumed, deferred,
or excluded with a reason:

| area                               | state                                   |
|------------------------------------|-----------------------------------------|
| timely runtime + operators         | complete (5 execution modes incl. TCP)  |
| differential collections + traces  | complete                                |
| join, reduce, iterate, arrange     | complete                                |
| graph algorithms + dogsdogsdogs    | complete                                |
| diagnostics (WebSocket + dtfl-diag)| complete                                |
| DDIR compiler + dtfl-ddir + server | complete                                |
| examples (timely, dd, dogs, diag)  | complete                                |
| manual + 73 man pages              | complete, gate-enforced                 |
| port ledger / test ledger          | 365 files, 130 tests, 0 outstanding     |

Validated on Linux x86_64 (gcc 14, all six build configurations: default,
ASan, UBSan, TSan, hegel, distcheck), FreeBSD (clang 19), and Linux riscv64
(gcc 15, a strict-alignment architecture).  The port and test ledgers in
`dev/` are the definition of done, and `tests/dist/test_completion.sh`
enforces them.

### Performance

Measured with `dev/bench-ec2` on a dedicated c7i.metal-24xl (Xeon
Platinum 8488C, one socket, clock pinned at 2.4 GHz, C and Rust runs
alternated, medians of five, at build 5cedb4c), against the pinned Rust
build on the same host:

| `arrange` benchmark, C slower by | 1 | 2 | 4 | 8 | 16 | 48 workers |
|---|---|---|---|---|---|---|
| `arrange` arm | 1.17x | 1.05x | 1.15x | 0.92x | 1.08x | 1.01x |
| `count` arm | 1.29x | 1.29x | 1.30x | 1.23x | 1.27x | 1.00x |

On a two-socket c6i.metal (Xeon 8375C, clock pinned at 2.9 GHz) the
ratios are the same with workers packed on one socket or spread across
both (1.27x to 1.40x at 1, 4 and 16 workers); spreading costs C and Rust
alike, about 20% of throughput at 16 workers.

The cost of a step is within 1.5 times too: timely's `barrier` example
(an idle dataflow) runs a round in 1,839 ns against Rust's 1,350 with
one worker and 1.10x with four, and its `event_driven` example (one of
many dataflows advanced per round) runs 100,000 rounds in 0.27 s
against 0.22 s, whether the dataflows hold 100 or 1,000 operators.

Latency is within 1.5 times at light load and not at heavy load.  In
the benchmark's open-loop mode, which schedules requests on a fixed clock
so a stall delays every request behind it, one worker answers as follows
(microseconds, p50 / p99 / p99.9, rates in records per second):

| arm, offered rate | C | Rust | C p99 slower by |
|---|---|---|---|
| `arrange`, 2.0M | 12.3 / 16.4 / 17.4 | 8.2 / 11.8 / 12.3 | 1.39x |
| `arrange`, 4.1M | 18.4 / 25.6 / 27.6 | 9.7 / 14.3 / 15.4 | 1.79x |
| `arrange`, 6.1M | 29.7 / 38.9 / 43.0 | 12.3 / 17.4 / 18.4 | 2.24x |
| `count`, 380k | 38.9 / 131 / 213 | 29.7 / 86.0 / 119 | 1.52x |
| `count`, 760k | 111 / 1638 / 1901 | 69.6 / 557 / 721 | 2.94x |
| `count`, 1.14M | 557 / 6291 / 7078 | 180 / 2621 / 2884 | 2.40x |

(c7i.metal-24xl at 2.4 GHz, build 7792070, medians of three alternated
runs.)  Both gaps come from per-update cost, not from doing different work.
Both builds take three steps per round, seal one batch per round and
make about the same number of allocation calls.  On `count` they apply
the same merge fuel: 19.3M against 20.5M units of merge effort over a
run, in calls of a few hundred to a few thousand units each.  What
differs is how long a merge step takes: 463 of C's steps ran over half
a millisecond, against 5 of Rust's.  On `arrange` a
round with one insert and one remove costs C 6.5 microseconds against
Rust's 4.9, and each further pair 0.21 microseconds against 0.13.  That
makes the gap grow with load, since a round carries more updates the
faster they arrive.  The fixed part is the progress tracker and channel
bookkeeping, about a quarter more instructions than Rust's.  The per-update
part is the sort, the batch builder and the session buffer.  On
`count` a merge step runs longer in C (the cursor seeks and the update
builder), so a request that arrives during one waits longer.  Closing
these gaps means changing the sort and the trie builder, which are
ported literally.  Allocation calls per record are 1.10 times Rust's on
the `arrange` arm and 1.35 times on `count`, and futex calls per record
at 16 workers are below Rust's.

An incrementally maintained view (`bench/ivm_latency`: a join and a
grouped sum over 100,000 rows, ten-row commits, one worker) turns a
commit into its view delta in 111 / 138 microseconds (p50 / p99) at
1,000 or 5,000 commits per second, when the sum is kept in the
difference (`explode`, then `count`).  Computed with `reduce` instead,
each commit costs the size of the groups it touches.

The record and container design (ADR-0001, ADR-0002) is unchanged; the
gains came from port bugs that kept arranged traces from compacting, a
benchmark driver that drew deletions from the wrong generator, chunk
sizes that ignored constant-width records, operators that ran on every
frontier change where upstream's do not, a lock-free queue between
workers in place of a mutex, upstream's chunk and buffer recycling and
capacity reservation, and fast paths for 8-byte records, one-word times
and keys, short change batches and 64-bit integer differences, each
checked against the general path by a test.

`bench/experiments/arrange` takes the Rust binary's arguments, so one
command line runs both: `arrange keys recs rate work comp [-w N ...]
[open-loop]`.  It prints the same `ARRANGE LOADING`, `THROUGHPUT` and
`LATENCY` lines, in the closed-loop and the open-loop mode, with each
worker pinned to its own CPU as upstream does.  Its random keys come
from a different generator than Rust's `StdRng`.

## Documentation

* The reference manual: `doc/libdtfl.texi` (build with `make html` or
  `make pdf`), with Concepts, Threads, Error handling, Embedding, Timely,
  Differential, Diagnostics, DDIR, and an API Reference chapter.
* Man pages: `man/man3` (the API), `man/man7` (`dtfl.7`, `dtfl_embedding.7`),
  `man/man1` (the `dtfl-diag`, `dtfl-ddir`, `dtfl-server` tools).
* Design decisions: `dev/adr/` (12 ADRs) and `dev/PLAN.md`.

## Embedding

libdtfl routes allocation, panic, diagnostic logging, and thread
creation through host hooks, so it embeds in a host such as PostgreSQL
(alloc to `MemoryContextAllocHuge` and `repalloc_huge`, panic to
ereport, log to elog, and one worker stepped on the backend's own
thread).  A panic that longjmps out leaves that worker unusable: the
host discards it, for example by resetting its memory context, and
rebuilds.  See `dtfl_embedding(7)` and the worked skeleton in
`examples/embed/`.  `examples/ivm/` shows an incrementally
maintained SQL view driven one commit at a time, and a harness that models
PostgreSQL transactions and transition tables around it.

## Contributing

The rules for contributors, human or otherwise, are in `AGENTS.md`.  Changes
to the correctness core are ported literally against the pinned upstream;
every claim lands with a test; `make check` must stay green across the build
configurations.

## License

MIT; see `COPYING`, which also carries the notices of the upstream projects
this port derives from.
