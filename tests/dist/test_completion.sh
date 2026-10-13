#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_completion.sh: the project is done only when nothing is
# left at todo or wip.  dev/COMPLETION.md defines "done" for the whole
# project; this gate measures it from the ledgers (the source of truth).
#
# Two modes:
#   reporting (default): print the remaining counts and exit 0, so the
#     normal build is not blocked and every `make check` shows progress.
#   enforcing (DTFL_COMPLETION_ENFORCE=1, set on the release branch):
#     exit nonzero if any port-ledger or test-ledger row is todo or wip.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$top" || exit 99

port_todo=$(awk -F'\t' '!/^#/ && $1 != "repo" && ($6 == "todo" || $6 == "wip") { n++ } END { print n+0 }' dev/port-ledger.tsv)
port_done=$(awk -F'\t' '!/^#/ && $1 != "repo" && $6 == "done" { n++ } END { print n+0 }' dev/port-ledger.tsv)
test_todo=$(awk -F'\t' '!/^#/ && $1 != "repo" && ($8 == "todo" || $8 == "wip") { n++ } END { print n+0 }' dev/test-ledger.tsv)
test_done=$(awk -F'\t' '!/^#/ && $1 != "repo" && $8 == "done" { n++ } END { print n+0 }' dev/test-ledger.tsv)

# dev/COMPLETION.md tracks items that are not a single ledger row (the
# deferred-with-reason items, e.g. the TCP property test).  Any unchecked
# [ ] box is unfinished work the release must still close.
completion_open=$(grep -c '^\[ \]' dev/COMPLETION.md 2>/dev/null || echo 0)

remaining=$((port_todo + test_todo))

if [ "${DTFL_COMPLETION_ENFORCE:-0}" = 1 ]; then
	if [ "$remaining" -ne 0 ] || [ "$completion_open" -ne 0 ]; then
		echo "[completion] FAIL: $port_todo port rows and $test_todo test rows still todo/wip, $completion_open open COMPLETION.md items"
		echo "    the project is not done; see dev/COMPLETION.md"
		awk -F'\t' '!/^#/ && $1 != "repo" && ($6 == "todo" || $6 == "wip") { print "    port  " $5 "  " $2 }' dev/port-ledger.tsv
		grep -n '^\[ \]' dev/COMPLETION.md | sed 's/^/    completion /'
		exit 1
	fi
	echo "[completion] OK (enforcing): all $port_done port rows and $test_done test rows done, COMPLETION.md fully checked"
	exit 0
fi

echo "[completion] REPORT: $port_done port done, $port_todo todo/wip; $test_done tests done, $test_todo todo/wip; $completion_open open COMPLETION.md items (set DTFL_COMPLETION_ENFORCE=1 to require zero)"
exit 0
