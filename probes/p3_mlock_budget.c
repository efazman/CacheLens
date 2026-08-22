// Gate 7 probe P3 (U3): how many perf ring buffers of what size can
// actually be mmapped on this machine, against the mlock budget the
// kernel charges for perf ring buffers (perf_event_mlock_kb *
// num_online_cpus, RLIMIT_MEMLOCK on top).
//
// For each candidate ring size, attempts to open+mmap 2 * nproc rings
// (2 events x per-CPU, matching U5 Option A's shape) and reports how many
// succeeded before the first failure, plus the errno of that failure.
//
// Usage: p3_mlock_budget

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/perf_event.h>

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

struct Ring { long fd; void *base; size_t len; };

static int open_and_map_one(int cpu, size_t data_pages, long page_size, struct Ring *out) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = PERF_COUNT_HW_CACHE_MISSES;
    attr.disabled = 1;
    attr.enable_on_exec = 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_PERIOD;
    attr.sample_period = 100000;

    long fd = perf_event_open(&attr, getpid(), cpu, -1, 0);
    if (fd < 0) { out->fd = fd; return -1; }
    size_t len = (1 + data_pages) * (size_t)page_size;
    void *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    if (base == MAP_FAILED) { out->fd = fd; out->base = NULL; return -2; }
    out->fd = fd; out->base = base; out->len = len;
    return 0;
}

static void close_ring(struct Ring *r) {
    if (r->base) munmap(r->base, r->len);
    if (r->fd >= 0) close((int)r->fd);
}

int main(void) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    long page_size = sysconf(_SC_PAGESIZE);
    long want = 2 * nproc;  // 2 events x per-CPU

    struct rlimit rl;
    getrlimit(RLIMIT_MEMLOCK, &rl);
    FILE *pf = fopen("/proc/sys/kernel/perf_event_mlock_kb", "r");
    long mlock_kb = -1;
    if (pf) { if (fscanf(pf, "%ld", &mlock_kb) != 1) mlock_kb = -1; fclose(pf); }

    printf("p3_mlock_budget: nproc=%ld, target rings=%ld (2 events x per-CPU)\n", nproc, want);
    printf("p3_mlock_budget: perf_event_mlock_kb=%ld, RLIMIT_MEMLOCK=%ld bytes (%.2f MiB)%s\n",
           mlock_kb, (long)rl.rlim_cur, rl.rlim_cur == RLIM_INFINITY ? 0.0 : (double)rl.rlim_cur / (1024.0*1024.0),
           rl.rlim_cur == RLIM_INFINITY ? " [unlimited]" : "");
    double predicted_mib = mlock_kb > 0 ? (double)mlock_kb * (double)nproc / 1024.0 : -1.0;
    printf("p3_mlock_budget: predicted allowance = perf_event_mlock_kb * nproc = %.2f MiB\n", predicted_mib);

    struct { size_t pages; const char *name; } sizes[] = {
        {256, "1 MiB"}, {128, "512 KiB"}, {64, "256 KiB"}, {32, "128 KiB"}, {16, "64 KiB"},
    };

    for (size_t s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
        size_t data_pages = sizes[s].pages;
        size_t ring_bytes = (1 + data_pages) * (size_t)page_size;
        struct Ring *rings = calloc((size_t)want, sizeof(struct Ring));
        long opened = 0;
        int fail_kind = 0;  // 0=none, -1=open failed, -2=mmap failed
        int fail_errno = 0;
        for (long i = 0; i < want; ++i) {
            int cpu = (int)(i % nproc);
            int rc = open_and_map_one(cpu, data_pages, page_size, &rings[i]);
            if (rc != 0) { fail_kind = rc; fail_errno = errno; break; }
            ++opened;
        }
        double total_mib = (double)opened * (double)ring_bytes / (1024.0*1024.0);
        if (fail_kind == 0) {
            printf("ring size %-8s (%zu data pages): ALL %ld rings opened+mmapped (%.2f MiB total)\n",
                   sizes[s].name, data_pages, opened, total_mib);
        } else {
            printf("ring size %-8s (%zu data pages): %ld/%ld rings succeeded (%.2f MiB) before %s failed: errno=%d (%s)\n",
                   sizes[s].name, data_pages, opened, want, total_mib,
                   fail_kind == -1 ? "perf_event_open" : "mmap", fail_errno, strerror(fail_errno));
        }
        for (long i = 0; i < opened; ++i) close_ring(&rings[i]);
        free(rings);
    }
    return 0;
}
