#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_examples_embed.sh: smoke test the embedding examples:
# the PostgreSQL skeleton (examples/embed/pg_extension), and the forked
# workers over socketpairs (examples/embed/forked_workers).  For the
# skeleton:  It installs the four host
# hooks (alloc, panic, log, and the single-thread cooperative mode in
# place of thread hooks), runs a small differential dataflow by hand
# with dtfl_worker_new_standalone/dtfl_worker_step, and reports the
# distinct-residue count.  A crash, a hang or wrong output fails the
# build.  The elog() lines go to stderr; only the final count line is
# checked on stdout.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
# macOS ships no timeout(1): there, run without the limit, as
# tests/run-test.sh does.
command -v timeout >/dev/null 2>&1 || timeout() { shift; "$@"; }
exp="$top/examples/embed"
bindir="examples/embed"
[ -x "$bindir/pg_extension" ] || bindir="$top/examples/embed"

bin="$bindir/pg_extension"
if [ ! -x "$bin" ]; then
	echo "[embed] SKIP: pg_extension not built"
	exit 77
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

if ! timeout 30 "$bin" >"$tmp/out" 2>"$tmp/err"; then
	echo "[embed] FAIL: pg_extension crashed, hung or exited non-zero"
	sed 's/^/    /' "$tmp/err" | head -10
	exit 1
fi
if ! diff "$exp/pg_extension.expected" "$tmp/out" >/dev/null 2>&1; then
	echo "[embed] FAIL: pg_extension output does not match expected"
	diff "$exp/pg_extension.expected" "$tmp/out" | head -10 | sed 's/^/    /'
	exit 1
fi
echo "[embed] PASS: pg_extension"

# Forked workers over socketpairs: the same totals for 1, 2 and 4.
fw="$bindir/forked_workers"
if [ ! -x "$fw" ]; then
	echo "[embed] SKIP: forked_workers not built"
	exit 77
fi
for n in 1 2 4; do
	if ! timeout 60 "$fw" "$n" >"$tmp/fw.out" 2>"$tmp/fw.err"; then
		echo "[embed] FAIL: forked_workers $n crashed, hung or failed"
		sed 's/^/    /' "$tmp/fw.err" | head -10
		exit 1
	fi
	if ! diff "$exp/forked_workers.expected" "$tmp/fw.out" >/dev/null 2>&1; then
		echo "[embed] FAIL: forked_workers $n output does not match expected"
		diff "$exp/forked_workers.expected" "$tmp/fw.out" | head -10 | sed 's/^/    /'
		exit 1
	fi
done
echo "[embed] PASS: forked_workers 1 2 4"
exit 0
