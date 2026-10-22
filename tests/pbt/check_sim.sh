#!/bin/sh
# SPDX-License-Identifier: MIT
# tests/pbt/check_sim.sh: simulator coverage (PLAN section 6, P16.7).
#
#   check_sim.sh [-n SEEDS] [-j JOBS] [-o DIR] [-s BASE]
#
# Runs every simulator-capable property under DTFL_COMM_SIM
# (DTFL_PBT_COMM=sim) with faults on (DTFL_PBT_FAULTS=on) for SEEDS cases
# each; every case draws its own simulator seed, worker count, progress
# mode and fault values, so SEEDS is the number of simulated schedules
# per property.  Set DTFL_PBT_FAULTS=draw to let hegel draw whether the
# faults are on, or =off to run fault-free schedules only.  The cases are split
# into JOBS shards that run in parallel; shard k runs hegel with seed
# BASE + k, so a run is reproducible from (SEEDS, JOBS, BASE).
#
# Defaults: SEEDS from DTFL_SIM_SEEDS or 2000, JOBS from DTFL_SIM_JOBS or
# 1, BASE from DTFL_SIM_BASE or 1, DIR a fresh directory under
# ${TMPDIR:-.}.  Run from the build directory (make check-sim does).
#
# Prints PASS, FAIL or SKIP per property and exits 0 if all passed, 1 if
# any failed, and 77 if the suites were built without hegel.  A FAIL
# prints the shard's minimal counterexample and the command that replays
# it; each shard's full log stays in DIR.
set -u

seeds=${DTFL_SIM_SEEDS:-2000}
jobs=${DTFL_SIM_JOBS:-1}
base=${DTFL_SIM_BASE:-1}
out=""
while getopts n:j:o:s: opt; do
	case $opt in
	n) seeds=$OPTARG ;;
	j) jobs=$OPTARG ;;
	o) out=$OPTARG ;;
	s) base=$OPTARG ;;
	*) echo "usage: $0 [-n seeds] [-j jobs] [-o dir] [-s base]" >&2
	   exit 2 ;;
	esac
done
[ "$jobs" -ge 1 ] 2>/dev/null || { echo "check-sim: bad -j $jobs" >&2; exit 2; }
[ "$seeds" -ge 1 ] 2>/dev/null || { echo "check-sim: bad -n $seeds" >&2; exit 2; }
if [ -z "$out" ]; then
	out=$(mktemp -d "${TMPDIR:-.}/check-sim.XXXXXX") || exit 2
fi
mkdir -p "$out" || exit 2
build=$(pwd)

# suite program and property: the simulator-capable properties.
props="
tests/pbt/pbt_comm sim_schedule_model
tests/pbt/pbt_comm sim_replay
tests/pbt/pbt_join join_multiworker
tests/pbt/pbt_reduce count_multiworker
tests/pbt/pbt_ops_notify notify_multiworker
tests/pbt/pbt_bfs bfs_multiworker
"

per=$(( (seeds + jobs - 1) / jobs ))
echo "check-sim: $seeds seeds per property in $jobs shard(s) of $per, base $base"
echo "check-sim: logs in $out"
start=$(date +%s)
status=0
skipped=0
echo "$props" | while read -r prog prop; do
	[ -n "$prog" ] || continue
	echo "$prog $prop"
done > "$out/props"
while read -r prog prop; do
	if [ ! -x "$build/$prog" ]; then
		echo "FAIL: $prop ($prog is not built; run make check-sim)"
		status=1
		continue
	fi
	k=0
	while [ "$k" -lt "$jobs" ]; do
		d="$out/$prop.$k"
		mkdir -p "$d"
		# Each shard runs in its own directory: hegel keeps its example
		# database under the working directory.
		( cd "$d" && DTFL_PBT_COMM=sim \
		    DTFL_PBT_FAULTS=${DTFL_PBT_FAULTS:-on} DTFL_PBT_ONLY=$prop \
		    DTFL_PBT_CASES=$per DTFL_PBT_SEED=$((base + k)) \
		    "$build/$prog" > log 2>&1; echo $? > rc ) &
		k=$((k + 1))
	done
	wait
	k=0
	res=PASS
	while [ "$k" -lt "$jobs" ]; do
		d="$out/$prop.$k"
		rc=$(cat "$d/rc")
		if [ "$rc" = 77 ]; then
			res=SKIP
		elif [ "$rc" != 0 ] || ! grep -q "/$prop OK" "$d/log"; then
			res=FAIL
			echo "--- $prop shard $k (DTFL_PBT_SEED=$((base + k)), rc $rc):"
			grep -v '^\[PBT\] .* OK ' "$d/log" | tail -40
		fi
		k=$((k + 1))
	done
	case $res in
	PASS) echo "PASS: $prop ($((per * jobs)) seeds)" ;;
	SKIP) echo "SKIP: $prop (built without hegel; configure --with-hegel)"
	      skipped=1 ;;
	FAIL) echo "FAIL: $prop"; status=1 ;;
	esac
done < "$out/props"
echo "check-sim: $(( $(date +%s) - start ))s"
if [ "$status" -ne 0 ]; then
	echo "check-sim: FAIL.  Run the printed replay line's variables on the"
	echo "check-sim: property's program (the suite named before the slash)."
	exit 1
fi
[ "$skipped" -ne 0 ] && exit 77
exit 0
