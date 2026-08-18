# Takeaways for Deep-Dives

Running log of debugging stories from building CacheLens, kept for interview deep-dives
("tell me about a bug you found in your own tooling"). Newest entries at the top. Each entry
should stand on its own — two or three sentences you could say out loud, plus the detail to
back it up if asked to go deeper.

---

## A null result is still a result: the governor test corroborates the IPC evidence

**When:** Phase 1 drift investigation, governor isolation (Set 1 vs Set 2), 2026-08-18.

**The finding:** `performance` vs `powersave` governor changed wall time by 0.07% (bad) and
0.11% (good) — indistinguishable from noise. That's not "the test found nothing" — it's
evidence about *what kind of bottleneck this is*. If the matrix-multiply pair's speedup came
from a compute-bound component (more arithmetic throughput, better pipelining), clock frequency
would matter, and `performance` vs `powersave` would show a real gap. It didn't, which means
core frequency isn't the constraint — consistent with, and independent corroboration of, the
IPC evidence already on record (cycles/instruction of 0.636 vs 0.268, i.e. IPC ~1.58 vs ~3.77):
matrix_bad isn't slow because the CPU is under-clocked, it's slow because it spends its cycles
stalled waiting on memory, and clocking the core higher doesn't un-stall a cache miss.

**Why it's worth keeping:** a 0% result from one experiment (governor) and a 3-6x IPC gap from
a completely different measurement (cycles/instruction) point the same direction through two
independent methods. That agreement is stronger evidence than either alone, and it only shows
up if you report the null result instead of treating "no effect" as "nothing to write down."

---

## Background load biases this measurement in the favorable direction

**When:** Phase 1 drift investigation, quiet-vs-noisy comparison, 2026-08-18.

**The finding:** the noisy run (Firefox + VSCode competing for L3) reported a *larger* speedup
than the quiet one — 2.20x vs 2.08x — not a smaller one. This isn't symmetric noise. L3
contention from other processes degrades matrix_bad (which already thrashes the cache) more
than matrix_good (which mostly doesn't need much L3 to begin with), so background load widens
the gap between them rather than just adding jitter to both. The direction matters: anyone
reproducing this benchmark on a machine they haven't quiesced will tend to see a *better*
number than the true one, not a worse one — the error is one-directional and favorable, which
is exactly the kind of error that survives unnoticed because nobody double-checks a result that
already looks good.

**Rule going forward:** state this plainly wherever the speedup number is reported (README,
Gate 6) — not as a footnote, since a one-directional bias toward the more impressive number is
the one a reader has the least reason to go looking for on their own.

---

## A performance result without a recorded environment isn't reproducible — not even by you

**When:** Phase 1 drift investigation, 2026-08-18. Rebuilding the benchmarks for Gate 4
(same flags, same source) produced numbers ~5-10% off Phase 1's original measurement, in the
*favorable* direction (2.09x -> 2.20x speedup). Investigated rather than adopted.

**The finding:** the drift traced entirely to background load (Firefox + VSCode + their
subprocesses, drawing 20-30%+ combined CPU) present during the later run and absent — or at
least unrecorded — during Phase 1. Isolated with a controlled comparison: same quiet machine,
`performance` vs `powersave` governor, n=5 each. Governor effect: +0.07%/+0.11%, noise-level.
Quiet-machine result vs. Phase 1's original: +0.46%/+0.97%, within normal run-to-run variance.
The ~10% "regression" was almost entirely explained by two browser/editor processes that had
nothing to do with the benchmark.

**Why this couldn't be resolved faster than it was:** Phase 1's result file recorded the CPU,
L3 size, RAM config, kernel, and `perf_event_paranoid` — real machine facts — but not the
*load* the machine was under at measurement time. That's the gap: a performance number is a
measurement of a system under some condition, and "condition" includes everything else
competing for the same cache and cores, not just the hardware spec sheet. Without that
recorded, there was no way to tell "the machine changed" from "the code changed" after the
fact — including for the person who ran the original measurement.

**The check that mattered first:** instruction-retired counts between the drifted and original
runs matched to within 0.04% (matrix_bad) and 0.008% (matrix_good) — while cycles-per-
instruction had moved 5-10%. That one comparison is what separated "the binaries changed"
(it didn't — same instructions, same count) from "the machine changed" (cycles for the same
instructions went up — classic contention/throttling signature) before any governor or process
list was even inspected. Any future drift investigation should run this check first: if
instruction counts match, the code is innocent and the search moves to the environment.

**Rule going forward:** `scripts/measure_baseline.sh` now captures governor, load average,
processes above 1% CPU, THP, `perf_event_paranoid`, kernel version, compiler version, build
flags, and core frequency alongside every `perf stat` run. A result without that block attached
is not a result — it can't be told apart from noise, including by whoever measured it.

---

## A fabricated headroom number, sitting next to the real one that contradicted it

**When:** Gate 3 report, matrix_good period-headroom summary, 2026-08-18.

**The bug:** reported "roughly 3.4x headroom" between the default sample period (100003,
~742 samples) and the practical throttle floor (~220, "~1500 samples"). The 1500 figure was
not computed from anything — the actual bisection output, printed a few tool calls earlier in
the same turn, read `period=220 samples=372291` and `period=220 samples=370812`. The real
headroom is `74M / 220 ≈ 336,000+` samples, i.e. **~500x**, not 3.4x — off by more than two
orders of magnitude, in the direction that made the tool's own sampling headroom look far more
constrained than it actually is.

**Why it happened:** the aggregate miss count (`counts.value`, printed on every run) and the
per-period sample count were both sitting in the terminal output already — the number could
have been checked with a division, not invented. It wasn't checked because the surrounding
paragraph read as a wrap-up summary rather than a claim that needed verifying, and summary
prose is exactly where an unverified number slides through.

**Why it's a good story, and the rule it produces:** this is the same failure class as the
0/0-multiplexing bug two entries down — a plausible-sounding number presented without checking
it against data already on hand — except that one was a code bug and this one was me, in the
same session, making the identical mistake in prose instead of C++. **Any derived figure
(a ratio, a headroom estimate, a percentage) must be recomputed from the raw counts it claims
to come from before it's reported, not estimated from a "feel" for the numbers already
discussed.** If the raw counts are on screen, division is cheaper than being wrong.

---

## `exclude_kernel=1` doesn't mean every sample is in userspace

**When:** Gate 2 validation, immediately after the precise_ip finding above, 2026-08-18.

**The finding:** with `exclude_kernel=1` set, a small fraction of samples (0.01%–0.13% across
ten n=5 runs of both benchmarks) still carry a canonical kernel-space IP
(`>=0xffff800000000000`). This is a *second, distinct* consequence of `precise_ip=0` — not
the same thing as the source-line skid recorded above. `exclude_kernel` controls which
*events increment the counter* (only user-mode cache misses count). It does not constrain
*where the PMI captures the PC* when the counter overflows. With no PEBS-equivalent, interrupt
delivery has latency, and that latency can carry the sample past a user→kernel privilege
transition (a syscall entry path, most likely) before the RIP gets recorded — so a correctly
user-mode-attributed *event* can still be captured at a kernel-mode *address*.

**Why it's not alarming, and why it's still worth recording:** the two kernel addresses
observed (`0xffffffffa5e00ef0`, `0xffffffffa751ba28`) are the *only* two seen across all ten
runs of both benchmarks — not a scatter of random addresses, a tight cluster of exactly two,
consistent with a specific, repeatable kernel entry path rather than noise. And the rate stays
three orders of magnitude under the 1% halt threshold in every run. But it's a clean
illustration that skid isn't just "the wrong source line" — at the boundary, it can be "the
wrong privilege level entirely," which is why sample classification (target / other-user /
kernel / unclassifiable, every sample counted, none dropped) is now a permanent part of the
report rather than something bolted on only when it looks like a problem.

---

## Zen 4 has no PEBS-equivalent for `cache-misses`: `precise_ip` stuck at 0

**When:** Gate 2, precise_ip negotiation, before any ring-buffer code was written, 2026-08-18.

**The finding:** requesting `precise_ip=2` or `precise_ip=1` on `PERF_TYPE_HARDWARE` /
`PERF_COUNT_HW_CACHE_MISSES` in sampling mode both fail with `ENOENT` — not `EACCES`/`EPERM`
(which would mean a permissions problem) and not `EINVAL` (a malformed request). `ENOENT`
specifically means the running PMU has no precise-sampling implementation of this event at
all. Only `precise_ip=0` is accepted. This is expected, not a bug: AMD Zen 4 has no
PEBS-equivalent facility for this event, so there is nothing for the kernel to offer above
skid level 0 — the sampled IP is wherever the PMI landed after counter overflow, with
unbounded skid, not the instruction that caused the miss.

**Consequence:** every IP this sampler records from here on is skidded by an unknown,
unbounded amount. Downstream attribution (Gate 4 onward) has to be validated *for skid
specifically* — agreement with `addr2line` on a sampled address only proves the DWARF lookup
is correct for whatever address was recorded, it says nothing about whether that address is
the right one. Gate 4 adds a separate skid-characterization step (source-line distribution
within the hot function, not just top-line agreement) precisely because of this gap.

**Deferred, deliberately:** AMD IBS (Instruction-Based Sampling, `ibs_op`/`ibs_fetch`) is the
AMD-side path to instruction-level precision — the rough equivalent of what PEBS gives on
Intel. It requires a different `perf_event_open` configuration entirely (a dynamic PMU type
discovered via `/sys/bus/event_source/devices/ibs_op`, not `PERF_TYPE_HARDWARE`) and is out
of scope for this pass. Proceeding with `precise_ip=0` and documenting the skid was a decision
made explicitly, not a default reached by not checking.

---

## The 0/0 read as "multiplexing 0.0", and a silent exit 0

**When:** increment 1 (counting-mode `perf_event_open` harness), 2026-08-16.

**The bug:** when the target command failed to exec (bad path, missing binary), the child
process exited via `_exit(127)` having never reached `execve`. Since the perf counter was
opened with `enable_on_exec=1`, it never actually turned on — `time_enabled` and `time_running`
both read back as `0`. The reporting code computed `time_running / time_enabled`, saw `0/0`,
and printed "multiplexing fraction: 0.0000" with a warning about PMU contention — which is a
real-sounding explanation for the wrong problem. Worse, the program's own exit code was
hardcoded to `0` regardless of what the child did, so a completely failed run reported success.

**Why it's a good story:** CacheLens exists to catch tools that report a number without
checking whether the number means anything. This is that exact failure class, self-inflicted,
caught by testing the unhappy path (a nonexistent binary) rather than only the two working
benchmarks. The fix distinguishes "counter never armed" from "counter armed but multiplexed"
as two different failure modes with two different messages, and propagates the child's real
exit status instead of always returning 0.

**One-line takeaway:** a ratio computed from two zeros will happily lie to you in the units of
a completely different failure mode — test the path where setup never completed, not just the
path where the measurement came out noisy.

---

## Gate 1: cachelens read 7.3% low on matrix_good, and a single run wasn't enough to tell why

**When:** validating increment 1 against the Phase 1 `perf stat` baseline, 2026-08-16.

**The bug (or: the thing that looked like one):** a single cachelens run against `matrix_good`
came in ~11% under the Phase 1 `perf stat` mean, while `matrix_bad` came in ~5% over — opposite
directions, which rules out a constant measurement-overhead explanation (that would push both
readings the same way). Comparing a single run to an n=5 mean isn't a real comparison anyway,
so the first move was re-running both tools n=5, same boot, same event.

At n=5, `matrix_bad` was already fine (+1.7% off perf's mean, inside perf's own ±1σ). But
`matrix_good` held a **consistent** −7.3% offset with far tighter run-to-run variance than
`perf stat`'s own runs of the identical binary (0.13% vs 2.29% stdev) — a consistent offset
paired with suspiciously low noise is the signature of a scope difference, not measurement
noise, so it was worth chasing before building Phase 2 on top of it.

**Root cause:** cachelens's `perf_event_attr` sets `exclude_kernel=1` / `exclude_hv=1`
deliberately (§2.2 of the architecture doc — works without `CAP_PERFMON`). Stock
`perf stat -e cache-misses` counts kernel-space misses too, by default. That kernel-side
contribution — mmap/brk syscalls allocating the ~132 MB working set, the write() behind
`printf` — is roughly constant in absolute terms between the two binaries. It's noise-floor
negligible against `matrix_bad`'s ~2.3B misses, but it's a material fraction of
`matrix_good`'s much smaller ~74M. Re-running the baseline with `perf stat -e cache-misses:u`
(user-space only, matching scope) collapsed the gap on both binaries (+1.1% / −0.2%, both
inside perf's ±1σ) and dropped perf's own variance to match cachelens's — confirming the
extra variance in the default measurement *was* kernel-side jitter, not cachelens undercounting.

**Why it's a good story:** it's a case where the honest move was refusing to accept "close
enough" from n=1, and the sign/magnitude asymmetry (not just the raw percentage) was the clue
that pointed at a scope mismatch rather than run-to-run noise. It also produced a concrete,
falsifiable prediction (`:u` should close the gap) that either would have confirmed a real bug
or explained it away — and it explained it away, which is the good outcome, but only because
it was checked rather than assumed.

**One-line takeaway:** when two measurements of the same thing disagree in opposite directions
across two workloads, look for a scope difference before you look for a bug — and a suspiciously
*low* variance compared to the reference measurement is as much a signal as a suspiciously high
one.
