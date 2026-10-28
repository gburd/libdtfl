#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_examples_xtc.sh: run examples/embed/xtc_runtime, which
# runs libdtfl workers as libxtc fibers through the sync and thread
# hooks (dtfl_set_sync_hooks(3)): dtfl_execute with 4 worker fibers in
# Process and ProcessBinary mode, and 4 dtfl_execute_worker fibers over
# a socketpair mesh.  With 1 carrier every fiber shares one thread, so a
# wait that blocks the carrier instead of parking the fiber hangs the
# run; 2 and 4 carriers let fibers migrate.  A hang, a crash or a wrong
# total fails.  SKIP when configure did not find libxtc.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
command -v timeout >/dev/null 2>&1 || timeout() { shift; "$@"; }
bin="examples/embed/xtc_runtime"
if [ ! -x "$bin" ]; then
	echo "[xtc] SKIP: libxtc not found at configure time" \
	    "(--with-libxtc=PREFIX or pkg-config xtc), so xtc_runtime is not built"
	exit 77
fi
out=$(mktemp)
err=$(mktemp)
trap 'rm -f "$out" "$err"' EXIT
for c in 1 2 4; do
	if ! timeout 120 "$bin" "$c" >"$out" 2>"$err"; then
		echo "[xtc] FAIL: xtc_runtime $c crashed, hung or got a wrong total"
		sed 's/^/    /' "$out" "$err" | head -20
		exit 1
	fi
	if ! diff "$top/examples/embed/xtc_runtime.expected" "$out" \
	    >/dev/null 2>&1; then
		echo "[xtc] FAIL: xtc_runtime $c output does not match expected"
		diff "$top/examples/embed/xtc_runtime.expected" "$out" |
		    head -10 | sed 's/^/    /'
		exit 1
	fi
	sed "s/^/    [$c carriers] /" "$err"
done
echo "[xtc] PASS: xtc_runtime on 1, 2 and 4 carriers"
exit 0
