// Gate 7 probe P1 (U1): does a per-CPU task event
// (pid = getpid(), cpu = i, i in [0, nproc)) open and mmap at the
// kernel.perf_event_paranoid level actually configured on this machine?
//
// The man page's paranoid language is written for the pid=-1 (system-wide)
// case. The kernel gates CPU-scoped events on task != NULL in
// find_get_context, so a pid>0,cpu>=0 event *should* be permitted for a
// process the caller owns -- this probe tests that claim rather than
// trusting the man page's phrasing.
//
// Usage: p1_percpu_open
// Exits 0 if every online CPU opened+mmapped; 1 otherwise. Always prints
// one line per CPU so a partial-permission case is visible, not inferred
// from CPU 0 alone.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/perf_event.h>

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

int main(void) {
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc <= 0) { perror("sysconf(_SC_NPROCESSORS_ONLN)"); return 1; }
    long page_size = sysconf(_SC_PAGESIZE);

    FILE *pf = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    int paranoid = -999;
    if (pf) { if (fscanf(pf, "%d", &paranoid) != 1) paranoid = -999; fclose(pf); }
    printf("p1_percpu_open: kernel.perf_event_paranoid=%d, online CPUs=%ld\n", paranoid, nproc);

    int all_ok = 1;
    for (long cpu = 0; cpu < nproc; ++cpu) {
        struct perf_event_attr attr;
        memset(&attr, 0, sizeof(attr));
        attr.size = sizeof(attr);
        attr.type = PERF_TYPE_HARDWARE;
        attr.config = PERF_COUNT_HW_CACHE_MISSES;
        attr.disabled = 1;
        attr.enable_on_exec = 0;   // this is a self-measuring probe, no exec boundary
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        attr.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_PERIOD;
        attr.sample_period = 10000;

        long fd = perf_event_open(&attr, getpid(), (int)cpu, -1, 0);
        if (fd < 0) {
            printf("  cpu %ld: perf_event_open FAILED: errno=%d (%s)\n", cpu, errno, strerror(errno));
            all_ok = 0;
            continue;
        }
        size_t mmap_len = (size_t)(1 + 1) * (size_t)page_size;  // 1 data page, minimum
        void *base = mmap(NULL, mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
        if (base == MAP_FAILED) {
            printf("  cpu %ld: perf_event_open OK, mmap FAILED: errno=%d (%s)\n", cpu, errno, strerror(errno));
            all_ok = 0;
            close((int)fd);
            continue;
        }
        printf("  cpu %ld: perf_event_open OK, mmap OK\n", cpu);
        munmap(base, mmap_len);
        close((int)fd);
    }

    printf("p1_percpu_open: %s\n", all_ok ? "PASS (all CPUs opened+mmapped)" : "FAIL (see per-CPU lines above)");
    return all_ok ? 0 : 1;
}
