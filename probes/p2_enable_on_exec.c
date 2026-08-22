// Gate 7 probe P2 (U2): does enable_on_exec=1 arm correctly on a cpu>=0
// task event, the same way it is already trusted to on cpu=-1 in
// src/main.cpp? "Counters arm exactly at the execve boundary" is
// load-bearing for every number cachelens produces; it is verified for
// cpu=-1 and unverified for cpu>=0 until this probe runs.
//
// Three cases, each fork+self-stop+parent-opens-event, shaped exactly like
// fork_stop_exec() in src/main.cpp:
//   A) SIGCONT -> child execs p2_victim -> counters should show non-zero
//      value and time_running approx equal to the victim's own runtime
//      (not the fork-to-exec gap).
//   B) child is killed while still stopped, before any SIGCONT/exec ->
//      counters should read zero value and zero time_running, proving
//      enable_on_exec did not arm early.
//   C) same as A but cpu = last online CPU, not just cpu 0, so the result
//      is not CPU-0-specific. The child is pinned (sched_setaffinity) to
//      the target CPU before SIGSTOP: a cpu>=0 task event only counts
//      while the task runs on that exact CPU, so an unpinned child left to
//      the scheduler's discretion would read back a false negative that
//      has nothing to do with enable_on_exec.
//
// Usage: p2_enable_on_exec <path-to-p2_victim>

#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/perf_event.h>

static long perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                             int group_fd, unsigned long flags) {
    return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

struct CountReadFormat { uint64_t value, time_enabled, time_running; };

static pid_t fork_stop(void) {
    pid_t child = fork();
    if (child < 0) { perror("fork"); exit(1); }
    if (child == 0) {
        if (raise(SIGSTOP) != 0) { perror("raise(SIGSTOP)"); _exit(1); }
        _exit(0);  // placeholder; real exec happens after caller decides
    }
    int status;
    if (waitpid(child, &status, WUNTRACED) < 0) { perror("waitpid(stop)"); exit(1); }
    if (!WIFSTOPPED(status)) { fprintf(stderr, "child did not stop\n"); exit(1); }
    return child;
}

static long open_event(pid_t child, int cpu) {
    struct perf_event_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = PERF_COUNT_HW_CACHE_MISSES;
    attr.disabled = 1;
    attr.enable_on_exec = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    long fd = perf_event_open(&attr, child, cpu, -1, 0);
    if (fd < 0) { fprintf(stderr, "perf_event_open(cpu=%d): errno=%d (%s)\n", cpu, errno, strerror(errno)); }
    return fd;
}

// fork+self-stop, but this time the child actually execs the victim once
// resumed -- fork_stop_exec()-shaped, matching src/main.cpp. Pinned via
// sched_setaffinity to `cpu` before the exec: a cpu>=0 task event only
// counts while the task is actually running on that specific CPU, so an
// unpinned child that the scheduler happens to place elsewhere would read
// back a false negative here that has nothing to do with enable_on_exec.
static pid_t fork_stop_exec_victim(const char *victim_path, int cpu) {
    pid_t child = fork();
    if (child < 0) { perror("fork"); exit(1); }
    if (child == 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0) { perror("sched_setaffinity"); _exit(1); }
        if (raise(SIGSTOP) != 0) { perror("raise(SIGSTOP)"); _exit(1); }
        execl(victim_path, victim_path, (char *)NULL);
        perror("execl");
        _exit(127);
    }
    int status;
    if (waitpid(child, &status, WUNTRACED) < 0) { perror("waitpid(stop)"); exit(1); }
    if (!WIFSTOPPED(status)) { fprintf(stderr, "child did not stop\n"); exit(1); }
    return child;
}

static void run_case_exec(const char *label, int cpu, const char *victim_path) {
    pid_t child = fork_stop_exec_victim(victim_path, cpu);
    long fd = open_event(child, cpu);
    if (fd < 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); return; }
    if (kill(child, SIGCONT) != 0) { perror("kill(SIGCONT)"); exit(1); }
    int status;
    waitpid(child, &status, 0);
    struct CountReadFormat c;
    if (read((int)fd, &c, sizeof(c)) != (ssize_t)sizeof(c)) { perror("read"); exit(1); }
    close((int)fd);
    double running_s = (double)c.time_running / 1e9;
    printf("case %s (cpu=%d, exec): value=%llu time_enabled=%llu time_running=%llu (%.4fs)\n",
           label, cpu, (unsigned long long)c.value, (unsigned long long)c.time_enabled,
           (unsigned long long)c.time_running, running_s);
    if (c.value == 0 || c.time_running == 0) {
        printf("  -> SUSPECT: zero value or zero time_running after an exec that should have run ~%s\n",
               "hundreds of ms");
    } else {
        printf("  -> counters armed and non-zero across the execve boundary\n");
    }
}

static void run_case_never_exec(const char *label, int cpu) {
    pid_t child = fork_stop();
    long fd = open_event(child, cpu);
    if (fd < 0) { kill(child, SIGKILL); waitpid(child, NULL, 0); return; }
    // Deliberately do NOT SIGCONT into an exec: kill while still stopped.
    // enable_on_exec must not have armed anything yet.
    if (kill(child, SIGKILL) != 0) { perror("kill(SIGKILL)"); exit(1); }
    waitpid(child, NULL, 0);
    struct CountReadFormat c;
    if (read((int)fd, &c, sizeof(c)) != (ssize_t)sizeof(c)) { perror("read"); exit(1); }
    close((int)fd);
    printf("case %s (cpu=%d, never exec): value=%llu time_enabled=%llu time_running=%llu\n",
           label, cpu, (unsigned long long)c.value, (unsigned long long)c.time_enabled,
           (unsigned long long)c.time_running);
    if (c.value == 0 && c.time_running == 0) {
        printf("  -> PASS: no early arming, counters read zero as expected\n");
    } else {
        printf("  -> FAIL: counters show activity despite the child never reaching execve\n");
    }
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: %s <path-to-p2_victim>\n", argv[0]); return 2; }
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);
    printf("p2_enable_on_exec: online CPUs=%ld\n", nproc);

    run_case_exec("A", 0, argv[1]);
    run_case_never_exec("B", 0);
    run_case_exec("C", (int)(nproc - 1), argv[1]);

    return 0;
}
