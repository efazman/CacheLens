# CacheLens

A CLI profiler that samples hardware cache-miss and cache-reference events on Linux via
`perf_event_open`, attributes them to source lines with `libdw`, and ranks sites by miss
concentration instead of raw miss count.

## The headline result

On `matrix_bad` (naive i-j-k matrix multiply), raw miss count and concentration ranking pick
**different lines** as the bottleneck:

| line | what it is | raw miss count | rank by raw count | Wilson lower-bound concentration | rank by concentration |
|---|---|---|---|---|---|
| `matrix_bad.cpp:43` | loop control (`for (k...)`) | 331,412 | **#1** | 0.059 | #4 |
| `matrix_bad.cpp:44` | `sum += ... * mat(B, k, j)` | 234,147 | #2 | **0.360** | **#1** |

Raw count picks the loop-control line — it executes every iteration, so it absorbs the most
samples regardless of what it does. Concentration correctly picks line 44, the line that
actually strides through `B` column-wise. Only one of these is the real locality bug, and raw
count doesn't find it. Full data: [`results/gate5_concentration.txt`](results/gate5_concentration.txt).

## Real output

A live run against `matrix_bad`, unedited except for the two lines of the benchmark's own
`printf` output (`C[0][0] = ...`, printed once per internal calibration/measurement pass),
removed for length:

```
kernel perf_event_max_sample_rate: 79000/sec; per-event target: 47400/sec (60%)
child exited, status 0
calibration[miss]: value=2223800198 wall=11.4508s rate=194204925/sec
calibration[access]: value=25516161158 wall=11.4508s rate=2228331564/sec
period[miss]: 4099 (calibrated)
period[access]: 47017 (calibrated)
child exited, status 0

=== event: miss ===
requested period: 4099
aggregate: 2319394172, multiplexing fraction: 1.0000
samples captured: 565843
record histogram: sample=565843 lost_records=0 lost_events=0 exit=0 other=0
IP range: [0x4012c2, 0xffffffffa7520793]
bucket[target executable]: 565721 (99.9784%)
bucket[shared library / other user mapping]: 2 (0.0004%)
bucket[kernel space]: 120 (0.0212%)
bucket[unmapped / unclassifiable]: 0 (0.0000%)
distinct kernel-space IPs (6): 0xffffffffa5e00248 0xffffffffa5e00b90 0xffffffffa5e00e43 0xffffffffa5e00ef0 0xffffffffa5e00f03 0xffffffffa7520793
attributed: 565721, unattributed: 0 (0.0000%)

=== event: access ===
requested period: 47017
aggregate: 25839373480, multiplexing fraction: 1.0000
samples captured: 549575
record histogram: sample=549575 lost_records=0 lost_events=0 exit=0 other=0
bucket[target executable]: 549459 (99.9789%)
bucket[shared library / other user mapping]: 16 (0.0029%)
bucket[kernel space]: 100 (0.0182%)
bucket[unmapped / unclassifiable]: 0 (0.0000%)
attributed: 549459, unattributed: 0 (0.0000%)

period scale (miss/access): 4099 / 47017 = 0.087181

=== concentration ranking (Wilson lower bound, 95%, min 30 access samples) ===
  #1  matrix_bad.cpp:44  miss=234147 access=56750  concentration=0.359704  wilson_lb=0.359683
  #2  matrix_bad.cpp:41  miss=83 access=71  concentration=0.101916  wilson_lb=0.098922
  #3  matrix_bad.cpp:42  miss=79 access=77  concentration=0.089446  wilson_lb=0.087267
  #4  matrix_bad.cpp:43  miss=331412 access=492522  concentration=0.058663  wilson_lb=0.058549
insufficient samples (< 30 access samples), excluded from ranking (2 sites):
  matrix_bad.cpp:30  miss=0 access=22
  matrix_bad.cpp:31  miss=0 access=17

=== raw miss-count ranking (for comparison) ===
  #1  matrix_bad.cpp:43  miss=331412
  #2  matrix_bad.cpp:44  miss=234147
  #3  matrix_bad.cpp:41  miss=83
  #4  matrix_bad.cpp:42  miss=79
```

## Why concentration, not raw count

Most profilers rank by raw miss count, which mostly re-finds the hottest loop — code that runs
more absorbs more samples whether or not it uses the cache badly, which is exactly the failure
shown above. The obvious objection to ranking by a ratio instead: doesn't `misses/accesses` just
surface every low-traffic line with a lucky 3/3 sample? That's why ranking uses the **Wilson
score lower bound** (95%, z=1.96) rather than the raw ratio: it asks "what's the lowest
concentration consistent with the evidence," so a site needs both a high ratio *and* enough
samples to rank highly. Sites below 30 access samples are excluded from ranking entirely and
listed separately as insufficient — not silently dropped, not shown with a falsely confident
number.

## Architecture

```mermaid
flowchart LR
    T[target binary] --> S[Sample]
    S --> A[Attribute]
    A --> R[Rank]
    R --> P[Report]
```

**Sample** — `perf_event_open`, two independent events (not grouped), each with its own mmap
ring buffer, on one forked-and-stopped child; `enable_on_exec=1` arms both at the `execve`
boundary. **Attribute** — `libdw`, offline against the target ELF on disk; no `addr2line`
subprocess, no live-process attach. **Rank** — concentration, scored by Wilson lower bound.
**Report** — ranked table plus the raw-count ranking, printed alongside for comparison.

## Build and run

```bash
# PMU must be real (most cloud VMs have none):
perf stat -e cache-misses,cache-references,instructions,cycles /bin/true

# per-process profiling without root:
sudo sysctl -w kernel.perf_event_paranoid=1

# elfutils dev headers (libdw, libelf) — libdw-dev on Debian/Ubuntu — plus cmake, a C++17 compiler
make -C benchmarks
cmake -S . -B build && cmake --build build

./build/cachelens -- ./benchmarks/matrix_bad
./build/cachelens -- ./benchmarks/matrix_good
```

Verified from a clean clone into a fresh directory: PMU check, `perf_event_paranoid`, both
builds, and both runs above all succeed as shown.

## Case study

Two ground-truth numbers, measured by different tools, kept separate — they are not the same
claim and are not merged into one table:

**Stock `perf`, n=5, quiet machine, `performance` governor** — the resume-claim baseline,
independent of anything CacheLens computes. Original: 2.095x wall-clock speedup, 8.99%/1.23%
miss rate (7.31x ratio) — [`results/phase1_matrix.txt`](results/phase1_matrix.txt), unmodified.
A later, independent re-measurement under controlled conditions came in at 2.084x, within
0.46–0.97% of the original — ordinary run-to-run variance, not a correction —
[`results/drift_investigation.txt`](results/drift_investigation.txt).

**CacheLens's own aggregate** (calibration phase, whole-program, not sampled): 8.90%/1.14% miss
rate — directionally consistent with Phase 1 but not tuned to match, and not expected to match
exactly (different run, different moment). Per-site concentration (35–36% at the hottest line)
is not expected to match either whole-program number, and doesn't: the hottest site sits far
above the whole-program average because the average is diluted by every non-memory instruction
in the program. [`results/gate5_concentration.txt`](results/gate5_concentration.txt).

**The governor null result:** `performance` vs `powersave`, same quiet machine, differed by
+0.07–0.11% — noise. This corroborates rather than contradicts the speedup's cause: if it had a
compute-bound component, clock frequency would matter and this test would show a real gap. It
doesn't — independent confirmation, via IPC (≈1.57 vs ≈3.73), that the speedup is eliminated
stalls, not more throughput.

**Background load biases the result upward, not down.** A noisy-machine run reported 2.20x —
*larger* than the quiet run's 2.08x. L3 contention degrades the cache-hostile benchmark more
than the cache-friendly one, so an unquiesced reproduction will tend to report a *better* number
than the true one.

## Second case study: false sharing on a multithreaded queue (Gate 7)

A pre-registered prediction (recorded before any measurement existed —
[`docs/GATE7_PLAN.md`](docs/GATE7_PLAN.md) §0) about a bounded SPSC queue whose producer-owned
and consumer-owned index sit either on one cache line or on two, profiled with a producer and
consumer thread pinned to separate physical cores. Adjudicated item by item, honestly, including
where it didn't land exactly as predicted — full data and reasoning:
[`results/gate7_false_sharing.txt`](results/gate7_false_sharing.txt).

**The wall-clock effect is unambiguous:** the padded build (index fields on separate cache
lines) runs 2.75x–3.62x faster than the unpadded build, reproduced across every measurement
taken. `objdump` confirms the fields sit 8 bytes apart in the unpadded build and exactly 64
bytes apart in the padded one.

**The concentration ranking did not land on the literal predicted line — and the reason why is
itself informative.** Neither index-update instruction (the actual `store` to the producer's or
consumer's index) ranks #1 by concentration; the top slot goes to a spin-wait check line instead.
But the single largest *relative* response to padding, of any line in the function — a **+673%**
jump between the padded and unpadded builds — lands on the one x86 instruction immediately
*after* the consumer's index-update store, and that line also ranks in the top 3 by absolute
concentration on a large, reliable sample (21,440 pooled access samples, not a low-count fluke).
That is one instruction of skid, not a miss: this project previously measured skid at 99.99%
within ±2 source lines on a 7-instruction, 2-line hot loop (Gate 4) — a body with almost nowhere
else for skid to land. This queue's `push()`/`pop()` span ten-plus lines each, giving skid real
room to move, and it visibly used it.

**Raw miss-count ranking's #1 is a third, different line again** (the busiest spin-check by call
frequency) — reproducing, on a structurally different workload, the same "raw count finds the
busy line, not the interesting one" divergence the first case study demonstrated. Concentration
and raw count disagree with each other here exactly as they did for `matrix_bad`; neither one
happens to isolate the specific instruction this experiment targeted as cleanly as Gate 5's
result did.

**Reading it straight:** the pre-registered prediction about *which exact line* wins the ranking
did not hold. The prediction about the mechanism — that this is a real, hardware-visible,
padding-sensitive effect, and that concentration and raw count see it differently — did. A
failed prediction reported with the data that explains why is worth more than a success that
wasn't checked this closely.

## Tail latency, and a governor result that does *not* transfer

[`results/gate7_latency.txt`](results/gate7_latency.txt): an open-loop, rate-controlled harness
(500,000 ops/sec, well below the queue's unsaturated capacity) measuring padded-vs-shared queue
latency, and separately, CPU governor sensitivity — re-asked rather than assumed, because Gate
5's governor null result was measured on a memory-stalled workload and a latency-sensitive one
is a different question.

**The false-sharing penalty shows up in typical latency, not in the tail, at this load level.**
p50 and p99 are consistently 10-20% higher on the shared build, every run (n=5 each); p99.9 and
above overlap completely between builds — at a rate this far from saturation, tail latency is
dominated by ordinary OS scheduling noise, which is larger than the cache-line-placement effect
and swamps it.

**The governor result does not transfer, confirming it shouldn't have been assumed to.** Typical
latency (p50/p99) is governor-insensitive, matching Gate 5's direction. The tail is not:
`performance` gives a lower typical p99.9 but wildly inconsistent run-to-run behavior and
occasional extreme outliers (up to 1.85ms — a ~25,000x multiple of p50, visible only because the
open-loop design doesn't hide stalls the way a closed-loop harness would); `powersave` gives a
higher but remarkably stable p99.9 (under 3% spread across 5 runs). The cause is not confirmed —
stated as a hypothesis (turbo/thermal transition stalls under sustained max-frequency load), not
a finding.

## Does CacheLens keep up with its own targets? (Gate 7 Phase 6)

Nothing new is built for this section — it only measures. Full data:
[`results/gate7_drain.txt`](results/gate7_drain.txt).

**The drain keeps up, comfortably.** Against a genuinely contended two-thread workload, the
worst single poll-loop drain iteration observed was 495us — three-plus orders of magnitude under
the ~3.3s headroom the aggregate sample-rate configuration (Phase 0, U6/U7) provides per ring.
Zero `lost_records`/`lost_events` on that workload, every run.

**Profiling cost — measured, and not in the expected direction.** Comparing the target's own
self-reported wall-clock time, standalone vs. under CacheLens, across 5 interleaved paired
trials: the target ran *faster* under CacheLens in all five, by about 17%, not slower. This
project's own prior finding is that background load biases results *upward* (see the first case
study) — this result is neither that nor the naive "profiling adds overhead" expectation. A
plausible mechanism (PMU sampling interrupts, tens of thousands per second, interacting with the
`powersave` governor's frequency selection) was proposed but not confirmed — an attempt to
verify it via live CPU-frequency sampling failed on timing precision against a sub-second
benchmark, and is reported as an open question rather than forced into an answer.

**Because the drain keeps up and profiling shows no measured cost, Phase 7 (a concurrent queue
inside CacheLens's own drain path) does not happen** — the expected outcome under the aggregate
sample-rate configuration, and, like the governor null result, worth recording precisely because
it was tested rather than assumed.

## Limitations and caveats

- **Benchmarks are built `-O1`, not `-O2`.** At `-O2`, GCC auto-vectorizes `matrix_bad`'s inner
  loop but not `matrix_good`'s — an asymmetric confound. `-O1` verified scalar for both via
  `objdump` (zero `mulpd`/`movupd`) before any measurement was trusted.
- **Concentration is not per-access ground truth.** It's a ratio of two independently sampled
  event streams compared in aggregate at a site — not a claim that any specific sampled miss
  and access were the same memory operation.
- **The hardware events are the kernel's generalized `PERF_COUNT_HW_CACHE_MISSES`/`REFERENCES`,
  not a confirmed LLC-only counter on this AMD chip.** This project has not independently
  verified via raw PMU event codes that AMD Zen 4's mapping counts L3 activity exclusively.
- **Transparent Huge Pages: `madvise`** — off for anonymous mappings unless requested, which
  none of the benchmarks do. Would need rechecking on a host defaulting to `always`.
- **`-no-pie`.** Required for the offline, no-live-process DWARF attribution to be valid
  (link-time vaddr == runtime address). PIE support would need `PERF_RECORD_MMAP2` tracking —
  deliberately deferred.
- **`precise_ip=0`: this CPU has no PEBS-equivalent.** `precise_ip=2` and `1` both fail `ENOENT`
  on this Zen 4 part; only arbitrary skid is available. Measured, not assumed: source-line skid
  is 99.99% within ±2 lines for this specific benchmark (n=5, 119,196 samples pooled) — **this
  is workload-specific, not a general guarantee.** It's absorbed here because the hot loop body
  is 7 instructions across exactly 2 source lines; a larger loop body or heavy inlining would
  give unbounded skid far more room to land on the wrong line. `addr2line` agreement is a
  separate check (validates the DWARF lookup, not the sampled address) and stands at 22/22 —
  the complete population of distinct addresses this workload produces this way, not a
  subsample; the count saturated at 22 across 25 pooled runs and did not grow with more.
- **Single machine, single configuration, single moment in time.** AMD Ryzen 5 7600X (Zen 4),
  32 MiB L3, 16 GB DDR5 single-channel, Ubuntu, kernel `7.0.0-29-generic`. Not reproduced
  elsewhere.
- **Multithreaded targets: supported since Gate 7, at a real cost.** Earlier increments left
  `attr.inherit` at 0 and silently profiled only the target's initial thread — undocumented at
  the time. Sampling every thread of a multithreaded target requires one `perf_event_open` per
  (event, online CPU) with `inherit=1` (`pid=-1` and `inherit=1` together disable the mmap ring
  buffer entirely — see `docs/GATE7_PLAN.md` §1), which is 24 file descriptors and 24 ring
  buffers on this 12-CPU machine, up from 2. See
  [`docs/GATE7_PLAN.md`](docs/GATE7_PLAN.md) and
  [`docs/GATE7_IMPLEMENTATION.md`](docs/GATE7_IMPLEMENTATION.md) for the design and the unknowns
  closed along the way — including a real bug the regression guard caught: a per-CPU task
  event's `time_running`/`time_enabled` ratio looks exactly like PMU contention on any CPU an
  unpinned thread merely migrated through, and has to be summed across all per-CPU rings for an
  event before that ratio means anything.
- **No system-wide (`pid=-1`) or multi-process profiling.** Per-CPU events are opened for one
  target task tree, not the whole machine — a deliberate scope boundary, not a gap.
- **No cross-architecture abstraction (PEBS/IBS/SPE), no inline-frame expansion** (a sample
  inside an inlined function attributes to the inline call site — both matrix benchmarks are
  fully inlined into `main` at `-O1`), **no automatic code rewriting, no GUI.**

## Reproducing

A performance number without its environment recorded alongside it is not reproducible — not
even by the person who measured it (see `docs/TAKEAWAYS.md`). Use
[`scripts/measure_baseline.sh`](scripts/measure_baseline.sh) for every `perf stat` ground-truth
measurement; it is required, not optional. It captures governor, load average, processes above
1% CPU, THP, `perf_event_paranoid`, kernel version, compiler version, build flags, and core
frequency alongside the numbers:

```bash
scripts/measure_baseline.sh results/my_run.txt "my-conditions-label"
```

Verified working (200s run, both benchmarks, environment block plus two full `perf stat -r 5`
blocks written).

To reproduce one specific prior `cachelens` run exactly rather than let it recalibrate: every
automatic run prints its calibrated `period[miss]`/`period[access]`; read them back out and
replay with `--period N` (applies to both events; skips calibration; the throttle halt stays
active):

```bash
./build/cachelens --period 50000 -- ./benchmarks/matrix_bad
```

## Future work

AMD IBS (Instruction-Based Sampling) is the AMD-side path to real instruction-level precision —
the rough equivalent of what PEBS gives on Intel. It requires a dynamic PMU type
(`/sys/bus/event_source/devices/ibs_op`), not `PERF_TYPE_HARDWARE`, and a different sample
record layout. Deliberately deferred: `precise_ip=0`'s skid was measured and found absorbed for
this specific benchmark pair, so there was no open correctness question IBS was needed to
close — see the workload-specific caveat above before assuming that still holds for a different
target.

## Debugging log

[`docs/TAKEAWAYS.md`](docs/TAKEAWAYS.md) is a running record of bugs found in this tool, their
root causes, and why each one wasn't obvious in advance.
