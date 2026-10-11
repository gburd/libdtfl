#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_symbols.sh: the shared library exports exactly the
# dtfl_* functions listed in src/*/exports.sym, and every writable or
# thread-local symbol it defines is on the allowlist (PLAN section 5).
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
lib="${DTFL_LIB:-.libs/libdtfl.so}"
NM="${NM:-nm}"
if [ ! -f "$lib" ]; then
	echo "[symbols] SKIP: $lib not built (static-only or non-ELF build)"
	exit 77
fi
fail=0

exported=$($NM -D --defined-only "$lib" | awk '$2 ~ /^[TDBRVW]$/ {print $3}' | sed 's/@.*//' | grep -v '^DTFL_[0-9]' | sort -u)
bad=$(echo "$exported" | grep -v '^dtfl_' || true)
if [ -n "$bad" ]; then
	echo "[symbols] FAIL: exported symbols without the dtfl_ prefix:"
	echo "$bad" | sed 's/^/    /'
	fail=1
fi

listed=$(cat "$top"/src/*/exports.sym 2>/dev/null | sed 's/#.*//' | grep -v '^[[:space:]]*$' | sort -u)
missing=$(comm -23 "$(printf '%s\n' "$listed" > exports.listed.tmp; echo exports.listed.tmp)" \
	"$(printf '%s\n' "$exported" > exports.actual.tmp; echo exports.actual.tmp)")
extra=$(comm -13 exports.listed.tmp exports.actual.tmp)
rm -f exports.listed.tmp exports.actual.tmp
if [ -n "$missing" ]; then
	echo "[symbols] FAIL: listed in exports.sym but not exported:"
	echo "$missing" | sed 's/^/    /'
	fail=1
fi
if [ -n "$extra" ]; then
	echo "[symbols] FAIL: exported but not listed in any src/*/exports.sym:"
	echo "$extra" | sed 's/^/    /'
	fail=1
fi

# Writable globals (D/B) and TLS in the library's own objects.
allowed='^(hooks|hooks_used|panic_fn|panic_ctx|err_detail)$'
writable=$($NM --defined-only "$lib" | awk '$2 ~ /^[dDbB]$/ {print $3}' \
	| grep -vE '^(__|_[A-Z]|completed\.|_edata|_end|__bss_start|_fini|_init|__dso_handle|__TMC_END__|__FRAME_END__|_DYNAMIC|_GLOBAL_OFFSET_TABLE_)' \
	| grep -vE '\.(0|[0-9]+)$' || true)
unexpected=$(echo "$writable" | grep -vE "$allowed" | grep -v '^$' || true)
if [ -n "$unexpected" ]; then
	echo "[symbols] FAIL: writable globals outside the allowlist (PLAN section 5):"
	echo "$unexpected" | sed 's/^/    /'
	fail=1
fi

[ $fail -eq 0 ] && echo "[symbols] OK ($(echo "$exported" | grep -c .) exported)"
exit $fail
