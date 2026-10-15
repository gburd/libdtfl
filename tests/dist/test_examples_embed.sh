#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_examples_embed.sh: smoke test the PostgreSQL embedding
# skeleton (examples/embed/pg_extension).  It installs the four host
# hooks (alloc, panic, log, and the single-thread cooperative mode in
# place of thread hooks), runs a small differential dataflow by hand
# with dtfl_worker_new_standalone/dtfl_worker_step, and reports the
# distinct-residue count.  A crash, a hang or wrong output fails the
# build.  The elog() lines go to stderr; only the final count line is
# checked on stdout.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
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
exit 0
