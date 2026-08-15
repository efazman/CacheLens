# CacheLens — Design Document

**Status:** design baseline, written 2026-07-26
**Scope:** V1 completion plan, V2 (C data-plane) architecture, interview prep map, risk register
**Audience:** the author, and any systems reviewer reading the repo cold

---

## 0. Read this first — three things that change the plan

The handoff describes a project that is "architecturally sound but has never run against real hardware." That is accurate. But three facts, verified against the actual tree, reorder the priorities.

### 0.1 The timeline in the handoff has already expired

The handoff targets "recruiting prep by late July 2026." Today **is** late July 2026, and the project has completed **zero** live profiling runs. The phased plan (V1 ship 2–3 weeks → V1 polish 1 week → V2 3–5 weeks → V2 polish 1–2 weeks) totals 7–11 weeks and lands in **late September–October 2026**.

This is recoverable, because the real deadline is not "late July" — it is the application windows:

| Target | Applications typically open | Drop-dead for resume |
|---|---|---|
| HFT (Jane Street, Optiver, HRT) | Aug–Sep 2026 | **~2 weeks** |
| FAANG+ SWE intern | Aug–Oct 2026 | ~4 weeks |
| Cloud infra (AWS, Datadog) | Sep–Nov 2026 | ~6 weeks |

**Re-baselined plan:**

- **Weeks 1–2 (now → ~Aug 9): V1 real, honest, and demo-ready.** This is the only hard deadline. Bullets 1 and 2 go on the resume with measured numbers.
- **Weeks 3–4 (~Aug 23): V1 polish + integration tests.** Resume is final for the HFT wave.
- **Weeks 5–10 (Sep–early Oct): V2 data plane.** Bullet 3 gets added mid-cycle, in time for later FAANG/cloud deadlines and for interviews, which run Oct–Dec.

**Bullet 3 does not go on the resume until the C agent produces real samples.** An interviewer who asks "walk me through your `perf_event_open` call" and gets a vague answer does more damage than a two-bullet resume.

### 0.2 `pip install -e .` fails today

`pyproject.toml` declares:

```toml
build-backend = "setuptools.backends._legacy:_Backend"
```

That module does not exist (`ModuleNotFoundError: No module named 'setuptools.backends'`). Every setup path in the README begins with `pip install -e ".[dev]"`, so a reviewer following the README hits a build error on their first command. This is a one-line fix (`setuptools.build_meta`) and it is the single highest-leverage change in the repo.

### 0.3 Resume bullet #2 currently overclaims

The bullet says:

> "cross-architecture capability negotiation (Intel PEBS, AMD IBS, Arm SPE)"

`capabilities.py` contains **no architecture detection of any kind**. It does not read `/proc/cpuinfo`, does not check vendor, and does not probe for PEBS, IBS, or SPE. What it actually does (`capabilities.py:115`) is run `perf mem record -o /dev/null -- /bin/true` and set one boolean based on the exit code. The three architecture names appear only in a docstring and a comment.

This will not survive an interview. "How do you detect IBS?" has no answer in the current code. Section 1.3 specifies the ~40 lines that make the claim true. This is not optional polish — it is the difference between a defensible bullet and one that collapses under one follow-up question.

---

## 1. V1 completion plan

### 1.1 Step 0 — Prove the PMU exists before writing any code

**This gates the entire project and must happen today.**

The handoff states "DigitalOcean droplet (bare metal PMU access confirmed)." Treat this as unverified. Standard DigitalOcean droplets are KVM guests, and KVM **does not expose a virtual PMU to guests by default**. On such a guest, `perf stat -e cache-misses` returns `<not supported>` for every hardware event, and `perf record -e cache-misses` fails outright. If this is the case, no amount of Python work produces a demo.

Run exactly this on the droplet, in order, before anything else:

```bash
# 1. Does the kernel expose a hardware PMU at all?
ls /sys/bus/event_source/devices/          # want to see: cpu (or cpu_core/cpu_atom)
cat /sys/bus/event_source/devices/cpu/type 2>/dev/null

# 2. Do hardware events actually count? THIS IS THE DECISIVE TEST.
perf stat -e cycles,instructions,cache-misses,cache-references /bin/true

# 3. Can we sample (not just count)?
perf record -e cache-misses -F 997 -o /tmp/t.data -- /bin/true && perf script -i /tmp/t.data | head

# 4. What precise-sampling facility exists?
ls /sys/bus/event_source/devices/ | grep -Ei 'ibs|spe'
perf mem record -o /tmp/m.data -- /bin/true
```

**Interpreting step 2 — the only result that matters:**

- Real integer counts for `cache-misses` and `cache-references` → **proceed**.
- `<not supported>` or `<not counted>` on the hardware events → **the droplet is unusable**. Stop and switch hosts.

**If the PMU is absent, in order of preference:**

1. **Bare-metal rental** — Hetzner (AX-series, ~€40/mo, hourly setup), OVH, or Vultr Bare Metal. Full PMU, including IBS on AMD. Best price/performance for this project.
2. **AWS `*.metal` instances** — `c5.metal`, `m5.metal`, `c6i.metal`. Real PEBS. Expensive per hour (~$4–5/hr) but you only need a few hours total; run them on demand and stop them.
3. **A physical Linux machine** — any x86 laptop/desktop with Ubuntu, even dual-boot. Zero cost, full PMU.
4. **A local Linux VM is NOT an option** — same vPMU problem as the droplet, plus VirtualBox/UTM never expose one.

Note that Apple Silicon Macs cannot run this project at all: no `perf`, and the ARM PMU is not accessible to userspace. The committed benchmark binaries are Mach-O ARM64 (the `benchmarks/*.dSYM` directories with `Relocations/aarch64` prove they were built on the Mac). **They must be rebuilt on Linux and removed from git.**

Also set, and record in the README:

```bash
sudo sysctl kernel.perf_event_paranoid=1     # default is 2 on Ubuntu
sudo sysctl kernel.kptr_restrict=0
```

`paranoid=2` still permits profiling your own child process with `exclude_kernel=1`, which is what both V1 and V2 do — so the tool works unprivileged. Say this in the README; it is a detail reviewers notice.

### 1.2 Step 1 — Unblock the build (day 1, ~1 hour)

Fix in this order:

1. **`pyproject.toml` build backend** → `build-backend = "setuptools.build_meta"`. Blocks everything.
2. **Move `openai` to an extra.** The stated constraint is "click is the only required dep for V1 core," and `--llm` is opt-in, but `openai>=1.0` is currently a hard dependency in both `pyproject.toml` and `requirements.txt`. Move it to `[project.optional-dependencies] llm = ["openai>=1.0"]`. `llm/explain.py` is already imported lazily inside the `if config.enable_llm` branch (`runner.py:117`), so nothing else changes.
3. **Drop `rich`.** `report/terminal.py` does not import it. Remove from `pyproject.toml` and `requirements.txt`, and fix the README, which still claims "Terminal (Rich)."
4. **Delete the stray `{src` directory tree.** It is the residue of a brace expansion that ran under a shell that did not support it (`{src/cacheprof/{perf,dwarf,...},benchmarks,...}` became a literal path). It contains no files, only empty directories. A reviewer who sees it concludes the author does not read their own `git status`.
5. **Remove committed build artifacts:** `benchmarks/matrix_*`, `benchmarks/pointer_chase` (ARM binaries), all `*.dSYM/`, all `__pycache__/`, and `.DS_Store`. Add to `.gitignore`.
6. **Delete `outputs/smoke_report/` and `outputs/test_runs/`.** Synthetic data committed to the repo actively misleads — a reviewer cannot tell it from a real run. Replace later with one genuine run under `docs/examples/`.
7. **Delete `dwarf/normalize.py` and `dwarf/symbols.py`.** Both are one-line docstrings saying "placeholder for v2." Empty placeholder files read as abandoned scaffolding. Create them when they do something.

### 1.3 Step 2 — Make capability negotiation real (day 1–2, ~3 hours)

This is the fix that makes bullet 2 honest. Restructure `capabilities.py` around an explicit, ordered negotiation.

**Add architecture detection.** The probes are all filesystem reads, no subprocesses needed:

| Facility | Detection | Meaning |
|---|---|---|
| Vendor | `/proc/cpuinfo` → `vendor_id` (`GenuineIntel` / `AuthenticAMD`), or `uname -m` = `aarch64` | Which branch to take |
| Intel PEBS | `/sys/bus/event_source/devices/cpu/events/mem-loads` exists | `perf mem` load latency available |
| AMD IBS | `/sys/bus/event_source/devices/ibs_op/` exists | IBS Op sampling available |
| Arm SPE | `/sys/bus/event_source/devices/arm_spe_0/` exists | Statistical Profiling Extension |
| PMU present at all | `/sys/bus/event_source/devices/cpu/` (or `cpu_core`) exists | If absent → virtualized, no hardware events |

Extend `CapabilityReport` with `cpu_vendor`, `cpu_model`, `precise_facility: Optional[Literal["PEBS","IBS","SPE"]]`, and `pmu_present: bool`. Print the facility in `format_capability_report`. Now the output literally says `precise facility  IBS (AMD)`, and the bullet is backed by code you can point at.

**Fix the two probe bugs while you are here:**

- `_probe_events` (`capabilities.py:94`) does `event in stdout` against the entire `perf list` output. `"LLC-loads" in stdout` is true whenever `LLC-load-misses` is present, and descriptive text can match too. Parse `perf list --json` where available, otherwise match on line-leading tokens.
- `_probe_mem_sampling` (`capabilities.py:127`) writes `perf.data` to `/dev/null`. Some perf builds error on a non-seekable output file, which produces a **false negative** — you fall back to counter sampling on a machine where `perf mem` works fine. Write to a temp file and delete it.

**Make the negotiation ladder explicit** and log each rung with its reason. The ladder is the resume bullet made concrete:

```
PMU absent                        → hard fail, actionable message ("virtualized host, no vPMU")
precise facility + perf mem works → MEM_SAMPLING     (PEBS / IBS / SPE — low skid)
LLC-load-misses available         → COUNTER_SAMPLING (event-specific, moderate skid)
cache-misses available            → COUNTER_SAMPLING (generic fallback, moderate skid)
none                              → hard fail with the perf list output attached
```

Store the chosen rung *and the reason it was chosen* in the report. "Why did it pick this mode on this machine?" then has a written answer in the JSON output, not a shrug.

### 1.4 Step 3 — Two correctness bugs that will corrupt the first real run

Both are invisible in unit tests (which feed synthetic fixtures) and will only appear against live data. Fix before the first run, or you will spend a day debugging the wrong layer.

**Bug A — the resolver ignores which binary a sample came from.**

`resolve_samples` (`dwarf/resolver.py:23`) resolves *every* sample address against the target binary:

```python
addr_map = _batch_resolve(unique_addrs, str(binary), config.addr2line_bin)
```

A real `perf record` run captures samples from `libc.so.6`, `ld-linux-x86-64.so.2`, and the kernel — the parser already extracts this into `PerfSample.dso` (`perf/parser.py:125`), and then nothing uses it. Feeding a libc address to `addr2line -e ./matrix_bad` returns `??:0` at best, and at worst silently resolves to a *wrong* line in your source that happens to occupy the same address range.

**Fix:** filter to samples whose `dso` resolves to the same file as the target binary before resolution, and count the excluded samples separately. This matters for reporting integrity too — `unresolved_sample_rate` currently blames your DWARF setup for samples that were never resolvable in principle. Report them as three buckets: `resolved`, `unresolved_in_target`, `outside_target`. The middle bucket is the only one that indicates a real problem, and it is the one the 0.35 warning threshold should apply to.

**Bug B — the perf script parser breaks on C++ symbols.**

`_parse_line` (`perf/parser.py:98`) finds the event token by taking the **last** regex match of `([A-Za-z][\w.-]*):` in the text after the timestamp:

```python
event_matches = list(_RE_EVENT.finditer(tail))
event_match = event_matches[-1]
```

The intent was to tolerate extra fields before the event. But the search region also contains the symbol name, and the benchmarks are C++ compiled at `-O2`, so perf emits demangled names containing `::`. On a line like:

```
matrix_bad 1234/1234 [001] 98765.432100: cache-misses: 401f3a std::vector<double>::operator[]+0x12 (/home/u/matrix_bad)
```

the last match is `std:`, not `cache-misses:`. The parser sets `event="std"`, then searches for the address in the *remaining* text (`:vector<double>::operator[]+0x12 (...)`), which no longer contains the IP. The sample is silently attributed to a bogus event with `address=0`, and `resolve_samples` skips it (`if s.address` at `resolver.py:43`). Result: a large, silent, symbol-dependent sample loss — and it hits `matrix_bad` and `pointer_chase` harder than `matrix_good`, which will look like a real profiling result.

**Fix:** anchor the event token to immediately after the timestamp rather than scanning for the last match. The format is stable: `<timestamp>: <event>: <ip> <symbol> (<dso>)`. Take the **first** token after the timestamp, and add a fixture with a `std::`-containing symbol to `test_perf_parser.py` so this cannot regress.

Two smaller items to fix at the same time:

- **Pin the perf script field set.** `run_perf_script` should pass an explicit `-F comm,pid,tid,cpu,time,event,ip,sym,dso`. Right now the output format depends on the kernel's defaults, which is exactly the cross-version variation the parser is fighting. Pinning the fields converts a parsing problem into a configuration problem.
- **Read the 0.35 threshold from config.** `report/terminal.py` hardcodes it while `config.unresolved_warn_threshold` exists (`config.py:24`). Trivial, but a reviewer grepping for the constant finds two sources of truth.

### 1.5 Step 4 — First real run and the honesty pass (day 2–4)

Run in this order, because each step isolates a different failure mode:

1. `perf stat -e cache-misses,cache-references,instructions,cycles ./matrix_bad` — confirms counters move. Record the numbers.
2. Same for `matrix_good`. **`matrix_bad` must show a materially higher miss rate.** If it does not, the benchmarks are not doing what the README claims — check that `N` is large enough that the working set exceeds LLC (for a 32MB LLC, `N=1024` doubles ≈ 8MB per matrix is borderline; you may need `N=2048`+). Fix the benchmark before blaming the tool.
3. `cacheprof check ./matrix_bad` — capability output with the real facility name.
4. `cacheprof profile ./matrix_bad` — the full pipeline.
5. Verify the top hotspot line against the source **by hand**. It should be the inner-loop access. Cross-check with `perf annotate -i perf.data`.
6. Repeat for `matrix_good` and `pointer_chase`.

**The honesty pass.** At `-O2`, with counter sampling, the reported line will sometimes be off by one or two from the "textbook" answer. This is PMU skid, it is expected, and it is a feature of the writeup, not a bug to hide. Document the observed skid in the README. An interviewer who sees "reported line is typically within ±2 lines of the true access under counter sampling; PEBS reduces this to 0" learns that the author understands their instrument. One who sees a suspiciously perfect result assumes the numbers are fabricated.

If line attribution is unusably noisy, add a `-O1` build variant of the benchmarks for the demo and show both. Do not silently switch the whole project to `-O0` — that changes the cache behavior you are trying to demonstrate.

**Capture real numbers for the resume.** You need actual figures for both bullets: sample counts, miss rates for good vs bad, resolution rate, wall-clock. Write them into `docs/examples/` alongside the real JSON reports.

### 1.6 Step 5 — Integration tests (week 2)

Current suite: 22 tests across 7 files (the handoff says 23/8; minor drift). All are pure-function tests over synthetic fixtures. They pass today and would have caught neither of the bugs in §1.4, because the fixtures do not contain C++ symbols or foreign-DSO samples.

Add three tiers:

**Tier 1 — runner partial-failure tests (highest value, no hardware needed).** This is the resume bullet you most need to defend. Use `monkeypatch` to inject failures at each stage boundary and assert the invariant:

- capability check fails → `capabilities.txt` written, JSON report exists, exit non-zero, no crash
- `perf stat` raises → capability artifact survives, partial JSON has `stat_result: null`
- sample collection raises → stat results survive in the JSON
- `perf script` raises → `perf.data` retained
- parse yields zero samples → partial report, clear "ran too briefly" warning
- LLM stage raises → **full report still written** (this path is already correct in `runner.py:125`; lock it in)

Assert on the run directory contents, not just the return value. The claim is "saves completed work," and the artifact on disk is the evidence.

**Tier 2 — CLI tests** via Click's `CliRunner`: `check` exit codes, `--llm/--no-llm` reaching `Config`, missing-binary handling, `profile` on a binary without debug info.

**Tier 3 — one end-to-end hardware test**, marked `@pytest.mark.hardware` and skipped when `perf` is absent or the PMU is missing. Compiles a tiny C file with a known-hot loop, profiles it, asserts the top hotspot lands in the expected line range. Skipped in CI, run manually on the droplet. This is the test that proves the tool works.

Also add a `tests/fixtures/perf_script_cpp_symbols.txt` fixture with `std::`-containing demangled names.

### 1.7 The README — structure for a systems reviewer

A reviewer gives the repo 90 seconds. Optimize for that. Order matters; put proof before prose.

1. **One-sentence description + a real terminal screenshot of actual output.** Not a diagram, not ASCII art — a screenshot of the tool finding the `matrix_bad` hotspot, with real numbers. This is the single highest-value element in the README.
2. **The result table.** Three benchmarks × (LLC miss rate, top hotspot line, samples collected). Measured, not expected. This is what makes the project look real.
3. **Quick start that works in 5 minutes**, verified by pasting it onto a fresh droplet. Include the `perf_event_paranoid` step and the PMU precondition — with an explicit note that cloud VMs generally lack a vPMU, which shows you know why it might fail on the reader's machine.
4. **Architecture diagram** — the 10 stages, with the seam marked. Mark which stages are V1-only (`perf record` → `perf script` → text parse) versus shared, because that seam is the V2 story.
5. **"How it decides what to measure"** — the capability ladder, with sample output from two different machines if you can get them (Intel and AMD ideally). This is bullet 2 made visible.
6. **"What this tool does not do"** — a short, honest limits section: skid under counter sampling, no PIE support, no inline frame expansion, single-binary attribution only, heuristic source signals. Counterintuitively this *increases* reviewer trust more than any feature list. It signals you know the boundary of your own instrument.
7. Design decisions with rationale (why `addr2line` over `pyelftools`, why signals stay numeric, why timestamped run dirs).
8. Project structure, testing, LLM setup — last, they are reference material.

Cut from the current README: the "Terminal (Rich)" claim (false), and the unqualified "PEBS/IBS/SPE" mention until §1.3 lands.

### 1.8 Defer to after V1 ships

- `_TEMPLATE_PATH` `../` chain in `llm/prompt.py` (known issue #2). Real, but only bites on `pip install`, and there is already an inline fallback. Fix with `importlib.resources` during polish week.
- Inline frame expansion (`addr2line -i`).
- PIE support.
- Multi-DSO resolution (resolving libc addresses against libc's debug info) — for now, correctly *excluding* them (§1.4) is sufficient and defensible.

---

## 2. V2 architecture — data plane / control plane split

### 2.1 The seam, and why this refactor is cheap

The critical architectural fact: **V1's pipeline already has a clean seam at `list[PerfSample]`.**

```
STAGES 1-5  (V1: perf record → perf script → regex parse)      ← replaced by the C agent
─────────────────────────── list[PerfSample] ────────────────────────────── the seam
STAGES 6-11 (addr2line → aggregate → rank → snippet → signals → report → LLM)  ← unchanged
```

Everything downstream of parsing consumes `PerfSample` objects and knows nothing about their origin. So V2 replaces stages 1–5 with a new producer that emits the same dataclass, and stages 6–11 are reused **verbatim**. No changes to `resolver.py`, `aggregate.py`, `ranking.py`, `signals.py`, or any report module.

This is worth stating explicitly in an interview: it is the payoff of having defined every cross-module struct in one place (`models.py`), and it is why a "rewrite the collector in C" project is a 5-week task and not a rewrite.

**A note on framing bullet 3.** The bullet says the agent replaces "CLI subprocess orchestration," which is the honest and defensible claim. Be careful not to drift into claiming the C agent samples *faster than perf*. It will not — `perf record` is mature, highly optimized C, and it will beat a first C agent at raw sampling. The real, measurable win is **end-to-end pipeline latency**: V1 writes `perf.data` to disk, spawns `perf script` to render it to text, and re-parses that text with Python regex. V2 deletes all three. That is where the numbers come from, and it is a more interesting engineering story anyway ("I found the bottleneck in my own tool's data path, not in the kernel's").

### 2.2 Process topology and who owns the socket

```
  Python (control plane)                    C agent (data plane)
  ─────────────────────                     ────────────────────
  1. bind() + listen() on
     <run_dir>/agent.sock
  2. fork/exec agent ─────────────────────► 3. connect()
                                            4. fork()
                                                 └─ child: block on sync pipe
                                            5. perf_event_open(child_pid)
                                            6. mmap ring buffer
                                            7. release child → execvp(target)
  8. accept(), read frames ◄──────────────  9. poll() → drain ring → batch → write()
  10. decode → PerfSample                   11. child exits → final drain
  12. stages 6-11 unchanged                 13. send STATS + EOS, exit
```

**Python binds and listens first; the agent connects.** This ordering is deliberate. If the agent created the socket, Python would need a retry/backoff loop to wait for it to appear, with an ambiguous timeout. With Python as the listener, the socket exists before the agent starts, so `connect()` either succeeds immediately or fails for a real reason. Python also already owns the run directory, so the socket lives in it and is cleaned up by existing logic.

Socket path: `<run_dir>/agent.sock` — filesystem-namespaced, not abstract. Abstract sockets (Linux-only, leading NUL) avoid filesystem cleanup but are invisible to `ls` and `ss -x` during debugging. For a project whose value is demonstrating you can debug systems code, choose the visible one.

### 2.3 Wire protocol

**Transport:** `AF_UNIX`, `SOCK_STREAM`. Stream (not `SOCK_DGRAM` or `SOCK_SEQPACKET`) because it gives kernel-level flow control for free — if Python falls behind, the socket buffer fills, the agent's `write()` blocks, and the agent stops draining the ring buffer. Backpressure then manifests as counted `PERF_RECORD_LOST` events rather than silent memory growth. Stream requires you to implement framing, which is the point: framing is what makes this a protocol design question worth discussing.

**Framing:** type-length-value. Every message carries an 8-byte header:

```
offset  size  field
0       4     type      u32   1=SESSION 2=SAMPLES 3=STATS 4=EOS 5=ERROR
4       4     length    u32   payload bytes following this header
```

The reader loop is then: read exactly 8 bytes, decode, read exactly `length` bytes, dispatch. `recv()` on a stream socket may return short reads; both sides must loop until the full count is read. This is the classic mistake in first socket code — write a `read_exactly()` helper on both sides and use it everywhere.

**SESSION (type 1)** — sent once, immediately after connect, before any samples:

```
offset  size  field
0       4     magic          u32   0x434C4E53  ('CLNS')
4       2     proto_version  u16   1
6       2     record_size    u16   32  (bytes per sample record)
8       1     endianness     u8    0=little 1=big
9       1     n_events       u8
10      2     _pad           u16
12      4     target_pid     u32
16      8     time_base_ns   u64   CLOCK_MONOTONIC at enable, for correlating with wall clock
24      4     sample_freq    u32   Hz, or 0 if period-based
28      4     sample_period  u32   0 if freq-based
32      1     precise_ip     u8    the level actually granted (see §2.4)
33      1     facility       u8    0=none 1=PEBS 2=IBS 3=SPE
34      2     _pad2          u16
36      n*32  event_table    n_events × { u32 event_id; char name[28]; }
```

Two fields deserve comment because interviewers ask about them:

- **`record_size` in the header** is what makes the protocol forward-compatible. If a later agent version adds fields to the sample record, an older Python consumer reads the fields it knows and skips `record_size - known_size` trailing bytes instead of desynchronizing the stream. Versioning a binary protocol by *declaring the stride* is more robust than bumping a version number and branching.
- **`endianness` is arguably unnecessary** — a Unix domain socket is same-host by definition, so both ends always share byte order, and native order is safe. It is recorded anyway so that a captured stream can be replayed or analyzed on a different machine. This is the right answer to "why did you include an endianness field for local IPC?": *because the wire format outlives the wire.*

**SAMPLES (type 2)** — a batch of fixed-size records, `length` = `k * 32`:

```
offset  size  field
0       8     timestamp   u64   ns, perf clock
8       8     ip          u64   instruction pointer
16      4     pid         u32
20      4     tid         u32
24      4     cpu         u32
28      4     event_id    u32   index into SESSION event_table
```

32 bytes exactly, naturally aligned, no padding on x86-64 or aarch64. Enforce with `_Static_assert(sizeof(cl_sample_t) == 32, ...)` in C and `assert struct.calcsize(FMT) == 32` in Python.

**Batching is the main performance argument in the protocol.** At 997 Hz one `write()` per sample is ~1000 syscalls/sec; at 10 kHz it is 10,000. Batching 256 records per message reduces this to ~4–40 `write()` calls/sec and moves 8 KB per call, which is comfortably within a socket buffer. Flush a partial batch when the ring drains empty or after a 100 ms timer, so a slow target still streams rather than stalling in a half-full buffer.

**STATS (type 3)** — sent once before EOS:

```
u64 samples_emitted
u64 samples_lost        from PERF_RECORD_LOST — data-quality signal, surface in the report
u64 ring_wakeups
u64 bytes_written
u32 target_exit_status  waitpid status
u32 target_signal       0 if exited normally
u64 wall_ns             enable → child exit
```

`samples_lost` flows into the report as a data-quality warning, exactly parallel to how V1 treats `unresolved_sample_rate`. Keeping that philosophy consistent between V1 and V2 is a small thing that reads as design maturity.

**EOS (type 4)** — zero-length, clean end of stream. Distinguishes "agent finished" from "agent died," which a bare EOF cannot.

**ERROR (type 5)** — `u32 code` + UTF-8 message. Sent when the agent fails after connecting (e.g. `perf_event_open` returns `EACCES`). Lets Python surface a specific reason and fall back, rather than reporting "agent exited 1."

**Protocol invariants:** exactly one SESSION first; zero or more SAMPLES; STATS then EOS to close. Any deviation → Python logs, keeps the samples it has, and marks the run degraded. Partial-failure model, preserved across the process boundary.

### 2.4 `perf_event_open` configuration

There is no glibc wrapper; the call is made through `syscall(2)` directly:

```
syscall(__NR_perf_event_open, &attr, pid, cpu, group_fd, flags)
```

**Answering the handoff's "mmap ring buffer vs `read()`" question directly:** these are not alternatives, they serve different purposes, and the agent uses **both**.

- **`read()` on the perf fd returns the counter value** — a single accumulated number. This is counting mode. It is how you replace `perf stat`, and it is where the aggregate `cache-misses` / `cache-references` figures for `llc_miss_rate` come from.
- **The mmap ring buffer is the only way to get samples** — records with instruction pointers. There is no `read()`-based path to per-sample IPs. Sampling *requires* mmap.

So: open two event groups. A counting group (`sample_period = 0`, read with `read()` at the end, `PERF_FORMAT_TOTAL_TIME_ENABLED|RUNNING` to detect multiplexing) that replaces stage 2, and a sampling event (mmap ring) that replaces stage 3. Knowing this distinction cleanly is a strong signal in an interview; getting it wrong is a common tell that someone has only read about `perf_event_open`.

**Sampling event attributes:**

| Field | Value | Why |
|---|---|---|
| `type` | `PERF_TYPE_HW_CACHE` | Lets you name LLC read misses precisely |
| `config` | `PERF_COUNT_HW_CACHE_LL \| (OP_READ << 8) \| (RESULT_MISS << 16)` | LLC read misses |
| `size` | `sizeof(struct perf_event_attr)` | **Mandatory** — the kernel versions the struct by size |
| `sample_freq` + `freq=1` | 997 | Matches V1's `-F 997`, so V1/V2 results are comparable |
| `sample_type` | `IP \| TID \| TIME \| CPU` | Exactly the fields in the 32-byte record |
| `precise_ip` | 2, with downgrade | See below |
| `disabled` | 1 | Start off; the child is not running yet |
| `enable_on_exec` | 1 | **Counting begins at `execvp`, not at `fork`** |
| `exclude_kernel` | 1 | Works under default `perf_event_paranoid=2`, no root needed |
| `exclude_hv` | 1 | Same |
| `mmap` | 1 | Emit `PERF_RECORD_MMAP2`, needed if PIE support is ever added |
| `wakeup_watermark` + `watermark=1` | ~1/4 of buffer | Wake on bytes available, not per-N-samples — better batching |
| `inherit` | 0 for now | Benchmarks are single-threaded; set to 1 for multithreaded targets |

Two of these carry the most interview weight:

**`enable_on_exec = 1`** is the elegant solution to a real race. You must open the counter *before* the target starts (so you miss nothing), but you must not count the agent's own `fork`/`exec` setup work (or you pollute the profile with your own tool). Setting `disabled=1` plus `enable_on_exec=1` makes the kernel arm the counter at the exact `execve` boundary. No ioctl race, no lost early samples, no self-pollution.

**`precise_ip` with graceful downgrade** is the C-side mirror of V1's capability negotiation, and it makes bullets 2 and 3 reinforce each other. `precise_ip` requests zero-skid attribution (PEBS on Intel, IBS on AMD). Not every event on every machine supports every level, and an unsupported level makes `perf_event_open` fail with `EOPNOTSUPP`/`EINVAL`. So: attempt 3, then 2, then 1, then 0, and report the granted level in the SESSION header. The same negotiate-and-degrade pattern, now at the syscall layer instead of the CLI layer.

**Ring buffer.** `mmap(NULL, (1 + 2^n) * page_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)` — one metadata page plus `2^n` data pages. Use `n=7` (128 pages, 512 KB); large enough that a brief Python stall does not lose samples, small enough to stay cache-friendly.

The consume loop is **the hardest correctness problem in V2**:

1. Load `data_head` from the metadata page with an **acquire** barrier (`__atomic_load_n(&mp->data_head, __ATOMIC_ACQUIRE)`).
2. Walk records from `data_tail` to `data_head`. Each begins with `struct perf_event_header { u32 type; u16 misc; u16 size; }`.
3. **Handle wraparound**: a record can straddle the end of the circular buffer. Copy in two `memcpy` parts into a linear scratch buffer before decoding. Forgetting this produces rare, load-dependent corruption — the worst possible bug class.
4. Decode `PERF_RECORD_SAMPLE` (type 9). **Sample fields appear in a fixed canonical order determined by the `sample_type` bitmask, not the order you listed them.** The order is IP, then PID/TID, then TIME, then ADDR, then ID, then STREAM_ID, then CPU, then PERIOD. Decode in that order or every field after the first mistake is garbage.
5. Count `PERF_RECORD_LOST` (type 2) into `samples_lost`.
6. Store `data_tail` with a **release** barrier.

The acquire/release pairing is not decoration: the kernel writes record bytes and then publishes `data_head`. Without an acquire load, the CPU may reorder your reads and let you observe a new `data_head` while reading stale record bytes. The release store on `data_tail` symmetrically guarantees the kernel does not overwrite records you have not finished reading. Being able to explain this pairing is worth more in an HFT interview than the rest of the project combined.

### 2.5 fork/exec lifecycle

The rendezvous problem: the agent must call `perf_event_open` with the child's PID, which requires the child to exist — but the child must not begin real work before the counters are armed.

Standard solution, a **sync pipe**:

```
pipe(sync)                    before fork
fork()
  child:  close(sync[1]); read(sync[0], &b, 1);   // blocks until parent is ready
          close(sync[0]); execvp(target, argv);    // enable_on_exec arms here
          _exit(127);                              // only reached if exec failed
  parent: close(sync[0]);
          perf_event_open(&attr, child_pid, -1, -1, PERF_FLAG_FD_CLOEXEC);
          mmap ring buffer;
          write(sync[1], "g", 1);  close(sync[1]); // release the child
```

`read()` returning 0 on the closed write end also correctly releases the child if the parent dies during setup — the child then execs unprofiled rather than hanging forever. Use `PERF_FLAG_FD_CLOEXEC` so the perf fds do not leak into the target.

Detecting exec failure is a subtlety worth handling: if `execvp` fails, the child `_exit(127)`, but the parent sees only an exit status. The clean fix is a second `CLOEXEC` pipe — the child writes `errno` to it on exec failure, and the pipe closing silently on success signals exec worked. This distinguishes "your target binary does not exist" from "your target ran and returned 127."

**Event loop.** Wait on multiple fds with a single `poll()`:

- the perf ring fd → readable when the watermark is crossed
- **`pidfd_open(child_pid, 0)`** → readable when the child exits

`pidfd_open` (Linux 5.3+, present on Ubuntu 22.04/24.04) gives a *pollable* file descriptor for process exit. This avoids the classic `SIGCHLD` + self-pipe dance and makes the loop a clean single `poll()` over two fds. It is a modern, correct choice and a good thing to have picked deliberately.

**Shutdown ordering matters.** When the child exits: drain the ring **one final time** (samples remain buffered after the process is gone — skipping this silently truncates the tail of every profile), then `waitpid` for status, then send STATS, then EOS, then `close()`, then `_exit`. Handle `SIGINT`/`SIGTERM` by forwarding to the child and running the same shutdown path, so Ctrl-C still produces a usable partial profile. That is the partial-failure model extended into the data plane.

`signal(SIGPIPE, SIG_IGN)` at startup, unconditionally — otherwise a Python-side crash kills the agent with SIGPIPE mid-write and you lose the STATS message that would have told you what happened.

### 2.6 Python-side integration

**New package `src/cacheprof/agent/`:**

| File | Role |
|---|---|
| `protocol.py` | `struct` format strings, message type constants, record decoding. Mirrors `protocol.h`. |
| `source.py` | Socket lifecycle: bind, listen, spawn agent, accept, read frames, yield `PerfSample`. |
| `discovery.py` | Locate the agent binary, run `--selftest`, parse its capability output. |

**Keeping C and Python in sync** is the real maintenance risk in any binary protocol. Mitigation, in order of strength: (1) `protocol.h` is the single source of truth and `protocol.py` carries a comment pointing at it; (2) `_Static_assert` on struct size in C, `struct.calcsize` assertion at import in Python; (3) the SESSION header carries `proto_version` and `record_size`, and Python refuses to proceed on a version mismatch with a clear message. Belt, braces, and a runtime check — because a silent struct-layout drift produces plausible-looking garbage, which is far worse than a crash.

**`capabilities.py` changes.** Add `CollectionMode.DIRECT_SAMPLING` as the new top rung:

```
agent binary present AND --selftest exits 0 AND PMU present  → DIRECT_SAMPLING
precise facility + perf mem works                            → MEM_SAMPLING
LLC-load-misses / cache-misses available                     → COUNTER_SAMPLING
none                                                         → hard fail
```

`--selftest` is the agent probing itself: it attempts `perf_event_open` on its own PID at each `precise_ip` level, reports which succeeded and which facility it found, and exits 0 only if sampling is actually possible. This is better than checking that the file exists, because it detects the `perf_event_paranoid` and virtualized-PMU cases *before* the pipeline commits to a mode. Cache the result per run.

**Fallback must be automatic and visible.** If the agent fails at any point before producing samples, Python logs the reason, downgrades to `MEM_SAMPLING`/`COUNTER_SAMPLING`, and re-runs stages 1–5 the V1 way. The report records both the attempted and the used mode. "The Python pipeline still works standalone; the C agent is an accelerator, not a requirement" is only a true claim if this path is implemented and tested — add an integration test that forces agent failure and asserts a complete report still comes out.

**`runner.py` changes** are confined to stages 1–5, which become one branch:

```
if mode is DIRECT_SAMPLING:  samples = agent.source.collect(binary, args, config, run_dir)
else:                        samples = <existing perf record → script → parse chain>
# stages 6-11 unchanged, operating on `samples`
```

Keep saving artifacts in both paths: the raw framed stream to `agent_stream.bin` (the V2 analogue of keeping `perf.data`, and invaluable for offline debugging and for replaying a run without hardware).

### 2.7 Build system

**Makefile, not CMake.** ~400 lines of C, one binary, no dependencies beyond libc and Linux UAPI headers. A reviewer can read a 30-line Makefile in ten seconds; CMake here signals unfamiliarity with the scale of the problem.

```
CC       = gcc
CSTD     = -std=c11
WARN     = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
DEFS     = -D_GNU_SOURCE
OPT      = -O2 -g -fno-omit-frame-pointer
HARDEN   = -fstack-protector-strong -D_FORTIFY_SOURCE=2

release: $(OPT)
debug:   -O0 -g3 -fsanitize=address,undefined
```

`-D_GNU_SOURCE` is required for `pidfd_open` and other GNU extensions. `-Werror` on a solo project of this size is affordable and keeps the code clean. **Build and test with the sanitizer target from day one** — ASan and UBSan catch exactly the ring-buffer wraparound and alignment bugs that are otherwise nearly impossible to find, and the performance cost is irrelevant for correctness runs. Do the final overhead measurement with the release target.

Layout: `agent/` at repo root (`src/`, `include/protocol.h`, `Makefile`, `README.md`). A `make check` target that runs `--selftest` gives reviewers a one-command sanity check.

### 2.8 Error handling matrix

| Failure | Agent behavior | Python behavior | Result |
|---|---|---|---|
| `perf_event_open` → `EACCES`/`EPERM` | ERROR msg, exit 2 | Log reason, fall back to CLI mode | Complete report, degraded mode noted |
| `perf_event_open` → `EINVAL` at `precise_ip=N` | Retry N-1, down to 0 | — | Transparent; granted level in SESSION |
| No PMU (virtualized) | `--selftest` fails | Never selects DIRECT_SAMPLING | Correct mode chosen up front |
| Target binary missing | exec-status pipe reports `ENOENT` | Clear "target not found" error | No misleading exit-127 |
| **Target crashes (SIGSEGV)** | Final drain, STATS with `target_signal=11`, EOS | Full pipeline on collected samples | **Report produced + crash noted** |
| Target exits non-zero | Same, `target_exit_status` set | Report + warning | Report produced |
| Ring buffer overflow | Count `PERF_RECORD_LOST` | Data-quality warning in report | Report + honest caveat |
| **Python dies mid-run** | `write()` → `EPIPE` (SIGPIPE ignored) → kill child, exit | n/a | No orphaned target process |
| **Agent dies mid-run** | — | EOF without EOS → use partial samples, mark degraded | Partial report |
| Agent hangs | — | Timeout → `SIGTERM`, then `SIGKILL`, use partial samples | Partial report, no hang |
| Socket path collision | `connect()` fails, exit 3 | Run dirs are timestamped + collision-safe | Should not occur |

Two rows are the ones to point at in an interview, because they show the partial-failure model surviving a process boundary: **the target crashing still produces a report** (the samples collected before the crash are real data and often the most interesting), and **Python dying never orphans the target**.

---

## 3. Interview preparation map

### 3.1 Bullet 1 — profiler, perf, DWARF, ranking, LLM

**Q1. When perf reports a cache miss at instruction X, was it really at X?**
Not necessarily — this is *skid*. The PMU raises an interrupt on counter overflow, and by the time the handler samples the IP, the pipeline has retired further instructions. Under plain counter sampling the reported IP can be tens of instructions past the actual miss. Precise facilities (Intel PEBS, AMD IBS, Arm SPE) have hardware record the IP at the event itself, giving zero or near-zero skid — this is precisely why the tool prefers `perf mem`/`precise_ip` and reports which mode it used. Have your measured skid for `matrix_bad` ready.

**Q2. Why `addr2line` instead of parsing DWARF with `pyelftools`?**
Correctness and time budget. DWARF line-number info is a bytecode-encoded state machine (`.debug_line`), and a correct decoder handles the opcode set, `DW_LNS_*` special opcodes, sequences, and discriminators. `addr2line` is the reference implementation and is already installed everywhere binutils is. Cost: a subprocess boundary and no structured access to the DIE tree. Mitigated by batching all unique addresses through **one** `addr2line` process over stdin (`resolver.py:78`) rather than one process per address. Know the deferred `pyelftools` path exists and why it is deferred.

**Q3. At `-O2`, code is inlined and reordered. How do you know line attribution is right?**
Partly you do not, and the tool says so. `addr2line` is run with `-f -C` and deliberately **without** `-i`, so output is exactly two lines per address and parsing stays deterministic (`resolver.py:114`). The cost is that an address inside an inlined callee is attributed to the inlined location rather than the full inline chain. Discriminators (multiple basic blocks on one source line) are stripped (`resolver.py:133`). The honest framing: the tool identifies the hot *region* reliably and the hot *line* approximately, and the README documents the observed error.

**Q4. How do you rank hotspots, and why that metric?**
By fraction of total misses at a `(file, line)` key, not raw count — raw counts are meaningless without a denominator, and fractions are comparable across runs of different lengths. `hotspot_concentration` is a first-class signal because it distinguishes the interesting case (one line owns 60% of misses → a fixable locality bug) from the diffuse case (misses spread evenly → memory-bound by design, no local fix).

**Q5. What stops the LLM from hallucinating a fix?**
Grounding and separation of concerns. `signals.py` emits **only numbers and booleans** — miss rate, concentration, nested-loop and pointer-deref flags, dataset size hint — and never a diagnosis. The LLM receives those numbers plus the real source snippet, and does the reasoning step. The raw response is always retained (`Diagnosis.raw_response`) with a `parse_ok` flag, and the entire stage is opt-in and wrapped so that failure never costs the rest of the report (`runner.py:125`). Be ready to say what the heuristics *cannot* see: they are regexes over a source window, so `_detect_nested_loops` just counts loop keywords and will be fooled by two sequential loops.

### 3.2 Bullet 2 — capability negotiation and partial failure

> **Do not put this bullet on a resume until §1.3 is implemented.** Q1 and Q2 have no answer in the current code.

**Q1. What actually differs between PEBS, IBS, and SPE?**
All three are hardware-assisted precise sampling, differing in mechanism. Intel **PEBS** has the hardware write a record (IP, registers, and for memory events the data address and a data-source encoding indicating which cache level serviced the load) into a buffer at the event, eliminating skid. AMD **IBS** has two flavors — IBS Fetch (front-end) and IBS Op (a tagged micro-op, carrying load/store address and latency). Arm **SPE** statistically samples operations and emits a packet stream with address and latency. Practical upshot for this tool: all three give a trustworthy IP and a data address; without them you get a skidded IP and no data address at all.

**Q2. How do you detect which one you have?**
Filesystem probes, no subprocess needed: vendor from `/proc/cpuinfo`; `/sys/bus/event_source/devices/cpu/events/mem-loads` for PEBS-backed load sampling; `/sys/bus/event_source/devices/ibs_op/` for IBS; `/sys/bus/event_source/devices/arm_spe_0/` for SPE. Absence of `/sys/bus/event_source/devices/cpu/` entirely means no hardware PMU — the virtualized-guest case, which is a hard fail with a specific message rather than a confusing cascade of empty results.

**Q3. Walk me through the fallback ladder and why it is ordered that way.**
Ordered by attribution quality: precise mem sampling → event-specific counter sampling (`LLC-load-misses`, semantically what you want) → generic `cache-misses` (available almost everywhere but includes instruction and prefetch traffic) → hard fail. Every rung records *why* it was chosen into the report, so the mode is explainable after the fact rather than mysterious.

**Q4. Your pipeline has 10 stages. Stage 6 fails. What is on disk?**
Everything from stages 1–5: `capabilities.txt`, `perf_stat.txt`, `perf.data`, `perf_script.txt`, plus a partial JSON report with `stat_result` populated and `hotspots` empty. The design rule is that the runner never lets a later failure discard earlier work (`runner.py:136`) — each stage boundary either advances or bails out through `_save_partial`. Rationale: profiling runs are expensive and non-deterministic, so throwing away a completed 3-minute `perf record` because `addr2line` was missing is unacceptable. Know that the LLM stage is deliberately *softer* than the rest — it warns and continues (`runner.py:125`), because it is optional by definition.

**Q5. What is the difference between `perf mem record` and `perf record -e cache-misses`?**
`perf mem record` configures a precise memory-sampling event (PEBS load-latency / IBS Op / SPE) and yields per-sample data addresses and data-source info alongside a precise IP. `perf record -e cache-misses` samples a generic counter with skid and gives you only an IP. Worth volunteering as a known limitation: V1's parser extracts only the IP, so even in `MEM_SAMPLING` mode it is not yet consuming the data-address and data-source fields that make `perf mem` genuinely more powerful. That is a concrete, credible "what would you do next."

### 3.3 Bullet 3 — the C data plane

**Q1. Walk me through your `perf_event_open` call.**
Five arguments: `&attr`, `pid`, `cpu`, `group_fd`, `flags`. We pass the child PID with `cpu = -1` to follow that task across all CPUs (the alternative, `pid=-1` with a specific CPU, is system-wide and requires elevated privilege). `attr.size = sizeof(attr)` is mandatory — it is how the kernel handles struct versioning across releases. Key flags: `disabled=1` + `enable_on_exec=1`, `exclude_kernel=1`, `precise_ip` negotiated downward, `sample_type = IP|TID|TIME|CPU`. There is no glibc wrapper, so it goes through `syscall(__NR_perf_event_open, ...)`.

**Q2. Why the mmap ring buffer instead of `read()`?**
They do different things. `read()` on a perf fd returns the accumulated **counter value** — that is counting mode, and it is how the agent replaces `perf stat`. Samples with instruction pointers exist **only** in the mmap ring buffer; there is no `read()`-based path to them. The agent uses both: `read()` for aggregate counters, mmap for the sample stream.

**Q3. Explain the memory barriers in your ring buffer consumer.**
Single-producer (kernel) / single-consumer (agent) circular buffer over shared memory. Load `data_head` with **acquire** semantics, so that reads of the record bytes cannot be reordered before the load that told you those bytes exist — without it, you can observe a published head while reading stale payload. Store `data_tail` with **release** semantics, so all your reads complete before the kernel is told the space is reusable. Also: a record can wrap the end of the buffer and must be reassembled with two `memcpy`s, and sample fields are laid out in a fixed canonical order determined by the `sample_type` mask, not the order you wrote them.

**Q4. How do you make sure you profile the target and not your own setup code?**
`disabled=1` with `enable_on_exec=1`. The counter is created armed-but-off and the kernel enables it exactly at the `execve` boundary. This solves both halves of the race at once: nothing from the agent's `fork`/`mmap`/setup pollutes the profile, and no early target instructions are missed. Separately, the child blocks on a sync pipe until the parent has finished `perf_event_open` and `mmap`, so the target cannot start before the counters exist.

**Q5. Why a Unix socket instead of a pipe or shared memory?**
A pipe would work but is unidirectional and offers no path to a control channel or to `SCM_RIGHTS` fd passing. Shared memory would be fastest but requires building your own synchronization and framing on top — a second ring buffer to debug, for a data rate (~1k samples/sec, ~32 KB/s) that is nowhere near needing it. `SOCK_STREAM` gives ordered delivery and kernel flow control for free, and the framing layer is trivial. It also means the transport can become a TCP socket later with almost no change, which matters for the "production monitoring agent" architecture this is modeled on. Choosing the simplest thing that meets the actual data rate, and knowing the number, is the answer.

**Q6. What is the actual measured win?**
Be precise and honest: the C agent does **not** sample faster than `perf record` — `perf` is mature, optimized C. The win is end-to-end pipeline latency. V1 writes `perf.data` to disk, spawns `perf script` to render it to text, then re-parses that text with Python regex. V2 deletes all three: samples go from the ring buffer to structured records over a socket. Bring the measured table from §4.4. "I found the bottleneck in my own tool's data path" is the story.

### 3.4 Memorize versus look up

**Memorize cold** — these are asked conversationally and hesitation reads as unfamiliarity:

- The 10-stage pipeline in order, and that the seam is `list[PerfSample]`
- The capability ladder and why it is ordered by attribution quality
- Skid, and what PEBS/IBS/SPE do about it
- The five `perf_event_open` arguments and the ~6 attributes that matter
- `enable_on_exec` and the problem it solves
- Ring buffer acquire/release pairing and the wraparound case
- The wire protocol shape: 8-byte TLV header, 32-byte fixed sample record, why batched
- Why `read()` and mmap are not alternatives
- The partial-failure rule and one concrete example of what survives what

**Fine to look up** — reasonable to say "I'd check the header":

- Exact `perf_event_attr` field offsets and the full bitfield list
- Exact `PERF_RECORD_*` numeric constants
- `PERF_COUNT_HW_CACHE_*` config encoding shifts
- The regexes in `parser.py` and `stat.py`
- DWARF discriminator semantics beyond "multiple blocks on one line"
- Exact `errno` values per failure mode

**Be able to open and explain on a shared screen:** `runner.py` (the partial-failure structure), `capabilities.py` (the ladder), `models.py` (the seam), and the agent's ring-buffer consume loop. Those four are the project.

---

## 4. Risk register

### 4.1 Phase risks

| # | Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|---|
| **R1** | **DigitalOcean droplet has no vPMU** — hardware events return `<not supported>`, no data ever | **High** | **Fatal** | §1.1 test **today**, before any code. Fallback: Hetzner/OVH bare metal (~€40/mo), AWS `*.metal` hourly, or any physical Linux box. Budget 1 day and ~$50. |
| R2 | Benchmarks do not show the expected good/bad difference (working set fits in LLC) | Medium | High | Verify with `perf stat` alone before trusting the pipeline. Size `N` from actual LLC size (`lscpu`); `N=2048`+ for a 32 MB LLC. Fix the benchmark, not the tool. |
| R3 | `-O2` line attribution too noisy to demo | Medium | Medium | Expected; document measured skid rather than hiding it. Add an `-O1` demo variant. Cross-check with `perf annotate`. |
| R4 | Parser bugs (§1.4) eat samples silently on real data | **High if unfixed** | High | Fix both before the first run. Add a C++-symbol fixture. Log parse-failure counts at INFO, not DEBUG, so loss is visible immediately. |
| R5 | Timeline slips past application windows | **High** | High | Re-baselined plan in §0.1. V1 is the only hard deadline. Bullet 3 is additive and can land mid-cycle. |
| R6 | Ring buffer consumer has subtle corruption (wraparound, barriers, field order) | **High** | High | Hardest part of V2 — see §4.2. Build in stages, ASan/UBSan from day one, validate against `perf record` on the same binary. |
| R7 | C/Python struct layout drift | Medium | High | `_Static_assert` + `struct.calcsize` + `proto_version` and `record_size` in the SESSION header. Fail loudly on mismatch. |
| R8 | LLM output quality is unimpressive in the demo | Medium | Low | It is opt-in and not the core claim. Cache one good response for the demo; lead with the profiler. |
| R9 | Reviewer cannot reproduce from the README | Medium | Medium | Paste the quick-start onto a fresh droplet and follow it literally. The `pip install` bug (§0.2) is exactly this failure already present. |

### 4.2 What is hardest with no prior C systems experience

Ranked by expected pain, with the honest reason:

1. **The mmap ring buffer consumer.** Everything unfamiliar at once: shared memory with a concurrent writer you do not control, memory-ordering semantics, a circular buffer with wraparound, and variable-length records whose field layout is determined at runtime by a bitmask. Bugs are load-dependent and non-deterministic. *Approach:* build in four separate, individually-verified steps — (a) counting mode with `read()` only, no sampling at all; (b) sampling with a huge fixed period so records trickle in one at a time and wraparound never triggers; (c) a normal period, still ignoring wraparound but asserting loudly if it would occur; (d) full wraparound handling. Validate each step's IP values against `perf record` on the same binary — you have a reference implementation, use it.

2. **Memory ordering.** `__ATOMIC_ACQUIRE`/`__ATOMIC_RELEASE` is a genuinely new concept and EECS 482 has not covered it yet. *Approach:* read the `perf_event_open(2)` man page section on the mmap layout, and copy the barrier structure from the kernel's own `tools/perf` sample code. This is one of the rare cases where following an established pattern exactly, and then understanding it, beats deriving it. Understand it before the interview, because Q3 in §3.3 is a likely HFT question.

3. **`fork`/`exec` plumbing.** Pipe direction, which ends to close in which process, `execvp` failure detection, `waitpid` status macros. Conceptually simple, but easy to get subtly wrong (a forgotten `close()` means a child that blocks forever). *Approach:* write and test this standalone, with no perf involvement, before combining.

4. **Manual memory and buffer management.** No GC, no bounds checks. The batch buffer and the scratch reassembly buffer are both fixed-size and both easy to overrun. *Approach:* ASan/UBSan on every debug build; fixed-size stack buffers with explicit bounds checks over dynamic allocation.

5. **Socket framing.** Mostly a matter of discipline: `read()`/`write()` on a stream socket may transfer fewer bytes than requested. *Approach:* write `read_exactly()` / `write_all()` helpers first and never call the raw functions afterward.

**Realistic estimate for someone learning C systems programming on this project: 5–6 weeks, not 3.** The handoff's 3–5 week estimate is achievable only with prior `perf_event_open` experience. Plan for 6 and be pleased if it takes 4.

### 4.3 Scope cuts if the timeline slips

Cut in this order. Each level is still a coherent, honest resume bullet.

**Level 1 — cut V2 polish.** Ship the agent with measurements but a thinner README. Bullet 3 stands with real numbers.

**Level 2 — minimum viable V2.** The smallest thing that makes bullet 3 fully true:
- One event (`cache-misses`), fixed `sample_period` (no frequency mode, no auto-adjust)
- `precise_ip = 0` only — no negotiation ladder in C (V1 still has one, so bullet 2 is unaffected)
- No counting mode; keep `perf stat` from V1 for aggregate counters
- Fixed-size sample batches, no partial-flush timer
- Sequential: run the target to completion, drain, send, exit — no `poll()` loop, no concurrent streaming
- No `pidfd`; plain `waitpid`

This is roughly 200 lines and still contains every claim in the bullet: `perf_event_open`, ring buffer, Unix socket IPC, `fork`/`exec`. It is a real data plane. **It is also the correct thing to build first even if the timeline holds** — treat it as milestone 1 of V2, not only as a fallback.

**Level 3 — counting-mode agent only.** No sampling, no ring buffer: `perf_event_open` in counting mode, `read()` the counters, send them over the socket, replacing `perf stat`. Reword bullet 3 to "hardware counter collection agent" and drop the sampling claim. Roughly 100 lines, ~1 week, and still demonstrates `perf_event_open`, `fork`/`exec`, and socket IPC. Sampling is where the ring buffer lives, so this is a real reduction in difficulty — but it is honest and it ships.

**Level 4 — cut V2 entirely.** Ship V1 with bullets 1 and 2, real numbers, a strong README, and integration tests. Add a "Roadmap" section describing the V2 design (this document) as planned work.

**Level 4 with a polished V1 beats a broken V2, every time.** A reviewer who runs your quick start and sees a real hotspot in 5 minutes is more impressed than one who reads about a C agent that does not build. Guard the V1 deadline absolutely; treat everything after it as upside.

### 4.4 Measurements to capture (needed for the resume numbers)

Nothing goes on the resume as a number until it is in this table, measured on real hardware.

**V1, per benchmark:** LLC miss rate (good vs bad), samples collected, resolution rate, top hotspot line vs the hand-verified true line, total wall clock.

**V2 comparison, the table bullet 3 rests on:**

| Metric | Baseline (no profiling) | V1 (perf CLI) | V2 (C agent) |
|---|---|---|---|
| Target wall-clock | — | | |
| **End-to-end time to hotspots** | n/a | | |
| Peak disk usage (`perf.data`) | n/a | | ~0 |
| Samples collected | n/a | | |
| Samples lost | n/a | | |

**End-to-end time to hotspots is the headline row** — it is where V2 actually wins, because it eliminates the `perf.data` write, the `perf script` subprocess, and the Python regex parse. Report target wall-clock overhead too, honestly, even though V1 will likely match or beat V2 there.

---

## 5. Immediate next actions

Ordered. Do not start item 4 before item 1 is answered.

1. **Today** — run the §1.1 PMU test on the droplet. If hardware events return `<not supported>`, provision a bare-metal host before writing a line of code. Everything is blocked on this.
2. **Day 1** — fix `pyproject.toml` (§1.2), delete the stray `{src` tree, committed binaries, `.dSYM`s, synthetic `outputs/`, and the two placeholder `dwarf/` files.
3. **Day 1–2** — implement real architecture detection in `capabilities.py` (§1.3). Bullet 2 depends on it.
4. **Day 2** — fix the DSO filter and the C++ symbol parser bugs (§1.4), with fixtures.
5. **Day 2–4** — rebuild benchmarks on Linux, first real profiling run, hand-verify hotspot lines, capture numbers.
6. **Week 2** — runner partial-failure integration tests, README rewrite with real screenshots and measured results.
7. **Week 3+** — V2 milestone 1: the §4.3 Level-2 minimum viable agent, built and validated in the four stages described in §4.2.
