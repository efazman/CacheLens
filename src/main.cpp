// cachelens — Gate 7 Phase 2: per-CPU sampling for multithreaded targets.
//
// Forks a child, stops it before exec, opens two independent sampling
// events (cache-misses = numerator, cache-references = denominator) PER
// ONLINE CPU on the child pid, drains all rings while it runs, resolves
// target-executable samples to file:line via libdw, and ranks sites by
// miss/access concentration (Wilson lower bound), not raw miss count.
//
// Why per-CPU (docs/GATE7_PLAN.md §1, U5): perf_event_open(2) states that
// `inherit=1` together with `cpu=-1` prevents creation of the mmap ring
// buffer used for sampling. Since inherit is required to see any thread
// other than the target's initial one, sampling a multithreaded target
// needs one perf_event_open per (event, online CPU) with `pid=child,
// cpu=i, inherit=1` -- this is what `perf record` itself does for a
// workload target. On this machine (12 online CPUs) that is 2 events x 12
// CPUs = 24 file descriptors and 24 rings, up from 2 in the
// single-threaded-only design this replaces.
//
// U8 (the calibration divisor) -- worth stating explicitly because it is
// easy to get backwards: the per-event period computed below from the
// *aggregate* calibrated rate (summed across all target threads, via
// `inherit=1` counting-mode) is applied UNCHANGED to every per-CPU ring,
// not divided again by the CPU count. This is correct, not an
// approximation: if a CPU's true local event rate is `cal.rate * (that
// CPU's share of total activity)`, then that CPU's ring sampling rate is
// `local_rate / period = local_rate * target_rate / cal.rate`, and summing
// over every CPU (shares sum to 1) gives exactly `target_rate` in
// aggregate, regardless of how activity happens to be distributed across
// CPUs -- including the degenerate case where only 2 of 12 CPUs are ever
// used by the target (this project's queue benchmarks). Re-dividing by
// nproc here would silently under-sample any target that doesn't spread
// itself across every online CPU.
//
// A second per-CPU subtlety, not in the man page, found by the Phase 2
// regression guard against matrix_bad (a single, unpinned thread the
// scheduler migrates across CPUs): `time_enabled` on a `pid=child, cpu=i`
// event tracks wall-clock time since the event armed at exec -- the same
// value on every per-CPU ring for that run -- regardless of whether the
// task ever ran on CPU i; `time_running` only tracks the sliver of that
// time the task actually spent scheduled on that specific CPU. Any single
// ring's own time_running/time_enabled ratio is therefore near-zero on
// every CPU the task merely passed through, which looks exactly like PMU
// contention but isn't. The multiplexing halt check below sums
// time_running across every per-CPU ring for an event before comparing it
// to time_enabled, which reconstructs the task's true total scheduled
// time regardless of which CPU it was on at any instant (see the comment
// at that check for the matrix_bad numbers that confirmed this).
//
// Per-event sample periods are still calibrated automatically per run, not
// hardcoded: a first child invocation measures both events in pure
// counting mode (cheap, no ring buffers) to get real aggregate event
// rates, then a second invocation samples both simultaneously with
// periods derived from those rates (60% of the kernel's own
// perf_event_max_sample_rate, rounded to the nearest prime). This keeps
// the tool target-agnostic -- no benchmark-specific constants.
//
// Ring size is 512 KiB per CPU (docs/GATE7_PLAN.md U7), not the 1 MiB used
// by the single-ring predecessor: P3's probe measured the actual mlock
// budget on this machine and found 1 MiB rings do not fit for all 24
// (2 events x 12 CPUs), while 512 KiB does with room to spare.
//
// precise_ip is pinned at 0 — Zen 4 has no PEBS-equivalent for these
// events (see docs/TAKEAWAYS.md). Samples are skidded by an unknown
// amount; source-line-level skid was characterized in Gate 4 and found
// absorbed (99.99% within +-2 lines) for a specific single-threaded
// benchmark pair -- that finding is workload-specific and has NOT yet been
// re-characterized for a multithreaded target (U9, Gate 7 Phase 3).
//
// Known limitation carried from before this gate, now fixed: earlier
// increments left `attr.inherit` at 0 and silently profiled only the
// target's initial thread. That gap is closed by this file; see the
// README's Limitations section.
//
// Usage: cachelens -- <command> [args...]

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <elf.h>
#include <elfutils/libdwfl.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

long perf_event_open(struct perf_event_attr* attr, pid_t pid, int cpu,
                      int group_fd, unsigned long flags) {
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "cachelens: fatal: %s\n", what);
    std::exit(1);
}

[[noreturn]] void die_errno(const char* what) {
    std::fprintf(stderr, "cachelens: fatal: %s: %s (errno %d)\n", what,
                 std::strerror(errno), errno);
    std::exit(1);
}

// Formats an (event, cpu) pair as a short label for error messages, per
// Gate 7 U13: with 24 independently-throttled/independently-failing rings,
// a bare event name is no longer enough to say which one broke.
std::string ring_label(const char* event_label, int cpu) {
    return std::string(event_label) + "@cpu" + std::to_string(cpu);
}

void diagnose_perf_open_failure(const std::string& event_label) {
    if (errno == EACCES || errno == EPERM) {
        std::fprintf(stderr,
            "cachelens: fatal: perf_event_open(%s): %s (errno %d)\n"
            "  Diagnosis: kernel.perf_event_paranoid is likely too restrictive.\n",
            event_label.c_str(), std::strerror(errno), errno);
        std::exit(1);
    }
    if (errno == ENOENT) {
        std::fprintf(stderr,
            "cachelens: fatal: perf_event_open(%s): %s (errno %d)\n"
            "  Diagnosis: this event is not implemented by the running PMU.\n",
            event_label.c_str(), std::strerror(errno), errno);
        std::exit(1);
    }
    std::fprintf(stderr, "cachelens: fatal: perf_event_open(%s): %s (errno %d)\n",
                 event_label.c_str(), std::strerror(errno), errno);
    std::exit(1);
}

// ---------------------------------------------------------------------
// Child process lifecycle: fork, disable ASLR, self-stop, exec. Shared by
// both the calibration run and the measurement run.
// ---------------------------------------------------------------------
pid_t fork_stop_exec(char** target_argv) {
    pid_t child = fork();
    if (child < 0) die_errno("fork");
    if (child == 0) {
        if (personality(ADDR_NO_RANDOMIZE) == -1) {
            std::fprintf(stderr, "cachelens: child: personality: %s\n", std::strerror(errno));
            _exit(1);
        }
        if (raise(SIGSTOP) != 0) {
            std::fprintf(stderr, "cachelens: child: raise(SIGSTOP): %s\n", std::strerror(errno));
            _exit(1);
        }
        execvp(target_argv[0], target_argv);
        std::fprintf(stderr, "cachelens: child: execvp(%s): %s\n", target_argv[0],
                     std::strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, WUNTRACED) < 0) die_errno("waitpid (initial stop)");
    if (!WIFSTOPPED(status)) die("child did not stop as expected before exec");
    return child;
}

int reap(pid_t child, int* exit_code_out) {
    int status = 0;
    if (waitpid(child, &status, 0) < 0) die_errno("waitpid (exit)");
    int exit_code = 0;
    if (WIFEXITED(status)) {
        std::printf("child exited, status %d\n", WEXITSTATUS(status));
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        std::printf("child killed by signal %d\n", WTERMSIG(status));
        exit_code = 128 + WTERMSIG(status);
    }
    *exit_code_out = exit_code;
    return status;
}

struct CountReadFormat {
    uint64_t value;
    uint64_t time_enabled;
    uint64_t time_running;
};

// ---------------------------------------------------------------------
// ELF text range and address classification (unchanged from Gate 4).
// ---------------------------------------------------------------------
struct TextRange { uint64_t lo, hi; };

bool get_target_text_range(const char* path, TextRange& out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    Elf64_Ehdr eh;
    bool ok = read(fd, &eh, sizeof(eh)) == static_cast<ssize_t>(sizeof(eh)) &&
              std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
              eh.e_ident[EI_CLASS] == ELFCLASS64;
    std::vector<Elf64_Phdr> phdrs;
    if (ok) {
        phdrs.resize(eh.e_phnum);
        ssize_t want = static_cast<ssize_t>(sizeof(Elf64_Phdr)) * eh.e_phnum;
        ok = lseek(fd, static_cast<off_t>(eh.e_phoff), SEEK_SET) >= 0 &&
             read(fd, phdrs.data(), want) == want;
    }
    close(fd);
    if (!ok) return false;
    bool found = false;
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
            uint64_t seg_lo = ph.p_vaddr, seg_hi = ph.p_vaddr + ph.p_memsz;
            out.lo = found ? std::min(out.lo, seg_lo) : seg_lo;
            out.hi = found ? std::max(out.hi, seg_hi) : seg_hi;
            found = true;
        }
    }
    return found;
}

enum class Bucket { kTarget, kOtherUser, kKernel, kUnclassifiable };

Bucket classify(uint64_t ip, const TextRange& target) {
    constexpr uint64_t kKernelHalfStart = 0xffff800000000000ULL;
    constexpr uint64_t kUserHalfEnd = 0x0000800000000000ULL;
    if (ip >= kKernelHalfStart) return Bucket::kKernel;
    if (ip >= kUserHalfEnd) return Bucket::kUnclassifiable;
    if (ip >= target.lo && ip < target.hi) return Bucket::kTarget;
    return Bucket::kOtherUser;
}

class DwarfResolver {
public:
    ~DwarfResolver() { if (dwfl_) dwfl_end(dwfl_); }

    bool init(const char* path) {
        callbacks_ = {};
        callbacks_.find_elf = nullptr;
        callbacks_.find_debuginfo = dwfl_standard_find_debuginfo;
        callbacks_.section_address = dwfl_offline_section_address;
        callbacks_.debuginfo_path = &debuginfo_path_;
        dwfl_ = dwfl_begin(&callbacks_);
        if (!dwfl_) return false;
        mod_ = dwfl_report_offline(dwfl_, "target", path, -1);
        if (!mod_) return false;
        dwfl_report_end(dwfl_, nullptr, nullptr);
        return true;
    }

    bool resolve(uint64_t ip, std::string& file, int& line) const {
        Dwfl_Line* dline = dwfl_module_getsrc(mod_, ip);
        if (!dline) return false;
        int line_no = 0;
        const char* src = dwfl_lineinfo(dline, nullptr, &line_no, nullptr, nullptr, nullptr);
        if (!src) return false;
        file = src;
        line = line_no;
        return true;
    }

private:
    Dwfl* dwfl_ = nullptr;
    Dwfl_Module* mod_ = nullptr;
    Dwfl_Callbacks callbacks_{};
    char* debuginfo_path_ = nullptr;
};

// ---------------------------------------------------------------------
// Ring buffer drain (Gate 2/3, extended with PERF_SAMPLE_PERIOD; Gate 7
// adds a ring label to die() messages since there are now up to 24 rings).
// ---------------------------------------------------------------------
struct Sample {
    uint64_t ip;
    uint32_t pid;
    uint32_t tid;
    uint64_t period;  // kernel-reported effective period for this sample
};

struct RecordHistogram {
    uint64_t sample = 0;
    uint64_t lost_records = 0;
    uint64_t lost_events = 0;
    uint64_t exit_records = 0;
    uint64_t other = 0;
};

void copy_wrapped(const uint8_t* data, size_t data_size, size_t off, void* dst, size_t len) {
    size_t first = std::min(len, data_size - off);
    std::memcpy(dst, data + off, first);
    if (first < len) std::memcpy(static_cast<uint8_t*>(dst) + first, data, len - first);
}

// See Gate 3 commit for the full acquire/release reasoning; unchanged here.
void drain(struct perf_event_mmap_page* meta, const uint8_t* data, size_t data_size,
           uint64_t& tail, std::vector<Sample>& samples, RecordHistogram& hist,
           const std::string& label) {
    uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
    if (head - tail > data_size) {
        die(("ring buffer overflow on " + label + ": data_head - data_tail exceeds "
             "data_size — unread samples were overwritten; this build does not attempt "
             "recovery.").c_str());
    }
    while (tail < head) {
        size_t off = tail % data_size;
        struct perf_event_header hdr;
        copy_wrapped(data, data_size, off, &hdr, sizeof(hdr));
        if (hdr.size < sizeof(hdr)) {
            die(("malformed ring buffer record on " + label + ": header.size too small").c_str());
        }
        if (tail + hdr.size > head) {
            die(("malformed ring buffer record on " + label +
                 ": header.size runs past data_head").c_str());
        }

        switch (hdr.type) {
            case PERF_RECORD_SAMPLE: {
                struct { uint64_t ip; uint32_t pid; uint32_t tid; uint64_t period; } body;
                copy_wrapped(data, data_size, (off + sizeof(hdr)) % data_size, &body, sizeof(body));
                samples.push_back({body.ip, body.pid, body.tid, body.period});
                ++hist.sample;
                break;
            }
            case PERF_RECORD_LOST: {
                struct { uint64_t id; uint64_t lost; } body;
                copy_wrapped(data, data_size, (off + sizeof(hdr)) % data_size, &body, sizeof(body));
                hist.lost_events += body.lost;
                ++hist.lost_records;
                break;
            }
            case PERF_RECORD_EXIT:
                ++hist.exit_records;
                break;
            case PERF_RECORD_THROTTLE:
            case PERF_RECORD_UNTHROTTLE:
                die(("PERF_RECORD_THROTTLE/UNTHROTTLE observed on " + label + ": the kernel "
                     "changed the effective sampling rate mid-run. The sample-count-to-"
                     "aggregate relationship no longer holds for this ring. With 24 "
                     "independently-throttled rings (Gate 7), this is materially more likely "
                     "than it was with 2.").c_str());
            default:
                ++hist.other;
                break;
        }
        tail += hdr.size;
    }
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------
// DWARF attribution (Gate 4), factored into a reusable function so it can
// run once per event instead of being duplicated inline. Gate 7: runs once
// per event on the samples concatenated across every per-CPU ring for that
// event, so everything downstream of this (ranking, Wilson bound, support
// gate) is completely unchanged from the single-ring design.
// ---------------------------------------------------------------------
struct SourceLoc { std::string file; int line; };

struct AttributionResult {
    uint64_t bucket_count[4] = {0, 0, 0, 0};
    std::vector<uint64_t> kernel_ips;
    std::map<std::pair<std::string, int>, uint64_t> line_counts;
    uint64_t attributed = 0, unattributed = 0;
    uint64_t ip_lo = 0, ip_hi = 0;
    std::map<uint64_t, SourceLoc> resolve_loc;
    std::map<uint64_t, bool> resolve_ok;
};

AttributionResult classify_and_attribute(const std::vector<Sample>& samples,
                                          const TextRange& target_range,
                                          const DwarfResolver& dwarf) {
    AttributionResult r;
    if (samples.empty()) return r;
    r.ip_lo = r.ip_hi = samples.front().ip;
    for (const auto& s : samples) {
        r.ip_lo = std::min(r.ip_lo, s.ip);
        r.ip_hi = std::max(r.ip_hi, s.ip);
        Bucket b = classify(s.ip, target_range);
        ++r.bucket_count[static_cast<int>(b)];
        if (b == Bucket::kKernel) r.kernel_ips.push_back(s.ip);
        if (b == Bucket::kTarget) {
            auto cached = r.resolve_ok.find(s.ip);
            bool ok;
            if (cached == r.resolve_ok.end()) {
                SourceLoc loc;
                ok = dwarf.resolve(s.ip, loc.file, loc.line);
                r.resolve_ok[s.ip] = ok;
                if (ok) r.resolve_loc[s.ip] = loc;
            } else {
                ok = cached->second;
            }
            if (ok) {
                ++r.attributed;
                ++r.line_counts[{r.resolve_loc[s.ip].file, r.resolve_loc[s.ip].line}];
            } else {
                ++r.unattributed;
            }
        }
    }
    std::sort(r.kernel_ips.begin(), r.kernel_ips.end());
    r.kernel_ips.erase(std::unique(r.kernel_ips.begin(), r.kernel_ips.end()), r.kernel_ips.end());
    return r;
}

// Wilson score lower bound at confidence z on p_hat = m/n, treating m
// (miss samples at a site) as successes out of n (access samples at that
// site) trials. This is the architecture's stated approximation, not an
// exact model: m and n come from two independently sampled event streams,
// not a single shared trial pool. It shrinks noisy small-n ratios toward
// 0.5 rather than trusting the point estimate, which is the whole point —
// a 3/3 site scores far lower than its 1.0 point estimate, a 3000/4000
// site barely moves. z=1.96 is the standard 95% two-sided critical value.
double wilson_lower_bound(double m, double n, double z) {
    if (n <= 0.0) return 0.0;
    double p = m / n;
    double z2 = z * z;
    double denom = 1.0 + z2 / n;
    double center = p + z2 / (2.0 * n);
    // p*(1-p) assumes p in [0,1], true for a real binomial proportion. It
    // is not guaranteed here: m and n are sample counts from two
    // differently-sampled event streams (see period_scale in main), so a
    // site whose miss period is much denser than its access period can
    // legitimately show raw m > n before period-scaling corrects it back
    // down -- that makes p*(1-p) negative, not underflow noise. Clamp the
    // variance term at 0 rather than let a negative value under the sqrt
    // produce NaN; do not treat this as a reason to reject the site.
    double variance_term = std::max(0.0, p * (1.0 - p) / n + z2 / (4.0 * n * n));
    double spread = z * std::sqrt(variance_term);
    return (center - spread) / denom;
}

bool is_prime(uint64_t v) {
    if (v < 2) return false;
    if (v % 2 == 0) return v == 2;
    for (uint64_t i = 3; i * i <= v; i += 2)
        if (v % i == 0) return false;
    return true;
}

uint64_t nearest_prime(uint64_t n) {
    if (n < 2) n = 2;
    uint64_t lo = n, hi = n;
    for (;;) {
        if (is_prime(hi)) return hi;
        if (lo > 2 && is_prime(lo)) return lo;
        ++hi;
        if (lo > 2) --lo;
    }
}

// ---------------------------------------------------------------------
// Phase A: calibration. Both events opened in pure counting mode
// (disabled=1, enable_on_exec=1, no sample_type/sample_period) on one
// throwaway child run, purely to measure real event rates. Gate 7: now
// with inherit=1 so the measured rate is the AGGREGATE across every thread
// the target creates, not just its initial thread -- counting mode has no
// mmap ring buffer, so inherit=1 with cpu=-1 is legal here even though it
// would not be for sampling mode (see file header, U5).
// ---------------------------------------------------------------------
struct EventPlan {
    const char* label;
    uint64_t hw_config;
};

struct Calibration {
    double rate;  // events/sec, from this run's own aggregate count / wall time
};

long open_counting(uint64_t hw_config, pid_t child) {
    struct perf_event_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = hw_config;
    attr.disabled = 1;
    attr.enable_on_exec = 1;
    attr.inherit = 1;  // Gate 7: aggregate across every thread the target creates
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    return perf_event_open(&attr, child, -1, -1, 0);
}

// Both events counted on the SAME child run, as two independent fds (not
// a perf_event_open group) — matches how the real measurement run will
// attach both events, and halves the number of times the target program
// has to execute versus calibrating each event on its own child.
void calibrate_both(const EventPlan plans[2], char** target_argv, Calibration out[2]) {
    pid_t child = fork_stop_exec(target_argv);
    long fd[2];
    for (int i = 0; i < 2; ++i) {
        fd[i] = open_counting(plans[i].hw_config, child);
        if (fd[i] < 0) diagnose_perf_open_failure(plans[i].label);
    }
    if (kill(child, SIGCONT) != 0) die_errno("kill(SIGCONT) [calibration]");
    int exit_code = 0;
    reap(child, &exit_code);
    for (int i = 0; i < 2; ++i) {
        CountReadFormat counts;
        ssize_t n = read(static_cast<int>(fd[i]), &counts, sizeof(counts));
        if (n != static_cast<ssize_t>(sizeof(counts))) die_errno("read(calibration fd)");
        close(static_cast<int>(fd[i]));
        if (counts.time_enabled == 0) die("calibration run: target never reached exec");
        double fraction =
            static_cast<double>(counts.time_running) / static_cast<double>(counts.time_enabled);
        if (fraction < 0.99) {
            die("calibration run: multiplexing fraction below 0.99 with two ungrouped "
                "counters — unexpected on this PMU, refusing to derive periods from a "
                "contended read");
        }
        double wall_seconds = static_cast<double>(counts.time_running) / 1e9;
        out[i].rate = static_cast<double>(counts.value) / wall_seconds;
        std::printf("calibration[%s]: value=%llu wall=%.4fs rate=%.0f/sec\n", plans[i].label,
                    static_cast<unsigned long long>(counts.value), wall_seconds, out[i].rate);
    }
}

// ---------------------------------------------------------------------
// Phase B: measurement. Both events opened in sampling mode on one child,
// once PER ONLINE CPU (Gate 7), each with the same calibrated period
// (see U8 in the file header), each with its own ring buffer.
// ---------------------------------------------------------------------
struct SamplingEvent {
    long fd = -1;
    int cpu = -1;
    void* mmap_base = nullptr;
    size_t mmap_len = 0;
    const uint8_t* data = nullptr;
    size_t data_size = 0;
    struct perf_event_mmap_page* meta = nullptr;
    uint64_t tail = 0;
    std::vector<Sample> samples;
    RecordHistogram hist;
    uint64_t requested_period = 0;
    CountReadFormat counts{};
};

void open_sampling(SamplingEvent& ev, uint64_t hw_config, uint64_t period, pid_t child, int cpu) {
    ev.requested_period = period;
    ev.cpu = cpu;
    struct perf_event_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = hw_config;
    attr.disabled = 1;
    attr.enable_on_exec = 1;
    attr.inherit = 1;  // Gate 7: required to see any thread beyond the target's initial one
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    attr.sample_period = period;
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_PERIOD;
    attr.precise_ip = 0;
    attr.wakeup_events = 1000;

    ev.fd = perf_event_open(&attr, child, cpu, -1, 0);
    if (ev.fd < 0) diagnose_perf_open_failure(ring_label("sampling", cpu));

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) die_errno("sysconf(_SC_PAGESIZE)");
    // 512 KiB data per ring (Gate 7 U7, docs/GATE7_PLAN.md): P3's probe
    // measured that 1 MiB rings do not fit for all 24 (2 events x 12
    // CPUs) under this machine's mlock budget, but 512 KiB does with room
    // to spare (~3.28s of headroom at the aggregate sample-rate target;
    // see the Decisions section of GATE7_PLAN.md).
    const size_t kDataPages = 1UL << 7;
    ev.mmap_len = (1 + kDataPages) * static_cast<size_t>(page_size);
    ev.mmap_base = mmap(nullptr, ev.mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                         static_cast<int>(ev.fd), 0);
    if (ev.mmap_base == MAP_FAILED) die_errno(("mmap(perf ring buffer) " + ring_label("sampling", cpu)).c_str());
    ev.meta = static_cast<struct perf_event_mmap_page*>(ev.mmap_base);
    ev.data = static_cast<uint8_t*>(ev.mmap_base) + ev.meta->data_offset;
    ev.data_size = ev.meta->data_size;
}

void finish_sampling(SamplingEvent& ev) {
    ssize_t n = read(static_cast<int>(ev.fd), &ev.counts, sizeof(ev.counts));
    if (n != static_cast<ssize_t>(sizeof(ev.counts))) die_errno("read(perf fd)");
    munmap(ev.mmap_base, ev.mmap_len);
    close(static_cast<int>(ev.fd));
}

// Prints one event's report, aggregated across every per-CPU ring for that
// event (Gate 7). `rings` holds one SamplingEvent per online CPU;
// `a` is the AttributionResult from classifying the samples concatenated
// across all of them, so bucket/attribution numbers are already combined.
void print_event_report(const char* label, const std::vector<SamplingEvent>& rings,
                         const AttributionResult& a) {
    uint64_t total_value = 0, total_samples = 0;
    RecordHistogram total_hist;
    uint64_t active_cpus = 0;
    for (const auto& ev : rings) {
        total_value += ev.counts.value;
        total_samples += ev.hist.sample;
        total_hist.sample += ev.hist.sample;
        total_hist.lost_records += ev.hist.lost_records;
        total_hist.lost_events += ev.hist.lost_events;
        total_hist.exit_records += ev.hist.exit_records;
        total_hist.other += ev.hist.other;
        if (ev.counts.time_running > 0) ++active_cpus;
    }

    std::printf("\n=== event: %s ===\n", label);
    std::printf("requested period: %llu (uniform across all %zu per-CPU rings)\n",
                static_cast<unsigned long long>(rings.front().requested_period), rings.size());
    std::printf("aggregate (summed across %zu rings, %llu with any scheduled time): %llu\n",
                rings.size(), static_cast<unsigned long long>(active_cpus),
                static_cast<unsigned long long>(total_value));
    std::printf("samples captured (summed): %llu\n", static_cast<unsigned long long>(total_samples));
    std::printf("record histogram (summed): sample=%llu lost_records=%llu lost_events=%llu exit=%llu other=%llu\n",
                static_cast<unsigned long long>(total_hist.sample),
                static_cast<unsigned long long>(total_hist.lost_records),
                static_cast<unsigned long long>(total_hist.lost_events),
                static_cast<unsigned long long>(total_hist.exit_records),
                static_cast<unsigned long long>(total_hist.other));

    std::printf("per-CPU breakdown (rings with any scheduled time):\n");
    for (const auto& ev : rings) {
        if (ev.counts.time_running == 0) continue;
        double frac = static_cast<double>(ev.counts.time_running) /
                      static_cast<double>(ev.counts.time_enabled);
        std::printf("  cpu %d: value=%llu samples=%llu lost_records=%llu lost_events=%llu mux_fraction=%.4f\n",
                    ev.cpu, static_cast<unsigned long long>(ev.counts.value),
                    static_cast<unsigned long long>(ev.hist.sample),
                    static_cast<unsigned long long>(ev.hist.lost_records),
                    static_cast<unsigned long long>(ev.hist.lost_events), frac);
    }

    if (!a.line_counts.empty() || total_samples > 0) {
        std::printf("IP range: [0x%llx, 0x%llx]\n", static_cast<unsigned long long>(a.ip_lo),
                    static_cast<unsigned long long>(a.ip_hi));
    }
    const char* names[4] = {"target executable", "shared library / other user mapping",
                             "kernel space", "unmapped / unclassifiable"};
    double total = static_cast<double>(total_samples);
    for (int i = 0; i < 4; ++i) {
        double pct = total > 0 ? 100.0 * static_cast<double>(a.bucket_count[i]) / total : 0.0;
        std::printf("bucket[%s]: %llu (%.4f%%)\n", names[i],
                    static_cast<unsigned long long>(a.bucket_count[i]), pct);
    }
    if (!a.kernel_ips.empty()) {
        std::printf("distinct kernel-space IPs (%zu):", a.kernel_ips.size());
        for (uint64_t ip : a.kernel_ips) std::printf(" 0x%llx", static_cast<unsigned long long>(ip));
        std::printf("\n");
    }
    uint64_t target_total = a.bucket_count[static_cast<int>(Bucket::kTarget)];
    double unattributed_pct = target_total > 0
        ? 100.0 * static_cast<double>(a.unattributed) / static_cast<double>(target_total) : 0.0;
    std::printf("attributed: %llu, unattributed: %llu (%.4f%%)\n",
                static_cast<unsigned long long>(a.attributed),
                static_cast<unsigned long long>(a.unattributed), unattributed_pct);

    uint64_t out_of_target = a.bucket_count[static_cast<int>(Bucket::kKernel)] +
                              a.bucket_count[static_cast<int>(Bucket::kUnclassifiable)];
    double out_of_target_pct = total > 0 ? 100.0 * static_cast<double>(out_of_target) / total : 0.0;
    if (out_of_target_pct > 1.0) {
        std::fprintf(stderr,
            "cachelens: HALT CONDITION: event %s: kernel+unclassifiable samples are %.4f%% "
            "of total, exceeding 1%%.\n", label, out_of_target_pct);
    }
}

}  // namespace

int main(int argc, char** argv) {
    // --period N overrides both events' periods and skips calibration
    // entirely — the escape hatch for reproducing one specific prior run
    // exactly (calibration reads live event rates, so two automatic runs
    // are not guaranteed to derive identical periods; an override is).
    // Default remains automatic calibration.
    bool period_override = false;
    uint64_t override_period = 0;
    int argi = 1;
    if (argi < argc && std::strcmp(argv[argi], "--period") == 0) {
        if (argi + 1 >= argc) { std::fprintf(stderr, "cachelens: --period needs a value\n"); return 2; }
        char* end = nullptr;
        long long v = std::strtoll(argv[argi + 1], &end, 10);
        if (end == argv[argi + 1] || *end != '\0' || v <= 0) {
            std::fprintf(stderr, "cachelens: --period value must be a positive integer\n");
            return 2;
        }
        period_override = true;
        override_period = static_cast<uint64_t>(v);
        argi += 2;
    }
    if (argi >= argc || std::strcmp(argv[argi], "--") != 0 || argi + 1 >= argc) {
        std::fprintf(stderr, "usage: %s [--period N] -- <command> [args...]\n", argv[0]);
        return 2;
    }
    char** target_argv = argv + argi + 1;

    long nproc_raw = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc_raw <= 0) die_errno("sysconf(_SC_NPROCESSORS_ONLN)");
    const int nr_cpus = static_cast<int>(nproc_raw);

    TextRange target_range;
    if (!get_target_text_range(target_argv[0], target_range)) {
        die("could not determine the target's executable range from its ELF headers");
    }
    DwarfResolver dwarf;
    if (!dwarf.init(target_argv[0])) {
        die("libdw could not load DWARF info from the target — was it built with -g?");
    }

    EventPlan plans[2] = {{"miss", PERF_COUNT_HW_CACHE_MISSES},
                           {"access", PERF_COUNT_HW_CACHE_REFERENCES}};
    uint64_t period[2];

    if (period_override) {
        std::printf("*** --period %llu given: CALIBRATION BYPASSED for both events. ***\n"
                    "*** Concentration scaling below uses this period for both miss and  ***\n"
                    "*** access, not independently-measured rates. The throttle halt (a  ***\n"
                    "*** PERF_RECORD_THROTTLE/UNTHROTTLE record) remains active.          ***\n",
                    static_cast<unsigned long long>(override_period));
        period[0] = period[1] = override_period;
    } else {
        long max_rate = 0;
        FILE* f = fopen("/proc/sys/kernel/perf_event_max_sample_rate", "r");
        if (!f) die_errno("open /proc/sys/kernel/perf_event_max_sample_rate");
        int got = fscanf(f, "%ld", &max_rate);
        fclose(f);
        if (got != 1 || max_rate <= 0) die("could not parse perf_event_max_sample_rate");
        // 60%: Gate 3's empirical bisection on this machine found period
        // choices landing at ~77% of this cap reliable and ~95% flaky
        // (burst variance pushes the instantaneous rate above nominal).
        // 60% keeps real margin below the point already shown marginal.
        // Gate 7 (U6): this remains a single AGGREGATE target rate, not
        // multiplied by nr_cpus -- see U8 in the file header for why the
        // same period derived from it is correct applied per-CPU as-is.
        double target_rate = 0.60 * static_cast<double>(max_rate);
        std::printf("kernel perf_event_max_sample_rate: %ld/sec; aggregate target: %.0f/sec (60%%); "
                    "online CPUs: %d\n", max_rate, target_rate, nr_cpus);

        Calibration cal[2];
        calibrate_both(plans, target_argv, cal);
        for (int i = 0; i < 2; ++i) {
            uint64_t raw = static_cast<uint64_t>(cal[i].rate / target_rate + 0.5);
            period[i] = nearest_prime(raw);
        }
    }

    // Printed unconditionally, calibrated or overridden: this is what makes
    // any run — automatic or not — reproducible by reading the periods
    // back out and passing them via --period on a subsequent run.
    for (int i = 0; i < 2; ++i) {
        std::printf("period[%s]: %llu (%s)\n", plans[i].label,
                    static_cast<unsigned long long>(period[i]),
                    period_override ? "override" : "calibrated");
    }

    // --- Measurement run ---
    pid_t child = fork_stop_exec(target_argv);
    std::vector<SamplingEvent> ev[2];
    for (int i = 0; i < 2; ++i) {
        ev[i].resize(nr_cpus);
        for (int c = 0; c < nr_cpus; ++c) open_sampling(ev[i][c], plans[i].hw_config, period[i], child, c);
    }
    if (kill(child, SIGCONT) != 0) die_errno("kill(SIGCONT)");

    std::vector<struct pollfd> pfd(2 * static_cast<size_t>(nr_cpus));
    for (int i = 0; i < 2; ++i) {
        for (int c = 0; c < nr_cpus; ++c) {
            pfd[static_cast<size_t>(i) * nr_cpus + c] = {static_cast<int>(ev[i][c].fd), POLLIN, 0};
        }
    }

    // Gate 7 Phase 6 (U24, U27): measurement only, nothing behavioral changes
    // here. Records how long each poll-loop iteration actually spends
    // draining ready rings, so "does the drain keep up" is a number
    // instead of an assumption. Negligible overhead (one steady_clock call
    // per iteration, not per sample) and does not affect what gets drained
    // or when.
    std::vector<double> drain_iter_ns;
    bool child_exited = false;
    int status = 0;
    while (!child_exited) {
        int pret = poll(pfd.data(), pfd.size(), 100);
        if (pret < 0 && errno != EINTR) die_errno("poll(perf fds)");
        auto drain_t0 = std::chrono::steady_clock::now();
        bool drained_any = false;
        for (int i = 0; i < 2; ++i) {
            for (int c = 0; c < nr_cpus; ++c) {
                auto& p = pfd[static_cast<size_t>(i) * nr_cpus + c];
                if (p.revents & (POLLIN | POLLHUP | POLLERR)) {
                    auto& e = ev[i][c];
                    drain(e.meta, e.data, e.data_size, e.tail, e.samples, e.hist,
                          ring_label(plans[i].label, c));
                    drained_any = true;
                }
            }
        }
        if (drained_any) {
            auto drain_t1 = std::chrono::steady_clock::now();
            drain_iter_ns.push_back(std::chrono::duration<double, std::nano>(drain_t1 - drain_t0).count());
        }
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w < 0) die_errno("waitpid (drain loop)");
        if (w == child) child_exited = true;
    }
    for (int i = 0; i < 2; ++i) {
        for (int c = 0; c < nr_cpus; ++c) {
            auto& e = ev[i][c];
            drain(e.meta, e.data, e.data_size, e.tail, e.samples, e.hist, ring_label(plans[i].label, c));
        }
    }

    int exit_code = 0;
    if (WIFEXITED(status)) {
        std::printf("child exited, status %d\n", WEXITSTATUS(status));
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        std::printf("child killed by signal %d\n", WTERMSIG(status));
        exit_code = 128 + WTERMSIG(status);
    }

    for (int i = 0; i < 2; ++i) {
        for (int c = 0; c < nr_cpus; ++c) finish_sampling(ev[i][c]);
    }

    // time_enabled tracks wall-clock since the event armed at exec,
    // regardless of which CPU the task actually used (see file header) --
    // so unlike time_running, it must be non-zero on EVERY ring if the
    // target reached exec at all. A zero here on any ring means that
    // ring's counter never armed, which is a real problem, not a benign
    // "target didn't use this CPU" case.
    for (int i = 0; i < 2; ++i) {
        for (int c = 0; c < nr_cpus; ++c) {
            if (ev[i][c].counts.time_enabled == 0) {
                std::fprintf(stderr,
                    "cachelens: warning: %s never armed (time_enabled=0); target likely "
                    "never reached exec. Nothing below is a real measurement.\n",
                    ring_label(plans[i].label, c).c_str());
                return exit_code == 0 ? 1 : exit_code;
            }
        }
    }

    // Halt condition, per event, aggregated across every per-CPU ring —
    // NOT per (event, CPU) as a naive port of the single-ring check would
    // do. Empirically verified against matrix_bad (a single, unpinned
    // thread the scheduler migrates across CPUs over an ~11.8s run): each
    // individual ring's own time_running/time_enabled ratio is low on
    // every CPU it merely passed through (e.g. 0.0002 on a CPU it touched
    // for 2.8ms out of 13.7s total) -- not because of PMU contention, but
    // because `time_enabled` tracks wall-clock since the event armed at
    // exec (the same value on every per-CPU ring), while `time_running`
    // on any ONE cpu-scoped ring only tracks the sliver of that time the
    // task happened to be scheduled on that specific CPU. Summing
    // time_running across all nr_cpus rings for one event reconstructs
    // the task's total scheduled time regardless of which CPU it was on
    // at any instant, and that sum matched time_enabled to the nanosecond
    // in the matrix_bad run above (13,703,492,089 both sides) -- meaning
    // zero genuine PMU contention, exactly as the single-ring design's
    // check intended to verify. Checking any individual ring's own ratio
    // instead would flag ordinary OS scheduling as a fatal multiplexing
    // failure on every multi-CPU machine running an unpinned target.
    for (int i = 0; i < 2; ++i) {
        uint64_t total_running = 0;
        uint64_t time_enabled = 0;
        for (int c = 0; c < nr_cpus; ++c) {
            total_running += ev[i][c].counts.time_running;
            time_enabled = ev[i][c].counts.time_enabled;  // identical on every ring; last write is fine
        }
        double frac = time_enabled > 0
            ? static_cast<double>(total_running) / static_cast<double>(time_enabled) : 0.0;
        if (frac < 0.99) {
            std::fprintf(stderr,
                "cachelens: fatal: HALT CONDITION: event %s: multiplexing fraction %.4f "
                "(sum of time_running across all %d per-CPU rings, over time_enabled) is "
                "below 0.99. The concentration ratio would be silently wrong; refusing to "
                "compute it.\n", plans[i].label, frac, nr_cpus);
            return 1;
        }
    }

    // Halt condition: every sample's kernel-reported PERF_SAMPLE_PERIOD
    // must equal what its ring actually requested (uniform across CPUs for
    // a given event, per U8) -- a mismatch means the kernel granted
    // something other than what was asked for, and the period-scaling
    // below would then be silently wrong.
    for (int i = 0; i < 2; ++i) {
        for (int c = 0; c < nr_cpus; ++c) {
            for (const auto& s : ev[i][c].samples) {
                if (s.period != ev[i][c].requested_period) {
                    std::fprintf(stderr,
                        "cachelens: fatal: HALT CONDITION: %s: a sample reports "
                        "PERF_SAMPLE_PERIOD=%llu but the requested period was %llu.\n",
                        ring_label(plans[i].label, c).c_str(),
                        static_cast<unsigned long long>(s.period),
                        static_cast<unsigned long long>(ev[i][c].requested_period));
                    return 1;
                }
            }
        }
    }

    // Concatenate every per-CPU ring's samples per event before
    // attribution, so everything downstream (ranking, Wilson bound,
    // support gate) is unchanged from the single-ring design.
    std::vector<Sample> combined_samples[2];
    for (int i = 0; i < 2; ++i) {
        size_t total = 0;
        for (const auto& e : ev[i]) total += e.samples.size();
        combined_samples[i].reserve(total);
        for (const auto& e : ev[i]) {
            combined_samples[i].insert(combined_samples[i].end(), e.samples.begin(), e.samples.end());
        }
    }

    AttributionResult attr[2];
    for (int i = 0; i < 2; ++i) attr[i] = classify_and_attribute(combined_samples[i], target_range, dwarf);
    for (int i = 0; i < 2; ++i) print_event_report(plans[i].label, ev[i], attr[i]);

    // Gate 7 Phase 6 (U24): drain service time, measured not assumed.
    if (!drain_iter_ns.empty()) {
        std::vector<double> sorted_ns = drain_iter_ns;
        std::sort(sorted_ns.begin(), sorted_ns.end());
        auto at = [&](double q) { return sorted_ns[std::min(sorted_ns.size() - 1,
                                       static_cast<size_t>(q * sorted_ns.size()))]; };
        double sum = 0;
        for (double v : sorted_ns) sum += v;
        std::printf("\n=== drain path (Gate 7 Phase 6, U24) ===\n");
        std::printf("drain-active poll iterations: %zu, mean=%.0fns p50=%.0fns p99=%.0fns max=%.0fns\n",
                    sorted_ns.size(), sum / sorted_ns.size(), at(0.50), at(0.99), sorted_ns.back());
    }
    {
        uint64_t total_lost_records = 0, total_lost_events = 0;
        for (int i = 0; i < 2; ++i)
            for (const auto& e : ev[i]) { total_lost_records += e.hist.lost_records; total_lost_events += e.hist.lost_events; }
        std::printf("total lost_records=%llu, total lost_events=%llu (the real SLO; drain "
                    "latency above is only a proxy for it)\n",
                    static_cast<unsigned long long>(total_lost_records),
                    static_cast<unsigned long long>(total_lost_events));
    }

    // --- Concentration ranking (unchanged from the single-ring design —
    // it operates only on attr[].line_counts, a plain map, regardless of
    // how many rings fed into it) ---
    // period_scale converts a raw sample-count ratio (m/n) into an
    // estimate of the true event-count ratio: E[true_miss] ~= m*period_miss,
    // E[true_access] ~= n*period_access, so concentration ~= (m/n) *
    // (period_miss/period_access). This scale factor is the same constant
    // for every site in this run.
    double period_scale =
        static_cast<double>(period[0]) / static_cast<double>(period[1]);
    std::printf("\nperiod scale (miss/access): %llu / %llu = %.6f\n",
                static_cast<unsigned long long>(period[0]),
                static_cast<unsigned long long>(period[1]), period_scale);

    // Support gate: sites with fewer than kMinAccessSamples access samples
    // are excluded from ranking, reported separately. 30 is the standard
    // rule-of-thumb minimum sample size for a normal-approximation CI to
    // be reasonable, and is also docs/ARCHITECTURE.md's own stated
    // starting point -- chosen before this run, not fit to its outcome.
    // Gate 7 U12: not re-tuned for a fragmented multithreaded workload; if
    // it excludes the interesting site, that is reported, not rescued.
    constexpr uint64_t kMinAccessSamples = 30;
    constexpr double kZ95 = 1.96;

    std::map<std::pair<std::string, int>, uint64_t> miss_counts = attr[0].line_counts;
    std::map<std::pair<std::string, int>, uint64_t> access_counts = attr[1].line_counts;

    std::vector<std::pair<std::string, int>> all_sites;
    for (const auto& [site, c] : miss_counts) all_sites.push_back(site);
    for (const auto& [site, c] : access_counts) all_sites.push_back(site);
    std::sort(all_sites.begin(), all_sites.end());
    all_sites.erase(std::unique(all_sites.begin(), all_sites.end()), all_sites.end());

    struct Ranked {
        std::string file; int line;
        uint64_t m, n;
        double concentration, wilson_concentration;
    };
    std::vector<Ranked> ranked;
    std::vector<std::pair<std::string, int>> insufficient;

    for (const auto& site : all_sites) {
        uint64_t m = miss_counts.count(site) ? miss_counts[site] : 0;
        uint64_t n = access_counts.count(site) ? access_counts[site] : 0;
        if (n < kMinAccessSamples) {
            insufficient.push_back(site);
            continue;
        }
        double point = (static_cast<double>(m) / static_cast<double>(n)) * period_scale;
        double wlb = wilson_lower_bound(static_cast<double>(m), static_cast<double>(n), kZ95) *
                     period_scale;
        ranked.push_back({site.first, site.second, m, n, point, wlb});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) { return a.wilson_concentration > b.wilson_concentration; });

    std::printf("\n=== concentration ranking (Wilson lower bound, 95%%, min %llu access samples) ===\n",
                static_cast<unsigned long long>(kMinAccessSamples));
    for (size_t i = 0; i < ranked.size(); ++i) {
        const auto& r = ranked[i];
        std::printf("  #%zu  %s:%d  miss=%llu access=%llu  concentration=%.6f  wilson_lb=%.6f\n",
                    i + 1, r.file.c_str(), r.line, static_cast<unsigned long long>(r.m),
                    static_cast<unsigned long long>(r.n), r.concentration, r.wilson_concentration);
    }
    std::printf("insufficient samples (< %llu access samples), excluded from ranking (%zu sites):\n",
                static_cast<unsigned long long>(kMinAccessSamples), insufficient.size());
    for (const auto& site : insufficient) {
        uint64_t m = miss_counts.count(site) ? miss_counts[site] : 0;
        uint64_t n = access_counts.count(site) ? access_counts[site] : 0;
        std::printf("  %s:%d  miss=%llu access=%llu\n", site.first.c_str(), site.second,
                    static_cast<unsigned long long>(m), static_cast<unsigned long long>(n));
    }

    std::printf("\n=== raw miss-count ranking (for comparison) ===\n");
    std::vector<std::pair<std::pair<std::string, int>, uint64_t>> by_raw(miss_counts.begin(),
                                                                          miss_counts.end());
    std::sort(by_raw.begin(), by_raw.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < by_raw.size(); ++i) {
        std::printf("  #%zu  %s:%d  miss=%llu\n", i + 1, by_raw[i].first.first.c_str(),
                    by_raw[i].first.second, static_cast<unsigned long long>(by_raw[i].second));
    }

    return exit_code;
}
