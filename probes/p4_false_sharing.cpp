// Gate 7 probe P4 (U4) -- THE GATING PROBE.
//
// Does PERF_COUNT_HW_CACHE_MISSES / PERF_COUNT_HW_CACHE_REFERENCES (the
// "generalized" hardware cache events cachelens already uses) see anything
// at all when two threads on two distinct physical cores false-share a
// cache line? A coherence ping-pong is typically satisfied by a cross-core
// snoop (an L3 hit / HITM), not an LLC miss -- Intel exposes the specific
// event (mem_load_l3_hit_retired.xsnp_hitm); the AMD equivalent lives in
// IBS or raw ls_any_fills_from_sys.* codes, neither of which this project
// has verified access to on Zen 4. This probe measures wall-clock time
// directly (the ground truth) and lets `perf stat -e cache-misses,...`
// wrapped around it answer the question independently.
//
// Two threads, each incrementing its own uint64_t counter
// kIters times, pinned via sched_setaffinity to CPU 0 and CPU 1 -- distinct
// physical cores on this machine (core_id 0 and 1; see
// /sys/devices/system/cpu/cpu*/topology/core_id), not SMT siblings of the
// same core (which would be CPU 0 and CPU 6 here).
//
// Compile-time switch CACHELENS_PAD_INDICES selects which layout is built:
//   0 (default): both counters in one struct, adjacent uint64_t fields --
//      12 bytes apart, same 64-byte line, true false sharing.
//   1: each counter is alignas(64), forced onto its own line.
// Build both from this one source so "the two differ in exactly one thing"
// is a property of the build system, not a claim about two hand-written
// files (matches the padded/unpadded discipline used for the queue
// benchmark pair in Phase 1).
//
// Usage: p4_shared / p4_padded (no args). Prints wall-clock time and a
// checksum (so the increments are not optimized away) to stdout; intended
// to be wrapped in `perf stat -r 5 -e cache-misses,cache-references`.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <sched.h>

#ifndef CACHELENS_PAD_INDICES
#define CACHELENS_PAD_INDICES 0
#endif

#if CACHELENS_PAD_INDICES
struct Counters {
    alignas(64) std::atomic<uint64_t> a{0};
    alignas(64) std::atomic<uint64_t> b{0};
};
#else
struct Counters {
    std::atomic<uint64_t> a{0};
    std::atomic<uint64_t> b{0};
};
#endif

static_assert(CACHELENS_PAD_INDICES == 1 ? (offsetof(Counters, b) - offsetof(Counters, a)) >= 64
                                          : (offsetof(Counters, b) - offsetof(Counters, a)) < 64,
              "Counters layout does not match the CACHELENS_PAD_INDICES build requested");

constexpr uint64_t kIters = 200'000'000ULL;

void pin_to(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::perror("sched_setaffinity");
        std::exit(1);
    }
}

void worker_a(Counters* c) {
    pin_to(0);
    for (uint64_t i = 0; i < kIters; ++i) {
        c->a.fetch_add(1, std::memory_order_relaxed);
    }
}

void worker_b(Counters* c) {
    pin_to(1);
    for (uint64_t i = 0; i < kIters; ++i) {
        c->b.fetch_add(1, std::memory_order_relaxed);
    }
}

int main() {
    printf("p4_false_sharing: build=%s, pinned to CPU 0 (physical core 0) and CPU 1 "
           "(physical core 1), kIters=%llu each\n",
           CACHELENS_PAD_INDICES ? "padded" : "shared",
           static_cast<unsigned long long>(kIters));
    Counters c;
    auto t0 = std::chrono::steady_clock::now();
    std::thread ta(worker_a, &c);
    std::thread tb(worker_b, &c);
    ta.join();
    tb.join();
    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    uint64_t checksum = c.a.load() + c.b.load();
    printf("elapsed: %.6fs  checksum=%llu\n", secs, static_cast<unsigned long long>(checksum));
    return checksum == 2 * kIters ? 0 : 1;
}
