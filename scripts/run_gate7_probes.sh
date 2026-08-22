#!/usr/bin/env bash
# Builds and runs all four Gate 7 Phase 0 probes (U1-U4), appending an
# environment block. Writes results/gate7_probes.txt. Nothing here is
# cachelens code -- these are throwaway probes kept as evidence.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=results/gate7_probes.txt
PROBES=probes
BUILD=/tmp/gate7_probes_build
mkdir -p "$BUILD"

echo "Building probes..."
gcc -O2 -Wall -o "$BUILD/p1_percpu_open" "$PROBES/p1_percpu_open.c"
gcc -O2 -Wall -o "$BUILD/p2_victim" "$PROBES/p2_victim.c"
gcc -O2 -Wall -o "$BUILD/p2_enable_on_exec" "$PROBES/p2_enable_on_exec.c"
gcc -O2 -Wall -o "$BUILD/p3_mlock_budget" "$PROBES/p3_mlock_budget.c"
g++ -O2 -g -no-pie -Wall -pthread -DCACHELENS_PAD_INDICES=0 -o "$BUILD/p4_shared" "$PROBES/p4_false_sharing.cpp"
g++ -O2 -g -no-pie -Wall -pthread -DCACHELENS_PAD_INDICES=1 -o "$BUILD/p4_padded" "$PROBES/p4_false_sharing.cpp"

{
  echo "=== Gate 7 Phase 0 probes ==="
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
  echo "--- environment ---"
  echo "cpu model: $(lscpu | grep 'Model name' | sed 's/Model name:\s*//')"
  echo "nproc: $(nproc)"
  echo "kernel: $(uname -r)"
  echo "gcc: $(gcc --version | head -1)"
  echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
  echo "perf_event_mlock_kb: $(cat /proc/sys/kernel/perf_event_mlock_kb)"
  echo "perf_event_max_sample_rate: $(cat /proc/sys/kernel/perf_event_max_sample_rate)"
  echo "ulimit -l: $(ulimit -l)"
  echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
  echo "topology (cpu:core_id):"
  for c in /sys/devices/system/cpu/cpu[0-9]*/topology/core_id; do
    cpu=$(echo "$c" | grep -oE 'cpu[0-9]+')
    echo "  $cpu: $(cat "$c")"
  done
  echo

  echo "=== P1: per-CPU task event open (U1) ==="
  "$BUILD/p1_percpu_open" || true
  echo

  echo "=== P2: enable_on_exec on cpu>=0 (U2) ==="
  "$BUILD/p2_enable_on_exec" "$BUILD/p2_victim" || true
  echo

  echo "=== P3: mlock budget (U3) ==="
  "$BUILD/p3_mlock_budget" || true
  echo

  echo "=== P4: false sharing visibility to generalized cache events (U4) ==="
  echo "-- objdump: shared build, worker_a increment loop --"
  objdump -d -C "$BUILD/p4_shared" | awk '/<worker_a\(Counters\*\)>:/{p=1} p; p&&/^$/{if(++n==2) exit}'
  echo
  echo "-- objdump: padded build, worker_a increment loop --"
  objdump -d -C "$BUILD/p4_padded" | awk '/<worker_a\(Counters\*\)>:/{p=1} p; p&&/^$/{if(++n==2) exit}'
  echo
  echo "-- static_assert layout check: both builds compiled clean (see above) --"
  echo
  echo "-- perf stat: p4_shared (counters share one cache line) --"
  perf stat -r 5 -e cache-misses,cache-references,instructions,cycles "$BUILD/p4_shared" 2>&1
  echo
  echo "-- perf stat: p4_padded (counters on separate cache lines) --"
  perf stat -r 5 -e cache-misses,cache-references,instructions,cycles "$BUILD/p4_padded" 2>&1
} | tee "$OUT"

echo
echo "Wrote $OUT"
