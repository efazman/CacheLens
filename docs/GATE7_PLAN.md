# CacheLens — Gate 7 Plan: Concurrent Targets and a Contended-Queue Case Study

**Status:** plan only, nothing implemented — 2026-08-22
**Builds on:** Gate 6 (README rewrite). Assumes the Gate 5 concentration pipeline as shipped.
**Relationship to `ARCHITECTURE.md`:** that document describes the system as built for a
single-threaded target. §1 below identifies a kernel constraint that makes the multithreaded
case a different design, not a parameterization of the existing one. Where this plan and
`ARCHITECTURE.md` disagree about the sampler, this plan is the newer analysis — but nothing
here is implemented, so `ARCHITECTURE.md` still describes the actual code.

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
