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

[ $fail -eq 0 ] && echo "[ledger] OK ($ndone file rows wip/done, $ntest tests done)"
exit $fail
