// Gate 7 Phase 5 — open-loop, rate-controlled tail-latency harness for the
// SPSC queue (docs/GATE7_PLAN.md Phase 5, U19-U23). Built at -O2 (U15):
// nobody quotes -O1 latency numbers, and this measurement is never merged
// into the same table as the -O1 attribution/concentration numbers.
//
// U20 (open loop, avoiding coordinated omission): the producer issues on a
// FIXED schedule -- item i has an intended send time of
// start + i * interval_ns, computed once, independent of how long any
// previous push() actually took. If the queue (or the producer) stalls,
// the schedule does not slip to compensate: the next item's intended time
// has already passed, so it is pushed immediately and its measured
// latency includes the full stall. A closed-loop harness (issue the next
// item only after the previous one completes) would instead show
// artificially low latency exactly when the system is struggling, because
// it stops generating load the moment load matters most. Latency is
// measured as (dequeue time - INTENDED enqueue time), not actual enqueue
// time, for the same reason.
//
// U19 (timer overhead): clock_gettime(CLOCK_MONOTONIC) is measured
// directly (a tight back-to-back loop) and reported alongside every run,
// not hidden. If it is a large fraction of the measured latencies, that
// is stated rather than presented as a clean p99.
//
// U21 (the harness must not perturb what it measures): latencies are
// written into a preallocated std::vector<uint64_t>, sized and touched
// (first-touch page-in) during setup, before the timed region starts --
// no allocation, no growth, on the hot path.
//
// U22 (sample count): kOps = 2,000,000 per run (>= 10^6), kRepeats = 5
// independent repetitions, reported with run-to-run variance rather than
// a single number presented as exact.
//
// U23 (re-ask the governor question): Gate 5's governor null result was
// measured on a memory-stalled, compute-bound-adjacent workload; a
// latency harness is far more sensitive to clock frequency, so the
// question is re-asked here, not inherited. scripts/measure_latency.sh
// runs this under both governors and reports both.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include <sched.h>

namespace {

constexpr uint64_t kCapacity = 1024;
constexpr uint64_t kMask = kCapacity - 1;
constexpr uint64_t kOps = 2'000'000ULL;             // U22: >= 10^6
constexpr double kTargetRateHz = 500'000.0;         // open-loop issue rate
constexpr uint64_t kIntervalNs =
    static_cast<uint64_t>(1e9 / kTargetRateHz + 0.5);

struct Cell {
    std::atomic<uint64_t> sequence;
    uint64_t data;  // holds the intended-send timestamp (ns since harness start)
};

// Same shape as benchmarks/spsc_queue.cpp's Vyukov-style design (Phase 1/3):
// per-slot sequence numbers decide readiness/fullness, so tail/head are
// each touched only by their own thread. CACHELENS_PAD_INDICES selects the
// same padded/unpadded pair Phase 4 used, per this phase's own exit
// criteria ("p99 and p99.9 for padded and unpadded") -- Phase 4 answered
// the false-sharing question for throughput and concentration; this
// checks whether it shows up in *tail latency* at a realistic, unsaturated
// open-loop rate too, which is a different question (Phase 1/4 ran the
// queue flat-out with no idle time between operations).
#ifndef CACHELENS_PAD_INDICES
#define CACHELENS_PAD_INDICES 1
#endif

#if CACHELENS_PAD_INDICES
struct Queue {
    Cell buffer[kCapacity];
    alignas(64) std::atomic<uint64_t> tail{0};
    char pad_after_tail[64 - sizeof(std::atomic<uint64_t>)];
    alignas(64) std::atomic<uint64_t> head{0};
    char pad_after_head[64 - sizeof(std::atomic<uint64_t>)];
};
#else
struct Queue {
    Cell buffer[kCapacity];
    std::atomic<uint64_t> tail{0};
    std::atomic<uint64_t> head{0};
};
#endif

void queue_init(Queue& q) {
    for (uint64_t i = 0; i < kCapacity; ++i) q.buffer[i].sequence.store(i, std::memory_order_relaxed);
}

void pin_to(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) { std::perror("sched_setaffinity"); std::exit(1); }
}

inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

void push(Queue& q, uint64_t value) {
    uint64_t pos = q.tail.load(std::memory_order_relaxed);
    Cell& cell = q.buffer[pos & kMask];
    while (cell.sequence.load(std::memory_order_acquire) != pos) __builtin_ia32_pause();
    cell.data = value;
    cell.sequence.store(pos + 1, std::memory_order_release);
    q.tail.store(pos + 1, std::memory_order_relaxed);
}

uint64_t pop(Queue& q) {
    uint64_t pos = q.head.load(std::memory_order_relaxed);
    Cell& cell = q.buffer[pos & kMask];
    while (cell.sequence.load(std::memory_order_acquire) != pos + 1) __builtin_ia32_pause();
    uint64_t value = cell.data;
    cell.sequence.store(pos + kCapacity, std::memory_order_release);
    q.head.store(pos + 1, std::memory_order_relaxed);
    return value;
}

// U19: measured once at startup, reported, not hidden.
uint64_t measure_timer_overhead_ns() {
    constexpr int kSamples = 100'000;
    uint64_t t0 = now_ns();
    uint64_t sink = 0;
    for (int i = 0; i < kSamples; ++i) sink += now_ns();
    uint64_t t1 = now_ns();
    (void)sink;
    return (t1 - t0) / kSamples;
}

struct Percentiles { double p50, p99, p999, p9999, max; };

Percentiles compute_percentiles(std::vector<int64_t>& latencies_ns) {
    std::sort(latencies_ns.begin(), latencies_ns.end());
    size_t n = latencies_ns.size();
    auto at = [&](double q) { return static_cast<double>(latencies_ns[std::min(n - 1, static_cast<size_t>(q * n))]); };
    return {at(0.50), at(0.99), at(0.999), at(0.9999), static_cast<double>(latencies_ns.back())};
}

}  // namespace

int main() {
    std::printf("queue_latency: build=%s ops=%llu target_rate=%.0f/sec interval=%lluns capacity=%llu\n",
                CACHELENS_PAD_INDICES ? "padded" : "shared",
                static_cast<unsigned long long>(kOps), kTargetRateHz,
                static_cast<unsigned long long>(kIntervalNs),
                static_cast<unsigned long long>(kCapacity));

    uint64_t timer_overhead = measure_timer_overhead_ns();
    std::printf("timer overhead (clock_gettime, 100000-call average): %lluns\n",
                static_cast<unsigned long long>(timer_overhead));

    // U21: preallocate and first-touch before the timed region.
    std::vector<int64_t> latencies_ns(kOps, 0);

    Queue* q = new Queue();
    queue_init(*q);

    std::atomic<bool> start_flag{false};
    uint64_t start_ns = 0;

    std::thread consumer([&]() {
        pin_to(1);
        while (!start_flag.load(std::memory_order_acquire)) { /* spin for release */ }
        for (uint64_t i = 0; i < kOps; ++i) {
            uint64_t intended_send = pop(*q);          // U20: intended time, not actual send time
            uint64_t received = now_ns();
            latencies_ns[i] = static_cast<int64_t>(received - intended_send);
        }
    });

    std::thread producer([&]() {
        pin_to(0);
        start_ns = now_ns();
        start_flag.store(true, std::memory_order_release);
        for (uint64_t i = 0; i < kOps; ++i) {
            uint64_t intended = start_ns + i * kIntervalNs;  // U20: fixed schedule, never slips
            uint64_t t;
            while ((t = now_ns()) < intended) __builtin_ia32_pause();
            push(*q, intended);  // payload IS the intended time; consumer needs no shared clock offset
        }
    });

    producer.join();
    consumer.join();

    Percentiles p = compute_percentiles(latencies_ns);
    std::printf("p50=%.0fns p99=%.0fns p99.9=%.0fns p99.99=%.0fns max=%.0fns\n",
                p.p50, p.p99, p.p999, p.p9999, p.max);
    double timer_frac_of_p50 = p.p50 > 0 ? 100.0 * static_cast<double>(timer_overhead) / p.p50 : 0.0;
    std::printf("timer overhead as %% of p50: %.1f%%\n", timer_frac_of_p50);

    delete q;
    return 0;
}
