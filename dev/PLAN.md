# libdtfl: plan for a C port of timely and differential dataflow

Status: proposed, 2026-10-10, revised the same day after an independent
review against the pinned upstream (24 findings, all checked and
addressed below).  Nothing is implemented yet.  This file is the design
of record.  Where it and the tree disagree, the tree wins and this file
gets fixed in the same commit that exposes the gap.

The per-file inventory is `dev/port-ledger.tsv` and the per-test
inventory is `dev/test-ledger.tsv`.  `dev/ledger-diff` generates both
from the pins.

- **Port ledger.**  458 upstream files: 365 `todo`, 58 `excluded`,
  18 `deferred`, 17 `subsumed`.
- **Test ledger.**  162 upstream `#[test]` functions: 139 `todo`,
  22 `subsumed`, 1 `deferred`.
- **What a row covers.**  The ledger lists `.rs`, `.html`, `.ddp`
  and `.ddir` files, the DDIR example programs and sessions, and the
  mdbook chapters.  It does not list READMEs, changelogs,
  `formal/*.lean` (the model tests cite these, section 6) or
  `diagnostics/console/src/` (that frontend is used unmodified).  It
  also leaves out `timely/examples/wasm-bindgen/`, which is outside
  upstream's own workspace.

This plan covers the architecture and the order of work.  The ledgers
say which upstream file or test is done.

## 1. Goal

libdtfl is a C11 library, `libdtfl.so.0`, that gives C programmers the
features of three Rust projects:

- timely dataflow: a data-parallel, cyclic dataflow runtime with
  partially ordered timestamps, progress tracking, and multi-thread
  and multi-process execution;
- differential dataflow: incrementally maintained collections (map,
  join, reduce, iterate, arrangements, shared traces), its graph
  algorithms, and the dogsdogsdogs worst-case-optimal join operators;
- the dataflow diagnostics: in-process WebSocket diagnostics and the
  `tdiag` out-of-process inspector.

It ships as a GNU package: autotools, `make check` and `make distcheck`,
a Texinfo manual, man pages for every public symbol, examples, MIT
license.  The library is thread-safe in the sense defined in section 5.

Out of scope for 1.0, recorded so nobody silently adds them: Windows,
WASM, interop with Rust peers on the data plane (section 4, D9), a C++
wrapper, language bindings, and the experimental Rust-only subsystems
listed as `deferred` or `subsumed` in the ledger.

## 2. Pinned upstream

The port targets released code, not moving heads.

| Upstream | Pin | Commit |
|---|---|---|
| timely-dataflow | `timely-v0.31.0` | `eba4ae52` |
| differential-dataflow | `differential-dataflow-v0.25.1` | `6e53ac67` |
| dataflow-diag (tdiag) | HEAD, untagged | `0fa322c9` |

Reasons for these pins:

- differential 0.25.1 builds against timely 0.31.  Timely HEAD has six
  more commits after the pin:
  - #817, #818 and #819 remove operators;
  - #813 adds a stamp-set message model;
  - #816 changes coordination and adds `--idle-spin`;
  - #821 hardens byte aliasing.
- tdiag was last touched in 2021 and depends on timely 0.11.  Its wire
  format and log hooks are gone from current timely.  libdtfl ports
  tdiag's *computations* and has them read the timely 0.31 log format.

**The pin is the API.**  Everything public at `timely-v0.31.0` is
ported, including the operators that upstream deleted after the pin:
`Notificator`, `FrontierNotificator`, `unary_notify`, `binary_notify`,
`delay`, `delay_total`, `delay_batch`, `branch`, `branch_when`,
`aggregate`, `state_machine`, `accumulate`, `count`, `reclock`,
`iterator_source`, the `Result` stream helpers, and `inspect_time`,
`inspect_batch` and `inspect_container`.  There are three
reasons:

- code at the pin depends on them: differential's
  `Collection::delay` uses `delay_batch`, which uses `unary_notify`;
  `Collection::inspect_batch` uses `inspect_batch`;
- upstream tests and examples use them: `tests/barrier.rs`, and the
  `barrier`, `bfs`, `pingpong`, `ngo` and `dogsdogsdogs` examples;
- porting their tests literally requires them.

They cost 1,555 upstream lines in P4, spread across 10 ledger rows.  The manual marks each one
"removed upstream after timely 0.31" and shows the `unary_frontier`
equivalent.  Removing them later is a pin move, not a decision made
during the port.

The only post-pin upstream changes adopted are the two divergences
recorded in ADR D12 (section 4).

Moving a pin forward is a deliberate change: update this table,
regenerate the ledgers (`dev/ledger-diff`, section 12), and port the
delta.

## 3. Shape of the result

### 3.1 Libraries and tools

One shared library, `libdtfl`, with one pkg-config file, `dtfl.pc`.
Optional pieces compile in when configure finds their dependencies.

| Component | Public header | Depends on |
|---|---|---|
| core runtime (timely) | `dtfl/timely.h` | libc, pthreads |
| collections (differential) | `dtfl/collection.h`, `dtfl/arrange.h`, `dtfl/trace.h` | core |
| algorithms | `dtfl/algo.h` | collections |
| dogsdogsdogs | `dtfl/dogs.h` | collections |
| logging and capture | `dtfl/log.h`, `dtfl/capture.h` | core |
| in-process diagnostics | `dtfl/diag.h` | logging; built-in SHA-1 and base64 for the WebSocket handshake |
| DDIR interpreter | `dtfl/ddir.h` | collections |
| plugin server | `dtfl/server.h` | collections, `dlopen(3)` |
| umbrella | `dtfl/dtfl.h` | all of the above |

Programs installed to `bindir`:

- `dtfl-diag`, the tdiag port, with subcommands `graph`, `profile`, and
  `differential arrangements`.
- `dtfl-ddir`, which runs DDIR programs (`ddir_vec`) and serves them
  interactively (`ddir_server`).
- `dtfl-server`, the `dd_server` port, which loads dataflow plugins
  with `dlopen(3)`.
- `dtfl-cat`, which decodes a capture or log stream to text.  It is
  needed for debugging the wire format.

### 3.2 Repository layout

```
configure.ac Makefile.am  bootstrap  dtfl.pc.in  .clang-format
README NEWS ChangeLog AUTHORS THANKS COPYING  INSTALL (generated)
include/dtfl/*.h              public API, installed
src/base/                     alloc hook, panic hook, bytes, batch, hash, codec, log registry
src/progress/                 time, antichain, change_batch, reachability, subgraph, progcaster
src/comm/                     allocators (thread, process, binary, tcp), networking, spill
src/timely/                   worker, scope, stream, channels, pacts, operators, capture
src/dd/                       collection, diff, lattice, consolidation, trace/, arrange, join, reduce, iterate
src/algo/  src/dogs/  src/diag/  src/ddir/
tools/dtfl-diag/  tools/dtfl-ddir/  tools/dtfl-server/  tools/dtfl-cat/
examples/{timely,comm,dd,dogs,diag,server}/
tests/unit/                   one file per src module
tests/upstream/{timely,dd,dogs,ddir}/   ports of upstream integration tests
tests/pbt/                    hegel property tests
tests/model/                  reference models (naive collection semantics)
tests/dist/                   gates: API discipline, symbols, headers, ledger, docs
tests/interop/                Rust-produced fixtures for the wire formats in D9
bench/
doc/libdtfl.texi  doc/timely/  doc/differential/  doc/snippets/
man/man1/ man/man3/ man/man7/
data/                         diagnostics.html, dataflow-graph.html (shipped verbatim)
dev/                          PLAN.md, the ledgers, ADRs, scripts; not installed
.agent/                       agent scratch, gitignored, never committed
```

### 3.3 Naming

- The public prefix is `dtfl_`, and public macros use `DTFL_`.
- Types are opaque handles: `dtfl_worker`, `dtfl_scope`, `dtfl_stream`,
  `dtfl_collection`, `dtfl_arranged`, `dtfl_trace`, and so on.
- Library-internal symbols use `__dtfl_`.  The linker version script
  (`src/libdtfl.map`) keeps them out of the dynamic symbol table, so
  they cannot become ABI by accident.
- Upstream names are kept wherever they translate.  `unary_frontier`
  becomes `dtfl_stream_unary_frontier()` and `arrange_by_key` becomes
  `dtfl_coll_arrange_by_key()`.  A Rust user should recognize every
  function.

## 4. Design decisions

Each decision below gets an ADR under `dev/adr/` when the phase that
depends on it starts.  The ADR records the alternatives in full, so the
summaries here can stay short.

### D1. Data model: records are byte strings with a type descriptor

Rust gets typed records from monomorphisation, which C cannot copy.
The two alternatives were:

- Code generation (macros or a generator that stamps out typed
  copies).  This repeats Rust's compile-time cost, which upstream is
  actively trying to escape (the 0.29 to 0.31 changelogs are about
  removing monomorphisation).  It also makes the public API a macro
  language.
- Fixed-width integer records only.  This is fast, but it cannot hold
  strings or tuples, and differential's examples need both.

libdtfl instead treats a record as a byte string, described by a
`dtfl_type`:

```c
typedef struct dtfl_type {
	const char *name;
	size_t      fixed_size;   /* 0 = variable length */
	size_t (*len)(const void *p, size_t avail, void *ctx);   /* bytes this value occupies; NULL when fixed_size */
	int  (*cmp)(const void *a, size_t alen, const void *b, size_t blen, void *ctx); /* NULL = memcmp then length */
	uint64_t (*hash)(const void *p, size_t len, void *ctx);   /* NULL = FNV-1a of bytes */
	int  (*fmt)(FILE *, const void *p, size_t len, void *ctx); /* for inspect/debug */
	void *ctx;
} dtfl_type;
```

The rules:

- **The library never inspects record bytes.** It calls `cmp`
  (arrangements, consolidation, reduce input order) and `hash`
  (exchange) and nothing else.  The exceptions are records the
  library itself builds, listed in the last bullet.
- **`cmp` must reproduce Rust's derived `Ord` for the type it
  models.** reduce hands its logic the values sorted by `cmp`, and
  user reducers depend on that.  For example, a "min" reducer reads
  `s[0]`, as in `experiments/src/bin/graphs-static.rs`.  The upsert
  tie rule ("greatest `Option<V>` wins, `None` < `Some`",
  `upsert.rs:222,262`) and the output order the examples print both
  follow from it.  A property test checks each built-in descriptor's
  `cmp` against `Ord` order in fixtures produced by the pinned Rust
  code.
- **Built-in descriptors use order-preserving encodings, so `cmp` is
  `memcmp` plus length:**
  - `u8` to `u64`: big-endian;
  - `i8` to `i64`: big-endian with the sign bit flipped;
  - `bytes` and `string`: raw bytes;
  - `unit`: empty.

  The public accessors (`dtfl_u64_get()`, `dtfl_i64_put()` and the
  rest) hide the encoding, so callers handle native values.
- **Composite descriptors:**
  - `tuple`: fields in order.  A variable-length field is preceded by
    a big-endian length, and fields compare one at a time.
  - `option`: a tag byte, 0 for None and 1 for Some, then the value.
  - `vec`: a length, then the elements.  Its `cmp` compares the
    elements one at a time and then the lengths, the way Rust's
    `Vec` `Ord` does.  A plain `memcmp` would order by length first,
    which is wrong.
  - User descriptors supply their own `cmp`.
- **Key and value.** A key/value collection stores `key || val` (concatenation).  The
  key descriptor's `len()` callback (fixed size, or the length prefix)
  gives the split point.
- **Records the library builds.** Some operators construct records
  themselves:
  - `count`, `count_total` and `count_core` turn a diff into a value
    (`collection.rs:924`, `count.rs:117,124`);
  - `upsert` builds `(key, prev)` (`upsert.rs:267`);
  - `threshold`, `distinct` and the algorithms build values through
    user closures, but their outputs carry library-built diffs.

  For these, the diff class supplies a `dtfl_type` (D4), and the
  operator's man page states the record layout it produces.

Because records are already bytes, serialization for exchange is a
memcpy.  That removes Rust's `ExchangeData` bound and the cost of
bincode.

### D2. One container: the refcounted batch

The Rust code is generic over containers (`Vec`, columnar, `Rc<C>`).
libdtfl has a single container, `dtfl_batch`, with these properties:

- **Layout.** One arena of record bytes plus an offsets array.  A
  differential batch adds parallel time and diff columns.
- **Sharing.** Batches are refcounted with atomic counts.  Fan-out
  (`Tee`) hands every consumer a reference instead of copying.  That
  covers `shared()` / `Rc<C>`.
- **Mutation.** A consumer that wants to mutate in place gets
  copy-on-write, and only when the refcount is above 1.

This is why the columnar subsystem, `rc.rs` and `iterate_container`
are marked `subsumed` in the ledger.  The flat arena already gives what
columnar gives Rust.  The cost: there is no user-defined container
type.  If profiling shows one is needed, a container vtable can be
added behind the same API without changing call sites.

### D3. Time model: short vectors of u64 coordinates with a class

Timestamps in the pinned, non-excluded code:

- `()`;
- unsigned integers (`u32` iteration counters in `accumulate.rs:20`
  and `monoid-bfs.rs:130`; `u64` and `usize` elsewhere);
- `Duration`;
- `Product<Outer, Inner>`, nested to any depth;
- `AltNeu<usize>` (dogsdogsdogs);
- `PointStamp<u64>` (dynamic scopes), including DDIR's
  `Product<u64, PointStamp<u64>>` (`interactive/src/ir.rs:9`);
- `Pair<isize, isize>`, a product-ordered pair of *signed* integers
  (`multitemporal.rs:19`);
- lexicographic tuples `(A, B)` (supported by `order.rs` and
  `lattice.rs:204`, used by no pinned program).

A C runtime cannot be generic over these the way Rust is.  libdtfl
represents every time as a vector of `uint64_t` coordinates whose
length, the class _depth_, is fixed per scope.  A scope carries a
`dtfl_time_class` that gives the vector its meaning.  Internally a time
is `const uint64_t *` plus the class, and arrays of times are flat runs
of `depth` words, so traces and progress tracking never chase
pointers or handle variable lengths.  Depth is at most `DTFL_TIME_MAX`
(16) coordinates.

**Coordinate kinds.** Each coordinate has a declared kind:

- `UNSIGNED` with a width of 8, 16, 32 or 64 bits.  Checked add
  overflows at the declared width, not at 64 bits.  This keeps the
  feedback-overflow drop behavior of `PathSummary::results_in`
  identical to upstream.
- `SIGNED` with a width.
  - The value is sign-extended to 64 bits and stored as
    `(uint64_t)x ^ (UINT64_C(1) << 63)`.  Unsigned comparison of the
    stored values is then signed comparison, for every width.
    `isize::MIN` is stored as 0.
  - Checked add rejects any result outside the declared width's
    signed range.
- `DURATION`: seconds and nanoseconds packed as `secs * 10^9 + nanos`
  in a u64, which covers about 584 years.  Rust's `Duration` reaches
  2^64 seconds, so values beyond 584 years are rejected with
  `DTFL_ERANGE` at the API boundary, and the limit is documented.
  The bincode codec converts to and from `(u64 secs, u32 nanos)`.

**Class shapes:**

| Rust timestamp | Class | Order |
|---|---|---|
| `()` | `UNIT`, no coordinates | trivial |
| integer, `Duration` | `TOTAL`, one coordinate | total |
| `Product<A, B>` | `PRODUCT` of two child classes | product |
| `Pair<S, T>` and similar user product types | `PRODUCT` | product |
| `(A, B)` tuple | `LEX` of two child classes | lexicographic, with `maximum` for meet (`lattice.rs:204-230`) |
| `AltNeu<T>` with `T` totally ordered | `TOTAL`, one coordinate, encoded `2t + neu` | total |
| `PointStamp<T>` | `POINTSTAMP(k)` of one child kind, padded to a declared maximum of `k` coordinates | product order; see below |
| `Product<A, PointStamp<B>>` | `PRODUCT(A, POINTSTAMP(B))` | product; the PointStamp tail is variable length |

Notes on AltNeu:

- The `2t + neu` encoding was checked exhaustively against
  `altneu.rs`'s `less_equal`, `join` and `meet` for totally ordered
  `T`.  It is exact.
- For a partially ordered `T` it would not be, so the class rejects
  that combination at scope creation.
- Every pinned use is `AltNeu<usize>`.
- `t >= 2^63` overflows the encoding.  Decoding such a time returns
  `DTFL_ERANGE`; entering one into an AltNeu scope (`to_inner`) panics,
  as a contract violation.
- `dtfl_time_is_total` reports that the partial order is total, which
  it is for AltNeu over a total base.  Upstream has no `TotalOrder` impl
  for `AltNeu`, so the `_total` operators accept an AltNeu scope here
  where Rust would not compile; that is an extension, not a divergence
  in behavior.

**Ord is word order.**  Every encoding above preserves Rust's derived
`Ord`: Product and tuples derive field order, AltNeu derives
`(time, neu)`, PointStamp's derived vector order equals the order of the
padded vector, and the signed and Duration encodings are monotone.  So
for every class the `cmp` that upstream gets from `Ord` is the
lexicographic comparison of the u64 words, and the library implements it
once, with no vtable call.  `ChangeBatch` and every sorted run of times
use it.  A property test checks it against each encoding.

**PointStamp padding.**  Upstream stores a `PointStamp` as a variable
vector with trailing minimum coordinates trimmed, and compares it as if
missing coordinates were the minimum.  A vector padded with the minimum
to a fixed `k` is the same value: product order, join (pointwise max),
meet (pointwise min) and lexicographic `Ord` all agree, because the
minimum sorts below every other coordinate and a canonical trimmed
vector has no trailing minimum.  `k` is declared when the dynamic scope
is created; `enter_dynamic` and `feedback_summary` at a level above `k`
fail with `DTFL_ERANGE`.  A PointStamp summary is `{ retain, actions }`
with `actions` padded the same way.  Two summaries that differ only by
trailing zero actions behave identically and compare equal here, where
upstream's order (`pointstamp.rs:199-213`) keeps them distinct; treating
behaviorally equal summaries as equal is sound for the reachability
antichains that consume the order.

**The class vtable.** Each class supplies the operations that the
progress tracker, the traces and reduce call:

- Order: `less_equal`, and `cmp`.  `cmp` is the total order that
  Rust's `Ord` gives the type, and it must extend `less_equal`
  (`timestamp.rs:14`).  reduce walks the `Ord` prefix on purpose
  (`reduce.rs:1267-1277`), and `sort_dedup` relies on it as well.
- Lattice: `join`, `meet`, `minimum`, and `maximum` (needed by `LEX`
  only).  `advance_by` is derived from `join` and `meet`, exactly as
  `lattice.rs:138-147` does it.
- Summaries: `results_in`, `followed_by`, and `summary_less_equal`.
  reachability composes summaries and keeps antichains of them
  (`reachability.rs:893-947`).
- Scopes: `to_inner`, `to_outer` and `summarize` (the `Refines` trait,
  `subgraph.rs:26`).  For `PRODUCT` these add and drop the inner
  coordinates.  For AltNeu, `to_inner(t)` is `2t` and `to_outer(c)` is
  `c / 2`.
- Codec: `encode` and `decode` to bincode, for progress messages and
  capture.

**Summary representation:**

- For `TOTAL`, `PRODUCT` and `LEX`, a summary is one coordinate per
  time coordinate, applied with checked add at the declared width.
- For `POINTSTAMP`, a summary is `{ retain: optional count, actions:
  variable list }`, with upstream's composition rule
  (`pointstamp.rs:164-196`): truncate by the other summary's `retain`
  first, then compose the actions pairwise.
- The order on PointStamp summaries is upstream's: equal `retain`,
  then a pointwise comparison (`pointstamp.rs:199-213`).  It is not a
  coordinatewise product order.
- AltNeu's summary is `()`.

Dispatch is a `switch` on the class shape in `src/progress/time.c`,
not a function-pointer vtable.  The shapes form a closed set, so a
switch keeps every operation in one place and lets the compiler inline
the common leaves.  Adding a shape means adding a case to each
operation, which is the same work as filling in a vtable.  Ord is
word order for every shape, so the hot comparison
(`__dtfl_words_cmp`) never dispatches at all.

**Refines pairs supported:** identity; `PRODUCT(outer, X)` refining
`outer`; `ALTNEU(outer)` refining `outer`; anything refining `UNIT`.
Upstream's `Refines<TOuter> for (TOuter, TInner)` (lexicographic pairs,
`order.rs:248-259`) is not supported, because no pinned program enters
a lexicographic scope; asking for it panics.

**Known limit.** There are no user-defined lattices beyond
compositions of these classes.  The pinned code needs none.
`multitemporal.rs` also defines an unused `Vector<T>` type whose
`less_equal` is not antisymmetric (`multitemporal.rs:240-303`), and it
is not ported.  If a real need appears, the vtable is the extension
point.

### D4. Diff types: a closed set plus a user class

A diff class (`dtfl_diff_class`) supplies:

- `plus_equals`, `is_zero` and `zero` (`Semigroup`, `Monoid`);
- `negate`, optional (`Abelian`);
- `one`, optional (the `From<i8>` constructor used by `count_core`,
  `distinct_core` and the algorithms);
- `cmp`, the Rust `Ord`, needed where a diff becomes a record value;
- a `dtfl_type`, for operators that turn diffs into record values
  (D1);
- `multiply(a, b) -> c`, declared per class pair.  It can change the
  class: `Present * T -> T` (`difference.rs:170-174`), and tuples
  multiply per component (`difference.rs:234-241`).

Built-in classes:

| Class | Algebra | Width |
|---|---|---|
| `i64` (default), `i32`, `i16`, `i8` | Abelian | fixed |
| `u64`, `u32`, `u16`, `u8` | Semigroup, Monoid | fixed |
| `present` | Semigroup only; `is_zero` is always false | zero |
| `wrapping` `i8` to `i64` | Abelian, with wrap-around arithmetic | fixed |
| `tuple` of up to four classes | the weakest algebra among the components | fixed when all components are fixed |
| `vec` of a class | Abelian when the element is; adding vectors of different lengths extends the shorter one (`difference.rs:270-282`) | variable |
| user | as declared | fixed or variable |

The diff column holds fixed-width values inline.  Variable-width
classes (`vec`, variable user classes) use an arena with offsets, the
same layout as records.  The hot loops for `i64` are specialized and
never call through a function pointer.

**The algebra each operator needs is checked when the dataflow is
built.** A missing capability is an error that names the operator.
The table goes in `dtfl-differential(7)`.  It comes from the pinned
trait bounds:

| Operator | Needs |
|---|---|
| map, filter, concat, inspect, arrange, consolidate | Semigroup |
| negate, `assert_eq`, antijoin, `Variable::new_from`, `iterate` on a collection (`iterate.rs:81`) | Abelian |
| `iterate` on a scope, `Variable::new` (`iterate.rs:102`; this is what `monoid-bfs` uses) | Semigroup |
| join, semijoin, explode, `join_function` | `multiply` declared for the pair |
| reduce, threshold (output) | Abelian output class |
| `count_core`, `distinct_core`, propagate, scc | output class with `one` |
| dogsdogsdogs propose and validate (`dogs lib.rs:87`) | Monoid with `multiply` |

### D5. Closures: function pointer, context, destructor

Every Rust closure becomes three things, with the context's lifetime
owned by the library:

```c
typedef struct dtfl_fn { void *fn; void *ctx; void (*ctx_free)(void *); } dtfl_fn;
```

- Operator logic is called with an operator handle (`dtfl_op *`) that
  exposes inputs, outputs, frontiers and capabilities.  That handle
  plays the role of Rust's borrowed handles: it is valid only during
  the call.
- The builder pattern (a constructor closure that returns a logic
  closure) becomes a `build` callback.  It receives the initial
  capabilities and returns the logic context.

### D6. Ownership and errors

- Every constructor returns a handle the caller owns.  Every handle has
  exactly one `_free` or `_drop` function.
- Rust's `Drop` side effects are explicit calls:
  `dtfl_cap_drop()` (releases a capability), `dtfl_input_close()`,
  `dtfl_trace_drop()` (releases a compaction hold).  This is the main
  ergonomic cost of C.  To catch leaks, a debug build checks at worker
  shutdown that no capability or trace handle is still live, and
  reports the name of the operator that leaked it.
- Fallible functions return `int`: `0`, or an errno-style `DTFL_E*`
  value.  `dtfl_strerror()` describes it.  `dtfl_last_error_detail()`
  returns context in a thread-local buffer.  That buffer is the one
  piece of per-thread state, and section 5 lists it.
- Rust panics on contract violations, such as a capability used with
  the wrong output or a downgrade that goes backwards.  libdtfl calls
  a panic hook instead: by default it prints the message and calls
  `abort()`, and `dtfl_set_panic_hook()` replaces it.  The upstream
  `#[should_panic]` tests port as tests that install a hook which
  records the call and longjmps out.
- All allocation goes through `dtfl_alloc_hooks` (`malloc`, `realloc`,
  `free`, `ctx`), set once before the first worker starts.  Library
  code never calls `malloc` directly, and the API discipline gate
  checks that (section 6, tier 5).
- Test names.  Some upstream test names repeat within a file
  (`avoid_stack_overflow_in_drop` at `event.rs:117,203`; six names in
  `decouple.rs`).  The C test name is `<module>__<upstream name>`,
  where `<module>` is the Rust module path, so names stay unique and
  can still be found by grep.

### D7. Threads and execution

| Rust | libdtfl |
|---|---|
| `execute(config, closure)` | `dtfl_execute(&cfg, worker_main, ctx, &guards)`: `cfg.threads` pthreads, each calling `worker_main(dtfl_worker *, ctx)` |
| `execute_directly` / `example` | `dtfl_execute_directly()`: runs one worker on the calling thread |
| `threadless` example | `dtfl_worker_new_standalone()`: the caller steps the worker by hand |
| `Config::from_args` | `dtfl_config_from_args(argc, argv, &cfg, &rest)`: parses the flags as they are at the pin (`-w -p -n -h -r -z` from `communication/src/initialize.rs:100-105`, and `--progress-mode` from `timely/src/worker.rs:101`).  It does not use getopt globals, and it leaves unknown arguments for the caller. |

Allocators:

- `Thread`: one worker, no locks.
- `Process`: typed queues between threads in one process.
- `ProcessBinary`: zero-copy byte queues between threads.
- `Cluster`: TCP between processes, one send thread and one receive
  thread per remote process, the same as upstream.

Parking and wakeup:

- Upstream parks with `thread::park` and unparks with a `Buzzer`.
- libdtfl gives each worker a futex on Linux, or a mutex/condvar pair
  elsewhere.
- `dtfl_sync_activator` (upstream `SyncActivator`) is the one
  cross-thread entry point.  It posts to a lock-free MPSC queue, then
  wakes the worker.

### D8. Determinism and the simulation allocator

Timely already depends on determinism: every worker must build the same
dataflows in the same order, or channel IDs disagree between workers.
libdtfl makes that a testable property:

- **Simulation allocator.** A fifth allocator, `DTFL_COMM_SIM`, runs N
  workers on one OS thread.  A seeded scheduler decides which worker
  steps next and in what order messages are delivered.  Clocks
  (`activate_after`, the logging timestamps, and `Duration` times)
  come from a virtual clock.  The same seed always produces the same
  interleaving.
- **Use.** The property tests and the model tests (section 6) run
  under it.  Ordering-dependent bugs in progress tracking then turn
  into replayable seeds, not flaky failures.
- **Production impact.** Nothing changes for the four real allocators.
  The simulator is a test harness that ships in the library so users
  can test their own dataflows the same way.
- **Precedent.** This follows the approach `libxtc` uses, but at a much
  smaller scope.  It is only an allocator and a clock, not a whole I/O
  simulator.

### D9. Wire formats and interop

| Stream | Format at the pin | Interop with Rust 0.31 |
|---|---|---|
| intra-cluster data and progress | 48-byte big-endian `MessageHeader` (`networking.rs:16-82`).  Then a body: `Message` is `from` and `seq` as u64 LE, the bincode time padded to 8 bytes, and the data (`channels/mod.rs:68-95`).  Progress is a bincode `(source, seq, ChangeBatch)`.  The handshake sends a big-endian u64 process index (`networking.rs:127,157`). | none.  Record bytes are libdtfl's (D1) and routing hashes differ.  A cluster is all-libdtfl.  The framing is kept anyway, so that the code can be ported literally. |
| capture and replay (`EventWriter`) | u64 LE length + bincode-1 fixint LE `Event<T, C>` (`event.rs:249-251`) | none for user data.  It is libdtfl bytes. |
| timely log stream | exactly what `BatchLogger::new(EventWriter::new(tcp))` writes when registered under `"timely"`: `Event<Duration, Vec<(Duration, TimelyEvent)>>`.  Variant order at `logging.rs:218-237`. | yes |
| differential log stream | exactly what `differential_dataflow::logging::enable(worker, tcp)` writes: `Event<Duration, Vec<(Duration, DifferentialEvent)>>`.  The variant order is Batch, Merge, Drop, MergeShortfall, TraceShare, Batcher. | yes |
| diagnostics WebSocket | upstream JSON `Frame` (`diagnostics/src/server.rs`) | yes.  The upstream `index.html` and React console work unmodified. |

Neither log stream carries a worker id: it was dropped from the record
in timely 0.16.  `dtfl-diag` gets the worker identity from connection
order.  This replaces tdiag's `(Duration, WorkerIdentifier,
TimelyEvent)` tuple and its `worker == 0` filter.

A Rust 0.31 program does not ship logs over TCP by itself.  The user
has to add about five lines (the `BatchLogger` registration).
`dtfl-wire(7)` and the `dtfl-diag(1)` page show those lines.
`logging-recv.rs` at the pin expects a `TimelySetup` element that
nothing at the pin produces, so it is not a format to target.

The interop rows are tested against fixtures produced by the pinned
Rust code (`tests/interop/`, regenerated by `dev/gen-fixtures`, which
needs cargo).  The fixture programs use exactly the registration code
above.  Wire compatibility is a claim, and every claim gets a test
(section 6).

Logging to a socket: timely 0.13 removed `TIMELY_WORKER_LOG_ADDR`.
libdtfl brings it back, along with `DIFFERENTIAL_LOG_ADDR`, through:

```c
int dtfl_config_from_env(dtfl_config *cfg,
    const char *(*lookup)(const char *name, void *ctx), void *ctx);
```

The caller passes `getenv`, or anything else.  The library itself never
reads the environment, so the API discipline gate stays absolute.  The
writer it installs produces the same streams as the table above.

### D10. Progress tracking: port it literally

`reachability.rs` (1,116 lines), `subgraph.rs` (825), `change_batch.rs`
(321) and `frontier.rs` (869), 3,131 lines in all, are the correctness
core.  They are ported
function by function, keeping upstream names, comments and the
algorithm.  They are not reworked, because this code carries every
subtle bug fix upstream has made.

The C versions keep upstream's data layout decisions: dense per-location
arrays, sorted runs, and no hash maps (0.31 removed them on purpose).
The port is verified by:

- the upstream tests;
- property tests against a naive reference tracker that recomputes
  every frontier from scratch.

### D11. Traces: port ord_neu and spine_fueled literally, then optimize with data

The same rule as D10 applies to the trace layer:

- ported literally: `ord_neu` (the 3-level trie batch), `spine_fueled`
  (fueled merging and its level policy), `merge_batcher`, the cursor
  API, `TraceAgent` sharing, and `reduce` with its
  interesting-time logic;
- kept exactly: upstream's fuel constants (`8 << level`, the 2,000,000
  join fuel, and the 32-entry ChangeBatch compaction);
- also ported literally: `join` with its tactics (`JoinTactic`,
  `CursorTactic`, `bilinear_wave`), `operators/common.rs`
  (`discover_times`, `tile_descriptions`), and the history replay in
  `operators/mod.rs`;
- changed: only what D1 requires.  The key, value, time and diff
  containers become byte arenas plus offset lists, which is the shape
  of `OffsetList` and `SliceContainer` upstream.

Performance work starts after the parity tests pass, and every change
needs a before/after benchmark (section 9).

### D12. Divergences from the pin

The port matches the pin.  Where it does not, the difference is listed
here and in the ledger note, and it gets its own ADR.  There are two:

1. **Byte aliasing.** `bytes/src/lib.rs` takes the aliasing and
   regeneration hardening from timely #821, which landed after the
   pin.  #821 fixes a soundness hole in the Rust `Bytes` type.  The C
   port has the same structure, so it would have the same hole.
   Porting the pre-fix code on purpose would be porting a bug.
2. **Diagnostics bind address.** The diagnostics WebSocket server
   binds `127.0.0.1` by default.  Upstream binds `0.0.0.0`
   (`diagnostics/src/server.rs:154`), which publishes operator names
   and timings to the network with no authentication.  The address is
   a parameter, so the upstream behavior is one argument away.

A new divergence needs an entry here, an ADR and a ledger note, all in
the same commit.

## 5. Thread-safety contract

The contract (also in `dtfl(7)` and `doc/libdtfl.texi`, node
"Threads"):

1. **Handles belong to one worker.** A `dtfl_worker` and everything
   derived from it (scopes, streams, collections, operators,
   capabilities, input handles, probes, traces) belong to the thread
   that runs that worker.  Using one from another thread is undefined
   behavior.  Debug builds detect it and call the panic hook.  This
   matches Rust exactly: those types are `!Send` upstream.
2. **The thread-safe objects.** These may be used from any thread:
   - `dtfl_sync_activator`;
   - `dtfl_probe_shared`, a read-only frontier snapshot that is updated
     atomically;
   - `dtfl_event_link_sync`, the `link_sync::EventLink` port;
   - `dtfl_guards` (join and status);
   - the global setters (`dtfl_set_alloc_hooks()`,
     `dtfl_set_panic_hook()`, `dtfl_set_log_hook()` and
     `dtfl_set_thread_hooks()`).  The global setters must be called
     before the first `dtfl_execute()`.

   Upstream's `ProbeHandle` holds an `Rc<RefCell<MutableAntichain>>`
   (`probe.rs:147-149`), so it is worker-owned like everything else in
   rule 1.  `dtfl_probe_shared` is an addition.  It is a lock-free
   snapshot that the worker publishes after each step, for programs
   that watch progress from another thread.
3. **Inter-worker data.** It moves only through the allocator's
   queues.  `dtfl_batch` refcounts are atomic, because batches cross
   threads in the `Process` and `ProcessBinary` allocators.
4. **Cross-worker trace sharing.** Rust forbids it (`TraceAgent` is
   `Rc`), and so does libdtfl.  A trace imports only into dataflows on
   the same worker.
5. **No hidden global state.** All state lives in the worker or the
   execution.  There are two kinds of exception:
   - the four host hooks in rule 2 (allocation, panic, logging and
     thread spawning), each a process-wide setter that must be called
     before the first `dtfl_execute()`.  Their backing globals are
     `hooks`, `hooks_used`, `panic_fn`, `panic_ctx`, `log_fn`,
     `log_ctx`, `thread_hooks` and `thread_hooks_set`;
   - the thread-local error-detail buffer behind
     `dtfl_last_error_detail()` (D6).

   The library does not install signal handlers, does not read the
   environment (D9), and does not use `getopt` globals.  Two
   independent `dtfl_execute()` calls can run at the same time in one
   process.
6. **Reentrancy.** Operator callbacks may call any API on their own
   worker except `dtfl_worker_step()`.  A nested step is detected and
   calls the panic hook.

How the contract is verified:

- `make check-tsan`: ThreadSanitizer over the Process, ProcessBinary
  and localhost Cluster suites;
- the `Process` allocator under `helgrind`, nightly;
- `tests/dist/test_symbols.sh`, which uses `nm` to fail on any
  writable or thread-local symbol in `libdtfl.so` outside the
  allowlist in rule 5.

## 6. Verification

A port is a claim that the C code behaves like the Rust code.  Each
tier below catches something the others miss.  `make check` runs
tiers 1 to 5.  The remaining tiers have their own targets because they
need extra tools.

1. **Unit tests** (`tests/unit/`).  One file per `src/` module.
   - Every upstream unit `#[test]` is ported as
     `<module>__<upstream name>` (D6).  `dev/test-ledger.tsv` is the
     checklist.
   - The ported tests are listed under that module's tests.
   - The harness is a vendored single-header runner, munit, as in
     libxtc.  It forks per test, so a crash in one test does not take
     down the others.
2. **Upstream integration tests** (`tests/upstream/`).  `bfs`, `scc`,
   `join`, `reduce`, `reduce_reference`, `import`, `trace`, `barrier`,
   `gh_523`, `shape_scaling` and the dogsdogsdogs regression.
   - They are ported with the same inputs, seeds and expected outputs.
   - Upstream's own oracles (sequential BFS and SCC, `ReferenceTactic`)
     are ported along with them.
3. **Model tests** (`tests/model/`).  For each differential operator,
   a naive reference evaluates the collection semantics from scratch at
   every time.  The incremental result must equal the model at every
   time the frontier passes.
   - This is the main correctness oracle.  It is the executable form of
     the `Adequate` square in upstream's
     `formal/Differential/Compositional.lean`,
     and each model test names the Lean theorem it checks.
4. **Property tests** (`tests/pbt/`, hegel via the in-process
   `libhegel` C library from hegeldev/hegel-rust).  Configure with
   `--with-hegel`; when it is absent the suites compile and report
   SKIP, as libxtc's ADR-0002 does.  The properties:
   - antichain and MutableAntichain against a naive set model;
   - ChangeBatch compaction;
   - lattice laws per time class (associativity, idempotence,
     `advance_by` soundness and canonicity, from
     `formal/Differential/Compaction.lean`);
   - consolidation;
   - batcher and merger round-trips;
   - spine invariants under random insert, compaction and exert
     sequences;
   - cursor `seek`/`step` against a sorted array;
   - reachability against the naive tracker (D10);
   - model tests from tier 3 with generated operator pipelines and
     update streams, run under the simulation allocator (D8), so
     interleavings are drawn too;
   - codec round-trips and parse robustness for every wire format.
5. **Distribution gates** (`tests/dist/`).  These are fast scripts.
   Each failure blocks a merge.
   - `test_api_discipline.sh`: no raw allocation outside
     `src/base/os_alloc.c`; no `pthread_create` outside
     `src/base/os_thread.c`; no `getenv`, `signal` or `getopt` anywhere
     in `src/`; and examples and tools use only the public API.
   - `test_symbols.sh`: every exported symbol starts with `dtfl_`,
     and the exported set equals the union of the per-component
     `src/*/exports.sym` lists.  Until 1.0, `src/libdtfl.map` exports
     `dtfl_*` by pattern.  At 1.0 it is generated from the lists and
     frozen (section 13).
   - `test_headers.sh`: each public header compiles on its own as C11
     and as C++17, with `-Wall -Wextra -Werror -pedantic`.
   - the writable-globals check, folded into `test_symbols.sh` and
     described in section 5.
   - `test_ledger.sh`: every `done` ledger row's target exists and
     carries the provenance header.  Every `done` test-ledger row names
     a C test that exists and is registered.  The ledgers parse.
   - `test_docs.sh`: every snippet under `doc/snippets/` compiles and
     runs with its expected output.  Every public function has a man
     page.  Every man page passes `mandoc -Tlint`.
6. **Sanitizers.** `make check-asan` (ASan and UBSan) and
   `make check-tsan`.  CI runs both.
7. **Interop** (`make check-interop`).  Decodes the Rust-produced
   fixtures from D9 and compares the results with the Rust decodes.
   It also checks that each built-in record descriptor's `cmp` sorts
   the fixture values in Rust `Ord` order (D1).
8. **Fuzzing** (`make fuzz`, libFuzzer).  Targets: every wire decoder
   (`MessageHeader`, progress messages, `EventReader`, the log decoder,
   the WebSocket frame parser), the DDIR parser, and `dtfl-diag`'s
   input path.  Crashing inputs go into `tests/corpus/` and stay there
   as regression tests.

Rules carried over from the maintainer's other C projects:

- A failing seed from tier 4 or 8 is pinned in `tests/corpus/seeds.txt`
  together with the commit that fixed it.
- A bug fix lands with a test that fails before the fix.
- A test that SKIPs says SKIP.  It never reports PASS for something it
  did not run.

## 7. Documentation

- **Texinfo manual** (`doc/libdtfl.texi`, built as info, HTML and PDF).
  - Part I, Timely, follows the upstream timely book's chapter
    structure.
  - Part II, Differential, follows the differential book's.
  - Part III, Diagnostics and Tools.
  - Part IV, Reference: threads, memory, errors, the time and diff
    classes, and wire formats.
  - Each chapter is written in C for C programmers, not translated
    sentence by sentence.  The ledger maps each upstream `.md` to its
    `.texi` node.
  - Every code example is a file under `doc/snippets/`, pulled in with
    `@verbatiminclude`.  The docs gate compiles and runs each one, so
    the manual cannot show code that does not work.
- **Man pages**, written in mdoc(7):
  - section 3, one page per public function family (for example,
    `dtfl_coll_join(3)` also covers `join_map`, `semijoin`, `antijoin`
    and `join_core`);
  - section 1, one page per installed program;
  - section 7, the overviews `dtfl(7)`, `dtfl-timely(7)`,
    `dtfl-differential(7)`, `dtfl-threads(7)` and `dtfl-wire(7)`.
- **API reference.** Doxygen comments in the public headers, generated
  with `make doxygen`.  The man pages are the authority.  Doxygen
  output is a convenience and is not installed.
- **README, NEWS, ChangeLog, AUTHORS, THANKS.**
  - GNU layout.
  - NEWS has one entry per release, describing user-visible changes.
  - ChangeLog is generated from git at `make dist`.
  - THANKS credits the upstream authors (section 11).

## 8. Examples

Each upstream example is ported under the same name, with the same
command-line arguments and output format, so the two can be compared
directly (`diff <(cargo run --example bfs ...) <(examples/dd/bfs ...)`
for deterministic examples).

- The ledger maps 57 upstream example programs under `*/examples/`.
- It maps the four server plugins separately.
- The manual's Examples appendix documents each one.

Chapters in the upstream timely book that teach operators which were
removed after the pin (`chapter_2_4.md` Notificator, `chapter_4_2.md`
`branch_when`, `chapter_4_3.md` Delay) are ported as written, because
those operators exist at the pin (section 2).  Each one gets a short
"removed upstream after timely 0.31" note that shows the
`unary_frontier` equivalent.

## 9. Performance

- Benchmarks (`bench/`) port `spines`, `chunk_bench`, and three
  `experiments/` programs:
  - `arrange`;
  - `graphs`, for its reach and bfs queries;
  - `graphs-static`, for its reach, bfs and connected-components
    queries.
- These feed one table, together with the differential examples
  `bfs`, `scc`, `pagerank` and `degrees`.  Every row runs at fixed
  sizes, C against Rust, on the same host, with the same threads and
  the same inputs.
- The target for 1.0 is within 1.5x of Rust's throughput on every row,
  and the table in the manual says honestly where it is not.
- Each optimization commit carries its before and after numbers.
- Benchmarks run on dedicated EC2 hosts through the `aws-benchmark`
  skill workflow, never on the development laptop.  Every published
  number records the commit, configure flags, instance type and
  compiler.

## 10. Phases

Line counts are the upstream Rust lines in each phase's ledger rows.
They measure scope, not effort.

A phase is finished when:

- its ledger rows are `done`;
- its tests pass under ASan and UBSan;
- `make distcheck` passes;
- its man pages are written;
- its manual chapter is drafted.

The phase of each ledger row was chosen from the pinned `use`
statements, so a phase never imports from a later one.  The
exceptions are noted below the table.  Inside a phase, the work splits
by module.

| Phase | Scope | Rows / upstream lines | Needs | Exit check |
|---|---|---|---|---|
| P0 | Skeleton: autotools, gates, CI, munit, hegel wiring, alloc and panic hooks, manual skeleton, ADRs D1 to D12 | 2 / 83 | - | `make distcheck` green on an empty library; every gate runs and reports PASS or SKIP |
| P1 | Base: bytes, batch, hash, record codecs, time classes and their lattice ops, antichain, change batch | 8 / 2,601 | P0 | lattice-law, antichain and `cmp`-vs-`Ord` properties pass |
| P2 | Logging registry; communication (thread, process, binary and TCP allocators, networking, handshake, the spill policy types and threshold policy); the simulation allocator | 24 / 4,103 | P1 | `comm_hello` runs on all five allocators; spill unit tests; TSan clean |
| P3 | Progress core plus the worker: operate, reachability, subgraph, progcaster, activations, worker, scope, execute, the timely logging events, capture `Event` | 13 / 4,893 | P1, P2 | reachability matches the naive tracker under PBT; a hand-built source-to-sink operator runs on Thread, Process and Cluster |
| P4 | Timely dataflow layer: stream, channels, pacts, capabilities, builders, every operator at the pin (including the ones removed after it, section 2), capture and replay, sync; examples; timely tests; manual Part I | 116 / 12,907 | P3 | every timely test-ledger row passes; capture and log interop fixtures decode |
| P5 | Differential foundations: diff classes, consolidation, input, the linear collection methods | 5 / 1,162 | P4 | model tests for the linear operators |
| P6 | Traces: layout, ord_neu, merge batcher, spine, cursors, history replay, differential logging events | 12 / 4,507 | P5 | `tests/upstream/dd/trace`; spine PBT |
| P7 | Arrangements: arrange, TraceAgent, import, writer, upsert, trace wrappers; `consolidate` | 10 / 2,280 | P6 | arrange model tests; upsert tie-rule test |
| P8 | Join, reduce, count, threshold, iterate, dynamic scopes; the rest of `collection.rs`; manual Part II | 59 / 8,066 | P7 | `join`, `reduce`, `reduce_reference` and `import`; model tests for every operator |
| P9 | Algorithms, dogsdogsdogs, the remaining differential examples | 39 / 5,412 | P8 | `bfs`, `scc` and dogsdogsdogs tests; examples diff-equal to Rust |
| P10 | Differential capture | 1 / 725 | P5 | capture round trip under message duplication and reordering |
| P11 | Diagnostics: in-process WebSocket server and `dtfl-diag` | 17 / 3,509 | P8; P9 for the `scc-bench` example only | upstream `index.html` renders a live libdtfl dataflow; `dtfl-diag graph` reads the Rust 0.31 log fixture |
| P12 | Trace spill: graded chunks, chunk spill, the `spines` and `chunk_bench` benchmarks, the `spill_stress` and `spill_compare` examples | 8 / 2,947 | P7 | `spill_stress`; spill PBT |
| P13 | DDIR: parser, lowering, interpreter, explain, `dtfl-ddir` | 42 / 6,548 | P8 | the explain test suite; the upstream `.ddp`/`.ddir` programs give the same results |
| P14 | Plugin server and its four example plugins | 6 / 520 | P8 | the `dd_server` session scripts replay |
| P15 | 1.0 release: the three `experiments/` benchmarks, performance table, fuzz campaign, full manual, `make distcheck` on Linux (glibc, musl), FreeBSD and macOS | 3 / 586 | all | section 13 |

The table above is a DAG, not a sequence.  After P8, the phases P9,
P10, P12, P13 and P14 are independent of each other and can run in
parallel.  P11 can run in parallel with them too, except for its
`scc-bench` example, which waits for P9.  P10 can start as early as
P5, and P12 as early as P7.

Two intra-crate cycles at the pin cross the P5 to P8 boundary:

- `input.rs` and `collection.rs` use each other;
- `arrange/arrangement.rs` calls `join_traces` and `reduce_trace`.

`Collection::consolidate` is built on `arrange_named`
(`collection.rs:974`), so it lands in P7 with the arrangements.  The P5
model tests consolidate inside the test harness instead.

These are handled as follows:

- **`collection.rs`** is ported method by method.  The linear methods
  land in P5, the arrange methods in P7, and the join and reduce
  methods in P8.  Its ledger row is P8, so it reaches `done` only when
  the last method lands.
- **`Arranged::join_core`, `reduce_abelian` and `reduce_core`** are
  declared in P7 and implemented in P8.  P7's tests do not call them.

## 11. License and attribution

libdtfl is MIT-licensed, like all three upstreams, and is a derivative
work of all three.  The attribution is applied as follows:

- `COPYING` holds the MIT license with the libdtfl copyright line,
  followed verbatim by the three upstream notices:
  - timely: "Copyright (c) 2014 Frank McSherry";
  - differential: "Copyright (c) 2015 Frank McSherry";
  - diagnostics: "Copyright (c) 2019 Timely Dataflow Diagnostics
    contributors";
  - also the ETH Zurich contributor lines from upstream `COPYRIGHT`
    (quoted verbatim in `COPYING`).
- `THANKS` names the upstream projects and their contributors.
- Every ported source file starts with a provenance header:

  ```c
  /*
   * SPDX-License-Identifier: MIT
   * Copyright (c) 2026 Greg Burd
   * Ported from timely-dataflow timely-v0.31.0
   *   timely/src/progress/reachability.rs
   * Copyright (c) 2014 Frank McSherry and contributors.
   */
  ```

  The ledger gate checks that the header names the same upstream path
  as the file's ledger row.
- `data/dataflow-graph.html` and `data/diagnostics.html` are shipped
  verbatim with their upstream notices.

## 12. Tooling for the port

- `dev/ledger-diff` regenerates both ledgers from the pinned tags and
  reports added, removed and changed upstream files.  It runs whenever
  a pin moves.
- `dev/gen-fixtures` builds small Rust programs against the pins (it
  needs cargo) and writes the interop fixtures under `tests/interop/`.
  The fixtures are committed so `make check-interop` needs no Rust.
- `dev/port-status` prints a per-phase summary of the ledgers.

## 13. Definition of 1.0

- Every `todo` row in both ledgers is `done`.
- `make distcheck` passes on Linux (glibc, gcc and clang), Linux
  (musl), FreeBSD and macOS.
- These are green: ASan/UBSan, TSan, interop, the docs gate, and
  100,000 seeds of the simulator property suites.
- The performance table is published with honest numbers.
- Every public function has a man page, and the manual has no
  placeholder nodes.
- The ABI is frozen: `src/libdtfl.map` gets its `DTFL_1.0` node, and
  from then on the symbol gate fails on any removal.

## 14. Open questions

These need the maintainer's decision.  Each has a default, and work
proceeds on the default unless told otherwise.

1. **Hosting and CI.** Default: Codeberg as `origin` with a GitHub
   mirror for Actions, as libxtc does.
2. **Library name.** Default: keep `libdtfl` and the `dtfl_` prefix.
3. **D3 limits.** Default: signed coordinates are supported (section
   4, D3), user-defined lattices are not.
4. **Copyright holder** in the provenance header.  Default: "Greg Burd".
5. **Removed-after-pin operators.** Section 2 ports them, 1,555
   lines.  The alternative is to rewrite the upstream tests and
   examples that use them, which breaks literal test parity.  Default:
   port them.
