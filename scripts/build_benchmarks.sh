#!/usr/bin/env bash
# Build all benchmark binaries.
set -euo pipefail
cd "$(dirname "$0")/../benchmarks"
make clean
make all
echo "✓ Benchmarks built."
file matrix_good matrix_bad pointer_chase
