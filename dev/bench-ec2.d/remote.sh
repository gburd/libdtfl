#!/bin/bash
# SPDX-License-Identifier: MIT
# dev/bench-ec2.d/remote.sh MATRIX REPS [PLACEMENT]: runs on the benchmark
# instance.
# Provisions (Debian), builds libdtfl and the Rust pin, tunes the host,
# runs the matrix and leaves results in ~/res.  Called by dev/bench-ec2.
set -euo pipefail
matrix=${1:-full}
reps=${2:-5}
placement=${3:-packed}
res=$HOME/res
mkdir -p "$res"
log() { printf '[remote] %s\n' "$*"; }

log "provisioning"
sudo DEBIAN_FRONTEND=noninteractive apt-get -qq update >/dev/null
sudo DEBIAN_FRONTEND=noninteractive apt-get -qq install -y build-essential \
    autoconf automake libtool pkg-config linux-perf linux-cpupower numactl \
    curl texinfo python3 >/dev/null
curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal >/dev/null 2>&1
. "$HOME/.cargo/env"
tar xzf libdtfl.tgz
tar xzf dd.tgz
tar xzf timely.tgz
# The pin builds against timely from crates.io (`timely = "0.31"`), so
# patch it to the pinned timely tree, and use the committed lockfile so
# every other dependency resolves the same way on every run.
cat >> dd/Cargo.toml <<'TOML'

[patch.crates-io]
timely = { path = "../timely/timely" }
timely_container = { path = "../timely/container" }
timely_communication = { path = "../timely/communication" }
timely_bytes = { path = "../timely/bytes" }
timely_logging = { path = "../timely/logging" }
TOML
cp differential-v0.25.1.Cargo.lock dd/Cargo.lock

log "building"
(cd libdtfl && ./bootstrap >/dev/null 2>&1)
mkdir -p b-c
(cd b-c && ../libdtfl/configure CFLAGS="-O2 -g -fno-omit-frame-pointer" >c.log 2>&1 &&
    make -j"$(nproc)" >m.log 2>&1)
(cd dd/experiments && RUSTFLAGS="-C force-frame-pointers=yes" \
    cargo build --release --bin arrange >../cargo.log 2>&1)
# The idle-step benchmark (P16.5) is timely's barrier example: one
# notification per round through a feedback loop, no data.
(cd timely/timely && RUSTFLAGS="-C force-frame-pointers=yes" \
    cargo build --release --example barrier >../cargo.log 2>&1)
CB=$HOME/b-c/examples/timely/.libs/barrier
RB=$HOME/timely/target/release/examples/barrier
C=$HOME/b-c/bench/experiments/.libs/arrange
R=$HOME/dd/target/release/arrange
L=$HOME/b-c/.libs

log "tuning"
sudo cpupower frequency-set -g performance >/dev/null 2>&1 || true
echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo >/dev/null 2>&1 || true
base=$(cat /sys/devices/system/cpu/cpu0/cpufreq/base_frequency 2>/dev/null ||
    cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq)
for f in /sys/devices/system/cpu/cpu*/cpufreq; do
	echo "$base" | sudo tee "$f/scaling_max_freq" >/dev/null
	echo "$base" | sudo tee "$f/scaling_min_freq" >/dev/null
done
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null
sudo sysctl -qw kernel.numa_balancing=0 kernel.nmi_watchdog=0 \
    kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0 || true
for t in $(systemctl list-timers --all --no-legend 2>/dev/null | awk '{print $(NF-1)}'); do
	sudo systemctl stop "$t" 2>/dev/null || true
done
sudo systemctl stop unattended-upgrades 2>/dev/null || true
{
	uname -a; lscpu | grep -E 'Model name|^CPU\(s\)|Socket|NUMA node\(s\)'
	echo "placement: $placement"
	gcc --version | head -1; rustc --version
	echo "tuning: governor performance, no_turbo 1, freq $base kHz pinned, THP never, numa_balancing 0, nmi_watchdog 0, timers stopped"
	echo "clock check: $(taskset -c 2 perf stat -x, -e cycles -- timeout 2 sh -c 'while :; do :; done' 2>&1 | head -1)"
} > "$res/host.txt"

# Physical cores (one hardware thread each), starting at core 2.  packed:
# socket 0's cores, then socket 1's.  spread: alternate sockets, so worker
# i and i+1 land on different sockets (PLAN P16.9).
mapfile -t cores < <(lscpu -p=CPU,CORE,SOCKET | grep -v '^#' |
    sort -t, -k2,2n -k1,1n | awk -F, -v p="$placement" '
	!seen[$2]++ { s = $3; n[s]++; c[s, n[s]] = $1; if (s + 1 > ns) ns = s + 1 }
	END {
		if (p == "spread") {
			for (i = 1; ; i++) {
				any = 0
				for (s = 0; s < ns; s++)
					if (i <= n[s]) { print c[s, i]; any = 1 }
				if (!any) break
			}
		} else
			for (s = 0; s < ns; s++)
				for (i = 1; i <= n[s]; i++) print c[s, i]
	}')
ncores=${#cores[@]}
cpus_for() { # n -> comma list of the first n physical cores from core 2, wrapping
	local n=$1 i out=""
	for ((i = 0; i < n; i++)); do out+="${cores[$(((i + 2) % ncores))]},"; done
	echo "${out%,}"
}

# One run: impl comp workers mode rate rep -> a CSV row and raw files.
EV=instructions:u,cycles:u,branch-misses:u,page-faults:u,context-switches,syscalls:sys_enter_futex
echo "impl,comp,workers,mode,rate,rep,recs_per_s,instr,cycles,br_miss,faults,cs,futex" > "$res/runs.csv"
run() {
	local impl=$1 comp=$2 w=$3 mode=$4 rate=$5 rep=$6 bin extra=""
	local tag="$impl-$comp-w$w-$mode-r$rate-$rep"
	[ "$impl" = c ] && bin=$C || bin=$R
	[ "$mode" = open ] && extra=open-loop
	sudo perf stat -x, -o "$res/$tag.perf" -e $EV -- env LD_LIBRARY_PATH=$L \
	    taskset -c "$(cpus_for "$w")" "$bin" 100000 100000 "$rate" 100000000 \
	    "$comp" -w "$w" $extra > "$res/$tag.out" 2>&1 || true
	local tp
	tp=$(awk -F'\t' '$2=="THROUGHPUT" {print $4}' "$res/$tag.out")
	v() { awk -F, -v e="$1" '$3 ~ "^"e {print $1}' "$res/$tag.perf"; }
	echo "$impl,$comp,$w,$mode,$rate,$rep,$tp,$(v instructions),$(v cycles),$(v branch-misses),$(v page-faults),$(v context-switches),$(v syscalls:sys_enter_futex)" >> "$res/runs.csv"
}
pair() { # alternate C and Rust, swapping the order on odd reps; rep 0 is the discarded warm-up
	local rep
	for ((rep = 0; rep <= reps; rep++)); do
		if ((rep % 2)); then run r "$@" "$rep"; run c "$@" "$rep"
		else run c "$@" "$rep"; run r "$@" "$rep"; fi
	done
}

if [ "$matrix" = quick ]; then workers="1 4 16"; else workers="1 2 4 8 16 $ncores"; fi
for comp in arrange count; do
	for w in $workers; do
		[ "$w" -le "$ncores" ] && pair "$comp" "$w" closed 1000
	done
done

# Open loop, one worker, at fractions of the C build's one-worker capacity.
for comp in arrange count; do
	cap=$(awk -F, -v c="$comp" '$1=="c" && $2==c && $3==1 && $4=="closed" && $6>0 {print $7}' \
	    "$res/runs.csv" | sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}')
	[ -z "$cap" ] && continue
	if [ "$matrix" = quick ]; then fracs="0.5"; else fracs="0.25 0.5 0.75"; fi
	for f in $fracs; do
		rate=$(awk -v c="$cap" -v f="$f" 'BEGIN {printf "%d", c * f}')
		pair "$comp" 1 open "$rate"
	done
done
# Idle-step cost: barrier rounds, C against Rust, one worker and four.
echo "impl,workers,rounds,rep,seconds" > "$res/step.csv"
rounds=2000000
for w in 1 4; do
	for ((rep = 0; rep <= reps; rep++)); do
		for impl in c r; do
			[ "$impl" = c ] && bin=$CB || bin=$RB
			t0=$(date +%s.%N)
			env LD_LIBRARY_PATH=$L taskset -c "$(cpus_for "$w")" "$bin" "$rounds" -w "$w" \
			    > /dev/null 2>&1 || true
			t1=$(date +%s.%N)
			echo "$impl,$w,$rounds,$rep,$(awk -v a="$t0" -v b="$t1" 'BEGIN {print b - a}')" >> "$res/step.csv"
		done
	done
done
log "done"
