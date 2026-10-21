#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/dist/test_bench_arrange.sh: smoke test bench/experiments/arrange,
# the port of experiments/src/bin/arrange.rs.  Each run uses tiny sizes
# and about a second of work (the closed loop through the --seconds=N
# test knob, the open loop through a small keys, since it stops after
# 10 * keys acknowledgements), and must exit 0 and print upstream's
# line formats:
#
#   ARRANGE LOADING    peers float
#   ARRANGE THROUGHPUT peers float                   (closed loop only)
#   ARRANGE LATENCY    peers keys recs rate work Comp Mode ns fraction
#
# with LATENCY lines in increasing latency and non-increasing fraction.
# This checks the format and the control flow, never performance.
set -u
top="${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}"

bin=bench/experiments/arrange
[ -x "$bin" ] || bin="$top/bench/experiments/arrange"
if [ ! -x "$bin" ]; then
	echo "[bench-arrange] SKIP: bench/experiments/arrange not built"
	exit 77
fi

fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Each worker pins itself to its own CPU, as upstream does, and aborts
# when there are fewer CPUs than workers.
ncpu=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)

# check NAME PEERS COMP MODE KEYS RECS RATE WORK ARGS...
check() {
	name=$1 peers=$2 comp=$3 mode=$4 keys=$5 recs=$6 rate=$7 work=$8
	shift 8
	if [ "$peers" -gt "$ncpu" ]; then
		echo "[bench-arrange] SKIP: $name needs $peers CPUs, have $ncpu"
		return
	fi
	out="$tmp/$name.out"
	if ! timeout 60 "$bin" "$keys" "$recs" "$rate" "$work" "$@" \
	    >"$out" 2>"$tmp/$name.err"; then
		echo "[bench-arrange] FAIL: $name crashed, hung or exited non-zero"
		sed 's/^/    /' "$tmp/$name.err" | head -10
		fail=1
		return
	fi
	if [ "$work" = x ]; then work='[0-9]+'; fi
	if ! awk -F '\t' -v p="$peers" -v c="$comp" -v m="$mode" \
	    -v k="$keys" -v r="$recs" -v rt="$rate" -v w="$work" '
	    function bad(why) { print "    " why ": " $0; err = 1 }
	    $1 != "ARRANGE" { bad("not an ARRANGE line"); next }
	    $2 == "LOADING" || $2 == "THROUGHPUT" {
		if (NF != 4 || $3 != p || $4 !~ /^[0-9]+\.[0-9]+$/)
			bad("bad " $2 " line")
		n[$2]++
		next
	    }
	    $2 == "LATENCY" {
		if (NF != 11 || $3 != p || $4 != k || $5 != r || $6 != rt ||
		    $7 !~ ("^" w "$") || $8 != c || $9 != m ||
		    $10 !~ /^[0-9]+$/ || $11 !~ /^0\.[0-9]+$/)
			bad("bad LATENCY line")
		else if (n["LATENCY"] > 0 && ($10 + 0 <= lat || $11 + 0 > fr))
			bad("LATENCY lines out of order")
		lat = $10 + 0; fr = $11 + 0
		n[$2]++
		next
	    }
	    { bad("unknown line") }
	    END {
		if (n["LOADING"] != 1) { print "    LOADING lines: " n["LOADING"]; err = 1 }
		if (n["THROUGHPUT"] != (m == "ClosedLoop")) {
			print "    THROUGHPUT lines: " n["THROUGHPUT"]; err = 1
		}
		if (n["LATENCY"] < 1) { print "    no LATENCY lines"; err = 1 }
		exit err
	    }' "$out"; then
		echo "[bench-arrange] FAIL: $name output format"
		fail=1
		return
	fi
	echo "[bench-arrange] PASS: $name"
}

check closed-arrange 1 Arrange ClosedLoop 1000 1000 100 x arrange --seconds=1
check closed-count 1 Count ClosedLoop 1000 1000 100 7 count --seconds=1
check closed-exchange-w2 2 Exchange ClosedLoop 1000 1000 100 7 exchange \
    -w 2 --seconds=1
check closed-arrange-w2 2 Arrange ClosedLoop 1000 1000 100 7 arrange -w2 \
    --seconds=1
check open-arrange 1 Arrange OpenLoop 500 500 20000 7 arrange open-loop
check open-arrange-w2 2 Arrange OpenLoop 500 500 20000 7 arrange open-loop \
    -w 2

# Upstream panics on a missing positional or an unknown timely option.
for args in "1000 1000 100" "1000 1000 100 7 arrange -q"; do
	# shellcheck disable=SC2086
	if timeout 10 "$bin" $args >/dev/null 2>&1; then
		echo "[bench-arrange] FAIL: '$args' exited 0"
		fail=1
	else
		echo "[bench-arrange] PASS: '$args' rejected"
	fi
done

exit $fail
