#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_examples.sh: smoke test a representative subset of the
# ported timely examples (P4).  Each runs under a timeout with a tiny
# fixed input and its stdout is compared to a committed expected file, so
# a crashing, hanging or wrong-output example fails `make check`.  Order
# that depends on worker scheduling is normalised with sort.
#
# The examples are built into the build tree; this script runs from the
# build tree root (where `make check` invokes it).  Expected outputs live
# beside the sources under $top_srcdir/examples/timely.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
exp="$top/examples/timely"
# Built binaries are relative to the build tree root (the cwd under
# `make check`); fall back to the source tree for an in-tree build.
bindir="examples/timely"
[ -x "$bindir/hello" ] || bindir="$top/examples/timely"

timeout_s=30
fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# run NAME EXPECTED SORT ARGS...
#   NAME      example binary under $bindir
#   EXPECTED  expected-output file under $exp (or "-" to only check exit 0)
#   SORT      "sort" to compare order-insensitively, else "cat"
run() {
	name="$1"; expected="$2"; norm="$3"; shift 3
	bin="$bindir/$name"
	if [ ! -x "$bin" ]; then
		echo "[examples] SKIP: $name not built"
		return
	fi
	out="$tmp/$name.out"
	if ! timeout "$timeout_s" "$bin" "$@" >"$out" 2>"$tmp/$name.err"; then
		echo "[examples] FAIL: $name crashed, hung or exited non-zero"
		sed 's/^/    /' "$tmp/$name.err" | head -10
		fail=1
		return
	fi
	if [ "$expected" = "-" ]; then
		echo "[examples] PASS: $name (ran clean)"
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
		echo "[examples] PASS: $name"
	else
		echo "[examples] FAIL: $name output does not match $expected"
		diff "$tmp/$expected.n" "$out.n" | head -20 | sed 's/^/    /'
		fail=1
	fi
}

# Deterministic single-worker outputs.
run hello hello.expected sort
run exchange exchange.expected sort 4 2
run wordcount wordcount.expected cat
run bfs bfs.expected sort
run unionfind unionfind.expected cat
run pagerank pagerank.expected cat
run hashjoin hashjoin.expected cat
run capture capture.expected sort
run logging-send logging-send.expected sort

# Multi-worker runs: exercise real cross-thread execution, output varies,
# so only require a clean exit (the point is no crash, no hang, no leak).
run hello - cat -w2
run exchange - cat 4 3 -w3
run bfs - cat
run wordcount - cat -w2
run hashjoin - cat -w2

if [ "$fail" -eq 0 ]; then
	echo "[examples] all smoke tests passed"
fi
exit "$fail"
