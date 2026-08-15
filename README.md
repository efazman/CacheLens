# CacheLens

A CLI profiler that finds cache locality bottlenecks in compiled programs and maps them back
to source lines.

You hand it a binary. It tells you which line is losing time to cache misses:

```
#1  matrix_bad.cpp:23  in multiply_bad(double*, double*, double*, int)
    concentration 0.71   (18,204 miss / 25,600 access samples)

     22 |     for (int k = 0; k < N; ++k)
     23 |       sum += A[i*N + k] * B[k*N + j];
```

*Format illustration, not a measurement — see [Status](#status).*

Stock `perf` will tell you that address `0x401f3a` in `multiply_bad+0x12` took 3.2% of your
cache misses. That is a measurement, not a decision. Turning it into "change line 23" means
knowing DWARF, knowing your PMU's skid characteristics, and knowing cache behavior. CacheLens
closes that last mile.

## The design opinion

Most profilers rank cache hotspots by **raw miss count**. That mostly re-finds your hottest
loop — code that runs more executes more memory accesses and therefore absorbs more misses,
whether or not it uses the cache badly.

CacheLens ranks by **miss concentration**: misses divided by accesses *at that site*.

A line taking 5% of your misses on 0.1% of your accesses is a locality bug. A line taking 30%
of your misses on 30% of your accesses is just busy. Concentration finds the first kind, which
is the kind you can fix.

This costs something. It needs two sampled event streams rather than one, and a ratio of two
independently-sampled distributions is more sensitive to PMU skid than a peak-finder is. Both
are addressed in the architecture rather than ignored — see
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) §2.4 and §3.

## Status

**In development. No measured results yet.** Stated plainly because a profiler that shows
numbers it did not measure is worse than one that shows none.

| | |
|---|---|
| Design | Complete — [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) |
| C++ implementation | Not started |
| Python prototype (`src/cacheprof/`) | Stages 1–4 present, never run against real hardware |
| Measured benchmark results | None |

The prototype in `src/cacheprof/` orchestrates the `perf` CLI and is being replaced by a C++
implementation that calls `perf_event_open` directly. It is kept in-tree as a working reference
during the port, not as the shipping tool. It also ranks by raw miss count, which is the
behavior the design opinion above exists to reject.

## How it will work

One statically-linked C++ binary. No subprocesses, no `perf` CLI, no `addr2line`.

```
  cachelens ./matrix_bad
       │
  ┌────▼─────────────────────────────────────────────────┐
  │ 1. SAMPLE       perf_event_open ×2 → mmap ring ×2    │
  │                 fork → open counters → exec          │
  │                 enable_on_exec=1, drain at exit      │
  └────┬─────────────────────────────────────────────────┘
       │  Sample{ ip, tid, time, event_id }
  ┌────▼─────────────────────────────────────────────────┐
  │ 2. ATTRIBUTE    libdw: ip → (file, line, function)   │
  └────┬─────────────────────────────────────────────────┘
       │  Site{ loc, n_miss, n_access }
  ┌────▼─────────────────────────────────────────────────┐
  │ 3. RANK         misses/accesses, Wilson lower bound  │
  └────┬─────────────────────────────────────────────────┘
       │
  ┌────▼─────────────────────────────────────────────────┐
  │ 4. REPORT       ranked table + source + honesty block│
  └──────────────────────────────────────────────────────┘
```

**Sample** — `perf_event_open` directly rather than shelling to `perf record`. Two independent
events (LLC read misses, LLC read accesses), each with its own ring buffer, both on the child.
`enable_on_exec=1` arms the counters at the exact `execve` boundary, so none of the target is
missed and none of the profiler's own setup work pollutes the result.

**Attribute** — `libdw` line-table lookup. Using real DWARF rather than shelling to `addr2line`
also means `dwfl_addrmodule()` identifies which module an address belongs to, so samples from
libc and the kernel are bucketed out instead of being falsely attributed to your source.

**Rank** — concentration, scored by Wilson lower bound so a site with 3 miss samples out of 3
accesses doesn't outrank the real bottleneck on a point estimate of 1.0.

**Report** — ranked table, source context, and an explicit honesty block: samples lost to ring
overflow, samples that could not be resolved, and the `precise_ip` level the kernel actually
granted.

## Requirements

- Linux, x86-64, with a **hardware PMU exposed to userspace**
- `elfutils` development headers (`libdw`, `libelf`)
- Target binaries built with `-O2 -g -fno-omit-frame-pointer -no-pie -std=c++17`

Most cloud VMs do not expose a virtual PMU. On such a host every hardware event reads
`<not supported>` and no profiler can produce data. Verify before anything else:

```bash
perf stat -e cache-misses,cache-references /bin/true
```

Real integer counts mean you can proceed. `<not supported>` means you need bare metal.

Apple Silicon cannot host this project at all — no `perf`, and the ARM PMU is not accessible
from userspace.

## Benchmarks

`benchmarks/` holds the locality test cases. Sources only; binaries are built locally and are
not committed.

| Benchmark | Pattern |
|---|---|
| `matrix_good` | i-k-j loop order, row-major B access |
| `matrix_bad` | i-j-k loop order, column-stride B access |
| `pointer_chase` | Shuffled linked-list traversal, defeats the prefetcher |

```bash
make -C benchmarks
```

`N` in the matrix benchmarks must be sized so the working set exceeds the host's LLC, or the
good/bad separation collapses. Check `lscpu` before trusting a null result.

## What this tool does not do

- **No cross-architecture abstraction.** Linux `perf_event_open` on one architecture. It
  negotiates `precise_ip` downward on the host it's running on, but it does not abstract over
  PEBS/IBS/SPE.
- **No PIE support.** Targets build `-no-pie` so the sampled IP equals the link-time address.
  Handling PIE means tracking `PERF_RECORD_MMAP2` and computing file-relative offsets — real
  work, deliberately deferred.
- **No system-wide or multi-process profiling.** One binary, launched by the tool.
- **No inline frame expansion.** A sample inside an inlined function attributes to the inline
  site.
- **No automatic code rewriting.** It points at a line; a human decides.
- **No GUI, flame graphs, or visualization.** Terminal and JSON.

## Layout

```
docs/ARCHITECTURE.md    the design this repo is being built toward
docs/DESIGN.md          earlier design baseline, partly superseded
docs/OVERVIEW.md        earlier product plan, partly superseded
benchmarks/             locality test cases (sources only)
src/cacheprof/          Python prototype — reference during the port
tests/                  unit tests for the prototype
```

## Running the prototype

Not the shipping tool, and it has never been run against a real PMU. Kept honest here because
it is in the tree.

```bash
pip install -e ".[dev]"
python -m cacheprof.cli check ./benchmarks/matrix_bad
python -m cacheprof.cli profile ./benchmarks/matrix_bad
pytest tests/ -q
```
