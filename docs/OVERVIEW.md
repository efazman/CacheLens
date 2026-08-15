# CacheLens — Product Overview & Roadmap

**Status:** baseline, 2026-07-26
**Companion document:** [`DESIGN.md`](./DESIGN.md) — implementation specification
**This document:** what the finished product is, what "done" means, and the plan to get there

---

## 1. The goal in one paragraph

**CacheLens takes a compiled binary and tells you which source lines are losing time to cache misses, with quantitative evidence and a suggested fix.**

`perf` already tells you that address `0x401f3a` in `multiply+0x12` accounted for 3.2% of `cache-misses`. That is a *measurement*. Turning it into a *decision* — which line, how bad, why, and what to change — is manual work that requires knowing DWARF, knowing your PMU's quirks, and knowing cache behavior. CacheLens automates that last mile: binary in, ranked source lines out, with the reasoning attached.

---

## 2. The finished product, concretely

Two commands. This is the target user experience — write it down now so every design decision can be checked against it.

**Command 1 — what can this machine measure?**

```
$ cacheprof check ./matrix_bad

=== Capability Check ===
  perf              ✓  perf version 6.8.0
  addr2line         ✓  GNU addr2line 2.42
  debug info        ✓  .debug_info present
  CPU               AMD EPYC 7543 (AuthenticAMD)
  precise facility  IBS Op
  agent             ✓  selftest passed (precise_ip=2 granted)
  mode chosen       DIRECT_SAMPLING
                    └─ C agent available, IBS Op supports precise attribution
  → Ready to profile.
```

The value here is that it explains *why* it chose what it chose, and it fails with an actionable message rather than a confusing cascade of empty results. On a machine without a PMU it says "virtualized host, no hardware PMU exposed" — not "0 samples collected."

**Command 2 — where are the misses?**

```
$ cacheprof profile ./matrix_bad

Collection: DIRECT_SAMPLING (IBS Op, precise_ip=2)   29,650 samples   0 lost
Aggregate:  LLC miss rate 47.2%   (18.4M misses / 39.0M references)

#1  matrix_bad.cpp:23  in multiply_bad(double*, double*, double*, int)
    61.4% of all LLC misses          18,204 samples

     21 |   for (int j = 0; j < N; ++j)
     22 |     for (int k = 0; k < N; ++k)
     23 |       sum += A[i*N + k] * B[k*N + j];
        |                           ^^^^^^^^^^ 61.4% of misses
     24 |   C[i*N + j] = sum;

    Signals: concentration 0.61 | nested loops | strided index | N=2048

    Diagnosis (LLM):
      B is traversed with stride N*8 bytes, so every access to B touches a
      new cache line and the 2048x2048 working set (32 MB) exceeds the
      32 MB LLC. Each inner iteration evicts the line the next iteration
      would have reused.
      Suggestion: swap the j and k loops (i-k-j order) so B is walked
      row-major. Expected: ~8 accesses per cache line instead of 1.

#2  matrix_bad.cpp:24  ...

Artifacts: outputs/run_20260810_143022/
```

Everything above the "Diagnosis" line is measured. The diagnosis is the only LLM-generated part, it is opt-in, and it is grounded in the numbers directly above it. That separation is the product's core integrity claim.

---

## 3. What makes it worth building

Three capabilities, each mapping to one resume bullet. If a feature does not strengthen one of these, it is out of scope.

| Pillar | The claim | Why it is credible |
|---|---|---|
| **Attribution** | Hardware counter → instruction address → source line → ranked hotspot | The hard part is being *honest* about skid and resolution failure instead of presenting a clean lie |
| **Portability** | Runs on Intel, AMD, and Arm; degrades gracefully; never discards completed work | Real machines differ wildly in PMU capability — negotiating rather than assuming is what production tools do |
| **Low-overhead collection** | A C data plane speaking `perf_event_open` directly, streaming to Python over a Unix socket | Demonstrates identifying a bottleneck in your own tool and fixing it at the syscall layer |

**The through-line for interviews:** this is a tool that knows the limits of its own instrument. It reports which sampling facility it used, how much skid that implies, what fraction of samples it could not resolve, and how many the kernel dropped. That posture — measuring the measurement — is what separates a systems engineer from someone who ran `perf` once.

---

## 4. Non-goals

Stating these prevents scope creep and, in an interview, demonstrates deliberate boundaries.

- **Not a general-purpose profiler.** Cache locality only. No CPU time, branch misprediction, or lock contention analysis.
- **Not a `perf` replacement.** It sits on top of the same kernel interface and is more opinionated and narrower by design.
- **Not continuous or production monitoring.** Single run, single process, offline analysis.
- **No automatic code rewriting.** It suggests; a human decides.
- **Not system-wide or multi-process.** One target binary, launched by the tool.
- **No PIE support in V1/V2.** Targets build with `-no-pie` so the sampled IP equals the link-time address. Handling PIE means tracking `PERF_RECORD_MMAP2` and computing file-relative offsets — a real feature, deliberately deferred.
- **Not cross-platform.** Linux with a hardware PMU. macOS and virtualized cloud hosts are explicitly unsupported, and the tool says so clearly rather than failing obscurely.

---

## 5. Final architecture

```
                        ┌──────────────────────────────────┐
                        │  cacheprof CLI  (check | profile)│
                        └────────────────┬─────────────────┘
                                         │
                        ┌────────────────▼─────────────────┐
                        │   Capability negotiation          │
                        │   vendor · PEBS/IBS/SPE · agent   │
                        └────────────────┬─────────────────┘
                                         │  picks one data plane
                 ┌───────────────────────┴───────────────────────┐
                 │                                               │
      ┌──────────▼───────────┐                     ┌─────────────▼──────────────┐
      │  V2 DATA PLANE (C)   │                     │  V1 DATA PLANE (perf CLI)  │
      │  perf_event_open     │                     │  perf record               │
      │  mmap ring buffer    │                     │  → perf.data               │
      │  fork/exec target    │                     │  → perf script             │
      │  ─── unix socket ─── │                     │  → regex parse             │
      └──────────┬───────────┘                     └─────────────┬──────────────┘
                 │                                               │
                 └───────────────────────┬───────────────────────┘
                                         │
                            ═════════════▼═════════════
                                 list[PerfSample]          ◄── the seam
                            ═══════════════════════════
                                         │
   ┌─────────────────────────────────────▼──────────────────────────────────────┐
   │  CONTROL PLANE (Python) — identical for both data planes                    │
   │                                                                             │
   │  addr2line → aggregate → rank → snippet → signals → reports → LLM (opt-in)  │
   └─────────────────────────────────────────────────────────────────────────────┘
                                         │
                        ┌────────────────▼─────────────────┐
                        │  terminal · JSON · markdown       │
                        │  + every intermediate artifact    │
                        └───────────────────────────────────┘
```

**The two structural ideas worth defending:**

1. **The seam at `list[PerfSample]`.** Both data planes produce the same dataclass, so the entire analysis half is shared verbatim. This is what makes "rewrite the collector in C" a 5-week project rather than a rewrite — and it exists because every cross-module struct was defined in one place from the start.

2. **Nothing is thrown away.** Every stage writes its artifacts before the next begins. A failure at stage 8 leaves stages 1–7 on disk. Profiling runs are slow and non-deterministic; discarding a completed 3-minute collection because `addr2line` was missing is unacceptable. This holds across the process boundary in V2 too — if the target segfaults, the samples collected before the crash are still real data and still produce a report.

---

## 6. What "done" means

Two acceptance tests. Both are about a human's experience of the project, because that is what the project is for.

### The 5-minute reviewer test

An engineer who has never seen the repo, on a machine with a PMU:

1. Reads the README's first screen and understands what the tool does — **from a screenshot of real output**, not prose.
2. Sees a results table with measured numbers for three benchmarks.
3. Copy-pastes the quick start and gets a working profile of `matrix_bad`.
4. Finds a "What this tool does not do" section and concludes the author knows the boundaries of their own instrument.

**Passes when:** step 3 works on a fresh host, following only the README, with no undocumented steps. This is currently a hard fail — `pip install -e .` errors out on the first command.

### The 30-minute interview test

The author can, without notes:

1. Draw the architecture above and name the seam.
2. Explain skid, and what PEBS/IBS/SPE do about it.
3. Walk through the `perf_event_open` call and explain `enable_on_exec`.
4. Explain the ring buffer's acquire/release barrier pairing and the wraparound case.
5. Justify the wire protocol — TLV framing, fixed 32-byte records, why batched.
6. State a concrete example of what survives what in the partial-failure model.
7. Name three things the tool gets wrong and why.

**Passes when:** item 7 is answered as readily as items 1–6.

### Measured artifacts required

Nothing goes on the resume as a number until it is in `docs/examples/` as a real run:

| Artifact | Why it is required |
|---|---|
| Real profile of all three benchmarks (JSON + markdown + terminal screenshot) | Proof the tool ran |
| `matrix_bad` vs `matrix_good` LLC miss rates | Proof it measures the right thing |
| Hand-verified hotspot lines + observed skid | Proof the attribution is honest |
| Capability output from ≥2 different CPUs | Proof the negotiation is real |
| V1 vs V2 end-to-end timing table | The number bullet 3 rests on |

---

## 7. The plan

Five milestones. Each is independently shippable and each ends with something on the resume. The ordering is strict: every milestone is a gate on the next.

```
M0 ──▶ M1 ─────────▶ M2 ────────▶ M3 ──────────▶ M4
1 day   2 weeks       1 week       3 weeks        2 weeks
Jul 27  Aug 9         Aug 16       Sep 6          Sep 20

        ▲ bullets 1+2 real          ▲ bullet 3 true   ▲ bullet 3 with numbers
        │                           │
        └── HFT applications ───────┴── FAANG / cloud applications
```

### M0 — Hardware truth (1 day) · **GATE**

Establish that a machine exists which can actually run this. Standard DigitalOcean droplets are KVM guests and KVM does not expose a virtual PMU by default — on such a host every hardware event reads `<not supported>` and no amount of software work produces a demo.

- **Exit criteria:** `perf stat -e cache-misses,cache-references /bin/true` returns real integer counts.
- **If it fails:** provision bare metal (Hetzner ~€40/mo, AWS `*.metal` hourly, or any physical Linux box). Budget 1 day and ~$50.
- **Nothing else starts until this passes.** Details in `DESIGN.md` §1.1.

### M1 — V1 credible (2 weeks, by ~Aug 9) · **the only hard deadline**

Turn "code exists" into "tool works and the claims are true."

- Unblock the build: `pyproject.toml` backend, dependency cleanup, delete the stray `{src` tree, committed ARM binaries, `.dSYM`s and synthetic `outputs/`.
- **Make capability negotiation real** — actual vendor and PEBS/IBS/SPE detection. Bullet 2 is currently unsupported by any code.
- Fix the two bugs that will silently corrupt live data: the resolver ignoring `dso`, and the parser breaking on C++ demangled symbols.
- Rebuild benchmarks on Linux; first real runs; hand-verify hotspot lines; capture numbers.

**Ships:** bullets 1 and 2, with measured figures, in time for the HFT window.
**Exit criteria:** all three benchmarks profiled, hotspots hand-verified, real numbers recorded.

### M2 — V1 defensible (1 week, by ~Aug 16)

Make it survive a reviewer and an interviewer.

- Integration tests for the runner's partial-failure model — inject a failure at each stage boundary and assert the artifacts on disk. This is the test suite that defends bullet 2.
- CLI tests; one hardware-gated end-to-end test.
- README rewritten for the 5-minute test: screenshot first, results table second, honest limits section.

**Ships:** the resume is final for the HFT wave. **The project is now complete and defensible on its own.**
**Exit criteria:** the 5-minute reviewer test passes on a fresh host.

### M3 — V2 minimum viable (3 weeks, by ~Sep 6)

The smallest C agent that makes bullet 3 *true*: `perf_event_open`, mmap ring buffer, `fork`/`exec`, Unix socket IPC. One event, fixed period, `precise_ip=0`, run-to-completion then drain — no `poll()` loop, no negotiation ladder in C. Roughly 200 lines.

Built in four independently-verified stages, because the ring buffer consumer is the hardest part of the project:

1. Counting mode with `read()` only — no sampling at all
2. Sampling with a huge fixed period, so records arrive one at a time and wraparound never triggers
3. Normal period, asserting loudly if wraparound would occur
4. Full wraparound handling

Validate each stage's IP values against `perf record` on the same binary. A reference implementation exists — use it.

**Ships:** bullet 3, honestly.
**Exit criteria:** agent produces the same hotspot ranking as V1 on all three benchmarks.

### M4 — V2 complete (2 weeks, by ~Sep 20)

- `poll()`-driven streaming, `pidfd` for child exit, `precise_ip` negotiation ladder, counting mode replacing `perf stat`.
- Automatic fallback to V1 tested by forcing agent failure.
- The V1-vs-V2 measurement table.
- README documents both data planes.

**Ships:** bullet 3 with real numbers.
**Exit criteria:** end-to-end timing table complete; forced-agent-failure test produces a full report.

---

## 8. Guardrails

**On sequencing.** M1 and M2 are load-bearing; M3 and M4 are upside. A polished V1 beats a broken V2 every time — a reviewer who runs the quick start and sees a real hotspot in five minutes is more impressed than one who reads about a C agent that does not build. If time compresses, cut from the M3/M4 end and never from M2.

**On the resume.** Bullet 3 goes on only after M3 produces real samples. A vague answer to "walk me through your `perf_event_open` call" does more damage than a two-bullet resume. The same rule already applies to bullet 2 today: it names PEBS, IBS, and SPE, and no code detects any of them until M1.

**On honesty as a feature.** The instinct will be to hide skid, hide unresolved samples, hide the cases where the reported line is off by two. Resist it. Documented limitations are the most credible thing in the repo — a suspiciously perfect result reads as fabricated, and every strong systems interviewer probes exactly there. "The reported line is typically within ±2 lines under counter sampling; precise mode reduces this to 0" is a sentence that wins interviews.

**On estimates.** M3's 3 weeks assumes the staged build discipline above. For someone learning C systems programming, 5–6 weeks total for M3+M4 is the realistic figure. Plan for M4 landing late September and treat anything earlier as a bonus.

---

## 9. Success metrics

| Metric | Target | Milestone |
|---|---|---|
| Benchmarks profiled with real counter data | 3 / 3 | M1 |
| `matrix_bad` LLC miss rate vs `matrix_good` | clearly separated | M1 |
| Top hotspot matches hand-verified line | within documented skid | M1 |
| Sample resolution rate (in-target samples) | > 85% | M1 |
| Distinct CPU vendors with captured capability output | ≥ 2 | M2 |
| Fresh-host README walkthrough | works, no undocumented steps | M2 |
| Runner partial-failure paths under test | every stage boundary | M2 |
| V2 hotspot ranking vs V1 | identical top-3 | M3 |
| Samples lost to ring overflow | < 1% | M4 |
| End-to-end time to hotspots, V2 vs V1 | measured and reported honestly | M4 |
