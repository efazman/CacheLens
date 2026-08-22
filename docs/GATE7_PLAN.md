# CacheLens — Gate 7 Plan: Concurrent Targets and a Contended-Queue Case Study

**Status:** implemented, Phases 0-6 complete, Phase 7 correctly not triggered — 2026-08-22
**Builds on:** Gate 6 (README rewrite). Assumes the Gate 5 concentration pipeline as shipped.
**Relationship to `ARCHITECTURE.md`:** that document described the system as built for a
single-threaded target; `src/main.cpp` now implements the multithreaded per-CPU design this
plan describes (see §1, Decisions (Phase 0), and `docs/GATE7_IMPLEMENTATION.md`).
`ARCHITECTURE.md`'s attr table has been spot-corrected for the `inherit` field but was not
otherwise rewritten for Gate 7 — this plan and its Adjudication section are the authoritative
account of what the sampler actually does and what it found.

---

## 0. The pre-registered prediction

Recorded 2026-08-22, before any queue benchmark exists and before any measurement is taken.
This is the same discipline Gate 5 applied to `kMinAccessSamples = 30` — the number was fixed
before the run so it could not be fitted to the outcome.

> On a bounded lock-free queue whose head and tail indices share a cache line, profiled while
> a producer and a consumer thread contend on it:
>
> **(a)** concentration ranking places the index-update line at #1;
> **(b)** raw miss-count ranking does not place it at #1;
> **(c)** the same queue built with head and tail on separate cache lines shows neither
>     the wall-clock cost nor the concentration signal.
>
> Any of (a), (b), (c) failing is reported as a failed prediction, not quietly dropped.

Prediction (b) is the one most likely to fail benignly: a queue's hot loop has few source
lines, so raw count may happen to agree. If it does, the honest reading is "this workload does
not discriminate between the two rankings," not "concentration was wrong."

Prediction (a) is the one that depends on hardware the project has not yet verified — see U4.

---

## 1. The kernel constraint that shapes the design

The current sampler opens each event with `pid = child, cpu = -1` and one `mmap` ring per event
(`src/main.cpp:482`, `src/main.cpp:490`). The natural-looking extension to multithreaded targets
is `attr.inherit = 1`. That does not work. From `perf_event_open(2)`:

> Inherit does not work for some combinations of `read_format` values, such as
> `PERF_FORMAT_GROUP`. Additionally, using it together with `cpu == -1` **prevents the creation
> of the mmap ring-buffer** used for logging asynchronous events in sampled mode.

So `inherit = 1` and the ring buffer are mutually exclusive in the current configuration.
Sampling a multithreaded target requires **per-CPU events**: one `perf_event_open(attr, child,
cpu = i, ...)` per online CPU per event, `inherit = 1`, one ring each.

On the reference machine (Ryzen 5 7600X, 6 cores / 12 hardware threads) that is
**12 CPUs x 2 events = 24 file descriptors and 24 rings**, up from 2.

This is what `perf record` does for a workload target, and it is why perf carries an internal
`uses_mmap` flag that forces a real CPU map instead of the `cpu = -1` dummy map. The design is
being pushed toward what the reference implementation already does, which is a point in its
favour — but it is a genuine architectural change, not a flag.

**Today's undocumented gap:** `attr.inherit` is 0 (the attr is `memset` to zero and `inherit` is
never set), so CacheLens currently profiles only the initial thread of the target and silently
reports nothing about any other thread. This limitation is not in the README. It should be added
regardless of whether the rest of this plan is executed.

---

## 2. Scope

**In scope**

1. Per-CPU sampling so multithreaded targets are measured correctly.
2. A bounded lock-free queue benchmark pair (padded / unpadded) as a new profiling target.
3. The experiment in §0, reported against the prediction.
4. A tail-latency harness measuring the padded/unpadded p99 and p99.9 gap under contention.
5. Measurement of CacheLens's own drain path under the new configuration, and a concurrent
   queue *inside* CacheLens only if that measurement shows one is needed.

**Explicitly out of scope**

- System-wide profiling (`pid = -1`). Per-CPU events are introduced for a *task*, not for the
  machine.
- PIE targets. The `-no-pie` requirement is unchanged and unrelated.
- AMD IBS. Still deferred, for the reasons in the README's Future Work.
- Cross-architecture support.
- MPMC-for-its-own-sake inside the profiler. See U25.

---

## 3. Unknowns register

Each unknown states what it is, why it matters, and how it gets resolved. Nothing downstream of
an unknown should be built before that unknown is closed.

### 3.1 Blocking — resolved in Phase 0

**U1 — Do per-CPU task events open at `kernel.perf_event_paranoid = 1`?**
The kernel gates CPU-event permission on `task == NULL` in `find_get_context`, so a
`pid > 0, cpu >= 0` event *should* be permitted for a process the user owns. The man page's
paranoid language is written for the system-wide case and does not settle it.
*Resolution:* probe. *If it fails:* the README's setup instruction degrades from `paranoid=1`
to `paranoid=0`, which is a materially worse ask of a reader — at which point U5's Option B
becomes the better design.

**U2 — Does `enable_on_exec = 1` behave correctly on a `cpu >= 0` task event?**
The claim that "counters arm exactly at the `execve` boundary" is load-bearing for every number
the tool produces. It is verified for `cpu = -1` and unverified for `cpu >= 0`.
*Resolution:* probe; check `time_enabled` and confirm no pre-exec counts.
*If it fails:* arming needs a different strategy and the exec-boundary guarantee must be
restated.

**U3 — Does the mlock budget accommodate 24 rings?**
`perf_mmap()` grants `perf_event_mlock_kb` x `num_online_cpus` and charges any excess to
`RLIMIT_MEMLOCK`. Default is 516 KiB x 12 CPUs ~= **6.0 MiB**. The current ring is
`kDataPages = 1 << 8` = 1 MiB data plus one metadata page; 24 of those is **~24 MiB**.
*Resolution:* probe by actually mmapping N rings at several sizes and recording what succeeds.
*If it fails:* rings shrink, which is not a free choice — it feeds directly into U6 and U7.

**U4 — Do the generalized `cache-misses` / `cache-references` events on Zen 4 register false
sharing at all?**
This is the unknown that can kill the edition. A coherence ping-pong is typically satisfied from
another core's cache — an L3 hit or a cross-core HITM — not an LLC miss. Intel exposes this as
`mem_load_l3_hit_retired.xsnp_hitm`; the AMD equivalents live in IBS or raw
`ls_any_fills_from_sys.*` codes. The README already states that this project has not verified
what `PERF_COUNT_HW_CACHE_MISSES` maps to on this part.
*Resolution:* ~60 lines of throwaway two-thread ping-pong, padded vs unpadded, measured with
`perf stat -e cache-misses,cache-references`. No tool code required.
*If it fails:* the false-sharing headline does not exist. Fall back to U14.

### 3.2 Design forks — decided in Phase 0, recorded before any measurement

These are choices, not discoveries. They are listed here so they are fixed in writing *before*
data exists, for the same reason the support gate was.

**U5 — Per-CPU + inherit (Option A) vs per-TID handshake (Option B).**
*Option A:* `pid > 0, cpu = i` for every CPU, `inherit = 1`. General; works on any target;
matches `perf record`. Costs 24 rings and depends on U1, U2, U3.
*Option B:* have the benchmark spawn its threads and then `raise(SIGSTOP)`; CacheLens enumerates
`/proc/<pid>/task` and opens one `cpu = -1, inherit = 0` event per TID — the existing attr shape
unchanged, ~4 rings, no dependency on U1/U2/U3. Costs generality: it requires target
cooperation and misses threads created after the handshake, so it is not a general profiler.
*Default:* A. *Fallback:* B, if U1 or U3 fails. Whichever is chosen, the README states which and
why.

**U6 — Aggregate vs per-CPU sample-rate target. This is the load-bearing choice.**
A `PERF_RECORD_SAMPLE` under the current `sample_type` (`IP | TID | PERIOD`) is 32 bytes: 8-byte
header plus `ip`, `pid/tid`, `period`. A 1 MiB ring therefore holds 32,768 records.

| configuration | per-ring rate | records | headroom before overwrite |
|---|---|---|---|
| 47.4k/s aggregate, 12 x 1 MiB rings | ~3,950/s | 32,768 | **~8.3 s** |
| 47.4k/s per CPU, 12 x 128 KiB rings | 47,400/s | 4,096 | **~86 ms** |

Under the first configuration the drain path has enormous slack and no concurrent queue inside
CacheLens will ever be justified. Under the second, the deadline is real — and the existing
`poll()` timeout of 100 ms (`src/main.cpp:641`) is *already* over budget on its own.
Phases 5 and 6 have a subject only under the second. **That is not a reason to choose it.** The
choice is made on sampling-statistics grounds and recorded; whether it happens to create work
for Phase 6 is not an input.

**U7 — Ring size per CPU.** Coupled to U3 and U6; cannot be chosen independently of either.

**U8 — The calibration divisor under `inherit`.**
Phase A calibration (`calibrate_both`, `src/main.cpp:418`) may legally set `inherit = 1` because
counting mode has no mmap. It then measures the *summed* rate across all threads. But the sample
period applies per event per CPU. What is the correct divisor — online CPU count, target thread
count, or something measured? Getting this wrong silently mis-scales every concentration number,
because `period_scale` (`src/main.cpp:~722`) assumes the two periods are directly comparable.
*Resolution:* derive it explicitly and validate against a known-thread-count target in Phase 2.

### 3.3 Attribution correctness — Phase 3

**U9 — The skid characterization must be redone.**
The README states the ±2-line finding is workload-specific and says exactly why it held: "the
hot loop body is 7 instructions across exactly 2 source lines." A queue `push()` with a CAS, a
mask, and a fence is not that body. With `precise_ip = 0` and unbounded skid this is a real
threat to the result, and the project's own documentation requires the recheck rather than
permitting the inheritance of the old finding.

**U10 — Inlining will probably break the demo.**
At `-O1`, `push()` and `pop()` will very likely inline into the driver loop. CacheLens has no
inline-frame expansion, so samples attribute to the *call site*, not to the index-update line
inside `push()` — which is the line the entire prediction is about.
*Cheap mitigation:* `__attribute__((noinline))` on the queue operations, disclosed as a
benchmark-shaping decision. *Expensive mitigation:* implement inline-frame expansion.
Decide before writing the benchmark; it changes how the benchmark is written.

**U11 — Per-thread attribution.** `AttributionResult::line_counts` is keyed on `(file, line)`
and discards `tid`, though `Sample` already carries it (`src/main.cpp:~215`). Under false
sharing the producer and consumer suffer at *different* lines; a single merged ranking may make
the result less legible rather than more.

**U12 — Support gate under fragmentation.** `kMinAccessSamples = 30` was chosen against a
workload with four hot lines and ~550k samples in one ring. Split across 12 rings and multiple
threads, with more distinct source lines in play, more sites may fall below the gate. The gate
is not re-tuned to rescue the result; if it excludes the interesting site, that is reported.

**U13 — Per-ring halt-condition identity.** The overflow `die()` in `drain()`, the
throttle `die()`, the 0.99 multiplexing check, and the `PERF_SAMPLE_PERIOD` equality check all
currently name an event. With 24 independently-throttled rings they must name an (event, CPU)
pair or the failure message is useless. Throttling also becomes materially more likely.

### 3.4 Benchmark design — Phase 1

**U14 — Fallback locality bug if U4 says false sharing is invisible.**
Two candidates that produce ordinary capacity misses the generalized events certainly count:
(a) a slot array sized past L2, or past the 32 MiB L3, so producer and consumer walk a working
set that does not fit; (b) packed vs cache-line-padded *slots*, so each dequeue touches a fresh
line. Both are weaker stories than false sharing. Both keep the edition alive.

**U15 — `-O1` vs `-O2`.** The Makefile pins `-O1` for a specific documented reason (the
auto-vectorization confound in the matrix pair) that does not apply to a queue. But nobody
quotes `-O1` latency numbers. *Likely resolution:* two binaries — `-O1 -g -no-pie` for
attribution, `-O2` for the latency harness — labelled as different measurements and never merged
into one table, the same discipline the README applies to the `perf stat` baseline versus
CacheLens's own aggregate.

**U16 — Topology and pinning.** 6 cores / 12 threads, single CCD, shared 32 MiB L3. Producer and
consumer on SMT siblings of one core versus on two distinct cores gives entirely different
contention. This is a reported parameter of the result, not an implementation detail, and it is
pinned explicitly via `sched_setaffinity` rather than left to the scheduler.

**U17 — Determinism.** The benchmark must perform identical total work regardless of scheduling,
or run-to-run comparison is meaningless. A queue benchmark that spins until a deadline does not
satisfy this; one that moves a fixed number of items does.

**U18 — Does the designed false sharing actually occur?** Store buffers, write coalescing, and
register allocation can all hide it. Confirm with `objdump` and a direct wall-clock A/B before
trusting any profiler output about it — the same order of operations the matrix pair used, where
`objdump` confirmed scalar code before any measurement was trusted.

### 3.5 Latency harness — Phase 5

**U19 — Timer overhead versus the thing being timed.** `clock_gettime(CLOCK_MONOTONIC)` costs
roughly 20–25 ns via the vDSO; a queue operation may be 20–100 ns. The instrument is the same
order of magnitude as the measurement. Its cost must be measured, reported, and either
subtracted or designed around (`rdtscp` with a calibration step, or batched timing).

**U20 — Coordinated omission.** A closed-loop harness that times `push()` in a tight loop
reports a p99.9 that means very little: a stalled producer stops generating load exactly when
load matters most, so the worst delays never appear in the sample. A meaningful tail requires a
rate-controlled producer measuring against *intended* start time, not actual. This is the
difference between a real tail-latency harness and a decorative one.

**U21 — The harness perturbs its own subject.** Recording per-operation latencies means touching
a results array, which pollutes the cache under test. Preallocated array versus streaming
histogram buckets is a measurement-validity trade-off here, not a style preference.

**U22 — Sample count for a stable p99.9.** On the order of 10^6 operations minimum, plus
repetitions, so run-to-run variance is reported rather than a single number presented as exact.

**U23 — The governor null result does not transfer.** Gate 5's finding that governor choice does
not matter was about a memory-stalled workload. A latency measurement is frequency-sensitive in
a way that one is not. The governor question is re-asked for this workload, not inherited.

### 3.6 The conditional queue inside CacheLens — Phase 6

**U24 — Whether it is needed at all.** Gated entirely on U6 and the Phase 6 measurement. Under
the aggregate-rate configuration the answer is almost certainly no, and "no" is a result worth
recording, on the same footing as the governor null result.

**U25 — The shape is MPSC, not MPMC.** N drain threads feeding one aggregator is
multiple-producer, single-consumer. MPMC enters only if DWARF symbolization is moved onto
multiple worker threads pulling from a shared work queue. Building MPMC before that architecture
exists would be a decision with no measurement behind it, which is the one thing this project
does not do.

**U26 — `Dwfl` thread safety.** One `Dwfl` handle per symbolizer thread is required. Whether
`dwfl_report_offline` against the same file from N threads is safe and cheap is unverified.

**U27 — Self-interference.** A profiler that spins 12 drain threads on a 6-core machine competes
with the target for the L3 it is trying to measure. This project already documents that
background load biases results *upward*; a profiler that is itself background load is the same
problem from the inside. Profiling overhead as a percentage of target runtime is currently
unmeasured for *any* configuration, including the one that shipped.

---

## Decisions (Phase 0)

Recorded 2026-08-22, immediately after `results/gate7_probes.txt` was produced, before any
Phase 1 benchmark exists. Full unedited probe output: [`results/gate7_probes.txt`](../results/gate7_probes.txt).

**U1 (blocking) — per-CPU task event open at `paranoid=1`: PASS.** All 12 online CPUs opened
and mmapped a `pid=self, cpu=i` event without error. The `find_get_context`/`task != NULL`
reading of the man page held; the system-wide-focused paranoid language did not apply here.

**U2 (blocking) — `enable_on_exec` on `cpu>=0`: PASS.** Case A (cpu 0) and case C (cpu 11, the
last online CPU) both showed non-zero counts and `time_running` tracking only the post-exec
runtime. Case B (killed before exec) read exactly zero. One correction made mid-probe: the first
run of case C read `value=0, time_running=0` with `time_enabled` non-zero — not a failure of
`enable_on_exec`, but a probe bug: an unpinned child was never scheduled onto CPU 11 during its
~110ms lifetime, and a `cpu>=0` task event only counts while the task runs on that exact CPU.
Fixed by pinning the child via `sched_setaffinity` to the target CPU before `SIGSTOP`, matching
what the real per-CPU sampler will need to reason about regardless (a per-CPU event's counts
are meaningless unless something guarantees the task actually ran on that CPU). Re-run passed.

**U3 (blocking) — mlock budget: prediction corrected.** The naive formula
(`perf_event_mlock_kb x nproc` = 516 KiB x 12 = 6.05 MiB) undershot reality by roughly 2x. Actual
measured ceiling for 24 rings (2 events x 12 CPUs, U5 Option A's shape): all 24 succeed at
512 KiB/ring (12.09 MiB total); at 1 MiB/ring only 13/24 succeed before `mmap` returns `EPERM`
(13.05 MiB). The true allowance sits somewhere in (12.09, ~14) MiB — closer to
`perf_event_mlock_kb x nproc` **plus** `RLIMIT_MEMLOCK` (8 MiB) than to either alone, which is
consistent with the kernel charging the free per-user quota first and the rest against
`RLIMIT_MEMLOCK`, but this was not independently re-derived past what the probe needed to answer
U7. Decided: **512 KiB is the ring size** (see U7 below), not 1 MiB — the plan's illustrative
1 MiB number does not fit the real budget on this machine.

**U4 (blocking, gating) — false sharing visibility: PASS, decisively.** Outcome landed in row one
of the three possible outcomes: large wall-clock gap **and** large `cache-misses` gap.

| build | elapsed (5-run mean) | cache-misses | cache-references |
|---|---|---|---|
| shared (false-sharing) | 1.994s +- 7.82% | 108,102,665 | 110,399,471 |
| padded (separate lines) | 0.271s +- 0.25% | 78,446 | 573,662 |

A 7.3x wall-clock gap and a ~1,380x `cache-misses` gap. `objdump -C` on both builds confirms the
cause before any of this is trusted: in the shared build, `worker_a`'s `lock addq` targets offset
`0x0` of the struct and `worker_b`'s targets offset `0x8` — 8 bytes apart, inside one 64-byte
line; in the padded build the same two instructions target `0x0` and `0x40` — exactly one cache
line apart, matching the `alignas(64)` fields. **The third outcome (no wall-clock gap) is
excluded.** The generalized `cache-misses` / `cache-references` events on this Zen 4 part do see
coherence/HITM-style traffic from two-thread false sharing; the false-sharing headline for Gate 7
is available and U14's fallback is not needed.

**U5 — sampler shape: Option A (per-CPU + inherit).** U1 and U2 both passed, so Option A's
dependencies are satisfied and its generality (works on any target, matches `perf record`, no
target cooperation required) is worth its cost. Option B is not needed.

**U6 — sample-rate target: aggregate, not per-CPU.** Decided on sampling-statistics grounds, not
on what it does to Phase 6's deadline (which the plan explicitly rules out as an input; the
extra headroom this creates is a side effect, not the reason). Calibration already measures the
*aggregate* event rate across all target threads (`calibrate_both`, `inherit=1` counting mode).
The period assigned to each per-CPU ring should reproduce that same aggregate target rate summed
across CPUs — i.e. `period_per_cpu` is derived from `aggregate_rate / nr_cpus_used_by_target`
against the 60%-of-max target — because that is what preserves the existing calibration
methodology's meaning ("sample at 60% of the kernel's cap relative to the target's actual event
rate"). Independently maxing every CPU's own per-context rate regardless of that CPU's actual
share of the load has no statistical justification: it would 12x the interrupt volume without
increasing the information content of the sample, since most of those samples would come from
CPUs the target barely touches. See U8 for the exact divisor.

**U7 — ring size: 512 KiB per ring**, per U3's measured (not predicted) ceiling. On this machine
`perf_event_max_sample_rate=100000/sec` (not the 79,000-100,000 range seen in earlier gates' runs
at different governor/frequency states — this value is read fresh per run, as the existing code
already does). At 60%: aggregate target = 60,000/sec; under U6's aggregate scheme, per-ring
target = 60,000 / 12 = 5,000/sec. A 512 KiB ring holds 524,288 / 32 = 16,384 records (32 bytes/
record under `IP | TID | PERIOD`). **Headroom = 16,384 / 5,000 ~= 3.28s** per ring before
overwrite — ample slack, on the same order as the plan's illustrative aggregate-config estimate,
confirming Phase 6/7 are very unlikely to find a real deadline miss under this configuration.

**U15 — optimization level: two binaries.** `-O1 -g -no-pie` for attribution (Phase 1/3/4),
`-O2 -g -no-pie` for the latency harness (Phase 5). Never merged into one table, matching the
discipline the README already applies to `perf stat` vs CacheLens's own aggregate.

**U16 — thread pinning: distinct physical cores, CPU 0 and CPU 1.** Confirmed via
`/sys/devices/system/cpu/cpu*/topology/core_id`: CPU 0 and CPU 1 are core_id 0 and 1 respectively
(distinct physical cores); CPU 0 and CPU 6 would be SMT siblings of the same core (both
core_id 0) and were deliberately avoided. P4 used this pinning and produced the clean result
above, which is itself evidence the choice was right for eliciting the effect.

**U10 — inlining mitigation: `__attribute__((noinline))`** on the queue's `push`/`pop`, disclosed
in the benchmark source as a benchmark-shaping decision (forces attribution to land inside the
operation instead of at the call site), not a performance claim. Inline-frame expansion remains
out of scope for this gate.

---

## Adjudication (Phase 4)

Recorded 2026-08-22, against `results/gate7_false_sharing.txt`'s full pooled data (n=5 runs per
build). Adjudicated item by item, per the discipline stated in §0: any failure is reported as a
failure, not quietly dropped or reframed as success.

**(a) "concentration ranking places the index-update line at #1": FAILS as literally stated.**
Neither `store_tail`'s nor `store_head`'s own store instruction ranks #1 by pooled Wilson
concentration; the top slot goes to a spin-wait check line instead (`pop()`'s `while` condition).
But the single largest *relative* response to padding of any line in the function — **+673.5%**
between the padded and unpadded builds — lands on the one instruction immediately after
`store_head`'s real store, on a large, reliable sample (21,440 pooled access samples), and that
line also ranks in the top 3 by absolute concentration. This is one instruction of skid, not
absence of an effect: Gate 4 measured 99.99% skid containment within ±2 source lines on a
7-instruction, 2-line hot loop that left skid almost nowhere else to land. This queue's
`push()`/`pop()` span ten-plus lines each; skid had room to move, and the data shows it moved by
exactly one instruction on the cleanest available signal.

**(b) "raw miss-count ranking does not place the index-update line at #1": true but not
meaningfully confirmatory.** Raw count's #1 is a *third* line (the busiest spin-check by call
frequency, 91,732 pooled raw misses) — distinct from both the literal index-update lines and
from concentration's own #1. Concentration and raw count do disagree with each other, reproducing
this project's core methodological point on a second, structurally different workload — but
neither ranking isolates the specific instruction this experiment targeted as cleanly as Gate 5's
matrix result did the first time.

**(c) "the padded build shows neither the wall-clock cost nor the concentration signal":
CONFIRMED for wall-clock** (2.75x–3.62x faster, every measurement). **Partially confirmed for
concentration:** 14 of 16 reliably-sampled lines show a real, positive increase under contention
(median +58%) — consistent with false sharing creating broad memory-system pressure, not a
perfectly localized signal — but the single cleanest, largest differential (the +673.5% line
above) sits exactly where the mechanism predicts: one instruction from the literal write. Two
lines showed a flat or negative relative change; both have small pooled sample counts (217–9,628
access) and are read as noise the support gate exists to guard against, not as counter-evidence.

**What this changes about how the result can be claimed:** the mechanism prediction (a real,
hardware-visible, padding-sensitive effect that concentration and raw count see differently)
held. The precision prediction (concentration's #1 lands exactly on the write instruction) did
not, for a reason this project's own prior skid work anticipated could happen on a less
compressed hot-loop body. Both are reported, not just the one that succeeded.

---

## 4. Implementation plan

**The executable plan lives in [`GATE7_IMPLEMENTATION.md`](GATE7_IMPLEMENTATION.md)** — files
touched, probe specifications, per-phase checklists, and the decision record template. This
section is the overview only; where the two disagree, the implementation document is the newer
one.

Eight phases, each with a written exit criterion that gates the next. Phases 0 and 1 exist
specifically to close unknowns before code depends on them. Note the ordering: **the benchmark
comes before the sampler**, because the unknown most likely to invalidate the whole edition (U4)
is testable with `perf stat` and no tool code at all.

| Phase | Does | Closes / decides | Exit criterion |
|---|---|---|---|
| **0 — Probes** | Four throwaway programs, no tool code | U1–U4; decides U5, U6, U7, U15, U16 | `results/gate7_probes.txt` answers U1–U4 with numbers; design forks recorded in writing before any data exists |
| **1 — Benchmarks** | SPSC then MPMC, padded/unpadded from one source | U10, U14, U17, U18 | Wall-clock A/B delta measured; `objdump` confirms the cause |
| **2 — Per-CPU sampler** | The real change to `src/main.cpp` | U8, U13 | **Gate 5's matrix result reproduces through the new path** |
| **3 — Attribution** | Skid recharacterization, per-TID decision | U9, U11, U12 | A skid number for the queue workload, published either way |
| **4 — The experiment** | Run it, adjudicate §0 | — | Prediction judged item by item in writing |
| **5 — Latency harness** | Open-loop, rate-controlled | U19–U23 | p99/p99.9 with timer overhead and coordinated-omission handling stated |
| **6 — Measure the drain** | Instrument CacheLens itself | U24, U27 | "Does the drain keep up, and what does profiling cost the target" — with numbers |
| **7 — Conditional MPSC** | Only if Phase 6 says so | U25, U26 | `lost_records = 0` restored, U27 re-measured |

Three properties of this sequence are deliberate:

**Phase 0 before Phase 1 before Phase 2.** U4 can end the edition and costs ~60 lines and a
`perf stat` invocation to test. It is tested before a sampler exists to be invalidated.

**Phase 2's exit criterion is a regression guard, not a feature check.** Gate 5's headline is the
strongest claim in this repo. A sampler rewrite that quietly perturbs it trades something sound
for something speculative.

**Phase 7 is allowed not to happen.** The concurrent queue gets written either way, in Phase 1,
as the benchmark. Whether CacheLens itself ever needs one is a question Phase 6's measurement
answers, and "it does not" is a result on the same footing as the governor null result.

---

## 5. What kills this, and what does not

**U4 is the only unknown that can end the edition**, and it is tested first, in Phase 0, with
60 lines of throwaway code. If the generalized cache events on Zen 4 are blind to coherence
traffic, the false-sharing headline is unavailable and U14's substitute is selected — a weaker
story, but still a genuine multithreaded case study on a workload that looks like production
code rather than a textbook loop.

**Phase 7 not happening is not a failure.** The concurrent queue gets written either way, in
Phase 1, as the *benchmark*. Whether CacheLens itself ever needs one is a question the
measurement answers, and "it doesn't" is the same class of result as the governor null result:
worth reporting precisely because it was tested rather than assumed.

**The Phase 2 regression guard is the real risk to the existing work.** The Gate 5 headline is
this project's strongest claim. A sampler rewrite that quietly perturbs it would damage
something already sound in pursuit of something speculative. That is why it is an exit criterion
rather than a spot check.
