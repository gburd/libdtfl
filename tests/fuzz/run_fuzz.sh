#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/fuzz/run_fuzz.sh: the libFuzzer tier driver (PLAN section 6 item 8).
#
#   run_fuzz.sh TARGET...
#
# Run from the build directory (make fuzz does).  For each target:
#   1. replay every committed input in tests/corpus/fuzz/<name>/ once,
#      seeds and crash reproducers alike, so a fixed bug stays fixed;
#   2. fuzz for DTFL_FUZZ_SECONDS (default 60), starting from that corpus.
#      New inputs go to fuzz-corpus/<name>/ in the build directory, never
#      to the source tree; a crashing input goes to fuzz-out/<name>/.
#
# Environment:
#   DTFL_FUZZ_SECONDS  seconds of fuzzing per target (default 60; 0 replays only)
#   DTFL_FUZZ_JOBS     parallel fuzzing processes per target (default 1;
#                      above 1, libFuzzer's -fork mode)
#   DTFL_FUZZ_ARGS     extra libFuzzer flags, e.g. -max_len=65536
#
# Prints PASS or FAIL per target with its exec count, then a summary.
# Exits 1 if any target failed, 0 otherwise.
set -u

secs=${DTFL_FUZZ_SECONDS:-60}
jobs=${DTFL_FUZZ_JOBS:-1}
extra=${DTFL_FUZZ_ARGS:-}
top=${top_srcdir:-$(cd "$(dirname "$0")/../.." && pwd)}
[ "$secs" -ge 0 ] 2>/dev/null || { echo "fuzz: bad DTFL_FUZZ_SECONDS $secs" >&2; exit 2; }
[ "$jobs" -ge 1 ] 2>/dev/null || { echo "fuzz: bad DTFL_FUZZ_JOBS $jobs" >&2; exit 2; }

pass=0 fail=0
for t in "$@"; do
	name=$(basename "$t")
	seeds=$top/tests/corpus/fuzz/$name
	work=fuzz-corpus/$name
	out=fuzz-out/$name
	log=fuzz-out/$name.log
	mkdir -p "$work" "$out" || exit 2
	if [ ! -d "$seeds" ] || [ -z "$(ls -A "$seeds" 2>/dev/null)" ]; then
		echo "FAIL: $name: no seed corpus in $seeds"
		fail=$((fail + 1))
		continue
	fi

	# 1. Regression replay: each committed input runs once.
	if ! "./$t" -runs=1 "$seeds"/* > "$log" 2>&1; then
		echo "FAIL: $name: a committed input crashes (see $log)"
		tail -30 "$log"
		fail=$((fail + 1))
		continue
	fi
	if [ "$secs" -eq 0 ]; then
		echo "PASS: $name: $(ls "$seeds" | wc -l | tr -d ' ') committed inputs replayed"
		pass=$((pass + 1))
		continue
	fi

	# 2. Fuzz.
	if [ "$jobs" -gt 1 ]; then
		mode="-fork=$jobs"
	else
		mode=""
	fi
	# shellcheck disable=SC2086
	"./$t" $mode -max_total_time="$secs" -print_final_stats=1 \
	    -artifact_prefix="$out/" $extra "$work" "$seeds" >> "$log" 2>&1
	rc=$?
	execs=$(grep -Eo '^(stat::number_of_executed_units: *[0-9]+|#[0-9]+)' "$log" |
	    tail -1 | grep -Eo '[0-9]+$')
	crashes=$(ls "$out" | grep -Ec '^(crash|leak|timeout|oom)-' || true)
	if [ "$rc" -ne 0 ] || [ "$crashes" -ne 0 ]; then
		echo "FAIL: $name: rc=$rc, $crashes crashing inputs in $out (log $log)"
		grep -E '^(==[0-9]+==ERROR|SUMMARY|fuzz assertion|libdtfl panic)' "$log" |
		    head -10
		fail=$((fail + 1))
	else
		echo "PASS: $name: ${execs:-?} execs in ${secs}s x $jobs"
		pass=$((pass + 1))
	fi
done
echo "fuzz: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
