#!/usr/bin/env bash
# Demo: run the CacheLens Python prototype against the benchmark binaries.
# Requires Linux with a hardware PMU — see README.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BENCH_DIR="$PROJECT_DIR/benchmarks"

cd "$PROJECT_DIR"
export PYTHONPATH="${PROJECT_DIR}/src"
PYTHON_BIN="${PYTHON_BIN:-python3}"

echo "=== Building benchmarks ==="
bash scripts/build_benchmarks.sh
echo ""

echo "=== Capability check (matrix_bad) ==="
"$PYTHON_BIN" -m cacheprof.cli check "$BENCH_DIR/matrix_bad"
echo ""

echo "=== Profile: matrix_bad ==="
"$PYTHON_BIN" -m cacheprof.cli profile "$BENCH_DIR/matrix_bad"
echo ""

echo "=== Profile: matrix_good ==="
"$PYTHON_BIN" -m cacheprof.cli profile "$BENCH_DIR/matrix_good"
echo ""

echo "=== Profile: pointer_chase ==="
"$PYTHON_BIN" -m cacheprof.cli profile "$BENCH_DIR/pointer_chase"
echo ""

echo "Done. Check outputs/ for results."
