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
# macOS ships no timeout(1): there, run without the limit, as
# tests/run-test.sh does.
command -v timeout >/dev/null 2>&1 || timeout() { shift; "$@"; }
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

bin="$bindir/scc-bench"
if [ -x "$bin" ]; then
	if ! timeout "$timeout_s" "$bin" >"$tmp/scc.out" 2>"$tmp/scc.err"; then
		echo "[diag] FAIL: scc-bench crashed, hung or exited non-zero"
		sed 's/^/    /' "$tmp/scc.err" | head -10
		exit 1
	fi
	if ! diff "$exp/scc-bench.expected" "$tmp/scc.out" >/dev/null 2>&1; then
		echo "[diag] FAIL: scc-bench output does not match scc-bench.expected"
		diff "$exp/scc-bench.expected" "$tmp/scc.out" | head -10 | sed 's/^/    /'
		exit 1
	fi
	echo "[diag] PASS: scc-bench"
fi
echo "[diag] all smoke tests passed"
exit $fail
