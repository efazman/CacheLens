# CacheLens — Architecture

**Status:** rearchitecture baseline, 2026-08-15
**Supersedes:** `DESIGN.md` and `OVERVIEW.md` (both predate the scope cut; see §8)
**Governing document:** the Scope Handoff. Where this doc and the handoff disagree, the handoff wins.

**Cleanup note (2026-08-19):** §7's deletions are now done — the Python prototype
(`src/cacheprof/`, `tests/`, `pyproject.toml`, `requirements.txt`, `scripts/demo.sh`) is gone
from the tree (git history still has it). `DESIGN.md` and `OVERVIEW.md` moved to `docs/archive/`
per §8 rather than being deleted. §7's other line items (Mach-O binaries, `.DS_Store`, the `{src/`
residue, the LLM layer) were already gone before this pass.

---

## 0. What changed and why this document exists

The handoff cut three things — the LLM layer, cross-architecture support, and the Python/C
split's reason for existing — and added one design opinion: **rank by miss concentration, not
raw count.**

Those cuts are not independent trims. Together they collapse the architecture into something
substantially smaller than either existing doc describes, and the collapse is the point of this
rewrite. Two consequences dominate everything below:

1. **No Python means no IPC.** `DESIGN.md` §2.2–2.3 — the Unix socket, the TLV framing, the
   SESSION header, the endianness field, the batching argument, the `record_size` forward-
   compatibility scheme — exists solely to move samples from a C data plane into a Python
   control plane. Delete the control plane and the entire wire protocol becomes dead design.
   That is roughly 60% of `DESIGN.md` and it should not be built.

2. **Concentration ranking is not implementable with the current sampling design.** The repo
   samples one event. A per-site miss *rate* needs a numerator and a denominator at that site.
   This is the one place the architecture has to grow rather than shrink, and §3 specifies it.

---

## 1. The system

One statically-linked C++ binary. No subprocesses, no sockets, no Python, no `perf` CLI, no
`addr2line`.

```
   cachelens ./matrix_bad
        │
        ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │ 1. SAMPLE                                    sampler/            │
   │                                                                  │
   │    perf_event_open ×2  ──►  mmap ring ×2                        │
   │      miss event            (512 KB each)                        │
   │      access event                                               │
   │                                                                  │
   │    fork() → child blocks → perf_event_open(child) → exec        │
   │    enable_on_exec=1   (counters arm exactly at execve)          │
   │    run to completion → waitpid → drain both rings               │
   └────────────────────────────┬────────────────────────────────────┘
                                │  vector<Sample>{ ip, tid, time, event_id }
   ┌────────────────────────────▼────────────────────────────────────┐
   │ 2. ATTRIBUTE                                 attrib/             │
   │    libdw / dwfl_module_getsrc()                                 │
   │    ip → (file, line, function)                                  │
   │    samples outside the target DSO are bucketed, not resolved    │
   └────────────────────────────┬────────────────────────────────────┘
                                │  vector<Site>{ loc, n_miss, n_access }
   ┌────────────────────────────▼────────────────────────────────────┐
   │ 3. RANK                                      rank/               │
   │    concentration = misses / accesses at this site                │
   │    scored by Wilson lower bound, min-support gated               │
   │    ── this is the tool's only opinion; see §3 ──                 │
   └────────────────────────────┬────────────────────────────────────┘
                                │  ranked vector<Site>
   ┌────────────────────────────▼────────────────────────────────────┐
   │ 4. REPORT                                    report/             │
   │    terminal table + source snippet + honesty block               │
   │    (JSON alongside, for the case study)                          │
   └─────────────────────────────────────────────────────────────────┘
```

Four stages, matching the handoff exactly. Every arrow is an in-process function call on a
`std::vector`. There is no seam to defend because there is no process boundary.

### 1.1 Layout

```
src/
  main.cpp              CLI, arg parsing, exit codes                    ~100
  core/types.hpp        Sample, SourceLoc, Site, Report — shared header   ~80
  sampler/
    perf_event.cpp      attr construction, perf_event_open, precise_ip   ~150
    ring_buffer.cpp     mmap, barrier-correct drain, record decode       ~180
    session.cpp         fork/exec/wait, owns N rings, lifecycle          ~120
  attrib/
    dwarf.cpp           libdw address → source line                       ~90
  rank/
    aggregate.cpp       samples → per-(file,line) counts, per event      ~70
    score.cpp           concentration, Wilson bound, support gate         ~60
  report/
    terminal.cpp        ranked output + snippet extraction               ~130
    json.cpp            machine-readable report                           ~70
```

Roughly 1,050 lines. Dependencies: `libdw`/`libelf` (elfutils). Nothing else.

`core/types.hpp` is the one header every module includes and no module's header includes
another's. It plays the role `models.py` played in the Python version, and for the same reason:
one place to see the shape of the data.

---

## 2. Stage 1 — Sample

### 2.1 Why not shell out to `perf record`

The audience is NVIDIA developer-tools engineers. A binary that shells out to `perf record`,
then `perf script`, then regex-parses the text is a wrapper regardless of what language it is
written in — and the handoff already identified "reads as a thin `perf` wrapper" as the failure
mode worth avoiding. Calling `perf_event_open` directly is what makes the measurement pipeline
the contribution rather than the glue.

It is also *less* code than the Python version, because the text-parsing layer disappears
entirely. `perf/parser.py` is 128 lines of regex fighting perf's output format, and it contains
a live bug (`DESIGN.md` §1.4 Bug B: the last-match event scan picks `std:` out of a demangled
C++ symbol). Reading binary records from the ring buffer has no equivalent failure mode — the
layout is declared by the `sample_type` bitmask you set yourself.

### 2.2 Event configuration

Two independently-opened sampling events on the child pid, each with its own ring buffer.

| Field | Value | Note |
|---|---|---|
| `type` | `PERF_TYPE_HW_CACHE` | lets you name LLC read misses precisely |
| `config` (miss) | `LL \| (OP_READ<<8) \| (RESULT_MISS<<16)` | numerator |
| `config` (access) | `LL \| (OP_READ<<8) \| (RESULT_ACCESS<<16)` | denominator — see §3 |
| `size` | `sizeof(perf_event_attr)` | mandatory; kernel versions the struct by size |
| `sample_period` | fixed, **not** `freq` | see §3.2 — freq mode breaks the ratio |
| `sample_type` | `IP \| TID \| TIME \| PERIOD` | exactly the fields decoded |
| `precise_ip` | 3→2→1→0 ladder | see §2.4 |
| `disabled` | 1 | child not running yet |
| `enable_on_exec` | 1 | arms at `execve`, not at `fork` |
| `exclude_kernel` | 1 | works under default `perf_event_paranoid=2`, no root |
| `exclude_hv` | 1 | same |
| `inherit` | 0 → **1 as of Gate 7** | was 0 when every benchmark here was single-threaded; multithreaded targets need `inherit=1` plus one event per (event, online CPU) rather than one shared `cpu=-1` event — see `docs/GATE7_PLAN.md` §1 and `src/main.cpp`'s header comment for why `cpu=-1` and `inherit=1` cannot be combined for sampling |

**Two independent events, not a group.** Grouping them onto one ring would require
`PERF_SAMPLE_STREAM_ID` and an id→event map to demultiplex, and group scheduling can fail with
`EINVAL` when the counters cannot be scheduled together on the PMU. Two separate events with
two separate rings sidesteps both problems and costs one extra `mmap`. Take the simpler design.

### 2.3 The ring buffer, and the wraparound de-risking

`DESIGN.md` §4.2 correctly identifies the ring buffer consumer as the hardest part of the
project. That assessment stands. But the handoff's scope makes most of the difficulty optional:

Run-to-completion changes the shape of the problem. There is no `poll()` loop, no concurrent
streaming, no partial-flush timer, no backpressure. You start the child, wait for it to exit,
then drain the ring once. The kernel is no longer writing while you read.

And for *these benchmarks*, the ring never wraps:

```
record size  = 8 (hdr) + 8 (ip) + 8 (tid) + 8 (time) + 8 (period)  = 40 bytes
3 s benchmark @ period yielding ~1 kHz                              ≈ 3,000 records ≈ 120 KB
ring data area, n=7 (128 pages)                                     = 512 KB  ≈ 13,000 records
```

So: **build the drain with a loud assert on wraparound rather than wraparound handling.** If
`data_head - data_tail > data_size` the run aborts with a clear message telling you to raise
the sample period or the ring order. That is honest, it is correct for the shipping scope, and
it removes the single highest-risk item (`DESIGN.md` R6) from the critical path. Handle
wraparound properly afterward, once the pipeline is producing case-study numbers.

The barrier pairing is **not** optional and is not de-risked by any of the above. Even draining
after exit, you load `data_head` with `__ATOMIC_ACQUIRE` and store `data_tail` with
`__ATOMIC_RELEASE`. Get this from the `perf_event_open(2)` man page's mmap section and
understand it — it is the most likely deep question in any systems interview about this project.

Decode order is fixed by the `sample_type` bitmask in canonical order (IP, TID, TIME, PERIOD),
not the order you wrote them. Count `PERF_RECORD_LOST` (type 2) into a `samples_lost` field and
surface it in the report.

### 2.4 `precise_ip` survives the cross-architecture cut

The handoff cuts "cross-architecture support (Intel PEBS / AMD IBS / ARM SPE abstraction)."
That cut is correct — building a vendor-detection abstraction layer is scope creep.

But `precise_ip` negotiation is a different thing and it must stay, for a reason that is now
*load-bearing rather than cosmetic*: **the concentration metric divides two independently-skidded
distributions.** Raw-count ranking tolerates skid because it only needs the peak to land near the
right line. A ratio does not — a numerator skidded onto line 23 and a denominator skidded onto
line 24 produce a spurious 100% concentration at 23 and 0% at 24. Skid is a first-order threat
to the tool's one design opinion.

The mitigation is ten lines: attempt `perf_event_open` with `precise_ip=3`, on `EOPNOTSUPP`/
`EINVAL` retry 2, then 1, then 0. Record the granted level and print it. No vendor detection, no
PEBS/IBS/SPE naming, no abstraction layer — just ask the kernel for the best it will give and
report what you got. Frame it as *"the ranking metric requires low skid, so the sampler
negotiates for it"*, which is a measurement argument, not a portability claim.

---

## 3. Stage 3 — Ranking, the one design opinion

This is the section that matters most, because it is the one place the current repo is not
merely incomplete but *wrong*, and because it is the claim you will be asked to defend.

### 3.1 The gap

`analysis/ranking.py` is nineteen lines and sorts by `miss_count` descending. That is exactly
the raw-count ranking the handoff rejects. The handoff's framing — *"misses relative to
accesses/samples at that site"* — is not implemented anywhere, and it **cannot be** implemented
from the data the current pipeline collects: one sampled event gives you misses per line and
nothing to divide by.

`fraction_of_total` in `models.py` looks like a rate but is not one. It is
`site_misses / total_misses` — a share of the global miss budget, which is monotonic in
`miss_count` and therefore produces *identical* ranking. Sorting by it changes the printed
number and nothing else.

So the second ring buffer in §2.2 is not a nice-to-have. It is what makes the tool's stated
thesis measurable.

### 3.2 The metric

For each source site *s*:

```
concentration(s)  =  miss_events(s) / access_events(s)

where  miss_events(s)   = miss_samples(s)   × miss_period
       access_events(s) = access_samples(s) × access_period
```

**Use fixed `sample_period`, not `freq`.** In frequency mode the kernel continuously adjusts the
period, so each sample stands for a varying and unequal number of underlying events. Multiplying
sample counts by a nominal period is then wrong, and the ratio silently inherits the error.
Fixed periods make each sample worth exactly *P* events and the ratio interpretable. Sample
`PERF_SAMPLE_PERIOD` anyway and assert it matches — cheap, and it catches a misconfigured attr.

**What this estimates, stated honestly:** two independent statistical samplings of two different
event streams, compared in aggregate at a site. It is *not* per-access ground truth — no sample
pairs a specific miss with a specific access. It is a density ratio, valid at sites with enough
samples on both sides, and the report should say so.

### 3.3 Small-sample noise, and the fix

A site with 3 miss samples and 3 access samples scores 1.0 and outranks the real bottleneck.
Any naive ratio ranking is dominated by its own noise floor. Two guards:

**Support gate.** Drop sites below a minimum absolute sample count on both events (start at 30
access samples; tune once you have real data). Report the count of sites dropped this way so the
filtering is visible rather than silent.

**Rank by the Wilson score lower bound, not the point estimate.** With p̂ = m/n at confidence z
(1.96 for 95%):

```
              p̂ + z²/2n − z·√( p̂(1−p̂)/n + z²/4n² )
   score  =  ─────────────────────────────────────────
                          1 + z²/n
```

This is the standard shrinkage estimator for exactly this problem: it asks "what is the lowest
miss rate consistent with the evidence at this site," so a site needs both a high ratio *and*
enough samples to rank highly. 3/3 scores ~0.44 despite a point estimate of 1.0; 3,000/4,000
scores ~0.74 — the noisy site loses to the real one even though its raw ratio is higher.

This is a genuinely strong thing to be able to explain. "I rank by concentration" invites the
obvious follow-up *"doesn't that surface every low-traffic line?"* — and having the answer
already in the code is the difference between an opinion and a designed metric.

### 3.4 Report both

Print concentration-ranked results as the primary output, with raw miss count as a visible
column. A reviewer who disagrees with the opinion can still read the conventional view, and
showing both is what demonstrates the choice was deliberate rather than accidental.

---

## 4. Stage 2 — Attribution

Use **libdw** (`elfutils`), specifically a `Dwfl` session with `dwfl_module_getsrc()`.

Not `addr2line`. Shelling to `addr2line` was defensible in a Python tool and is not in a C++
one — it re-introduces a subprocess into an otherwise self-contained binary, and for an audience
that ships DWARF consumers professionally, "I call the binutils CLI" is the weakest sentence in
the design. `dwfl_module_getsrc` is roughly 90 lines including setup, and it is a real DWARF
line-table lookup.

Two behaviors carry over from the Python design because they were right:

**DSO filtering.** A real run captures samples from `libc`, `ld-linux`, and the kernel.
`dwarf/resolver.py` resolves *every* address against the target binary, which at best yields
`??:0` and at worst attributes a libc address to a wrong line in your source that happens to
occupy the same range (`DESIGN.md` §1.4 Bug A). Under `Dwfl` this is handled structurally:
`dwfl_addrmodule()` tells you which module an address belongs to, so out-of-target samples fall
out naturally rather than needing a filter.

**Three buckets, not one rate.** Report `resolved`, `unresolved_in_target`, and `outside_target`
separately. Only the middle bucket indicates a problem with your DWARF setup; collapsing all
three into one "unresolved rate" blames you for samples that were never resolvable.

`-no-pie` stays for V1: the sampled IP then equals the link-time address and no normalization is
needed. Document it, and note that PIE support is `PERF_RECORD_MMAP2` tracking plus a
file-relative offset — real work, deliberately deferred.

---

## 5. Sequencing against the deadline

The handoff says reqs drop within days and the repo link is already on the resume. A C++
rewrite is not a days-long project, so the sequencing has to be honest about that.

The load-bearing insight: **the resume bullet does not depend on the tool.**

> *"Identified a locality bottleneck in [benchmark]; restructuring the access pattern yielded a
> [N]x speedup."*

That number is `time` and `perf stat -e cache-misses,cache-references` on `matrix_bad` vs
`matrix_good`. It is measurable on day one, on rented hardware, with zero CacheLens code. The
tool's job is to *find* the line — which you already know, because you wrote the benchmark — and
the case study's job is to show the tool found it independently. Those are separable, and
separating them takes the deadline off the critical path.

**Phase 0 — stop the bleeding (hours, do first).**
The live repo link currently shows a README selling AI explanations and a committed
`outputs/smoke_report/` whose own capability block reads `perf: not found` while presenting a
full report. Synthetic data committed as though it were a real run is the single most damaging
thing in the repo for this audience — it is the exact failure mode a profiling engineer is
trained to spot. Delete it (§7), and rewrite the README to describe the tool the handoff
specifies and state plainly what is measured so far.

**Phase 1 — hardware gate + the case-study numbers (1–2 days).**
`DESIGN.md` §1.1 is right that this gates everything, and it has still not been run. Apple
Silicon cannot host this project at all — no `perf`, no userspace PMU access — and the committed
`.dSYM/Relocations/aarch64/` directories confirm every binary in the repo was built on the Mac.
Get bare metal (Hetzner AX ~€40/mo, any physical Linux box, AWS `*.metal` hourly), confirm
`perf stat -e cache-misses,cache-references /bin/true` returns real integers, rebuild the
benchmarks, and capture the before/after table with stock `perf`. **The resume bullet is true at
the end of this phase.**

**Phase 2 — C++ pipeline, single event (~1 week).**
Stages 1, 2, 4 end-to-end with one ring buffer and raw-count ranking. Ranking is knowingly
"wrong" here; the goal is a working sampler and correct attribution. Validate IPs against
`perf record` on the same binary at every step — you have a reference implementation, use it.
Build the ring consumer in the staged order `DESIGN.md` §4.2 prescribes; that advice survives
intact.

**Phase 3 — the second ring and concentration ranking (~3–4 days).**
Add the access event, the ratio, the support gate, the Wilson score. The tool now has its
opinion. This is deliberately *after* Phase 2 because concentration ranking is worthless until
the sampler and attribution are trustworthy.

**Phase 4 — the case study and the README (~2 days).**
Re-run everything through CacheLens, confirm it surfaces the lines you expect, write it up.
Budget the time the handoff asks for here; this is the artifact that gets evaluated.

Roughly 2.5 weeks to full ship, with a defensible repo from hour one and a true resume number
from day two.

---

## 6. Benchmarks — three problems

The handoff asks for 2–3 benchmarks *with before/after numbers*. The current set cannot produce
that table as written.

**`pointer_chase` has no "after" variant.** `matrix_bad`/`matrix_good` is a proper pair.
`pointer_chase` is a single program with no array-traversal counterpart, so there is no
before/after row to report — only a "the tool found the deref" observation. Add
`array_traverse.cpp` walking the same data contiguously, or drop to the AoS/SoA pair the handoff
lists third. One pair is worth more than two singletons.

**`matrix_bad` is sized at `N=1024`.** Three 1024² double matrices ≈ 24 MB total. On a host with
a 32 MB LLC the working set substantially fits, and the good/bad separation collapses. Size `N`
from the actual `lscpu` LLC figure on the machine you rent — likely `N=2048`+. Verify the
separation with plain `perf stat` **before** trusting any CacheLens output, so a null result
implicates the benchmark rather than sending you debugging the tool.

**`-O2` may optimize the demo away.** Not in the existing risk register. `matrix_bad`'s inner
loop is a reduction over a strided access; both GCC and Clang may vectorize it, and the
resulting access pattern is not the one the source implies. Check the emitted assembly, or build
the demo variants at `-O1` and show both. Do not drop the whole project to `-O0` — that changes
the cache behavior you are trying to demonstrate.

---

## 7. What to delete

All of the Python. Git history preserves it; a repo containing both a C++ tool and a
half-finished Python tool that does the same thing reads as indecision, and the handoff's
"narrow and finished beats broad and partial" applies to the tree itself.

Delete outright, independent of any other decision:

- `outputs/smoke_report/` — synthetic data presented as a real run (§5, Phase 0)
- `benchmarks/matrix_bad`, `matrix_good`, `pointer_chase` — Mach-O ARM64 binaries, unbuildable
  on the target platform, committed
- `benchmarks/*.dSYM/` — Mac debug bundles
- `{src/` — the literal-path residue of a brace expansion that ran under a shell without brace
  support; four empty nested directories
- `.DS_Store`, and a `.gitignore` entry for it
- `prompts/`, `src/cacheprof/llm/` — the cut LLM layer
- `src/cacheprof/dwarf/normalize.py`, `symbols.py` — one-line "placeholder for v2" docstrings

Worth carrying forward in some form:

- `analysis/aggregate.py`'s grouping logic and `report/markdown.py`'s layout — port the shape,
  not the code
- `tests/fixtures/` — useful only if you keep a Python cross-check; see below

**On keeping Python as a validation oracle:** tempting, and I would not. Cross-validating the
C++ sampler against `perf record` directly is stronger evidence than validating it against
another implementation you also wrote, and maintaining two pipelines is the scope creep the
handoff warns about. Validate against `perf`.

---

## 8. Relationship to the existing docs

`DESIGN.md` and `OVERVIEW.md` were written 2026-07-26, before the scope cut, and both are now
partly counterfactual. They should be moved to `docs/archive/` rather than deleted — the V2 IPC
design in particular is good work and reads well as documented-and-deliberately-not-built.

What is now false in them:

- `OVERVIEW.md` §2's hero output block features a `Diagnosis (LLM)` section; §3 makes portability
  ("Intel, AMD, and Arm") pillar #2 and a resume bullet; §7's M1–M4 plan runs to Sep 20 with a
  Python control plane throughout
- `DESIGN.md` §1.3 specifies ~3 hours of PEBS/IBS/SPE vendor detection to make the portability
  bullet honest — that bullet is cut, so this work is cut with it (but see §2.4: `precise_ip`
  negotiation survives, reframed)
- `DESIGN.md` §2.2–2.3 (process topology, TLV wire protocol) — no longer has a purpose

What survives intact and should be read as still-current:

- §1.1, the PMU hardware gate, and the interpretation of its results
- §4.2's staged approach to the ring buffer consumer, and its estimate that this takes a learner
  5–6 weeks rather than 3 — which is precisely why §2.3 above de-risks wraparound out of the
  shipping scope
- §2.4's `perf_event_open` attribute table, `enable_on_exec` reasoning, and the `read()`
  vs. mmap distinction (counting mode reads a number; sampling *requires* mmap)
- §2.4's ring-buffer consume loop, minus the wraparound step
- The honesty posture throughout — documented skid, visible lost samples, an explicit limits
  section. That is the most credible thing in the repo and none of it is affected by the cut.

---

## 9. Ship criteria, restated in architectural terms

Mapping the handoff's five criteria onto this design:

| Handoff criterion | Satisfied by | Gate |
|---|---|---|
| Runs on an arbitrary binary, ranked hotspots | §1 four-stage binary | Phase 2 |
| DWARF attribution verified against known-bad code | §4 libdw + hand-verified benchmark lines | Phase 2 |
| Case study, before/after, 2–3 benchmarks | §5 Phase 1 numbers + §6 fixed benchmark set | Phase 1 / 4 |
| README an NVIDIA engineer respects | Real output screenshot first, results table, §3 metric defense, honest limits | Phase 4 |
| Resume bullets with real numbers | Phase 1 output, independent of the tool | Phase 1 |

The one addition this document makes to the criteria list: **the ranking metric has to be
defensible, not just implemented.** Concentration ranking is the tool's only opinion, it is the
thing an interviewer will push on, and §3.3 is the answer to the push.
