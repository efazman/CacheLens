#!/usr/bin/env bash
# Captures the full environment a perf measurement was taken under, then
# runs perf stat -r 5 on both matrix benchmarks. Every number this script
# produces is written with its environment block attached — a measurement
# without that block is not a reproducible result (see docs/TAKEAWAYS.md).
#
# Usage: measure_baseline.sh <output-file> <label>
#   <output-file>  appended to, not overwritten — run this multiple times
#                  under different conditions to build up a comparison set.
#   <label>        short free-text tag identifying the run's conditions
#                  (e.g. "performance-governor-quiet", "powersave-quiet").
set -euo pipefail

OUT="${1:?usage: measure_baseline.sh <output-file> <label>}"
LABEL="${2:?usage: measure_baseline.sh <output-file> <label>}"
BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../benchmarks" && pwd)"

{
  echo "================================================================"
  echo "ENVIRONMENT BLOCK — label: $LABEL"
  echo "Captured (UTC): $(date -u +'%Y-%m-%dT%H:%M:%SZ')"
  echo "================================================================"

  echo "--- scaling_governor (per core) ---"
  for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo "$f: $(cat "$f")"
  done

  echo "--- /proc/loadavg ---"
  cat /proc/loadavg

  echo "--- processes above 1% CPU ---"
  ps -eo pid,comm,%cpu --sort=-%cpu | awk 'NR==1 || $3+0 > 1.0'

  echo "--- transparent hugepage ---"
  cat /sys/kernel/mm/transparent_hugepage/enabled

  echo "--- perf_event_paranoid ---"
  cat /proc/sys/kernel/perf_event_paranoid

  echo "--- kernel ---"
  uname -a

  echo "--- compiler ---"
  g++ --version | head -1

  echo "--- build flags (benchmarks/Makefile) ---"
  grep '^CXXFLAGS' "$BENCH_DIR/Makefile"

  echo "--- core frequency, sampled before run (MHz) ---"
  grep MHz /proc/cpuinfo

  echo
  echo "================================================================"
  echo "MEASUREMENT — label: $LABEL"
  echo "================================================================"

  for bin in matrix_bad matrix_good; do
    echo "--- $bin ---"
    perf stat -r 5 -e cache-misses,cache-references,instructions,cycles \
      "$BENCH_DIR/$bin" 2>&1
    echo
  done

  echo "--- core frequency, sampled after run (MHz) ---"
  grep MHz /proc/cpuinfo
  echo
} >> "$OUT"

echo "✓ Appended '$LABEL' block to $OUT"
