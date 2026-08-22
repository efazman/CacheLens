# CacheLens — Gate 7 Implementation Plan

**Status:** not started — 2026-08-22
**Companion document:** [`GATE7_PLAN.md`](GATE7_PLAN.md) — the scope, the pre-registered
prediction, and the unknowns register (U1–U27). That document says *why* and *what is unknown*.
This one says *how*, in the order it gets done.
**Rule:** every U-number referenced here is defined in `GATE7_PLAN.md` §3. Nothing downstream of
an unknown gets built before that unknown is closed.

---

## 0. How this is sequenced

Three ordering rules, in priority order:

1. **Cheapest instrument first.** An unknown that can be closed with `perf stat` and no tool code
   is closed before any tool code exists. This is why Phase 0 comes before everything and why
   the benchmark (Phase 1) comes before the sampler (Phase 2).
2. **Nothing that can invalidate later work runs late.** U4 can end the edition. It is the first
   thing measured.
3. **Choices are recorded before the data that could bias them.** The design forks (U5, U6, U7,
   U15, U16) are written down at the end of Phase 0, before Phase 1 produces a single number.

### Dependency graph

```
Phase 0  probes ────────────────┬──► Phase 1  benchmarks ──┐
         (U1 U2 U3 U4)          │            (U10 U14 U17 U18)
         decides U5 U6 U7       │                          │
                 U15 U16        │                          │
                                └──► Phase 2  sampler ◄─────┘
                                             (U8 U13)
                                                 │
                                     Phase 3  attribution
                                             (U9 U11 U12)
                                                 │
                                     Phase 4  the experiment
                                                 │
                                     Phase 5  latency harness
                                             (U19–U23)
                                                 │
                                     Phase 6  measure the drain
                                             (U24 U27)
                                                 │
                                     Phase 7  conditional MPSC
                                             (U25 U26)   ← may never run
```

Phase 2 needs Phase 0's decisions and Phase 1's binary (something multithreaded to point at).
Phases 1 and 2 can otherwise proceed in parallel if convenient; the graph only requires that
both precede Phase 3.

### Branch and commit convention

The repo's existing convention is one commit per gate, direct to `master`
(`Gate 5: second event, concentration ranking`). Gate 7 is larger than previous gates and has
phases that can independently fail, so: **branch `gate7`, one commit per phase**, message
prefixed `Gate 7 Phase N: …`, squashed or kept per taste at merge. Each phase commit includes
its results file, so a failed phase leaves evidence rather than a revert.

---

## Phase 0 — Probes

**Goal:** close U1, U2, U3, U4. Decide U5, U6, U7, U15, U16.
**Machine:** the reference box. Every probe needs the real PMU; none of this runs on a VM.
**Estimated size:** ~250 lines total across four throwaway programs.

### Files

```
probes/                        NEW — kept in tree as evidence, not deleted
  p1_percpu_open.c             U1: does per-CPU task event open at paranoid=1
  p2_enable_on_exec.c          U2: does enable_on_exec arm correctly on cpu >= 0
  p3_mlock_budget.c            U3: how many rings of what size can be mmapped
  p4_false_sharing.cpp         U4: does the PMU see coherence traffic at all
  README.md                    what each probe answers and how to read its output
scripts/run_gate7_probes.sh    NEW — runs all four, appends environment block
results/gate7_probes.txt       NEW — the output
```

### P1 — per-CPU task event at `paranoid = 1` (U1)

Open `PERF_COUNT_HW_CACHE_MISSES` with `pid = getpid(), cpu = 0`, then `mmap` one ring.
Report `errno` by name on failure. Repeat for every online CPU so a partial-permission case is
visible rather than inferred from CPU 0 alone.

- **Pass:** all CPUs open and mmap at `paranoid = 1`.
- **Fail:** record which `errno`. `EACCES` means the kernel gates this the way it gates
  system-wide events, and the README's setup instruction has to change to `paranoid = 0`.
  At that point U5 Option B becomes the better design and Phase 2's scope changes.

### P2 — `enable_on_exec` on a `cpu >= 0` event (U2)

`fork_stop_exec`-shaped: fork, child self-stops, parent opens the event with
`disabled = 1, enable_on_exec = 1, cpu = i`, SIGCONT, child execs a trivial program.
Read `time_enabled` / `time_running` and the counter value.

- **Pass:** counters non-zero, and `time_enabled` consistent with post-exec runtime only.
- **Fail:** the "counters arm exactly at the `execve` boundary" guarantee does not hold for
  per-CPU events, and every claim in the README that depends on it has to be restated for the
  multithreaded path. This does not kill the edition but it does change what can be claimed.

Also worth capturing here: whether a child that never execs produces zero, which is the same
assertion `main.cpp` already makes for the `cpu = -1` case.

### P3 — mlock budget (U3)

Loop: for ring sizes {1 MiB, 512 KiB, 256 KiB, 128 KiB, 64 KiB}, attempt to open and mmap
`2 × num_online_cpus` rings. Record the largest size where all succeed, and the exact
count/size at which `EPERM` first appears.

Also record, for the results file:

```
/proc/sys/kernel/perf_event_mlock_kb
ulimit -l                       (RLIMIT_MEMLOCK)
nproc                           (num_online_cpus)
```

Expected budget is `perf_event_mlock_kb × num_online_cpus` plus whatever `RLIMIT_MEMLOCK`
allows on top; the arithmetic in `GATE7_PLAN.md` U3 predicts ~6.0 MiB of allowance against a
~24 MiB ask at the current ring size. **The prediction is written down here so the probe either
confirms or corrects it.**

### P4 — does the PMU see false sharing (U4) — the gating probe

`p4_false_sharing.cpp`: two threads, pinned to two distinct physical cores, each incrementing
its own `uint64_t` counter a fixed number of times. One build has the two counters adjacent in
one struct (same cache line); the other pads them onto separate lines. Compile-time switch, one
source, so the two builds differ in exactly one thing.

Measure both under:

```
perf stat -r 5 -e cache-misses,cache-references,instructions,cycles ./p4_shared
perf stat -r 5 -e cache-misses,cache-references,instructions,cycles ./p4_padded
```

Three outcomes:

| outcome | reading | action |
|---|---|---|
| Large wall-clock gap **and** large `cache-misses` gap | The generalized events see coherence traffic. | Proceed as planned. |
| Large wall-clock gap, **no** `cache-misses` gap | False sharing is real but invisible to these counters — exactly the risk U4 names. | **Select U14's fallback.** Rewrite and re-date §0's prediction in `GATE7_PLAN.md` to the substituted bug before Phase 1. |
| No wall-clock gap | The false sharing did not occur. Compiler or hardware hid it (U18). | Fix the probe before concluding anything about the PMU. |

The third outcome must be excluded before the second can be believed. Check it with `objdump`
on the increment loop, the same way the matrix pair's scalar codegen was confirmed before any
measurement was trusted.

### Decision record — written before Phase 1

Append a `## Decisions (Phase 0)` section to `GATE7_PLAN.md` filling this in:

```
U5  sampler shape      : Option A (per-CPU + inherit) | Option B (per-TID handshake)
    because            : …
U6  sample-rate target : aggregate 47.4k/s | per-CPU 47.4k/s
    because            : … (sampling-statistics grounds only; Phase 6 workload is not an input)
U7  ring size per CPU  : … KiB   (largest that fits P3's answer at U6's rate)
    resulting headroom : … ms    (records ÷ per-ring rate — state it explicitly)
U15 optimization level : one binary at -O1 | two binaries (-O1 attribution, -O2 harness)
U16 thread pinning     : distinct physical cores | SMT siblings   (and the CPU ids used)
U10 inlining mitigation: __attribute__((noinline)) | implement inline-frame expansion
```

### Exit criteria

- [x] `results/gate7_probes.txt` exists, with an environment block, answering U1–U4 with numbers.
- [x] The U3 arithmetic prediction is confirmed or corrected in writing — corrected (real ceiling
      is ~2x the naive formula's estimate; 512 KiB, not 1 MiB, is the ring size that fits).
- [x] P4's outcome is classified into one of the three rows above, with the third excluded — row
      one (large wall-clock **and** large `cache-misses` gap), `objdump` confirms the offsets.
- [x] The decision record is filled in and committed **before** Phase 1 starts — see
      `GATE7_PLAN.md`'s "Decisions (Phase 0)" section.
- [x] P4 landed in row one, not row two — §0's prediction stands unmodified.

---

## Phase 1 — The queue benchmarks

**Goal:** close U10, U14, U17, U18. Apply U15, U16.
**Estimated size:** ~200–300 lines.

### Files

```
benchmarks/spsc_queue.cpp      NEW
benchmarks/mpmc_queue.cpp      NEW
benchmarks/Makefile            EDIT — new targets, padded/unpadded variants
scripts/measure_queue.sh       NEW — wall-clock A/B with environment block
results/gate7_queue_baseline.txt  NEW
```

### Build matrix

Both variants come from one source under `-DCACHELENS_PAD_INDICES=0|1`, so "the two differ in
exactly one thing" is a property of the build system rather than a claim about two files.
Per U15's decision, this may be four binaries rather than two:

```
spsc_queue_shared        -O1 -g -no-pie   (attribution target)
spsc_queue_padded        -O1 -g -no-pie   (attribution control)
spsc_queue_shared_O2     -O2 -g -no-pie   (latency-harness target, Phase 5)
spsc_queue_padded_O2     -O2 -g -no-pie   (latency-harness control, Phase 5)
```

The `-O1`/`-O2` split is a *disclosed* asymmetry, unlike the one the matrix pair avoids: there,
`-O2` vectorized one benchmark and not the other, which is a confound. Here both variants get
the same flags as each other; only the *purpose* differs between binary pairs, and the two
purposes never share a table.

### Benchmark requirements

- Bounded, power-of-two capacity, preallocated. No allocation on the hot path.
- **Fixed item count**, not a deadline (U17) — identical total work every run.
- Results consumed (checksum printed) so nothing is optimized away.
- Threads pinned via `sched_setaffinity` to the CPU ids fixed in Phase 0's U16 decision, printed
  at startup so the results file records them.
- `__attribute__((noinline))` on `push`/`pop` if that was U10's decision — with a comment saying
  it is a benchmark-shaping decision made so attribution lands inside the operation rather than
  at the call site, not a performance choice.
- SPSC first (simpler correctness problem, cleaner specimen). MPMC second, needed for Phase 5's
  contended case.

### Verification before any profiler is involved (U18)

1. `objdump -d` the push/pop bodies. Confirm the memory operations and the index layout survived
   the compiler in both variants.
2. Confirm the struct layout with a static assert on the offset of the two indices — that the
   unpadded variant really puts them on one 64-byte line and the padded one really doesn't.
3. Wall-clock A/B via `scripts/measure_queue.sh`, environment block attached, `-r 5`.

### Exit criteria

- [x] A measured, environment-stamped wall-clock delta between padded and unpadded — SPSC: 3.75x
      (-O1), 4.58x (-O2). MPMC (2P2C): 2.07x. `results/gate7_queue_baseline.txt`.
- [x] `objdump` and static-assert evidence that the delta has the cause claimed for it — confirmed
      tail/head offsets: 8 bytes apart (shared) vs. 64 bytes apart (padded).
- [x] The `perf stat` event signature matches what Phase 0's P4 predicted for this shape — large
      wall-clock gap with a large accompanying `cache-misses` gap (SPSC: 100M vs 31M, ~3.2x).
- [x] There *was* no wall-clock delta, twice, before there was one — **Phase 1 iterated**, per
      this exact exit criterion. Two design bugs found and fixed along the way (see
      `docs/TAKEAWAYS.md`): `alignas(64)` doesn't reserve a field's full line, and naive
      always-reload / locally-cached designs both let *true* sharing swamp the *false*-sharing
      signal. The design that worked (Vyukov-style per-slot sequence numbers, degenerated to one
      writer per index for SPSC) is documented in `benchmarks/spsc_queue.cpp`'s header.

---

## Phase 2 — Per-CPU sampler

**Goal:** close U8, U13. Implement U5's option and U6/U7's sizing.
**Estimated size:** ~150–250 lines of delta in `src/main.cpp`, almost all mechanical.

### Changes to `src/main.cpp`

| Site | Change |
|---|---|
| `open_sampling` (`:466`) | Takes a `cpu` parameter; sets `attr.inherit = 1`; ring size from U7 |
| `SamplingEvent` (`:449`) | One instance per (event, CPU) — array or vector, indexed `[event][cpu]` |
| `open_counting` (`:400`) | `attr.inherit = 1` (legal — counting mode has no mmap) |
| `calibrate_both` (`:418`) | Apply U8's divisor when converting aggregate rate → per-CPU period |
| drain loop (`:640`) | `pollfd` array grows from 2 to `2 × nr_cpus`; drain every ready ring |
| `print_event_report` (`:505`) | Aggregate `samples`, `hist`, buckets across an event's rings |
| `drain()` overflow `die()` (`:243`) | Message names the (event, CPU) pair |
| throttle `die()` (`:~290`) | Same; also note 24 independently-throttled events raises the odds |
| multiplexing check (`:~683`) | Per (event, CPU); halt names which one failed |
| `PERF_SAMPLE_PERIOD` check (`:~699`) | Per-ring; halt names which one failed |
| `finish_sampling` (`:498`) | Sum `counts.value` across an event's rings before reporting |

### U8 — the calibration divisor

The counting phase with `inherit = 1` measures the summed rate across all target threads. The
sample period applies per event per CPU. Getting the conversion wrong silently mis-scales every
concentration number, because `period_scale` (`:~722`) assumes the two periods are directly
comparable.

Derive it explicitly, write the derivation in a comment next to the code, and validate it in the
exit criteria against a target whose thread count is known (Phase 1's queue benchmark, which has
exactly two hot threads).

### Exit criteria

- [ ] **Regression guard — the important one.** `./build/cachelens -- ./benchmarks/matrix_bad`
      through the new per-CPU path reproduces the Gate 5 headline within ordinary run-to-run
      variance: `matrix_bad.cpp:44` at #1 by concentration, `matrix_bad.cpp:43` at #1 by raw
      count, concentration ≈ 0.36. **If the new sampler changes the old result, the new sampler
      is wrong until proven otherwise.**
- [ ] `lost_records = 0` and `lost_events = 0` across all rings on both targets.
- [ ] Multiplexing fraction ≥ 0.99 on every ring.
- [ ] Phase 1's two-thread queue benchmark produces samples attributed to **both** threads —
      the capability this phase exists to add, demonstrated rather than assumed.
- [ ] U8's divisor validated against the known thread count, derivation committed as a comment.
- [ ] README limitations list gains the multithreading entry — **this ships even if every later
      phase is abandoned**, because the gap exists in the code today.

---

## Phase 3 — Attribution under threads

**Goal:** close U9, U11, U12.
**Estimated size:** small in code, most of the work is measurement.

### U9 — re-run the skid characterization

The README's ±2-line skid finding is explicitly workload-specific, and states the reason it
held: "the hot loop body is 7 instructions across exactly 2 source lines." A queue `push()` with
a CAS, a mask, and a fence is not that body. Repeat the Gate 4 method against the queue
benchmark, pool the same way, and publish the number.

If skid is not absorbed for this workload, that goes in the README next to the existing caveat
and it **constrains how strongly Phase 4's result can be claimed** — a result about a specific
source line is only as good as the confidence that samples landed on that line.

### U11 — per-TID breakdown

`AttributionResult::line_counts` is keyed `(file, line)` and discards `tid`, though `Sample`
already carries it. Under false sharing the producer and consumer suffer at different lines, so
a merged ranking may be less legible, not more. Decide: add a per-TID column, or explicitly
decline and say why in the README.

### U12 — support gate under fragmentation

Report how many sites the `kMinAccessSamples = 30` gate excludes, as the existing output already
does. **The gate is not re-tuned to rescue the result.** If it excludes the interesting site,
that is the finding.

### Exit criteria

- [ ] A skid number for the queue workload, published whether or not it is favourable.
- [ ] U11 decided and either implemented or documented as declined.
- [ ] Excluded-site count reported for the new workload.

---

## Phase 4 — The experiment

**Goal:** adjudicate the pre-registered prediction in `GATE7_PLAN.md` §0.

Run Phase 2's tool against Phase 1's benchmarks. Report the outcome **item by item** — (a), (b),
(c) — including any item that failed, and including the case where (b) fails benignly because
the queue's hot loop has too few source lines for the two rankings to disagree.

### Files

```
results/gate7_false_sharing.txt   NEW — environment block + full unedited runs
README.md                         EDIT — second headline result section
docs/TAKEAWAYS.md                 EDIT — any bug found on the way, per existing convention
```

The README section is written to the same standard as the existing headline: the table, the
reasoning, and the limitation. If the prediction failed, the section says so and explains what
was learned instead — a failed pre-registered prediction reported honestly is worth more than
an unregistered success.

### Exit criteria

- [ ] Prediction adjudicated in writing, item by item.
- [ ] Results file committed unedited, with environment block.
- [ ] README updated to match whatever actually happened.

---

## Phase 5 — The tail-latency harness

**Goal:** close U19–U23.
**Estimated size:** ~200–300 lines.

### Files

```
benchmarks/queue_latency.cpp   NEW — built at -O2 per U15
scripts/measure_latency.sh     NEW
results/gate7_latency.txt      NEW
```

### Requirements, each tied to its unknown

- **U20 — open loop.** Rate-controlled producer; each operation has an *intended* start time
  computed from a fixed schedule, and latency is measured from intended start, not actual. A
  closed-loop harness stops generating load exactly when load matters most and reports a p99.9
  that means very little.
- **U19 — timer overhead.** Measure the cost of the timestamp call itself, report it as a number
  next to the results, and either subtract it or design around it (`rdtscp` + calibration, or
  batched timing). If timer overhead is a large fraction of the measured operation, say so
  rather than reporting a p99 that is mostly instrument.
- **U21 — recording must not perturb.** Preallocated, touched during warmup. State how much
  cache the recording path itself occupies, since it competes with the queue under test.
- **U22 — sample count.** ≥ 10^6 operations, multiple repetitions, variance reported. A p99.9
  from one run is one number, not a measurement.
- **U23 — re-ask the governor question.** Gate 5's null result was about a memory-stalled
  workload and does not transfer to a latency measurement.

### Exit criteria

- [ ] p99 and p99.9 for padded and unpadded, with run-to-run variance.
- [ ] Timer-overhead figure stated alongside.
- [ ] Coordinated-omission handling described explicitly in the results file.
- [ ] Governor sensitivity re-measured for this workload.
- [ ] **Kept in its own table.** Never merged with concentration numbers — same discipline the
      README applies to the `perf stat` baseline versus CacheLens's own aggregate.

---

## Phase 6 — Measure CacheLens's own drain path

**Goal:** close U24, U27. **Nothing is built here.** This phase only measures.

### What gets measured

1. **Drain service time.** Per-iteration wall time across all rings — worst case and
   distribution, not just mean. Compare against the headroom figure fixed in Phase 0's U7
   decision.
2. **Actual loss.** `lost_records` / `lost_events` totals under Phase 1's workload at Phase 0's
   sizing. This is the real SLO; drain latency is only a proxy for it.
3. **Profiling overhead (U27).** Target runtime under CacheLens versus target runtime alone, as
   a percentage. Currently unmeasured for *any* configuration, including the one that shipped —
   so this produces a number the project should have had since Gate 2.

### Exit criteria

- [ ] A stated answer to "does the drain keep up, and what does profiling cost the target,"
      with numbers.
- [ ] If the answer is "comfortably, and little": **Phase 7 does not happen**, and that null
      result is written into the README and `TAKEAWAYS.md` on the same footing as the governor
      null result. This is the expected outcome under U6's aggregate-rate configuration
      (~8.3 s of headroom) and it is not a failure.

---

## Phase 7 — Conditional: concurrency inside CacheLens

**Runs only if Phase 6 shows a real deadline miss.**
**Goal:** close U25, U26.

Shape is **MPSC**: N drain threads, one aggregator. Parallel DWARF symbolization — and therefore
any case for MPMC — is a separate question, opened only if Phase 6 identifies symbolization
rather than the drain as the bottleneck. **U26** (`Dwfl` handle per thread; whether
`dwfl_report_offline` against the same file from N threads is safe and cheap) is unverified and
gets its own probe if that path opens.

### Exit criteria

- [ ] `lost_records = 0` restored at the rate that broke it, before/after numbers reported.
- [ ] **U27 re-measured**, because the fix adds threads that compete with the target for the
      same L3 the tool is trying to measure. A profiler that fixes its own drain by becoming
      background load has traded one bias for another, and this project already documents what
      background load does to these measurements.

---

## Appendix — running list of what ships even if the edition is abandoned

Some of this work is worth keeping regardless of whether the false-sharing case study survives
U4. If Gate 7 is cut short, these still land:

- The README limitations entry for single-thread-only profiling (Phase 2). The gap exists in the
  shipped code today.
- The profiling-overhead number (Phase 6, item 3). Should have existed since Gate 2.
- `results/gate7_probes.txt` — the answers to U1–U4 are facts about this machine and this PMU
  that are worth having recorded whether or not anything is built on them.
- Any `TAKEAWAYS.md` entry produced along the way.
