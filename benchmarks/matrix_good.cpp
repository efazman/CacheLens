/*
 * matrix_good.cpp — Row-major matrix multiply (cache-friendly).
 *
 * Inner loop traverses columns of B in row-major order, so consecutive
 * memory accesses hit the same cache line.  Compare with matrix_bad.cpp.
 *
 * Build: g++ -O1 -g -fno-omit-frame-pointer -no-pie -std=c++17 -o matrix_good matrix_good.cpp
 * (matches matrix_bad.cpp's -O1 -- see that file's note on the -O2 vectorization asymmetry)
 */

#include <cstdlib>
#include <cstdio>
#include <cstring>

static constexpr int N = 2400;

// Heap-allocate to keep stack small and avoid BSS-section weirdness
// that can confuse some perf setups.
static double* A;
static double* B;
static double* C;

inline double& mat(double* m, int r, int c) { return m[r * N + c]; }

static void init() {
    A = new double[N * N];
    B = new double[N * N];
    C = new double[N * N];
    for (int i = 0; i < N * N; ++i) {
        A[i] = static_cast<double>(std::rand()) / RAND_MAX;
        B[i] = static_cast<double>(std::rand()) / RAND_MAX;
    }
    std::memset(C, 0, sizeof(double) * N * N);
}

static void multiply_good() {
    // i-k-j order: inner loop varies j → B[k][j] and C[i][j] are both
    // accessed sequentially along rows → excellent spatial locality.
    for (int i = 0; i < N; ++i) {
        for (int k = 0; k < N; ++k) {
            double a_ik = mat(A, i, k);
            for (int j = 0; j < N; ++j) {  // <-- hotspot here, but few cache misses
                mat(C, i, j) += a_ik * mat(B, k, j);
            }
        }
    }
}

int main() {
    init();
    multiply_good();
    // Prevent dead-code elimination
    printf("C[0][0] = %f\n", mat(C, 0, 0));
    delete[] A;
    delete[] B;
    delete[] C;
    return 0;
}
