// cachelens — increment 1: counting-mode perf_event_open validation.
//
// Forks a child, stops it before exec, opens a single hardware counter
// (cache-misses) attached to that child's pid with enable_on_exec=1, resumes
// the child, waits for it to exit, and reads the counter.
//
// Deliberately NOT here yet: sampling, mmap ring buffers, DWARF attribution.
// Those are later increments (see docs/ARCHITECTURE.md).
//
// Usage: cachelens -- <command> [args...]

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <linux/perf_event.h>
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "--") != 0) {
        std::fprintf(stderr, "usage: %s -- <command> [args...]\n", argv[0]);
        return 2;
    }
    char** target_argv = argv + 2;  // argv[2] is the target executable

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
        // Only reached if exec failed.
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
    std::memset(&attr, 0, sizeof(attr));  // zero every field not set below, deliberately

    attr.size = sizeof(attr);  // mandatory: lets the kernel version the struct by size

    attr.type = PERF_TYPE_HARDWARE;         // generalized hardware event, not raw/cache-tuple
    attr.config = PERF_COUNT_HW_CACHE_MISSES;  // the event this increment counts

    attr.disabled = 1;        // child isn't running yet; don't count time-to-exec
    attr.enable_on_exec = 1;  // arm exactly at execve, not at this open() call

    attr.exclude_kernel = 1;  // don't attribute kernel-side cycles to this event
    attr.exclude_hv = 1;      // same, for hypervisor context; both keep this working
                               // without CAP_PERFMON even if paranoid tightens later

    attr.inherit = 0;   // child is not expected to fork; don't propagate to descendants
    attr.pinned = 0;    // single ungrouped counter; no need to demand permanent PMU residency
    attr.exclusive = 0; // not claiming exclusive use of the PMU

    // read_format: request enabled/running time alongside the raw count.
    // This increment has exactly one event, so PMU multiplexing shouldn't
    // happen — but the read()-side accounting for it has to exist now,
    // because the next increment adds a second event and multiplexing
    // becomes a real possibility the moment two hardware counters compete
    // for the same PMU slots.
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

    // Left at zero deliberately: this is counting mode, not sampling.
    //   sample_period / sample_freq — only meaningful once we sample
    //   sample_type                 — no ring-buffer record layout to declare yet
    //   wakeup_events / wakeup_watermark — no ring buffer to wake on
    //   mmap / comm / task / precise_ip — no sampling metadata requested
    // Those all get filled in when the sampler increment adds mmap rings.

    long fd = perf_event_open(&attr, child, -1 /* cpu: any */, -1 /* group_fd: ungrouped */,
                               0 /* flags */);
    if (fd < 0) diagnose_perf_open_failure();

    if (kill(child, SIGCONT) != 0) die_errno("kill(SIGCONT)");

    if (waitpid(child, &status, 0) < 0) die_errno("waitpid (exit)");

    struct read_format {
        uint64_t value;
        uint64_t time_enabled;
        uint64_t time_running;
    } counts;

    ssize_t n = read(static_cast<int>(fd), &counts, sizeof(counts));
    if (n != static_cast<ssize_t>(sizeof(counts))) die_errno("read(perf fd)");
    close(static_cast<int>(fd));

    int exit_code = 0;
    if (WIFEXITED(status)) {
        std::printf("child exited, status %d\n", WEXITSTATUS(status));
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        std::printf("child killed by signal %d\n", WTERMSIG(status));
        exit_code = 128 + WTERMSIG(status);  // conventional shell convention for signal death
    }

    std::printf("cache-misses: %llu\n", static_cast<unsigned long long>(counts.value));

    // time_enabled==0 means enable_on_exec never fired, i.e. the child never
    // reached execve (see its stderr above for why). That's a distinct
    // failure from multiplexing and must not be reported as one — a 0/0
    // fraction and a "PMU multiplexing" warning would actively mislead
    // about why the count is meaningless.
    if (counts.time_enabled == 0) {
        std::fprintf(stderr,
            "cachelens: warning: counter was never enabled (time_enabled=0 ns). "
            "The target likely never reached exec, so cache-misses above is not "
            "a real measurement.\n");
        if (exit_code == 0) exit_code = 1;  // don't report success for a non-measurement
        return exit_code;
    }

    double fraction =
        static_cast<double>(counts.time_running) / static_cast<double>(counts.time_enabled);
    std::printf("time_enabled: %llu ns, time_running: %llu ns, multiplexing fraction: %.4f\n",
                static_cast<unsigned long long>(counts.time_enabled),
                static_cast<unsigned long long>(counts.time_running), fraction);

    if (fraction < 1.0) {
        std::fprintf(stderr,
            "cachelens: warning: counter was only scheduled %.2f%% of the time "
            "(PMU multiplexing). Reported count is scaled by the kernel but the "
            "estimate is noisier than a fully-scheduled run.\n",
            fraction * 100.0);
    }

    return exit_code;
}
