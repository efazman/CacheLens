// Gate 7 Phase 1 — bounded lock-free SPSC queue, padded/unpadded pair from
// one source. This is the queue named in the pre-registered prediction in
// docs/GATE7_PLAN.md §0: "a bounded lock-free queue whose head and tail
// indices share a cache line, profiled while a producer and a consumer
// thread contend on it."
//
// CACHELENS_PAD_INDICES=0 (default): head and tail are adjacent
// std::atomic<uint64_t> fields in one struct -- 8 bytes apart, inside one
// 64-byte cache line.
// CACHELENS_PAD_INDICES=1: each is alignas(64) *and* explicitly padded out
// to a full 64 bytes each, so the next struct member (buffer) cannot pack
// into the unused tail of the line the way it would with alignas(64) alone
// (alignas only forces a field's start address, not its extent -- an
// actual bug caught during Phase 1 development, see git history).
//
// Iteration history (U18) -- three designs were tried before this one,
// because Phase 1's own exit criterion is "no wall-clock delta means
// iterate, there is nothing downstream worth building":
//
//   1. Unconditionally reload both atomics every push()/pop() call.
//      Padded and shared measured within 2% of each other: this creates
//      heavy *true* sharing (a real cross-core read every call, needed
//      regardless of layout) that swamps the marginal *false*-sharing
//      cost of adjacency.
//   2. Cache each side's last-observed view of the other index locally,
//      refilling on a cache miss, publishing/consuming one item at a
//      time. Instrumented refill counts showed why this also failed:
//      with producer and consumer doing near-identical per-item work,
//      whichever side is even slightly faster exhausts its cached view
//      and refills on nearly every call (75% observed) regardless of
//      buffer capacity -- degenerating back into design 1's problem.
//   3. Batch the publish/consume granularity (the LMAX Disruptor's
//      technique) so the shared atomics are touched only once per 256
//      items. This did reduce absolute runtime by >10x, but also diluted
//      whatever contention cost exists to a negligible fraction of the
//      (now much smaller) total -- padded and shared were indistinguishable
//      again, for the opposite reason: touches got too *rare* to matter.
//
// The design below (Vyukov's bounded-queue technique, degenerated to one
// writer per index since SPSC has no CAS race to resolve) is what actually
// produces a clean, repeatable effect: readiness (is there something to
// pop?) and fullness (is there room to push?) are decided by a per-slot
// sequence number stored *with the data*, not by reading the other
// thread's index. That means `head` is written only by the consumer and
// read only by the consumer; `tail` is written only by the producer and
// read only by the producer -- structurally identical to the two
// independent, never-cross-read counters in probes/p4_false_sharing.cpp,
// which is exactly what made that probe's effect so clean. This is not a
// contrived simplification: it is the same technique real lock-free queues
// (Vyukov's MPMC design, boost::lockfree::queue) use for exactly this
// reason, applied here to the single-writer-per-index case.
//
// U10's decision: noinline on push/pop, so samples land inside the
// operation instead of at the (would-be-inlined-at-O1) call site. This is
// a benchmark-shaping decision made for attribution, not a performance
// choice -- disclosed here per the Gate 7 implementation plan.
//
// U16's decision: producer pinned to CPU 0, consumer to CPU 1 -- distinct
// physical cores (core_id 0 and 1 on this machine), not SMT siblings.
//
// U17 (determinism): a fixed item count, not a deadline, so every run
// performs identical total work regardless of scheduling.

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

struct Cell {
    std::atomic<uint64_t> sequence;
    uint64_t data;
};

#if CACHELENS_PAD_INDICES
struct Queue {
    Cell buffer[kCapacity];
    alignas(64) std::atomic<uint64_t> tail{0};  // producer-owned: written and read only by push()
    char pad_after_tail[64 - sizeof(std::atomic<uint64_t>)];
    alignas(64) std::atomic<uint64_t> head{0};  // consumer-owned: written and read only by pop()
    char pad_after_head[64 - sizeof(std::atomic<uint64_t>)];
};
#else
struct Queue {
    Cell buffer[kCapacity];
    std::atomic<uint64_t> tail{0};
    std::atomic<uint64_t> head{0};
};
#endif

static_assert(CACHELENS_PAD_INDICES == 1
                  ? (offsetof(Queue, head) - offsetof(Queue, tail)) >= 64
                  : (offsetof(Queue, head) - offsetof(Queue, tail)) < 64,
              "Queue head/tail layout does not match the CACHELENS_PAD_INDICES build requested");

void queue_init(Queue& q) {
    for (uint64_t i = 0; i < kCapacity; ++i) q.buffer[i].sequence.store(i, std::memory_order_relaxed);
}

void pin_to(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        std::perror("sched_setaffinity");
        std::exit(1);
    }
}

// `tail` is this function's own private reservation counter -- no other
// thread ever writes or reads it. Fullness is decided by the slot's own
// sequence number, not by reading `head`.
__attribute__((noinline)) void push(Queue& q, uint64_t value) {
    uint64_t pos = q.tail.load(std::memory_order_relaxed);
    Cell& cell = q.buffer[pos & kMask];
    uint64_t seq = cell.sequence.load(std::memory_order_acquire);
    while (seq != pos) {  // slot not yet freed by the consumer
        __builtin_ia32_pause();
        seq = cell.sequence.load(std::memory_order_acquire);
    }
    cell.data = value;
    cell.sequence.store(pos + 1, std::memory_order_release);
    q.tail.store(pos + 1, std::memory_order_relaxed);
}

// `head` is this function's own private reservation counter -- no other
// thread ever writes or reads it. Readiness is decided by the slot's own
// sequence number, not by reading `tail`.
__attribute__((noinline)) uint64_t pop(Queue& q) {
    uint64_t pos = q.head.load(std::memory_order_relaxed);
    Cell& cell = q.buffer[pos & kMask];
    uint64_t seq = cell.sequence.load(std::memory_order_acquire);
    while (seq != pos + 1) {  // producer hasn't published this slot yet
        __builtin_ia32_pause();
        seq = cell.sequence.load(std::memory_order_acquire);
    }
    uint64_t value = cell.data;
    cell.sequence.store(pos + kCapacity, std::memory_order_release);
    q.head.store(pos + 1, std::memory_order_relaxed);
    return value;
}

void producer(Queue* q) {
    pin_to(0);
    for (uint64_t i = 0; i < kTotalItems; ++i) push(*q, i);
}

void consumer(Queue* q, uint64_t* checksum_out) {
    pin_to(1);
    uint64_t checksum = 0;
    for (uint64_t i = 0; i < kTotalItems; ++i) checksum += pop(*q);
    *checksum_out = checksum;
}

}  // namespace

int main() {
    std::printf("spsc_queue: build=%s, capacity=%llu, items=%llu, "
                "producer pinned to CPU 0, consumer pinned to CPU 1\n",
                CACHELENS_PAD_INDICES ? "padded" : "shared",
                static_cast<unsigned long long>(kCapacity),
                static_cast<unsigned long long>(kTotalItems));

    Queue* q = new Queue();
    queue_init(*q);
    uint64_t checksum = 0;

    auto t0 = std::chrono::steady_clock::now();
    std::thread producer_thread(producer, q);
    std::thread consumer_thread(consumer, q, &checksum);
    producer_thread.join();
    consumer_thread.join();
    auto t1 = std::chrono::steady_clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    // sum(0..N-1) = N*(N-1)/2, exact in uint64_t for N=1e8 (fits well
    // under 2^64), so this is an exact equality check, not a tolerance.
    uint64_t expected = kTotalItems * (kTotalItems - 1) / 2;
    std::printf("elapsed: %.6fs  checksum=%llu  expected=%llu  %s\n", secs,
                static_cast<unsigned long long>(checksum),
                static_cast<unsigned long long>(expected),
                checksum == expected ? "OK" : "MISMATCH");

    delete q;
    return checksum == expected ? 0 : 1;
}
