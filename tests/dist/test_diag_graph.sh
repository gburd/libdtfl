#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_diag_graph.sh: dtfl-diag graph reads the committed log
# stream fixture and reconstructs the operator graph (PLAN P11 exit
# criterion: "dtfl-diag graph reads the Rust 0.31 log fixture").
#
# The fixture (tests/interop/fixtures/diag_graph_stream.bin) is one
# Messages batch in the EventWriter log-stream wire format (D9),
# encoding a root scope [0] with a subscope [0,2] and channels that
# cross the subscope boundary.  The test runs `dtfl-diag graph -f` on it
# and asserts the generated HTML contains the leaf operators and the
# re-wired ingress/egress channels, so a regression in the scope
# rewiring or the replay decode fails the build.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"
tool="tools/dtfl-diag/dtfl-diag"
[ -x "$tool" ] || tool="$top/tools/dtfl-diag/dtfl-diag"
fixture="$top/tests/interop/fixtures/diag_graph_stream.bin"
data="$top/data"

if [ ! -x "$tool" ]; then
	echo "[diag-graph] SKIP: dtfl-diag not built"
	exit 77
fi
if [ ! -f "$fixture" ]; then
	echo "[diag-graph] SKIP: fixture missing"
	exit 77
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
out="$tmp/graph.html"

if ! DTFL_DATA_DIR="$data" timeout 30 "$tool" -f "$fixture" -o "$out" graph \
    >"$tmp/log" 2>&1; then
	echo "[diag-graph] FAIL: dtfl-diag graph exited non-zero"
	sed 's/^/    /' "$tmp/log" | head
	exit 1
fi

fail=0
need() {
	grep -qF "$1" "$out" || {
		echo "[diag-graph] FAIL: output missing: $1"
		fail=1
	}
}

# Leaf operators present (the subscope [0,2] and root [0] are excluded).
need '"name": "Input", "addr": [0, 1, ]'
need '"name": "Map", "addr": [0, 2, 1, ]'
need '"name": "Filter", "addr": [0, 2, 2, ]'
# The subscope operator itself is not a leaf and must not be listed.
if grep -qF '"name": "Region"' "$out"; then
	echo "[diag-graph] FAIL: subscope Region listed as a leaf operator"
	fail=1
fi

# Ingress channel re-wired: external Input->subscope joined with the
# internal ingress, subgraph=true, Input [0,1] -> Map [0,2,1].
need '"id": [10, 11, ], "subgraph": true, "from_addr": [0, 1, ], "to_addr": [0, 2, 1, ]'
# Egress channel re-wired: internal egress joined with external, subgraph
# true, Filter [0,2,2] -> Input [0,1].
need '"id": [13, 14, ], "subgraph": true, "from_addr": [0, 2, 2, ], "to_addr": [0, 1, ]'
# Non-subgraph internal channel passes through.
need '"id": [12, ], "subgraph": false, "from_addr": [0, 2, 1, ], "to_addr": [0, 2, 2, ]'
# The HTML template was embedded (the renderer entry point).
need 'run(operate, channel);'

[ "$fail" -eq 0 ] && echo "[diag-graph] PASS"

# ----------------------------------------------------------------------
# profile: Schedule Start/Stop pairs give elapsed time per operator.
# ----------------------------------------------------------------------
pfix="$top/tests/interop/fixtures/diag_profile_stream.bin"
if [ -f "$pfix" ]; then
	if ! DTFL_DATA_DIR="$data" timeout 30 "$tool" -f "$pfix" profile \
	    >"$tmp/profile" 2>&1; then
		echo "[diag-graph] FAIL: dtfl-diag profile exited non-zero"
		sed 's/^/    /' "$tmp/profile" | head
		fail=1
	else
		# Filter (300 ns) is reported before Map (100 ns): sorted desc.
		if [ "$(grep -c 'Map\|Filter' "$tmp/profile")" != 2 ] ||
		    ! head -1 "$tmp/profile" | grep -q Filter; then
			echo "[diag-graph] FAIL: profile order/content wrong"
			sed 's/^/    /' "$tmp/profile"
			fail=1
		else
			echo "[diag-graph] PASS: profile"
		fi
	fi
fi

# ----------------------------------------------------------------------
# arrangements: Batch events give the logical size per arrangement.
# ----------------------------------------------------------------------
dfix="$top/tests/interop/fixtures/diag_differential_stream.bin"
if [ -f "$pfix" ] && [ -f "$dfix" ]; then
	if ! DTFL_DATA_DIR="$data" timeout 30 "$tool" -f "$pfix" \
	    --diff-file "$dfix" arrangements >"$tmp/arr" 2>&1; then
		echo "[diag-graph] FAIL: dtfl-diag arrangements exited non-zero"
		sed 's/^/    /' "$tmp/arr" | head
		fail=1
	else
		# Map (id 1) totals 10+5=15 tuples; Filter (id 2) totals 7.
		if ! grep -qE '^1\s+Map\s+15' "$tmp/arr" ||
		    ! grep -qE '^2\s+Filter\s+7' "$tmp/arr"; then
			echo "[diag-graph] FAIL: arrangement sizes wrong"
			sed 's/^/    /' "$tmp/arr"
			fail=1
		else
			echo "[diag-graph] PASS: arrangements"
		fi
	fi
fi

exit "$fail"
