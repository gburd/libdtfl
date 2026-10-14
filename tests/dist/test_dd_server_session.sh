#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_dd_server_session.sh: replay each dd_server plugin session
# and check its transcript.
#
# The session scripts (shipped as data) drive the plugin server through
# load/tick/peek/list: random_graph publishes a shared edge trace and the
# query plugins import it.  The sessions are deterministic (the graph is
# seeded, finite, and inserted at time 0), so the stdout transcript matches
# a committed expected file exactly.  Plugin inspect output goes to stderr
# and is ignored.
#
# A hang is a bug: every run is under a wall-clock timeout.  A server that
# never accepts, a plugin dataflow that does not converge, or a shared trace
# whose frontier never advances would hang here and fail the build.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
tool="tools/dtfl-server/dtfl-server"
[ -f "$tool" ] || tool="$top/tools/dtfl-server/dtfl-server"
sessdir="$top/tools/dtfl-server/sessions"
expdir="$top/tests/upstream/server/sessions"

if [ ! -f "$tool" ]; then
	echo "[dd-server] SKIP: dtfl-server not built"
	exit 77
fi
case "$tool" in /*) ;; *) tool="$(pwd)/$tool" ;; esac

# A timeout wrapper so a hang fails rather than wedges the suite.
run() {
	if command -v timeout >/dev/null 2>&1; then
		timeout 120 "$@"
	else
		"$@"
	fi
}

fail=0
for sess in degr_dist reachability shared; do
	exp="$expdir/$sess.expected"
	got="$(mktemp "${TMPDIR:-/tmp}/dd-sess-XXXXXX")" || exit 99
	if [ ! -f "$exp" ]; then
		echo "[dd-server] FAIL: missing expected $exp"
		fail=1
		rm -f "$got"
		continue
	fi
	run "$tool" "$sessdir/$sess.txt" >"$got" 2>/dev/null
	rc=$?
	if [ "$rc" -eq 124 ]; then
		echo "[dd-server] FAIL: $sess timed out (hang is a bug)"
		fail=1
		rm -f "$got"
		continue
	fi
	if diff -u "$exp" "$got" >/dev/null 2>&1; then
		echo "[dd-server] PASS: $sess"
	else
		echo "[dd-server] FAIL: $sess transcript differs"
		diff -u "$exp" "$got" | sed 's/^/    /'
		fail=1
	fi
	rm -f "$got"
done

exit $fail
