# CacheScope

A CLI profiler that detects cache locality bottlenecks in compiled programs,
maps them to source lines via DWARF debug info, and uses an LLM to generate
grounded explanations and fix suggestions.

## How it works

```
detect capabilities → collect raw data → parse samples → resolve to source
→ rank hotspots → extract signals → generate reports → add AI explanations
```

1. **Capability check** — probes `perf`, `addr2line`, available hardware events,
   and debug info before doing any work.
2. **Data collection** — uses `perf mem record` (precise, via PEBS/IBS/SPE) if
   available, falls back to `perf record -e cache-misses` (reduced precision).
3. **Sample parsing** — converts `perf script` text output into structured samples.
4. **DWARF resolution** — batches addresses through `addr2line -f -C` to get
   source file, line number, and demangled function name.
5. **Aggregation** — groups misses by source location, ranks by count.
6. **Signal extraction** — computes quantitative signals (LLC miss rate, hotspot
   concentration, loop nesting, pointer dereferences) for the LLM.
7. **Reporting** — terminal (Rich), JSON, and Markdown outputs.
8. **LLM explanation** — feeds signals + source snippets to an LLM for grounded
   cache-locality analysis and fix suggestions.

## Quick start

```bash
# Install dependencies
pip install -e ".[dev]"

# Build benchmarks
bash scripts/build_benchmarks.sh

# Run capability check
PYTHONPATH=src python -m cacheprof.cli check benchmarks/matrix_bad

# Profile a binary
PYTHONPATH=src python -m cacheprof.cli profile benchmarks/matrix_bad

# Enable LLM explanations
PYTHONPATH=src python -m cacheprof.cli --llm profile benchmarks/matrix_bad

# Run tests
pytest tests/ -v
```

## Requirements

- Linux with `perf` (linux-tools-common)
- `addr2line` (binutils)
- Python 3.10+
- For LLM explanations: `OPENAI_API_KEY` environment variable

## Compile flags for target binaries

All binaries must be compiled with these flags for accurate profiling:

```
g++ -O2 -g -fno-omit-frame-pointer -no-pie -std=c++17
```

- `-g` — DWARF debug info (required for addr2line)
- `-fno-omit-frame-pointer` — accurate stack unwinding for perf
- `-no-pie` — avoids PIE/ASLR address normalization complexity
- `-O2` — realistic optimization level

## Demo targets

| Benchmark | Pattern | Expected result |
|-----------|---------|-----------------|
| `matrix_good` | i-k-j loop order (row-major B access) | Low LLC misses |
| `matrix_bad` | i-j-k loop order (column-stride B access) | High LLC misses, hotspot on inner loop |
| `pointer_chase` | Shuffled linked-list traversal | Hotspot on dereference, poor spatial locality |

## Project structure

```
src/cacheprof/
├── cli.py              # Click CLI entry point
├── config.py           # Configuration defaults
├── models.py           # All data structs (cross-module)
├── runner.py           # Pipeline orchestrator
├── capabilities.py     # Pre-flight environment probes
├── perf/               # perf stat, mem, script wrappers + parser
├── dwarf/              # addr2line resolver
├── analysis/           # Aggregation, ranking, signal extraction
├── llm/                # Prompt building, API client, response parsing
├── report/             # Terminal, JSON, Markdown outputs
└── utils/              # Subprocess, logging, filesystem helpers
```
