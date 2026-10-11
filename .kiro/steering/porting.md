# libdtfl: porting workflow

Use this for any task that ports an upstream file.

## Pick work from the ledger

```sh
# todo rows for a phase, largest first
awk -F'\t' '$5=="P4" && $6=="todo"' dev/port-ledger.tsv | sort -t$'\t' -k3,3nr
```

Phases form a DAG (PLAN section 10, "Needs" column).  A phase starts
only when every phase it needs is finished.  Inside a phase, port the
dependencies before the files that use them.  The ledger's `target`
column is the C file to create.  Several upstream files may share one
target.  The ledger's `test` column is the C test function name.

## Port one file

1. **Read upstream at the pin.**  Read the file and its callers in the
   same crate: `git show <pin>:<path>`.  Note every `#[test]` in it,
   since the test ledger lists them.
2. **Write the C test first.**  Each upstream test becomes a munit test
   named `<module>__<upstream name>`, in the test-ledger `target` file.
   For `#[should_panic]` tests, install a recording panic hook and
   assert that it fired.  Watch the test fail.
3. **Port the code.**  Keep the upstream function order and names
   (`MutableAntichain::update_iter` becomes
   `__dtfl_mutable_antichain_update_iter()`).  Keep upstream comments
   that explain why, and rewrite the ones that are about Rust
   mechanics.
4. **Add the provenance header** (PLAN section 11).
5. **Add the property test** if the module has a model (PLAN section
   6, tier 4).
6. **Run the checks**, all three: `make check`, `make check-asan`, and
   for anything that touches threads or atomics, `make check-tsan`.
7. **Update the ledgers** in the same commit: the port-ledger row goes
   to `done`, and each test-ledger row it owns goes to `done` once its
   test passes.
8. **Write the man page** for any new public symbol.  It is part of the
   change, not a follow-up.

## Translating Rust idioms

| Rust | libdtfl |
|---|---|
| `Rc<RefCell<T>>` shared within a worker | plain pointer plus a worker-local refcount (`__dtfl_rc`); no locking |
| `Arc<T>` across threads | atomic refcount (`__dtfl_arc`) |
| `Vec<T>` | `__dtfl_vec`, with explicit element size and alloc hooks |
| `SmallVec<[T; N]>` | inline array plus spill pointer; antichains and times use this |
| `BinaryHeap` | `__dtfl_heap` |
| `HashMap` | avoid it.  Upstream 0.31 removed every hash map from progress tracking, so keep sorted vectors.  Elsewhere use `__dtfl_hmap` |
| `Option<T>` return | `bool` return plus out-parameter |
| `Result<T, E>` | `int` return (0 or `DTFL_E*`) plus out-parameter |
| `impl Drop` with side effects | an explicit `_drop()` call, plus a debug-build leak check at worker shutdown |
| `panic!` / `assert!` on a contract | `__dtfl_panic(fmt, ...)`, which calls the panic hook |
| `debug_assert!` | `DTFL_DEBUG_ASSERT`, compiled in with `--enable-debug` |
| trait objects (`Box<dyn Operate>`) | vtable struct plus `void *self` |
| generic over `T: Timestamp` | a `dtfl_time_class *` carried by the scope (D3).  Port `Ord` comparisons on times as `cmp` and `PartialOrder` ones as `less_equal`, never one for the other.  `reduce.rs` depends on the difference. |
| generic over `R: Semigroup` | a `dtfl_diff_class *` carried by the collection (D4) |
| closures | `dtfl_fn` (fn, ctx, ctx_free) (D5) |
| iterators | callback visitor, or an explicit cursor struct with `_next()` |

## When upstream does something odd

Port it anyway.  Then file an entry in `dev/upstream-notes.md` with:

- the pinned path and line;
- what looks wrong;
- the smallest Rust program that shows it, if you can write one.

If a fix is warranted, it goes upstream first.  libdtfl matches the pin
until the pin moves.  A deliberate divergence needs an entry in PLAN
D12, an ADR and a ledger note.
