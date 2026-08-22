CXX      := g++
# These flags are load-bearing — do not remove any without reading the notes.
#   -g                    DWARF debug info, required for addr2line / libdw
#   -fno-omit-frame-pointer  accurate stack unwinding for perf
#   -no-pie               avoids PIE/ASLR address normalization in v1
#   -O1, not -O2          at -O2, GCC auto-vectorizes matrix_bad's inner loop but not
#                         matrix_good's (see results/phase1_matrix.txt §3) -- an asymmetric
#                         confound. -O1 is scalar for both, verified via objdump.
CXXFLAGS := -O1 -g -fno-omit-frame-pointer -no-pie -std=c++17

# Gate 7 (docs/GATE7_PLAN.md, U15): the queue benchmarks get their own flag
# sets. -O1 for attribution (Phases 1/3/4, same rationale as above --
# comparable codegen across the padded/unpadded pair). -O2 for the latency
# harness (Phase 5) -- nobody quotes -O1 latency numbers, and the two
# purposes are never merged into one table. Both need -pthread.
QUEUE_ATTR_FLAGS := -O1 -g -fno-omit-frame-pointer -no-pie -std=c++17 -pthread
QUEUE_LAT_FLAGS  := -O2 -g -fno-omit-frame-pointer -no-pie -std=c++17 -pthread

TARGETS := matrix_good matrix_bad pointer_chase
QUEUE_TARGETS := spsc_queue_shared spsc_queue_padded spsc_queue_shared_O2 spsc_queue_padded_O2 \
                 mpmc_queue_shared mpmc_queue_padded mpmc_queue_shared_O2 mpmc_queue_padded_O2

.PHONY: all clean queues

all: $(TARGETS)

queues: $(QUEUE_TARGETS)

matrix_good: matrix_good.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

matrix_bad: matrix_bad.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

pointer_chase: pointer_chase.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

spsc_queue_shared: spsc_queue.cpp
	$(CXX) $(QUEUE_ATTR_FLAGS) -DCACHELENS_PAD_INDICES=0 -o $@ $<

spsc_queue_padded: spsc_queue.cpp
	$(CXX) $(QUEUE_ATTR_FLAGS) -DCACHELENS_PAD_INDICES=1 -o $@ $<

spsc_queue_shared_O2: spsc_queue.cpp
	$(CXX) $(QUEUE_LAT_FLAGS) -DCACHELENS_PAD_INDICES=0 -o $@ $<

spsc_queue_padded_O2: spsc_queue.cpp
	$(CXX) $(QUEUE_LAT_FLAGS) -DCACHELENS_PAD_INDICES=1 -o $@ $<

mpmc_queue_shared: mpmc_queue.cpp
	$(CXX) $(QUEUE_ATTR_FLAGS) -DCACHELENS_PAD_INDICES=0 -o $@ $<

mpmc_queue_padded: mpmc_queue.cpp
	$(CXX) $(QUEUE_ATTR_FLAGS) -DCACHELENS_PAD_INDICES=1 -o $@ $<

mpmc_queue_shared_O2: mpmc_queue.cpp
	$(CXX) $(QUEUE_LAT_FLAGS) -DCACHELENS_PAD_INDICES=0 -o $@ $<

mpmc_queue_padded_O2: mpmc_queue.cpp
	$(CXX) $(QUEUE_LAT_FLAGS) -DCACHELENS_PAD_INDICES=1 -o $@ $<

clean:
	rm -f $(TARGETS) $(QUEUE_TARGETS)
