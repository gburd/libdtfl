#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_usdt.sh: the library carries every documented USDT
# probe.
#
# The probe list is the @item and @itemx lines of the "Static probes"
# table in doc/diagnostics.texi (name, then argument count).  When configure
# enabled USDT, `readelf -n` on the built libdtfl.so must list each of
# them under provider dtfl with that many arguments, and no dtfl probe
# the manual does not name.  SKIP (77) when USDT is off, when readelf is
# missing, or when the shared library is not built.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
build="$(pwd)"

if ! grep -qs '^#define DTFL_USDT 1' "$build/config.h"; then
	echo "[usdt] SKIP: configured without USDT probes (--disable-usdt or no sys/sdt.h)"
	exit 77
fi
READELF="${READELF:-readelf}"
if ! command -v "$READELF" >/dev/null 2>&1; then
	echo "[usdt] SKIP: readelf not found"
	exit 77
fi
lib=$(ls "$build"/.libs/libdtfl.so 2>/dev/null | head -1)
if [ -z "$lib" ]; then
	echo "[usdt] SKIP: .libs/libdtfl.so not built"
	exit 77
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/dtfl-usdt.XXXXXX") || exit 99
trap 'rm -rf "$tmp"' EXIT

# Documented: "@item[x] @code{dtfl:NAME} (N arguments)" -> "NAME N".
sed -n '/^@c usdt-probes-begin/,/^@c usdt-probes-end/p' \
    "$top/doc/diagnostics.texi" \
	| sed -n 's/^@itemx\{0,1\} @code{dtfl:\([a-z_]*\)}.*(\([0-9]*\) argument.*/\1 \2/p' \
	| sort -u > "$tmp/doc"
if [ ! -s "$tmp/doc" ]; then
	echo "[usdt] FAIL: no probes found in doc/diagnostics.texi"
	exit 1
fi

# Built: each stapsdt note's Name and argument count, provider dtfl only.
# One probe may have several sites (inlined copies); count each once.
"$READELF" -n "$lib" | awk '
	$1 == "Provider:" { prov = $2; name = ""; next }
	$1 == "Name:" && prov == "dtfl" { name = $2; next }
	$1 == "Arguments:" && prov == "dtfl" && name != "" {
		print name, NF - 1; name = ""; next
	}' | sort -u > "$tmp/elf"

fail=0
missing=$(comm -23 "$tmp/doc" "$tmp/elf")
extra=$(comm -13 "$tmp/doc" "$tmp/elf")
if [ -n "$missing" ]; then
	echo "[usdt] FAIL: documented probes (name, args) not in $lib:"
	echo "$missing" | sed 's/^/    /'
	fail=1
fi
if [ -n "$extra" ]; then
	echo "[usdt] FAIL: probes (name, args) in $lib not documented in doc/diagnostics.texi:"
	echo "$extra" | sed 's/^/    /'
	fail=1
fi
[ $fail -eq 0 ] && echo "[usdt] OK ($(wc -l < "$tmp/doc" | tr -d ' ') probes under provider dtfl)"
exit $fail
