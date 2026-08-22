#!/usr/bin/env bash
# Gate 7 Phase 1: builds all queue benchmark variants and measures the
# padded/unpadded wall-clock A/B with `perf stat -r 5`, plus the objdump
# and static_assert evidence for *why* (U18). Writes
# results/gate7_queue_baseline.txt.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=results/gate7_queue_baseline.txt

echo "Building queue benchmarks..."
(cd benchmarks && make queues)

{
  echo "=== Gate 7 Phase 1: queue benchmark baseline ==="
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo
  echo "--- environment ---"
  echo "cpu model: $(lscpu | grep 'Model name' | sed 's/Model name:\s*//')"
  echo "nproc: $(nproc)"
  echo "kernel: $(uname -r)"
  echo "gcc: $(gcc --version | head -1)"
  echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
  echo "topology (cpu:core_id): $(for c in /sys/devices/system/cpu/cpu[0-9]*/topology/core_id; do echo -n "$(basename $(dirname $(dirname $c))):$(cat $c) "; done)"
  echo

  echo "=== U18 evidence: struct layout (static_assert already enforced at compile time) ==="
  echo "-- spsc_queue: tail/head offsets --"
  objdump -d -C benchmarks/spsc_queue_shared | grep -E "mov\s+0x[0-9a-f]+\(%rdi\),%r(dx|si)\s*$" | head -2
  objdump -d -C benchmarks/spsc_queue_padded | grep -E "mov\s+0x[0-9a-f]+\(%rdi\),%r(dx|si)\s*$" | head -2
  echo "(shared: tail=0x4000, head=0x4008 -- 8 bytes apart, one cache line)"
  echo "(padded: tail=0x4000, head=0x4040 -- 64 bytes apart, separate cache lines)"
  echo

  echo "=== SPSC queue: padded vs shared, -O1 (attribution build) ==="
  for b in spsc_queue_shared spsc_queue_padded; do
    echo "-- perf stat -r 5: $b --"
    perf stat -r 5 -e cache-misses,cache-references,instructions,cycles "benchmarks/$b" 2>&1
    echo
  done

  echo "=== SPSC queue: padded vs shared, -O2 (latency-harness build) ==="
  for b in spsc_queue_shared_O2 spsc_queue_padded_O2; do
    echo "-- perf stat -r 5: $b --"
    perf stat -r 5 -e cache-misses,cache-references,instructions,cycles "benchmarks/$b" 2>&1
    echo
  done

  echo "=== MPMC queue (2 producers, 2 consumers): padded vs shared, -O1 ==="
  for b in mpmc_queue_shared mpmc_queue_padded; do
    echo "-- perf stat -r 5: $b --"
    perf stat -r 5 -e cache-misses,cache-references,instructions,cycles "benchmarks/$b" 2>&1
    echo
  done
} | tee "$OUT"

echo
echo "Wrote $OUT"
