#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_examples_ivm.sh: smoke test the incremental
# materialized view examples (examples/ivm).  Each checks itself against
# a from-scratch recomputation after every commit and exits non-zero on a
# mismatch; this script also compares stdout with the committed expected
# file, so a crash, a hang, a failed self-check or changed output fails
# the build.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
exp="$top/examples/ivm"
bindir="examples/ivm"
[ -x "$bindir/ivm_view" ] || bindir="$top/examples/ivm"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pass=0
fail=0
for name in ivm_view txn_harness; do
	bin="$bindir/$name"
	if [ ! -x "$bin" ]; then
		echo "[ivm] SKIP: $name not built"
		continue
	fi
	if ! LC_ALL=C timeout 30 "$bin" >"$tmp/$name.out" 2>"$tmp/$name.err"
	then
		echo "[ivm] FAIL: $name crashed, hung or failed its self-check"
		sed 's/^/    /' "$tmp/$name.err" | head -10
		grep 'MISMATCH\|FAILED' "$tmp/$name.out" | head -10 |
		    sed 's/^/    /'
		fail=$((fail + 1))
		continue
	fi
	if ! LC_ALL=C diff "$exp/$name.expected" "$tmp/$name.out" \
	    >"$tmp/$name.diff" 2>&1; then
		echo "[ivm] FAIL: $name output does not match expected"
		head -10 "$tmp/$name.diff" | sed 's/^/    /'
		fail=$((fail + 1))
		continue
	fi
	echo "[ivm] PASS: $name"
	pass=$((pass + 1))
done
[ "$fail" -eq 0 ] || exit 1
[ "$pass" -gt 0 ] || exit 77
exit 0
