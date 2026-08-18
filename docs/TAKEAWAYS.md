# Takeaways for Deep-Dives

Running log of debugging stories from building CacheLens, kept for interview deep-dives
("tell me about a bug you found in your own tooling"). Newest entries at the top. Each entry
should stand on its own — two or three sentences you could say out loud, plus the detail to
back it up if asked to go deeper.

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
