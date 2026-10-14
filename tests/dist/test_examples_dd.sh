#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_examples_dd.sh: smoke test a representative subset of the
# ported differential dataflow and dogsdogsdogs examples (P9).  Each runs
# under a timeout with a fixed input and its stdout is compared to a
# committed expected file, so a crashing, hanging or wrong-output example
# fails `make check`.  Output that depends on worker scheduling is
# normalised with sort.  The examples cap any loop at a small fixed round
# count, and the harness runs each under a timeout, so a hung example is a
# build failure, not a wait.
#
# Expected outputs live beside the sources under $top_srcdir/examples/dd
# and $top_srcdir/examples/dogs.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"

timeout_s=30
fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# run DIR NAME EXPECTED SORT ARGS...
#   DIR       example subdirectory under examples (dd or dogs)
#   NAME      example binary
#   EXPECTED  expected-output file under $top/examples/DIR (or "-" to only
#             check exit 0)
#   SORT      "sort" to compare order-insensitively, else "cat"
run() {
	dir="$1"; name="$2"; expected="$3"; norm="$4"; shift 4
	exp="$top/examples/$dir"
	bindir="examples/$dir"
	[ -x "$bindir/$name" ] || bindir="$top/examples/$dir"
	bin="$bindir/$name"
	if [ ! -x "$bin" ]; then
		echo "[examples-dd] SKIP: $dir/$name not built"
		return
	fi
	out="$tmp/$name.out"
	if ! timeout "$timeout_s" "$bin" "$@" >"$out" 2>"$tmp/$name.err"; then
		echo "[examples-dd] FAIL: $dir/$name crashed, hung or exited non-zero"
		sed 's/^/    /' "$tmp/$name.err" | head -10
		fail=1
		return
	fi
	if [ "$expected" = "-" ]; then
		echo "[examples-dd] PASS: $dir/$name (ran clean)"
		return
	fi
	if [ "$norm" = "sort" ]; then
		sort "$out" >"$out.n"
		sort "$exp/$expected" >"$tmp/$expected.n"
	else
		cp "$out" "$out.n"
		cp "$exp/$expected" "$tmp/$expected.n"
	fi
	if cmp -s "$out.n" "$tmp/$expected.n"; then
		echo "[examples-dd] PASS: $dir/$name"
	else
		echo "[examples-dd] FAIL: $dir/$name output does not match $expected"
		diff "$tmp/$expected.n" "$out.n" | head -20 | sed 's/^/    /'
		fail=1
	fi
}

# Deterministic single-worker outputs over fixed inputs.
run dd degrees degrees.expected sort
run dd bfs bfs.expected sort
run dd projekt projekt.expected sort
run dd compact compact.expected cat
run dd stackoverflow stackoverflow.expected sort
run dd hello hello.expected sort
run dd accumulate accumulate.expected sort
run dd interpreted interpreted.expected sort
run dd itembased_cf itembased_cf.expected sort
run dd scc scc.expected sort
run dd pagerank pagerank.expected sort
run dd dynamic dynamic.expected sort
run dogs delta_query_wcoj delta_query_wcoj.expected sort
run dogs dogsdogsdogs dogsdogsdogs.expected sort

if [ "$fail" -eq 0 ]; then
	echo "[examples-dd] all smoke tests passed"
fi
exit "$fail"
