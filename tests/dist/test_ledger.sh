#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_ledger.sh: the port and test ledgers tell the truth.
#
# - Both ledgers parse (7 and 8 tab-separated columns).
# - Every port row marked done or wip names a target that exists.
# - Every done .c/.h target under src/ carries a provenance header that
#   names the row's upstream path ("Ported from <repo> ... <path>").
# - Every test row marked done names a C test that exists: the function
#   name appears in the target file.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
cd "$top" || exit 99
fail=0

awk -F'\t' '!/^#/ && $1 != "repo" && NF != 7 { printf "port-ledger.tsv:%d: %d columns\n", NR, NF; bad=1 } END { exit bad }' dev/port-ledger.tsv || fail=1
awk -F'\t' '!/^#/ && $1 != "repo" && NF != 8 { printf "test-ledger.tsv:%d: %d columns\n", NR, NF; bad=1 } END { exit bad }' dev/test-ledger.tsv || fail=1

done_rows=$(awk -F'\t' '!/^#/ && $1 != "repo" && ($6 == "done" || $6 == "wip") { print $1 "\t" $2 "\t" $4 "\t" $6 }' dev/port-ledger.tsv)
ndone=0
if [ -n "$done_rows" ]; then
	printf '%s\n' "$done_rows" | while IFS='	' read -r repo path target status; do
		case "$target" in
		*/) [ -d "$target" ] || { echo "[ledger] FAIL: $repo:$path -> $target missing"; exit 1; } ;;
		*) [ -f "$target" ] || { echo "[ledger] FAIL: $repo:$path -> $target missing"; exit 1; } ;;
		esac
		if [ "$status" = done ]; then
			case "$target" in
			src/*.c|src/*.h|tests/upstream/*.c)
				grep -q "Ported from $repo" "$target" && grep -qF "$path" "$target" || {
					echo "[ledger] FAIL: $target lacks provenance for $repo:$path"; exit 1; }
				;;
			esac
		fi
	done || fail=1
	ndone=$(printf '%s\n' "$done_rows" | grep -c .)
fi

test_rows=$(awk -F'\t' '!/^#/ && $1 != "repo" && $8 == "done" { print $4 "\t" $6 }' dev/test-ledger.tsv)
ntest=0
if [ -n "$test_rows" ]; then
	printf '%s\n' "$test_rows" | while IFS='	' read -r name target; do
		[ -f "$target" ] && grep -q "$name" "$target" || {
			echo "[ledger] FAIL: test $name not found in $target"; exit 1; }
	done || fail=1
	ntest=$(printf '%s\n' "$test_rows" | grep -c .)
fi

# A done port row whose upstream file has #[test]s in the test ledger
# must have at least one of those tests ported (not all left todo).
# This makes "done" mean "ported and its upstream tests are landing,"
# and fails early and loudly when a file is marked done but its tests
# are skipped.  A port row opts out by listing its tests as subsumed,
# deferred or excluded in the test ledger (those are not todo).
untested=$(awk -F'\t' '
	!/^#/ && $1 != "repo" && FNR == NR {
		# test ledger: upstream path is column 2, status is the last.
		st[$2] = st[$2] " " $NF; next
	}
	!/^#/ && $1 != "repo" && $6 == "done" {
		p = $2
		if (!(p in st)) next          # no upstream tests for this file
		n = split(st[p], a, " ")
		for (i = 1; i <= n; i++)
			if (a[i] == "done") next   # at least one ported: ok
		# every test for this done file is todo (or absent): fail.
		has = 0
		for (i = 1; i <= n; i++) if (a[i] == "todo") has = 1
		if (has) print $2
	}
' dev/test-ledger.tsv dev/port-ledger.tsv)
if [ -n "$untested" ]; then
	echo "[ledger] FAIL: port rows marked done whose upstream tests are all todo:"
	printf '%s\n' "$untested" | sed 's/^/    /'
	echo "    (port a test, or mark the test rows subsumed/deferred/excluded)"
	fail=1
fi

[ $fail -eq 0 ] && echo "[ledger] OK ($ndone file rows wip/done, $ntest tests done)"
exit $fail
