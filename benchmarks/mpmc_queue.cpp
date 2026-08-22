// Gate 7 Phase 1 — bounded lock-free MPMC queue, padded/unpadded pair from
// one source (Vyukov's per-slot-sequence design). Needed for Phase 5's
// contended latency case; the false-sharing prediction in
// docs/GATE7_PLAN.md §0 itself is tested against spsc_queue.cpp (exactly
// "a producer and a consumer thread"), not this one. See U25: this project
// does not build MPMC-for-its-own-sake inside cachelens, but a queue
// benchmark under contention is exactly what a latency harness for a
// realistic multithreaded target needs, hence it exists here.
//
// CACHELENS_PAD_INDICES=0 (default): enqueue_pos and dequeue_pos are
// adjacent std::atomic<uint64_t> fields, inside one 64-byte cache line.
// CACHELENS_PAD_INDICES=1: each is alignas(64), forced onto its own line.
//
// Unlike spsc_queue.cpp, this queue's push/pop genuinely need a CAS (a
// mask, and a fence): multiple producers race to reserve a slot via
// compare_exchange on enqueue_pos, multiple consumers race the same way on
// dequeue_pos; per-slot sequence numbers (not just head/tail) are what
// make a slot's producer-writes-then-consumer-reads handoff safe without a
// lock. This is the "CAS, mask, fence" body U9 says is not the 7-
// instruction loop the original skid characterization was measured
// against.
//
// noinline on push/pop (U10) and CPU pinning (U16, generalized to 4
// distinct physical cores here since MPMC needs more than 2 threads) match
// spsc_queue.cpp's disclosed benchmark-shaping decisions. Fixed total item
// count (U17): every run performs identical total work.

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

#ifndef CACHELENS_PAD_INDICES
#define CACHELENS_PAD_INDICES 0
#endif

namespace {

constexpr uint64_t kCapacity = 1024;  // power of two
constexpr uint64_t kMask = kCapacity - 1;
constexpr uint64_t kTotalItems = 100'000'000ULL;
constexpr int kProducers = 2;
constexpr int kConsumers = 2;
static_assert(kTotalItems % kProducers == 0, "kTotalItems must divide evenly across producers");

struct Cell {
    std::atomic<uint64_t> sequence;
    uint64_t data;
};

#if CACHELENS_PAD_INDICES
struct Queue {
    Cell buffer[kCapacity];
    alignas(64) std::atomic<uint64_t> enqueue_pos{0};
    alignas(64) std::atomic<uint64_t> dequeue_pos{0};
};
#else
struct Queue {
    Cell buffer[kCapacity];
    std::atomic<uint64_t> enqueue_pos{0};
    std::atomic<uint64_t> dequeue_pos{0};
};
#endif

static_assert(CACHELENS_PAD_INDICES == 1
                  ? (offsetof(Queue, dequeue_pos) - offsetof(Queue, enqueue_pos)) >= 64
                  : (offsetof(Queue, dequeue_pos) - offsetof(Queue, enqueue_pos)) < 64,
              "Queue enqueue_pos/dequeue_pos layout does not match the CACHELENS_PAD_INDICES build requested");

void pin_to(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::perror("sched_setaffinity");
        std::exit(1);
    }
}

void queue_init(Queue& q) {
    for (uint64_t i = 0; i < kCapacity; ++i) q.buffer[i].sequence.store(i, std::memory_order_relaxed);
}

__attribute__((noinline)) void push(Queue& q, uint64_t data) {
    uint64_t pos = q.enqueue_pos.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = q.buffer[pos & kMask];
        uint64_t seq = cell.sequence.load(std::memory_order_acquire);
        int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);
        if (diff == 0) {
            if (q.enqueue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                cell.data = data;
                cell.sequence.store(pos + 1, std::memory_order_release);
                return;
            }
        } else if (diff < 0) {
            __builtin_ia32_pause();  // full: wait for a consumer to free a slot
            pos = q.enqueue_pos.load(std::memory_order_relaxed);
        } else {
            pos = q.enqueue_pos.load(std::memory_order_relaxed);
        }
    }
}

__attribute__((noinline)) uint64_t pop(Queue& q) {
    uint64_t pos = q.dequeue_pos.load(std::memory_order_relaxed);
    for (;;) {
        Cell& cell = q.buffer[pos & kMask];
        uint64_t seq = cell.sequence.load(std::memory_order_acquire);
        int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos + 1);
        if (diff == 0) {
            if (q.dequeue_pos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                uint64_t data = cell.data;
                cell.sequence.store(pos + kCapacity, std::memory_order_release);
                return data;
            }
        } else if (diff < 0) {
            __builtin_ia32_pause();  // empty: wait for a producer to fill a slot
            pos = q.dequeue_pos.load(std::memory_order_relaxed);
        } else {
            pos = q.dequeue_pos.load(std::memory_order_relaxed);
        }
    }
}

void producer(Queue* q, int cpu, uint64_t lo, uint64_t hi) {
    pin_to(cpu);
    for (uint64_t i = lo; i < hi; ++i) push(*q, i);
}

void consumer(Queue* q, int cpu, uint64_t count, uint64_t* checksum_out) {
    pin_to(cpu);
    uint64_t checksum = 0;
    for (uint64_t i = 0; i < count; ++i) checksum += pop(*q);
    *checksum_out = checksum;
}

}  // namespace

int main() {
    std::printf("mpmc_queue: build=%s, capacity=%llu, items=%llu, producers=%d (CPUs 0..%d), "
                "consumers=%d (CPUs %d..%d)\n",
                CACHELENS_PAD_INDICES ? "padded" : "shared",
                static_cast<unsigned long long>(kCapacity),
                static_cast<unsigned long long>(kTotalItems), kProducers, kProducers - 1,
                kConsumers, kProducers, kProducers + kConsumers - 1);

    Queue* q = new Queue();
    queue_init(*q);

    uint64_t per_producer = kTotalItems / kProducers;
    uint64_t per_consumer = kTotalItems / kConsumers;
    static_assert(kTotalItems % kConsumers == 0, "kTotalItems must divide evenly across consumers");

    std::vector<uint64_t> partial_sums(kConsumers, 0);
    std::vector<std::thread> threads;

    auto t0 = std::chrono::steady_clock::now();
    for (int p = 0; p < kProducers; ++p) {
        threads.emplace_back(producer, q, p, p * per_producer, (p + 1) * per_producer);
    }
    for (int c = 0; c < kConsumers; ++c) {
        threads.emplace_back(consumer, q, kProducers + c, per_consumer, &partial_sums[c]);
    }
    for (auto& t : threads) t.join();
    auto t1 = std::chrono::steady_clock::now();

    uint64_t checksum = 0;
    for (uint64_t s : partial_sums) checksum += s;

    double secs = std::chrono::duration<double>(t1 - t0).count();
    uint64_t expected = kTotalItems * (kTotalItems - 1) / 2;
    std::printf("elapsed: %.6fs  checksum=%llu  expected=%llu  %s\n", secs,
                static_cast<unsigned long long>(checksum),
                static_cast<unsigned long long>(expected),
                checksum == expected ? "OK" : "MISMATCH");

    delete q;
    return checksum == expected ? 0 : 1;
}
