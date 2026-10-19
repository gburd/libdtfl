#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_completion.sh: the project is done only when nothing is
# left at todo or wip in the port and test ledgers (the source of truth).
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

# Work that is not a single ledger row is tracked in a local checklist,
# .agent/dev/COMPLETION.md, which is a working file and not committed
# (AGENTS.md section 8).  When it is present, each unchecked [ ] box
# counts as open; a checkout or tarball without it counts only the
# ledgers.
checklist=.agent/dev/COMPLETION.md
if [ -f "$checklist" ]; then
	completion_open=$(grep -c '^\[ \]' "$checklist")
else
	completion_open=0
fi

remaining=$((port_todo + test_todo))

if [ "${DTFL_COMPLETION_ENFORCE:-0}" = 1 ]; then
	if [ "$remaining" -ne 0 ] || [ "$completion_open" -ne 0 ]; then
		echo "[completion] FAIL: $port_todo port rows and $test_todo test rows still todo/wip, $completion_open open checklist items"
		echo "    the project is not done"
		awk -F'\t' '!/^#/ && $1 != "repo" && ($6 == "todo" || $6 == "wip") { print "    port  " $5 "  " $2 }' dev/port-ledger.tsv
		[ -f "$checklist" ] && grep -n '^\[ \]' "$checklist" |
		    sed 's/^/    checklist /'
		exit 1
	fi
	echo "[completion] OK (enforcing): all $port_done port rows and $test_done test rows done, no open checklist items"
	exit 0
fi

echo "[completion] REPORT: $port_done port done, $port_todo todo/wip; $test_done tests done, $test_todo todo/wip; $completion_open open checklist items (set DTFL_COMPLETION_ENFORCE=1 to require zero)"
exit 0
