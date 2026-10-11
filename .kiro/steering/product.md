# libdtfl: product steering

Always-on context for work in this repo.

## What it is

A C11 library, `libdtfl`, plus tools, that gives C programmers what
three Rust projects give Rust programmers:

| Upstream (pinned) | libdtfl surface |
|---|---|
| timely-dataflow `timely-v0.31.0` | `dtfl/timely.h`: workers, scopes, streams, capabilities, progress tracking, operators, capture/replay, logging; Thread, Process, ProcessBinary, Cluster (TCP) execution |
| differential-dataflow `differential-dataflow-v0.25.1` | `dtfl/collection.h`, `dtfl/arrange.h`, `dtfl/trace.h`, `dtfl/algo.h`, `dtfl/dogs.h`, `dtfl/ddir.h`, `dtfl/server.h` |
| dataflow-diag `0fa322c9` and the differential `diagnostics` crate | `dtfl/diag.h` (in-process WebSocket diagnostics) and the `dtfl-diag` program (the tdiag port) |

It looks like a GNU project:

- build: autotools, `make check`, `make distcheck`;
- documentation: a Texinfo manual and mdoc man pages in sections 1, 3
  and 7;
- metadata: `NEWS`, `ChangeLog`, `AUTHORS`, `THANKS`;
- license: MIT, with upstream notices preserved.

## Who it is for

C programmers who want incremental, data-parallel computation without
Rust in their build.  The API has to make sense to someone who has
never read the Rust code.  Upstream names are kept, so someone who has
read it can find their way around.

## The decisions that shape everything

These are listed in full, with alternatives, in `dev/PLAN.md` section 4.
The short form:

- **Records** are byte strings described by a `dtfl_type` (len, cmp,
  hash, fmt).  Built-in encodings preserve order, and `cmp` must
  match Rust's `Ord`.  There is no code generation and no per-type
  monomorphisation.
- **There is one container**, the refcounted `dtfl_batch`.  This
  subsumes the columnar and `Rc` paths.
- **Times** are short vectors of u64 coordinates with a class per
  scope.  Coordinates are unsigned, signed, or `Duration`, each with a
  declared width.  The classes cover `()`, integers, `Duration`,
  `Product`, `AltNeu`, `PointStamp`, product pairs and lexicographic
  pairs.
- **Diffs** come from a closed set of classes plus one user class:
  signed and unsigned integers, `present`, `wrapping`, tuples and
  `vec`.  An operator that needs an algebra the diff lacks fails when
  the dataflow is built.
- **Closures** become a function pointer plus a context plus a
  destructor.  Rust's `Drop` side effects are explicit calls.
- **Progress tracking, the trace layer, join and reduce** are ported
  literally.
- **API scope.** Every timely 0.31 API is ported, including operators
  removed upstream after the pin.
- **Threads.** Worker handles stay on their worker thread, a few
  objects are thread-safe, and there is no hidden global state.
- **Interop.** The timely and differential log streams and the
  diagnostics protocol are wire-compatible with Rust 0.31.  Capture of
  user data and the cluster data plane are libdtfl-only.

## What "done" means

`dev/PLAN.md` section 13.  In brief: both ledgers have no `todo` rows,
all verification tiers are green on four platforms, the performance
table is published, and the ABI is frozen.

## Non-goals for 1.0

- Windows and WASM.
- Rust peers in a mixed cluster.
- A C++ API.
- Language bindings.
- User-defined timestamp lattices beyond compositions of the built-in
  classes.
- The upstream subsystems marked `deferred` or `subsumed` in the ledger.
