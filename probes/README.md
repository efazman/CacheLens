# Gate 7 — Phase 0 probes

Four throwaway programs, kept in tree as evidence rather than deleted, each closing exactly one
blocking unknown from `docs/GATE7_PLAN.md` §3.1. None of this is `cachelens` code; each probe is
independent, minimal, and answers one question with a number.

Run all four with `scripts/run_gate7_probes.sh`, which appends an environment block and writes
`results/gate7_probes.txt`.

## P1 — `p1_percpu_open.c` (U1)

Does a per-CPU task event (`pid = getpid(), cpu = i`) open and mmap at this machine's configured
`kernel.perf_event_paranoid`? Opens one event per online CPU, reports pass/fail per CPU by errno
name. A pass on every CPU means `pid>0, cpu>=0` events are permitted the way `find_get_context`'s
`task != NULL` gate suggests they should be, regardless of what the man page's system-wide-focused
paranoid language implies.

## P2 — `p2_enable_on_exec.c` (U2), with victim `p2_victim.c`

Does `enable_on_exec=1` arm exactly at the `execve` boundary on a `cpu>=0` event, the same
guarantee `src/main.cpp` already relies on for `cpu=-1`? Three cases:

- **A** — child execs the victim on cpu 0; counters should be non-zero afterward.
- **B** — child is killed while still `SIGSTOP`'d, before any exec; counters should read exactly
  zero, proving nothing armed early.
- **C** — same as A but on the last online CPU, so the result isn't CPU-0-specific.

Build `p2_victim` separately; pass its path as P2's one argument.

## P3 — `p3_mlock_budget.c` (U3)

For each candidate ring size (1 MiB down to 64 KiB), attempts to open+mmap `2 * nproc` rings
(2 events x per-CPU, matching U5 Option A's shape) and reports how many succeeded before the
first failure and what errno stopped it. Also prints `perf_event_mlock_kb`, `RLIMIT_MEMLOCK`, and
`nproc` so the arithmetic prediction in `GATE7_PLAN.md` U3 can be checked against what actually
happened rather than trusted.

## P4 — `p4_false_sharing.cpp` (U4) — the gating probe

The one unknown that can end the Gate 7 edition. Two threads pinned to two distinct physical
cores (CPU 0 and CPU 1 on this machine — see `topology/core_id`, not CPU 0/6 which are SMT
siblings of the same core) each increment their own counter a fixed number of times.
`CACHELENS_PAD_INDICES=0` puts both counters on one cache line (true sharing); `=1` pads them
apart. One source, two builds, so the two differ in exactly one thing.

Wrap both builds in `perf stat -r 5 -e cache-misses,cache-references,instructions,cycles` and
compare. Three possible outcomes, spelled out in `GATE7_IMPLEMENTATION.md`'s Phase 0 section —
including the one where the wall-clock gap exists but the generalized events don't see it, which
is exactly the risk U4 names and would send the project to U14's fallback instead.

`objdump -d` both builds before trusting any of it: the third outcome (no wall-clock gap at all,
meaning the compiler or hardware hid the sharing) has to be excluded first.
