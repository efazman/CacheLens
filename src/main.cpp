// cachelens — increment 2: sampling mode + ring buffer drain.
//
// Forks a child, stops it before exec, opens a sampling event (cache-misses,
// fixed sample_period) attached to the child's pid with enable_on_exec=1,
// resumes the child, drains the mmap ring buffer while it runs, and reports
// the raw sampled instruction pointers.
//
// Deliberately NOT here yet: DWARF attribution, a second event, ranking.
// precise_ip is pinned at 0 — Zen 4 has no PEBS-equivalent for this event
// (precise_ip=2 and 1 both fail ENOENT; see docs/TAKEAWAYS.md). Samples are
// therefore skidded by an unknown amount; that is a known, accepted limit
// of this increment, not a bug.
//
// Usage: cachelens [--period N] -- <command> [args...]

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <elf.h>
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
    // No glibc wrapper exists for this syscall; go through syscall(2) directly.
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

// perf_event_open failure diagnosis for the two common causes on this class
// of machine: a too-restrictive perf_event_paranoid setting, and a config
// value the running PMU doesn't implement. Anything else falls through to
// the generic errno message in die_errno — still fails loudly, just without
// a canned explanation.
void diagnose_perf_open_failure() {
    if (errno == EACCES || errno == EPERM) {
        std::fprintf(stderr,
            "cachelens: fatal: perf_event_open: %s (errno %d)\n"
            "  Diagnosis: kernel.perf_event_paranoid is likely too restrictive\n"
            "  for this event/attach mode. Check with:\n"
            "    cat /proc/sys/kernel/perf_event_paranoid\n"
            "  and compare against perf_event_open(2)'s paranoid table.\n",
            std::strerror(errno), errno);
        std::exit(1);
    }
    if (errno == ENOENT) {
        std::fprintf(stderr,
            "cachelens: fatal: perf_event_open: %s (errno %d)\n"
            "  Diagnosis: this event (type/config pair) is not implemented by\n"
            "  the running PMU. Cross-check with `perf list` on this machine.\n",
            std::strerror(errno), errno);
        std::exit(1);
    }
    die_errno("perf_event_open");
}

struct Sample {
    uint64_t ip;
    uint32_t pid;
    uint32_t tid;
};

struct TextRange { uint64_t lo, hi; };  // [lo, hi), union of executable PT_LOAD segments

// Reads the target's own ELF program headers to find its executable range,
// statically — no /proc/pid/maps, no running process required. Correct only
// because the target is built -no-pie: link-time vaddr == runtime address,
// so this is valid before the child even execs. Does not resolve a bare
// command name via PATH; matches how this project always invokes targets
// (a path containing a slash).
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

// x86-64 canonical address partitioning: bit 47 is sign-extended by real
// hardware, so [0x0000_8000_0000_0000, 0xffff_7fff_ffff_ffff] can never be a
// genuine address on this architecture. A sample landing there is a decode
// or corruption signal, not "some mapping we don't know about" — hence its
// own bucket rather than folding it into "other user mapping".
Bucket classify(uint64_t ip, const TextRange& target) {
    constexpr uint64_t kKernelHalfStart = 0xffff800000000000ULL;
    constexpr uint64_t kUserHalfEnd = 0x0000800000000000ULL;  // exclusive
    if (ip >= kKernelHalfStart) return Bucket::kKernel;
    if (ip >= kUserHalfEnd) return Bucket::kUnclassifiable;
    if (ip >= target.lo && ip < target.hi) return Bucket::kTarget;
    return Bucket::kOtherUser;
}

// Copies `len` bytes starting at circular offset `off` (already < data_size)
// out of the ring's data area, splitting the copy in two if it crosses the
// physical end of the mmap'd region back to offset 0. This is ordinary
// circular-buffer indexing, distinct from the overflow check in drain(): a
// record can legitimately straddle the wrap point long before the buffer
// as a whole has overflowed.
void copy_wrapped(const uint8_t* data, size_t data_size, size_t off, void* dst, size_t len) {
    size_t first = std::min(len, data_size - off);
    std::memcpy(dst, data + off, first);
    if (first < len) std::memcpy(static_cast<uint8_t*>(dst) + first, data, len - first);
}

// Drains whatever the kernel has published since the last call. `tail` is
// owned entirely by this (single-threaded) consumer; `data_head` is owned by
// the kernel producer, which is why it alone needs the acquire load — it
// pairs with the kernel's release store after writing a record, and without
// it we could read a record the kernel hasn't finished writing yet. The
// release store on data_tail pairs with the kernel's own acquire load of it
// before deciding whether writing further would overwrite unread data.
void drain(struct perf_event_mmap_page* meta, const uint8_t* data, size_t data_size,
           uint64_t& tail, std::vector<Sample>& samples, uint64_t& lost_count,
           uint64_t& unknown_count) {
    uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);

    if (head - tail > data_size) {
        die("ring buffer overflow: data_head - data_tail exceeds data_size — "
            "unread samples were overwritten. Raise --period or shrink the "
            "workload; this build deliberately does not attempt recovery.");
    }

    while (tail < head) {
        size_t off = tail % data_size;
        struct perf_event_header hdr;
        copy_wrapped(data, data_size, off, &hdr, sizeof(hdr));

        if (hdr.size < sizeof(hdr)) {
            die("malformed ring buffer record: header.size smaller than the header itself");
        }
        if (tail + hdr.size > head) {
            die("malformed ring buffer record: header.size runs past data_head");
        }

        if (hdr.type == PERF_RECORD_SAMPLE) {
            struct { uint64_t ip; uint32_t pid; uint32_t tid; } body;
            copy_wrapped(data, data_size, (off + sizeof(hdr)) % data_size, &body, sizeof(body));
            samples.push_back({body.ip, body.pid, body.tid});
        } else if (hdr.type == PERF_RECORD_LOST) {
            struct { uint64_t id; uint64_t lost; } body;
            copy_wrapped(data, data_size, (off + sizeof(hdr)) % data_size, &body, sizeof(body));
            lost_count += body.lost;
        } else {
            // Anything else (MMAP, COMM, THROTTLE, ...) is out of scope for
            // this increment; counted, not decoded, not silently dropped.
            ++unknown_count;
        }

        tail += hdr.size;
    }

    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t sample_period = 100003;  // prime: avoids harmonic aliasing with loop trip counts
    int argi = 1;
    if (argi < argc && std::strcmp(argv[argi], "--period") == 0) {
        if (argi + 1 >= argc) { std::fprintf(stderr, "cachelens: --period needs a value\n"); return 2; }
        char* end = nullptr;
        long long v = std::strtoll(argv[argi + 1], &end, 10);
        if (end == argv[argi + 1] || *end != '\0' || v <= 0) {
            std::fprintf(stderr, "cachelens: --period value must be a positive integer\n");
            return 2;
        }
        sample_period = static_cast<uint64_t>(v);
        argi += 2;
    }
    if (argi >= argc || std::strcmp(argv[argi], "--") != 0 || argi + 1 >= argc) {
        std::fprintf(stderr, "usage: %s [--period N] -- <command> [args...]\n", argv[0]);
        return 2;
    }
    char** target_argv = argv + argi + 1;

    TextRange target_range;
    if (!get_target_text_range(target_argv[0], target_range)) {
        die("could not determine the target's executable range from its ELF headers "
            "(bad path, not ELF64, no executable PT_LOAD segment, or the name needs "
            "PATH resolution this tool does not do) — sample classification depends on it");
    }

    pid_t child = fork();
    if (child < 0) die_errno("fork");

    if (child == 0) {
        // Child: disable ASLR so a later increment can map sampled IPs back
        // to link-time addresses without runtime slide bookkeeping. Must
        // happen before exec — it's the exec'd image that gets laid out.
        if (personality(ADDR_NO_RANDOMIZE) == -1) {
            std::fprintf(stderr, "cachelens: child: personality: %s\n", std::strerror(errno));
            _exit(1);
        }
        // Stop ourselves so the parent can open the counter — attached to
        // this exact pid — before any target code (and its cache traffic)
        // runs. The parent resumes us with SIGCONT once the counter exists.
        if (raise(SIGSTOP) != 0) {
            std::fprintf(stderr, "cachelens: child: raise(SIGSTOP): %s\n", std::strerror(errno));
            _exit(1);
        }
        execvp(target_argv[0], target_argv);
        std::fprintf(stderr, "cachelens: child: execvp(%s): %s\n", target_argv[0],
                     std::strerror(errno));
        _exit(127);
    }

    // Parent: wait for the child to hit its self-inflicted SIGSTOP before
    // touching perf_event_open, so the attach point is well-defined.
    int status = 0;
    if (waitpid(child, &status, WUNTRACED) < 0) die_errno("waitpid (initial stop)");
    if (!WIFSTOPPED(status)) die("child did not stop as expected before exec");

    struct perf_event_attr attr;
    std::memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = PERF_COUNT_HW_CACHE_MISSES;
    attr.disabled = 1;
    attr.enable_on_exec = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.inherit = 0;
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    attr.sample_period = sample_period;               // fixed period, not freq — see header
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
    attr.precise_ip = 0;                               // only value Zen 4 accepts for this event
    attr.wakeup_events = 1000;                          // see kDataPages comment below

    long fd = perf_event_open(&attr, child, -1, -1, 0);
    if (fd < 0) diagnose_perf_open_failure();

    // Ring buffer: 1 metadata page + 2^n data pages. n=8 -> 256 data pages
    // * 4 KiB = exactly 1 MiB, comfortably above the ~552 KB a full
    // matrix_bad run produces at this period (23k samples * 24 bytes), so
    // wraparound is not expected in practice for these benchmarks — but the
    // check above fires loudly if that assumption is ever wrong.
    // wakeup_events=1000 wakes the drain loop well before the ~43,690-record
    // capacity, with headroom to spare.
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) die_errno("sysconf(_SC_PAGESIZE)");
    const size_t kDataPages = 1UL << 8;
    size_t mmap_len = (1 + kDataPages) * static_cast<size_t>(page_size);
    void* base = mmap(nullptr, mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                       static_cast<int>(fd), 0);
    if (base == MAP_FAILED) die_errno("mmap(perf ring buffer)");
    auto* meta = static_cast<struct perf_event_mmap_page*>(base);
    const uint8_t* data = static_cast<uint8_t*>(base) + meta->data_offset;
    size_t data_size = meta->data_size;

    if (kill(child, SIGCONT) != 0) die_errno("kill(SIGCONT)");

    std::vector<Sample> samples;
    uint64_t tail = 0, lost_count = 0, unknown_count = 0;
    struct pollfd pfd { static_cast<int>(fd), POLLIN, 0 };
    bool child_exited = false;
    while (!child_exited) {
        int pret = poll(&pfd, 1, 100 /* ms: also re-checks waitpid on timeout */);
        if (pret < 0 && errno != EINTR) die_errno("poll(perf fd)");
        if (pret > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            drain(meta, data, data_size, tail, samples, lost_count, unknown_count);
        }
        pid_t w = waitpid(child, &status, WNOHANG);
        if (w < 0) die_errno("waitpid (drain loop)");
        if (w == child) child_exited = true;
    }
    drain(meta, data, data_size, tail, samples, lost_count, unknown_count);  // final catch-up

    struct read_format {
        uint64_t value;
        uint64_t time_enabled;
        uint64_t time_running;
    } counts;
    ssize_t n = read(static_cast<int>(fd), &counts, sizeof(counts));
    if (n != static_cast<ssize_t>(sizeof(counts))) die_errno("read(perf fd)");
    munmap(base, mmap_len);
    close(static_cast<int>(fd));

    int exit_code = 0;
    if (WIFEXITED(status)) {
        std::printf("child exited, status %d\n", WEXITSTATUS(status));
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        std::printf("child killed by signal %d\n", WTERMSIG(status));
        exit_code = 128 + WTERMSIG(status);
    }

    if (counts.time_enabled == 0) {
        std::fprintf(stderr,
            "cachelens: warning: counter was never enabled (time_enabled=0 ns). "
            "The target likely never reached exec, so nothing below is a real measurement.\n");
        if (exit_code == 0) exit_code = 1;
        return exit_code;
    }

    double fraction =
        static_cast<double>(counts.time_running) / static_cast<double>(counts.time_enabled);
    std::printf("cache-misses (aggregate): %llu\n", static_cast<unsigned long long>(counts.value));
    std::printf("multiplexing fraction: %.4f\n", fraction);
    if (fraction < 1.0) {
        std::fprintf(stderr,
            "cachelens: warning: counter was only scheduled %.2f%% of the time (PMU multiplexing).\n",
            fraction * 100.0);
    }

    std::printf("sample_period: %llu\n", static_cast<unsigned long long>(sample_period));
    std::printf("samples captured: %zu\n", samples.size());
    std::printf("records lost: %llu\n", static_cast<unsigned long long>(lost_count));
    std::printf("unknown records skipped: %llu\n", static_cast<unsigned long long>(unknown_count));
    std::printf("target text range: [0x%llx, 0x%llx)\n",
                static_cast<unsigned long long>(target_range.lo),
                static_cast<unsigned long long>(target_range.hi));

    // Every sample lands in exactly one bucket; all four are always printed,
    // never silently dropped, even when zero. Only kTarget feeds attribution
    // and ranking in later increments.
    uint64_t bucket_count[4] = {0, 0, 0, 0};
    std::vector<uint64_t> kernel_ips;
    if (!samples.empty()) {
        uint64_t lo = samples.front().ip, hi = samples.front().ip;
        for (const auto& s : samples) {
            lo = std::min(lo, s.ip);
            hi = std::max(hi, s.ip);
            Bucket b = classify(s.ip, target_range);
            ++bucket_count[static_cast<int>(b)];
            if (b == Bucket::kKernel) kernel_ips.push_back(s.ip);
        }
        std::printf("IP range: [0x%llx, 0x%llx]\n", static_cast<unsigned long long>(lo),
                    static_cast<unsigned long long>(hi));
    }

    double total = static_cast<double>(samples.size());
    const char* names[4] = {"target executable", "shared library / other user mapping",
                             "kernel space", "unmapped / unclassifiable"};
    for (int i = 0; i < 4; ++i) {
        double pct = total > 0 ? 100.0 * static_cast<double>(bucket_count[i]) / total : 0.0;
        std::printf("bucket[%s]: %llu (%.4f%%)\n", names[i],
                    static_cast<unsigned long long>(bucket_count[i]), pct);
    }

    if (!kernel_ips.empty()) {
        std::sort(kernel_ips.begin(), kernel_ips.end());
        kernel_ips.erase(std::unique(kernel_ips.begin(), kernel_ips.end()), kernel_ips.end());
        std::printf("distinct kernel-space IPs (%zu):", kernel_ips.size());
        for (uint64_t ip : kernel_ips) std::printf(" 0x%llx", static_cast<unsigned long long>(ip));
        std::printf("\n");
    }

    uint64_t out_of_target = bucket_count[static_cast<int>(Bucket::kKernel)] +
                              bucket_count[static_cast<int>(Bucket::kUnclassifiable)];
    double out_of_target_pct = total > 0 ? 100.0 * static_cast<double>(out_of_target) / total : 0.0;
    if (out_of_target_pct > 1.0) {
        std::fprintf(stderr,
            "cachelens: HALT CONDITION: kernel + unclassifiable samples are %.4f%% of "
            "total, exceeding the 1%% threshold. Something has materially changed from "
            "the 0.01-0.05%% baseline; do not trust this run's attribution.\n",
            out_of_target_pct);
    }

    return exit_code;
}
