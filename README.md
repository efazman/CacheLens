# CacheLens

A CLI profiler that samples hardware cache-miss and cache-reference events on Linux
(`perf_event_open`, no `perf` CLI, no subprocess), attributes them to source lines via `libdw`,
and ranks sites by **miss concentration** (misses ÷ accesses at that site) instead of raw miss
count. Built and measured on one machine so far — see [Status and scope](#status-and-scope)
before trusting any number here on a different one.

## The headline result

On `matrix_bad` (the naive i-j-k matrix multiply), raw miss count and concentration ranking
**disagree about which line is the bottleneck**:

| | raw miss count | rank by raw count | concentration (Wilson lower bound, 95%) | rank by concentration |
|---|---|---|---|---|
| `matrix_bad.cpp:43` (loop control — `for (k...)`) | 314,923 | **#1** | 0.057 | #4 |
| `matrix_bad.cpp:44` (`sum += ... * mat(B, k, j)`) | 225,682 | #2 | **0.352** | **#1** |

Raw count picks the loop-control line — it executes every iteration, so it absorbs more
skidded samples than any other line, the same way a hot loop absorbs raw profiler samples
regardless of what it's actually doing. Concentration correctly picks line 44, the line that
actually dereferences `B[k][j]` with a column stride — a ~6x gap in Wilson lower bound, stable
across every trial run. The same reversal holds on `matrix_good`: raw count favors `:42` (the
`for (j...)` header, 165,822 vs 124,951), concentration correctly favors `:43` (the hot
accumulate line, 0.063 vs 0.008). Full output: [`results/gate5_concentration.txt`](results/gate5_concentration.txt).

This is the tool's one design opinion, working, on real samples, against a benchmark whose
correct answer was known in advance because the benchmark's author wrote the bug into the
source.

## What this tool measures, and what it does not

**Measures:** `PERF_TYPE_HARDWARE` cache-misses and cache-references, sampled via
`perf_event_open` at a calibrated period, on one traced child process, attributed to
`file:line` via DWARF, ranked by a Wilson-lower-bound concentration score.

**Does not measure:**
- **Kernel-space activity.** Every event is opened with `exclude_kernel=1`. A profiled program's
  syscalls, page faults, and interrupt handling are invisible to it by design.
- **True per-access ground truth.** Concentration is a ratio of two independently sampled event
  streams compared in aggregate at a site — not a claim that any specific miss and access were
  the same memory operation. See [Skid](#skid-characterization-workload-specific).
- **Exact, unskidded instruction pointers.** This CPU has no PEBS-equivalent for these events
  (see below); every sampled IP carries unknown, unbounded skid.
- **PIE binaries, multi-threaded attribution beyond the traced pid, or shared-library code.**
  See [What this tool does not do](#what-this-tool-does-not-do).
- **Confirmed LLC-only cache activity.** See the AMD cache-event caveat below.

## The pipeline

```
   cachelens -- ./matrix_bad
        │
   ┌────▼──────────────────────────────────────────────────────────────┐
   │ 0. CALIBRATE   both events, counting mode, one throwaway child run │
   │                measures real event rates -> derives per-event     │
   │                sample_period (60% of perf_event_max_sample_rate,  │
   │                nearest prime) -- no hardcoded constants           │
   ├────▼──────────────────────────────────────────────────────────────┤
   │ 1. SAMPLE      perf_event_open x2 (independent, not grouped)      │
   │                fork -> personality(ADDR_NO_RANDOMIZE) -> SIGSTOP  │
   │                -> parent opens both counters -> SIGCONT -> exec   │
   │                enable_on_exec=1; poll() + drain both ring buffers │
   │                while the child runs, single-threaded              │
   ├────▼──────────────────────────────────────────────────────────────┤
   │ 2. ATTRIBUTE   libdw, offline against the target ELF on disk      │
   │                (no live-process attach, no PERF_RECORD_MMAP2 --   │
   │                valid because the target is -no-pie: link-time     │
   │                vaddr == runtime address)                          │
   ├────▼──────────────────────────────────────────────────────────────┤
   │ 3. RANK        concentration = (miss_samples * miss_period) /     │
   │                (access_samples * access_period); Wilson lower     │
   │                bound at 95%; support gate at 30 access samples    │
   ├────▼──────────────────────────────────────────────────────────────┤
   │ 4. REPORT      per-event bucket/histogram breakdown, ranked table,│
   │                insufficient-samples sites listed separately, raw- │
   │                count ranking printed alongside for comparison     │
   └──────────────────────────────────────────────────────────────────┘
```

**Sample.** `perf_event_open` directly, two independent events (not a `perf_event_open` group)
on the same traced child, each with its own 1 MiB mmap ring buffer. `enable_on_exec=1` arms both
counters at the exact `execve` boundary. Every ring-buffer record type is handled explicitly
(`PERF_RECORD_SAMPLE`, `LOST`, `EXIT`, `THROTTLE`/`UNTHROTTLE` — the latter two are a hard halt,
not a warning, because throttling silently invalidates the sample-count-to-aggregate
relationship everything downstream depends on). `data_head`/`data_tail` use explicit
acquire/release atomics, paired with the kernel producer. Ring overflow asserts rather than
attempting recovery.

**Attribute.** `libdw`'s `Dwfl` in offline-report mode — reads the target ELF directly from
disk, no `addr2line` subprocess, no live-process attach. Every sample is classified into exactly
one of four buckets (target executable / other user mapping / kernel space / unclassifiable,
using x86-64's canonical address-space split) before attribution is attempted; only the target
bucket feeds ranking, and all four bucket counts are always printed, never silently dropped.

**Rank.** Concentration, scored by the Wilson lower bound so a low-sample site doesn't outrank
a well-supported one on a noisy point estimate. See [below](#why-concentration-not-raw-count)
for how this specifically answers "doesn't ranking by a ratio just surface every low-traffic
line?"

**Report.** Ranked table with both the point estimate and the Wilson lower bound, insufficient-
sample sites listed separately (never dropped), and the raw-miss-count ranking printed alongside
so a reader who disagrees with the design opinion can see the alternative directly.

## Why concentration, not raw count

Most profilers rank cache hotspots by raw miss count, which mostly re-finds the hottest loop —
code that executes more, and therefore absorbs more skidded samples, regardless of whether it
uses the cache badly. The [headline result](#the-headline-result) above is exactly this failure
mode caught in the act: `matrix_bad`'s loop-control line outranks its actual bad access by raw
count.

The obvious objection to ranking by a ratio: doesn't `misses/accesses` just surface every
low-traffic line with a lucky 1/1 or 3/3 sample? Yes, on the point estimate. That's why ranking
uses the **Wilson score lower bound** (95%, z=1.96) instead: it asks "what's the lowest
concentration consistent with the evidence at this site," so a site needs both a high ratio
*and* enough samples to rank highly. A 3/3 site scores far below its 1.0 point estimate; a
3,000/4,000 site barely moves. Sites below 30 access samples (a standard rule-of-thumb minimum
for the normal approximation the Wilson interval relies on, and `docs/ARCHITECTURE.md`'s own
stated starting point) are excluded from ranking entirely and reported separately as
insufficient — not silently dropped, not included with a misleadingly confident number.

## Skid characterization — workload-specific, not a general guarantee

With no PEBS-equivalent on this CPU (see [below](#precise_ip-this-cpu-has-no-pebs-equivalent)),
every sampled IP is skidded by an unknown amount. Whether that skid still lands close enough to
matter was checked directly, not assumed: `B[k][j]`'s line was identified from `matrix_bad.cpp`
source independently of the profiler (line 44), then the actual sample distribution around it
was measured (n=5, default calibrated period, 119,196 target-attributed samples pooled):

| | fraction of samples |
|---|---|
| exact line (44) | 45.51% |
| within ±2 lines (42–46) | **99.99%** |
| outside the loop body entirely (not line 43 or 44) | 0.012% |

`objdump -dS` shows why: the entire loop body is **7 instructions spanning 29 bytes**
(`0x4012e0`–`0x4012fd`), split across exactly two source lines (43 and 44). Per the
interpretation fixed *before* this measurement was taken (>60% within ±2 lines → skid is
absorbed by source-line granularity, proceed; roughly uniform smear → halt), this passes
cleanly.

**This finding is workload-specific and should not be read as "skid doesn't matter here."** It
is absorbed *because* the loop body is tiny and touches only two source lines. A larger loop
body, a function with real branching, or heavy inlining collapsing several source lines'-worth
of logic into one hot instruction sequence would give unbounded skid far more room to land on
the *wrong* line before source-line granularity could absorb it. Every number in this README
comes from a 7-instruction loop; do not extrapolate the skid conclusion to a different shape of
hot code without re-running this same characterization.

## DWARF validation: 22/22, the complete population

`addr2line -f -i` agreement on every *distinct* target-executable address this workload
produces under cache-miss sampling: **22 out of 22 (100%)**, pooled across 25 runs (both
benchmarks, three sample periods). This is not a 22-address subsample falling short of some
larger target — with a ~150-byte hot code region and skid staying tightly localized, the
distinct-address count saturated at 22 after pooling and did not grow with more runs. 22 is the
complete population of addresses this specific workload can produce this way, and all 22 agree
with `addr2line`. This validates the DWARF *lookup* — that a resolved address maps to the
correct line. It does not validate that the resolved address is the *right* address; that's a
separate claim, and it's what the skid characterization above is for.

## Case study: matrix_bad vs matrix_good

Two ground-truth numbers, kept explicitly separate because they were measured by different
tools for different purposes:

**Stock `perf`, n=5, quiet machine, `performance` governor** (the resume-claim baseline,
independent of anything CacheLens computes):

| | wall time | LLC miss rate | speedup / ratio |
|---|---|---|---|
| `matrix_bad` (i-j-k) | 11.462s | 8.75–8.94%* | **2.08x** wall-clock |
| `matrix_good` (i-k-j) | 5.500s | 1.22–1.24%* | **~7.1–7.3x** miss-rate ratio |

*Original Phase 1 measurement: 8.99% / 1.23%, 7.31x, 2.095x speedup — see
[`results/phase1_matrix.txt`](results/phase1_matrix.txt). The 2.08x/2.084x figure above is a
later, independent re-measurement (quiet machine, `performance` governor — see
[`results/drift_investigation.txt`](results/drift_investigation.txt)) that came in within
0.46%/0.97% of the original, i.e. ordinary run-to-run variance. **Phase 1's file is the
resume-claim baseline and was not altered** by this later, consistent re-measurement.

**CacheLens's own aggregate (calibration phase, whole-program, not sampled)** —
[`results/gate5_concentration.txt`](results/gate5_concentration.txt):

| | miss rate |
|---|---|
| `matrix_bad` | 8.90% |
| `matrix_good` | 1.14% |

Directionally consistent with Phase 1 (8.90% vs 8.99%, 1.14% vs 1.23%) but **not tuned to
match, and not expected to match exactly** — different run, different moment, ordinary
variance. Per-site concentration values (35.2% at `matrix_bad`'s hottest line) are *not*
expected to match either of these whole-program averages, and don't: the hottest single site
sits far above the whole-program average because the average blends in every non-memory
instruction in the program, which the hot site by definition isn't diluted by.

### The governor null result

`performance` vs `powersave` governor, same quiet machine, n=5 each: wall time differed by
**+0.071% (matrix_bad) and +0.113% (matrix_good)** — indistinguishable from noise. This is a
positive finding, not an absent one: if the speedup had a compute-bound component (more
arithmetic throughput, better pipelining), clock frequency would matter and this test would
show a real gap. It doesn't, which means core frequency isn't the constraint — independent
corroboration, via a completely different method, of the IPC evidence already on record
(cycles/instruction 0.632 vs 0.268, i.e. **IPC ≈ 1.57 vs ≈ 3.73**): `matrix_bad` is slow because
it spends its cycles stalled on memory, not because the CPU is under-clocked. Two independent
methods agreeing is stronger evidence than either alone.

### Background load biases the result upward

The noisy-machine measurement (Firefox + VSCode running, governor unrecorded) reported **2.20x**
wall-clock speedup — *larger* than the quiet measurement's 2.08x, not smaller. This is not
symmetric jitter: L3 contention from other processes degrades `matrix_bad` (which already
thrashes the cache) more than `matrix_good` (which mostly doesn't need much L3), so background
load widens the gap between them rather than adding noise to both equally. **Reproducing this
benchmark on a machine you haven't quiesced will tend to report a *better* number than the true
one, not a worse one** — a one-directional bias, and the kind that survives unnoticed because
nobody double-checks a result that already looks good. See
[`docs/TAKEAWAYS.md`](docs/TAKEAWAYS.md) for the full investigation.

## Caveats, stated plainly

- **Benchmarks are built `-O1`, not `-O2`.** At `-O2`, GCC auto-vectorizes `matrix_bad`'s inner
  loop but not `matrix_good`'s — an asymmetric confound that changes the access pattern the
  source implies. `-O1` was verified scalar for both (`objdump`, zero `mulpd`/`movupd`) before
  any measurement was trusted. See `results/phase1_matrix.txt` §3.
- **`PERF_COUNT_HW_CACHE_MISSES`/`REFERENCES` are the kernel's *generalized* hardware events,
  not a verified LLC-only counter on this AMD chip.** These are a portable abstraction the
  kernel maps to a vendor- and microarchitecture-specific PMU counter; the mapping is documented
  as LLC-specific on some Intel parts, but this project has not independently confirmed via raw
  PMU event codes that AMD Zen 4's mapping counts L3 activity exclusively as opposed to broader
  cache-hierarchy activity. Every miss-rate number in this repo should be read as "the kernel's
  generalized cache-miss counter," not a confirmed-L3-only count.
- **Transparent Huge Pages: `madvise`** (`cat /sys/kernel/mm/transparent_hugepage/enabled`) —
  off for anonymous mappings unless explicitly requested, which none of the benchmarks do. Not
  a confound here; would need rechecking on a host where this defaults to `always`.
- **`-no-pie`.** Targets build non-PIE so the sampled IP equals the link-time ELF address, which
  is what makes the offline, no-live-process DWARF attribution above valid. A PIE target would
  need `PERF_RECORD_MMAP2` tracking and a file-relative offset computation instead — real work,
  deliberately deferred.
- **`precise_ip`: this CPU has no PEBS-equivalent.** `precise_ip=2` and `1` both fail `ENOENT`
  on `PERF_TYPE_HARDWARE`/`PERF_COUNT_HW_CACHE_MISSES` on this Zen 4 part; only `precise_ip=0`
  (arbitrary skid) is accepted. Two distinct consequences, both measured rather than assumed:
  source-line skid (see [above](#skid-characterization-workload-specific), workload-specific,
  currently absorbed) and occasional kernel-half sampled IPs despite `exclude_kernel=1` (0.01–
  0.13% observed rate, clustering to a handful of addresses shared across both benchmarks —
  `exclude_kernel` gates which *events* count, not where the PMI *lands*). AMD IBS
  (Instruction-Based Sampling) is the AMD-side path to real instruction-level precision and is
  deliberately deferred — see `docs/TAKEAWAYS.md`.
- **Single machine, single configuration, single moment in time.** AMD Ryzen 5 7600X (Zen 4),
  32 MiB L3, 16 GB DDR5 single-channel, Ubuntu, kernel `7.0.0-29-generic`. Every number in this
  README is specific to this exact hardware and kernel and has not been reproduced elsewhere.

## Requirements

- Linux, x86-64, with a **hardware PMU exposed to userspace** — most cloud VMs do not expose
  one; verify with `perf stat -e cache-misses,cache-references /bin/true` before anything else.
  Real integer counts mean you can proceed; `<not supported>` means you need bare metal.
- `kernel.perf_event_paranoid <= 1` for per-process (not system-wide) profiling without root.
- `elfutils` development headers (`libdw`, `libelf`) — `libdw-dev` on Debian/Ubuntu.
- `cmake`, a C++17 compiler.

Apple Silicon cannot host this project at all — no `perf`, and the ARM PMU is not accessible
from userspace.

## Reproducing the measurements

**Environment capture is not optional.** A performance number without its environment recorded
alongside it is not reproducible — not even by the person who measured it. This was learned the
hard way; see the drift-investigation entry in `docs/TAKEAWAYS.md`. Use
[`scripts/measure_baseline.sh`](scripts/measure_baseline.sh) for every `perf stat` ground-truth
measurement — it captures governor, load average, processes above 1% CPU, THP,
`perf_event_paranoid`, kernel version, compiler version, build flags, and core frequency
alongside the numbers. Do not run `perf stat` directly and call the result reproducible.

```bash
# 1. Confirm the PMU works
perf stat -e cache-misses,cache-references,instructions,cycles /bin/true

# 2. kernel.perf_event_paranoid <= 1
sudo sysctl -w kernel.perf_event_paranoid=1

# 3. Build the benchmarks (-O1, -g, -no-pie -- see Caveats)
make -C benchmarks

# 4. Ground truth: stock perf, with environment capture
scripts/measure_baseline.sh results/my_run.txt "my-conditions-label"

# 5. Build cachelens
cmake -S . -B build && cmake --build build

# 6. CacheLens's own measurement (automatic per-event period calibration)
./build/cachelens -- ./benchmarks/matrix_bad
./build/cachelens -- ./benchmarks/matrix_good

# 7. To reproduce one specific prior run exactly: bypass calibration
#    (every automatic run prints its calibrated periods; read them back
#    out and pass with --period, applied to both events)
./build/cachelens --period 50000 -- ./benchmarks/matrix_bad
```

`N` in the matrix benchmarks must be sized so the working set exceeds the host's LLC, or the
good/bad separation collapses — check `lscpu` before trusting a null result on different
hardware.

## What this tool does not do

- **No cross-architecture abstraction.** Linux `perf_event_open` on x86-64. It negotiates
  `precise_ip` downward on the host it runs on, but does not abstract over PEBS/IBS/SPE — AMD
  IBS specifically is a real, deliberately deferred alternative (see Caveats).
- **No PIE support.** See Caveats.
- **No system-wide or multi-process profiling.** One binary, launched by the tool, `inherit=0`.
- **No inline frame expansion.** A sample inside an inlined function attributes to the inline
  site (which is where `multiply_bad`/`multiply_good` end up, both inlined into `main` at
  `-O1`).
- **No automatic code rewriting.** It points at a line; a human decides.
- **No GUI, flame graphs, or visualization.** Terminal output only.

## Status and scope

**Built and measured, on one machine, so far.** Gates 1–5 (counting-mode validation, sampling +
ring buffer, correctness hardening, DWARF attribution, second event + concentration ranking)
are complete, each independently validated against an external reference (`perf stat`,
`perf record`/`perf script`, `addr2line`) and committed with its evidence in `results/`. The
central design claim — concentration ranking beats raw count — has been checked against real
samples on both benchmarks and holds.

Not yet done: a second machine, a benchmark whose hot code isn't a 7-instruction loop (to test
whether the skid finding generalizes), and AMD IBS for real instruction-level precision.

## Layout

```
docs/ARCHITECTURE.md         the design this repo was built toward
docs/TAKEAWAYS.md            debugging log: bugs found, root causes, and the rules they produced
docs/DESIGN.md, OVERVIEW.md  earlier design baselines, superseded by ARCHITECTURE.md
benchmarks/                  matrix_bad / matrix_good / pointer_chase (sources only)
scripts/measure_baseline.sh  required wrapper for any perf stat ground-truth measurement
results/                     committed evidence: phase1_matrix.txt, drift_investigation.txt,
                              gate5_concentration.txt
src/main.cpp                 cachelens itself
src/cacheprof/                Python prototype -- reference during the port, not the shipping
                              tool, never run against real hardware, ranks by raw count (the
                              behavior this project's design opinion exists to reject)
```
