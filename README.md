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
| port ledger / test ledger          | 356 files, 110 tests, 0 outstanding     |

Validated on Linux x86_64 (gcc 14, all six build configurations: default,
ASan, UBSan, TSan, hegel, distcheck), FreeBSD (clang 19), and Linux riscv64
(gcc 15, a strict-alignment architecture).  `dev/COMPLETION.md` is the
binding definition of done and `tests/dist/test_completion.sh` enforces it.

### Known gaps before 1.0

One substantive item is tracked honestly rather than hidden:

* **Performance.** The port is correct but not yet performance-competitive.
  After a first optimisation pass it runs about 8 times slower than the Rust
  originals on the `arrange` benchmark and about 29 times slower on `count`
  throughput, measured against the pinned Rust on the same host.  The
  residual is the structural cost of the fidelity-first record and container
  decisions (ADR-0001, ADR-0002): byte-span records with separate time and
  diff columns, and a byte-arena trie rather than inline typed tuples and a
  columnar trie.  Closing it to the 1.5x goal would require undoing those
  decisions.  See `dev/P15-measurements.md`.

A `1.0` release requires this closed or a decision to relax the goal; a `0.y`
release ships with it documented.

## Documentation

* The reference manual: `doc/libdtfl.texi` (build with `make html` or
  `make pdf`), with Concepts, Threads, Error handling, Embedding, Timely,
  Differential, Diagnostics, DDIR, and an API Reference chapter.
* Man pages: `man/man3` (the API), `man/man7` (`dtfl.7`, `dtfl_embedding.7`),
  `man/man1` (the `dtfl-diag`, `dtfl-ddir`, `dtfl-server` tools).
* Design decisions: `dev/adr/` (12 ADRs) and `dev/PLAN.md`.

## Embedding

libdtfl routes allocation, panic, diagnostic logging, and thread creation
through host hooks, so it embeds in a host such as PostgreSQL (alloc to
palloc, panic to ereport, log to elog, threads to background workers, or a
single-thread cooperative mode).  See `dtfl_embedding(7)` and the worked
skeleton in `examples/embed/`.  `examples/ivm/` shows an incrementally
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
