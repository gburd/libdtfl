#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_ddir_smoke.sh: a dtfl-ddir smoke test.
#
# Run a batch program (reach) through `dtfl-ddir run` and check the result,
# and run the explain subcommand, confirming the tool links against the
# installed library and produces the expected output.  Everything under a
# timeout: a program that does not converge is a bug.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
# macOS ships no timeout(1): there, run without the limit, as
# tests/run-test.sh does.
command -v timeout >/dev/null 2>&1 || timeout() { shift; "$@"; }
tool="tools/dtfl-ddir/dtfl-ddir"
[ -f "$tool" ] || tool="$top/tools/dtfl-ddir/dtfl-ddir"
progs="$top/tools/dtfl-ddir/programs"

if [ ! -f "$tool" ]; then
	echo "[ddir-smoke] SKIP: dtfl-ddir not built"
	exit 77
fi

fail=0

# reach: chain 1->2->3->4 (input 0), root 1 (input 1) => 4 reachable nodes,
# collapsed by the program's MAP(..,(;)) to one (unit;unit) row with x4.
out=$(printf '1 2\n2 3\n3 4\n@1\n1\n' | \
    timeout 60 "$tool" run "$progs/reach.ddir" 2>/dev/null)
if [ $? -ne 0 ]; then
	echo "[ddir-smoke] FAIL: run reach.ddir did not finish"
	fail=1
elif ! printf '%s\n' "$out" | grep -q 'x4'; then
	echo "[ddir-smoke] FAIL: reach.ddir result missing x4:"
	printf '%s\n' "$out"
	fail=1
else
	echo "[ddir-smoke] OK: run reach.ddir -> x4"
fi

# explain: the rewrite exports one demand collection per source, so the
# rewritten program has at least as many exports as the original.
out=$(timeout 60 "$tool" explain "$progs/reach.ddir" 2 2>/dev/null)
if [ $? -ne 0 ]; then
	echo "[ddir-smoke] FAIL: explain reach.ddir did not finish"
	fail=1
elif ! printf '%s\n' "$out" | grep -q 'EXPLAIN'; then
	echo "[ddir-smoke] FAIL: explain output missing EXPLAIN section"
	fail=1
else
	echo "[ddir-smoke] OK: explain reach.ddir"
fi

[ "$fail" -eq 0 ] && echo "[ddir-smoke] OK"
exit "$fail"
