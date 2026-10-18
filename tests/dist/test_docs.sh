#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_docs.sh: documentation gates.
#
# - Every function declared in include/dtfl/*.h appears as .Nm in some
#   man/man3 page.
# - Every man page passes mandoc -Tlint (SKIP for this check when mandoc
#   is not installed; the result says so).
# - Every doc/snippets/*.c compiles against the built library and, when
#   a matching .expected file exists, prints exactly that.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
build="$(pwd)"
CC="${CC:-cc}"
fail=0
skipped=""

# Only headers that are installed (listed in some local.mk) are checked:
# an interface committed ahead of its implementation is not yet public.
installed=""
for h in "$top"/include/dtfl/*.h; do
	n=$(basename "$h")
	if grep -qs "include/dtfl/$n" "$top"/src/*/local.mk; then
		installed="$installed $h"
	fi
done
[ -n "$installed" ] || { echo "[docs] SKIP: no installed headers"; exit 77; }
decls=$(cat $installed | tr '\n' ' ' | sed 's/;/;\n/g' \
	| grep -oE '[[:space:]*]dtfl_[a-z0-9_]+[[:space:]]*\(' \
	| sed -E 's/^[[:space:]*]+//; s/[[:space:]]*\($//' | sort -u)
documented=$(cat "$top"/man/man3/*.3 2>/dev/null | awk '$1 == ".Nm" {print $2}' | sed 's/,$//' | sort -u)
# A declared public function is only "shipped" once it is defined in the
# built library.  Declarations for a later phase (forward declarations in
# an installed header) carry no implementation yet, so they need no page
# until the phase that defines them lands.  When the library is not built
# yet, fall back to requiring every declaration.
lib=$(ls "$build"/.libs/libdtfl.so* "$build"/.libs/libdtfl.*.dylib "$build"/.libs/libdtfl.a 2>/dev/null | head -1)
if [ -n "$lib" ] && command -v nm >/dev/null 2>&1; then
	# nm -g with an awk filter for defined symbols works on GNU and Apple nm
	# (Apple nm has no --defined-only); Mach-O names carry a leading _.
	defined=$(nm -g "$lib" 2>/dev/null | awk '$2 != "U" {print $NF}' \
		| sed 's/^_//' | grep -oE '^dtfl_[a-z0-9_]+' | sort -u)
	decls=$(comm -12 "$(printf '%s\n' "$decls" > docs.d1.tmp; echo docs.d1.tmp)" \
		"$(printf '%s\n' "$defined" > docs.d2.tmp; echo docs.d2.tmp)")
	rm -f docs.d1.tmp docs.d2.tmp
fi
undoc=$(comm -23 "$(printf '%s\n' "$decls" > docs.decl.tmp; echo docs.decl.tmp)" \
	"$(printf '%s\n' "$documented" > docs.doc.tmp; echo docs.doc.tmp)" | grep -v '^$' || true)
rm -f docs.decl.tmp docs.doc.tmp
if [ -n "$undoc" ]; then
	echo "[docs] FAIL: public functions without a man page (.Nm in man/man3):"
	echo "$undoc" | sed 's/^/    /'
	fail=1
fi

MANDOC="${MANDOC:-$(command -v mandoc 2>/dev/null)}"
if [ -z "$MANDOC" ]; then
	for m in /nix/store/*-mandoc-*/bin/mandoc; do [ -x "$m" ] && MANDOC="$m"; done
fi
if [ -n "$MANDOC" ]; then
	for m in "$top"/man/man*/*.[137]; do
		[ -f "$m" ] || continue
		out=$("$MANDOC" -Tlint -W warning "$m" 2>&1) || { echo "[docs] FAIL: mandoc -Tlint $m"; echo "$out" | sed 's/^/    /'; fail=1; }
	done
else
	skipped="$skipped mandoc-lint"
fi

nsnip=0
for s in "$top"/doc/snippets/*.c; do
	[ -f "$s" ] || continue
	name=$(basename "$s" .c)
	exe="$build/doc-snippet-$name"
	if ! $CC -std=c11 -I"$top/include" "$s" -o "$exe" -L"$build/.libs" -ldtfl -Wl,-rpath,"$build/.libs" -lpthread 2> "$exe.err"; then
		echo "[docs] FAIL: snippet $name does not compile"; sed 's/^/    /' "$exe.err"; fail=1; continue
	fi
	if [ -f "$top/doc/snippets/$name.expected" ]; then
		# When the library is ASan-instrumented, a plainly-compiled
		# snippet loads libasan after the executable, so the runtime's
		# link-order check would abort the run.  The snippet code is
		# correct (it builds clean under -fsanitize=address), so relax
		# that one check for the run; it is a no-op in a plain build.
		if ! ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}verify_asan_link_order=0" "$exe" > "$exe.out" 2>&1 || ! cmp -s "$exe.out" "$top/doc/snippets/$name.expected"; then
			echo "[docs] FAIL: snippet $name output differs"; diff "$top/doc/snippets/$name.expected" "$exe.out" | sed 's/^/    /'; fail=1
		fi
	fi
	rm -f "$exe" "$exe.err" "$exe.out"
	nsnip=$((nsnip + 1))
done

if [ $fail -eq 0 ]; then
	echo "[docs] OK ($(printf '%s\n' "$decls" | grep -c .) functions documented, $nsnip snippets)${skipped:+; skipped:$skipped}"
fi
exit $fail
