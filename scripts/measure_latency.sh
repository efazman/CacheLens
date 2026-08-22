#!/usr/bin/env bash
# Gate 7 Phase 5: runs the open-loop queue latency harness 5x (U22) under
# whatever governor is currently active, with an environment block, and
# appends to results/gate7_latency.txt. Does NOT switch governors itself
# (requires sudo interactively on this machine) -- run once under each
# governor you want compared, e.g.:
#   echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
#   scripts/measure_latency.sh
#   echo powersave | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
#   scripts/measure_latency.sh
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=results/gate7_latency.txt
GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)

echo "Building queue_latency..."
(cd benchmarks && make queue_latency)

{
  echo "=== Gate 7 Phase 5: queue latency, governor=$GOVERNOR ==="
  echo "generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "cpu model: $(lscpu | grep 'Model name' | sed 's/Model name:\s*//')"
  echo "governor: $GOVERNOR"
  echo "nproc: $(nproc)"
  echo
  for i in 1 2 3 4 5; do
    echo "-- run $i --"
    ./benchmarks/queue_latency
    echo
  done
} | tee -a "$OUT"

echo
echo "Appended to $OUT (governor=$GOVERNOR)"
