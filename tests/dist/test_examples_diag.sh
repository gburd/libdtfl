#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_examples_diag.sh: smoke test the diagnostics example
# (P11).  smoke runs the full in-process path (diag session + WebSocket
# server on a loopback ephemeral port + Frame stream) over a bounded
# map/probe dataflow, then exits.  A crash, a hang (the server thread or
# a socket) or wrong output fails the build.  The server binds 127.0.0.1
# (D12), so no network listener is exposed by the test.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
exp="$top/examples/diag"
bindir="examples/diag"
[ -x "$bindir/smoke" ] || bindir="$top/examples/diag"

timeout_s=30
fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

bin="$bindir/smoke"
if [ ! -x "$bin" ]; then
	echo "[diag] SKIP: smoke not built"
	exit 77
fi

if ! timeout "$timeout_s" "$bin" >"$tmp/smoke.out" 2>"$tmp/smoke.err"; then
	echo "[diag] FAIL: smoke crashed, hung or exited non-zero"
	sed 's/^/    /' "$tmp/smoke.err" | head -10
	exit 1
fi
if ! diff "$exp/smoke.expected" "$tmp/smoke.out" >/dev/null 2>&1; then
	echo "[diag] FAIL: smoke output does not match smoke.expected"
	diff "$exp/smoke.expected" "$tmp/smoke.out" | head -10 | sed 's/^/    /'
	exit 1
fi
echo "[diag] PASS: smoke"
echo "[diag] all smoke tests passed"
exit $fail
