# AGENTS.md: working rules for libdtfl

Read this first, then `.kiro/steering/product.md`, then `dev/PLAN.md`.
These rules apply to every agent (Claude Code, Kiro, Pi, Maki) and to
humans.  `CLAUDE.md` points here.

## What this is

libdtfl is a C11 port of timely dataflow, differential dataflow, and the
timely diagnostics tools, packaged as a GNU-style MIT library.  The
design of record is `dev/PLAN.md`.  The port is tracked file by file in
`dev/port-ledger.tsv` and test by test in `dev/test-ledger.tsv`.

## 1. The upstream is pinned; read it at the pin

The Rust sources live in `~/src/timely-dataflow`, `~/src/differential-dataflow`
and `~/src/dataflow-diag`.  Their working trees may be ahead of the pins.
Always read upstream code at the pinned revision:

```sh
git -C ~/src/timely-dataflow show timely-v0.31.0:timely/src/progress/reachability.rs
git -C ~/src/differential-dataflow show differential-dataflow-v0.25.1:differential-dataflow/src/operators/reduce.rs
git -C ~/src/dataflow-diag show 0fa322c9:tdiag/src/commands/graph.rs
```

Timely HEAD removed operators that the pin still has.  At the pin,
differential depends on them (`delay_batch`, `inspect_batch`), and so
do upstream tests (`unary_notify`).  Reading HEAD instead of the pin
has already produced one wrong inventory in this project.  Never edit
the upstream trees.

## 2. Port literally where it is the correctness core

`progress/` (reachability, subgraph, change_batch, frontier) and the
trace layer (`ord_neu`, `spine_fueled`, `merge_batcher`, cursors,
the history replay in `operators/mod.rs` and `common.rs`, `reduce`,
`join`) are ported function by function, with upstream names,
comments and constants kept (PLAN D10, D11).  Do not "improve" the
algorithm while porting it.  An improvement is a separate commit with a
benchmark and a passing parity test before and after.

Elsewhere, translate to idiomatic C under the decisions in PLAN section
4 (D1 to D12).  If you think a decision is wrong, say so and propose an
ADR in `dev/adr/`.  Do not route around it in code.

## 3. Every ported file carries provenance, and the ledger moves with it

- Each file ported from upstream begins with the provenance header in
  PLAN section 11, naming the pinned tag and upstream path.
- In the same commit, flip that file's `dev/port-ledger.tsv` row to
  `wip` or `done`, and flip the matching `dev/test-ledger.tsv` rows as
  their tests land.
- `done` means three things: the target exists, the header matches the
  row, and every test-ledger row the file owns passes.
- `tests/dist/test_ledger.sh` enforces this, so a row cannot say `done`
  for work that is not there.
- Excluding something is a ledger edit with a reason.  Never just skip
  a row.

## 4. Claims need tests

- Upstream tests port as `<module>__<upstream name>` (PLAN D6).
  `#[should_panic]` tests become tests that assert the panic hook
  fired.
- A bug fix lands with a test that fails before the fix.  Show that
  failure in the commit message or the PR.
- PASS, SKIP and FAIL are distinct.  A test whose prerequisite is
  missing (no hegel, no cargo for fixtures, no IPv6) prints SKIP and
  says why.  It never returns 0 as if it had passed.
- A failing property-test or fuzz seed goes in `tests/corpus/seeds.txt`
  with the commit that fixed it.  Before trusting a pin, check it both
  ways: it fails without the fix and passes with it.
- `make check` green is not a release qualification.  State which tiers
  ran (PLAN section 6) and which did not.

## 5. Library code rules (enforced by tests/dist/test_api_discipline.sh)

1. No raw `malloc`/`calloc`/`realloc`/`free`/`strdup` in `src/`.  Use
   `__dtfl_malloc()` and friends, which route through the user's
   allocation hooks.  The raw calls appear only in `src/base/os_alloc.c`.
2. Library code does not use `getenv`, `signal`/`sigaction` or
   `getopt`.  It has no writable globals beyond the three listed in
   PLAN section 5, and `pthread_create` is called only in
   `src/base/os_thread.c`.  Configuration arrives through
   `dtfl_config`, and environment lookup happens only through a
   callback the caller supplies (PLAN D9).
3. `__dtfl_alloc()`, `__dtfl_calloc()` and `__dtfl_realloc()` never
   return NULL.  On failure they call the panic hook, the way Rust aborts
   on allocation failure.  Public constructors that can fail for other
   reasons return a `DTFL_E*` code and clean up with goto-cleanup.
4. Examples and tools use only the public `dtfl_*` API.  If one needs an
   internal, the public API is missing something: add it there.
5. Every public symbol is listed in its component's `src/*/exports.sym`,
   has a man page, and is declared in exactly one public header.

Mark a deliberate single-line exception with `/* DTFL_RAW_OK: reason */`.

## 6. Threads

The thread-safety contract is PLAN section 5.  Worker-owned handles never
leave their worker thread.  Anything shared across threads uses C11
atomics or the `src/base/os_sync.c` wrappers, with the memory order spelled
out and a comment naming what it synchronizes with.  A change to
`src/comm/` or to anything with an atomic in it is not done until
`make check-tsan` passes.

## 7. Code style

- C11, `-std=c11 -Wall -Wextra -Wpedantic`, with zero warnings under
  both gcc and clang.
- BSD KNF via `.clang-format`.  Formatting-only changes are their own
  commit.
- ASCII-only source.  The one exception is the upstream notices quoted
  verbatim in `COPYING` and the provenance headers, which contain
  "Zürich".
- Comments state invariants and ownership: who frees this, which
  worker owns it, what the frontier must satisfy.  Do not narrate the
  code.
- `ponytail:` marks a deliberate shortcut, and names its limit and how
  to remove it.

## 8. Never commit

- Build output: `*.o`, `*.lo`, `*.la`, `.libs/`, `.deps/`, binaries,
  `autom4te.cache/`, `config.status`, `config.log`, generated
  `Makefile`s, and any Rust `target/` from `dev/gen-fixtures`.  The m4
  files in `m4/` are vendored inputs, not outputs, and are committed.
- `configure` and the other autotools outputs.  `./bootstrap`
  generates them, and they ship only in the `make dist` tarball.
- Core dumps, secrets, and private keys (generate test certificates at
  run time).
- Agent working notes.  They live in `.agent/`, which is gitignored.
  Never `git add -f` anything under it.  `dev/` holds the plan, the
  ADRs and the ledgers, all of which are maintained documents.  It is
  not a scratchpad.

## 9. Git

- Stage by explicit path.  Never `git add -A`, `git add .` or
  `git commit -a`.
- Commit with explicit paths (`git commit <paths>`), and check
  `git diff --cached --stat` first.  Other agents may share this
  worktree.
- Never reset, amend a pushed commit, or force-push.
- Work on feature branches and merge through PRs.
- Commit messages: an imperative subject, then prose saying why.  Use
  two spaces after a period and no em dashes.  For a port commit, the
  body names the upstream files and the ledger rows it moves.

## 10. Where to build and run

Use `/scratch/$USER/` (or `$TMPDIR`, which already points there) for
build trees and test output, never `/tmp`:

```sh
./bootstrap
B=$(mktemp -d /scratch/$USER/dtfl-XXXXXX)
( cd "$B" && ~/ws/libdtfl/configure --enable-debug && make -j"$(nproc)" check )
```

Benchmarks run on EC2 through the `aws-benchmark` skill, never on this
host.  Terminate what you launch.

## 11. Sub-agent work

Integration happens on the local `build` branch.  `main` holds only
reviewed integration points.  Each worker gets its own worktree,
created by the lead:

```sh
git worktree add .local/worktrees/agent-<task> -b feat/<task> build
```

A worker commits only on its own `feat/<task>` branch, inside its own
worktree, and never touches another worktree or `build`.  The lead
merges into `build` after review.

Each component directory owns a `local.mk` fragment that lists its
sources, headers and unit tests.  The top-level `Makefile.am` only
includes fragments.  A worker edits only the fragments of the
components it owns, which keeps parallel branches merge-clean.


A phase in PLAN section 10 is large enough for a worker, reviewer and
re-reviewer team (the `subagent-teams` skill).  Split a phase by module,
not by layer.  Give each worker its own git worktree under
`.local/worktrees/agent-*`, so parallel workers do not clobber each
other.  A reviewer checks the port against the pinned upstream file,
not against the worker's summary of it.
