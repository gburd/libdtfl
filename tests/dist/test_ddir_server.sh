#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_ddir_server.sh: replay each DDIR server session and check
# its transcript.
#
# The session scripts (shipped as data) are command streams the server
# replays: install/feed/tick/drop/peek/list.  Their install lines name
# program paths relative to interactive/examples/server/, so the test
# stages the shipped programs and sessions under examples/server/ in a
# scratch directory, runs `dtfl-ddir server <session>` there, and diffs the
# transcript (stdout; inspect output goes to stderr and is ignored) against
# the committed expected output.
#
# A hang is a bug: every run is under a wall-clock timeout.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
tool="tools/dtfl-ddir/dtfl-ddir"
[ -f "$tool" ] || tool="$top/tools/dtfl-ddir/dtfl-ddir"
data="$top/tools/dtfl-ddir/server"
expdir="$top/tests/upstream/ddir/sessions"

if [ ! -f "$tool" ]; then
	echo "[ddir-server] SKIP: dtfl-ddir not built"
	exit 77
fi
# Resolve to an absolute path so the staged working dir does not shadow it.
case "$tool" in /*) ;; *) tool="$(pwd)/$tool" ;; esac

# Stage the shipped programs and sessions so install paths resolve.
stage=$(mktemp -d "${TMPDIR:-/tmp}/ddir-sess-XXXXXX") || exit 99
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/examples/server"
cp -r "$data/programs" "$stage/examples/server/" || exit 99
cp -r "$data/sessions" "$stage/examples/server/" || exit 99

fail=0
for sess in clock derived peek values drop generated shared_trace arrange_idem; do
	exp="$expdir/$sess.expected"
	got="$stage/$sess.got"
	if [ ! -f "$exp" ]; then
		echo "[ddir-server] FAIL: no expected transcript for $sess"
		fail=1
		continue
	fi
	# 60s wall timeout: a session that hangs is a bug, not a pass.
	( cd "$stage" && timeout 60 "$tool" server \
	    "examples/server/sessions/$sess.txt" 2>/dev/null ) > "$got"
	rc=$?
	if [ "$rc" -eq 124 ]; then
		echo "[ddir-server] FAIL: session $sess timed out (hang)"
		fail=1
		continue
	fi
	if [ "$rc" -ne 0 ]; then
		echo "[ddir-server] FAIL: session $sess exited $rc"
		fail=1
		continue
	fi
	if diff -u "$exp" "$got" > "$stage/$sess.diff" 2>&1; then
		echo "[ddir-server] OK: $sess"
	else
		echo "[ddir-server] FAIL: $sess transcript differs:"
		cat "$stage/$sess.diff"
		fail=1
	fi
done

[ "$fail" -eq 0 ] && echo "[ddir-server] OK (8 sessions)"
exit "$fail"
