# Takeaways for Deep-Dives

Running log of debugging stories from building CacheLens, kept for interview deep-dives
("tell me about a bug you found in your own tooling"). Newest entries at the top. Each entry
should stand on its own — two or three sentences you could say out loud, plus the detail to
back it up if asked to go deeper.

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
