CXX      := g++
# These flags are load-bearing — do not remove any without reading the notes.
#   -g                    DWARF debug info, required for addr2line / libdw
#   -fno-omit-frame-pointer  accurate stack unwinding for perf
#   -no-pie               avoids PIE/ASLR address normalization in v1
#   -O1, not -O2          at -O2, GCC auto-vectorizes matrix_bad's inner loop but not
#                         matrix_good's (see results/phase1_matrix.txt §3) -- an asymmetric
#                         confound. -O1 is scalar for both, verified via objdump.
CXXFLAGS := -O1 -g -fno-omit-frame-pointer -no-pie -std=c++17

TARGETS := matrix_good matrix_bad pointer_chase

.PHONY: all clean

all: $(TARGETS)

matrix_good: matrix_good.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

matrix_bad: matrix_bad.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

pointer_chase: pointer_chase.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
