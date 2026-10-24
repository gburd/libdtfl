#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_headers.sh: each public header compiles on its own as
# C11 and as C++17 with all warnings as errors.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
CC="${CC:-cc}"
CXX="${CXX:-c++}"
fail=0
n=0
tmp=$(mktemp -d "${TMPDIR:-/tmp}/dtfl-hdr.XXXXXX") || exit 99
trap 'rm -rf "$tmp"' EXIT
for h in "$top"/include/dtfl/*.h; do
	name=$(basename "$h")
	printf '#include "dtfl/%s"\nint dtfl_header_probe(void);\nint dtfl_header_probe(void) { return 0; }\n' "$name" > "$tmp/c.c"
	cp "$tmp/c.c" "$tmp/cxx.cc"
	if ! $CC -std=c11 -Wall -Wextra -Wpedantic -Werror -I"$top/include" -c "$tmp/c.c" -o "$tmp/c.o" 2> "$tmp/err"; then
		echo "[headers] FAIL: $name as C11"; sed 's/^/    /' "$tmp/err"; fail=1
	fi
	if command -v "$CXX" >/dev/null 2>&1; then
		if ! $CXX -std=c++17 -Wall -Wextra -Wpedantic -Werror -I"$top/include" -c "$tmp/cxx.cc" -o "$tmp/cxx.o" 2> "$tmp/err"; then
			echo "[headers] FAIL: $name as C++17"; sed 's/^/    /' "$tmp/err"; fail=1
		fi
	fi
	n=$((n + 1))
done
[ $fail -eq 0 ] && echo "[headers] OK ($n headers)"
exit $fail
