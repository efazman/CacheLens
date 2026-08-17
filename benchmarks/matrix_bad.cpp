/*
 * matrix_bad.cpp — Naive i-j-k matrix multiply (cache-unfriendly).
 *
 * Inner loop iterates k, accessing B[k][j] with stride N — every access
 * jumps to a different cache line.  This is the classic cache-thrashing
 * pattern.  Compare with matrix_good.cpp (i-k-j order).
 *
 * Build: g++ -O2 -g -fno-omit-frame-pointer -no-pie -std=c++17 -o matrix_bad matrix_bad.cpp
 */

#include <cstdlib>
#include <cstdio>
#include <cstring>

static constexpr int N = 2400;

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

static void multiply_bad() {
    // i-j-k order: inner loop varies k → B[k][j] strides through column
    // with step = N doubles = N*8 bytes.  For N=1024, each B access is
    // 8 KiB apart — guaranteed cache line miss on every iteration.
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            double sum = 0.0;
            for (int k = 0; k < N; ++k) {  // <-- hotspot: column-stride on B
                sum += mat(A, i, k) * mat(B, k, j);
            }
            mat(C, i, j) = sum;
        }
    }
}

int main() {
    init();
    multiply_bad();
    printf("C[0][0] = %f\n", mat(C, 0, 0));
    delete[] A;
    delete[] B;
    delete[] C;
    return 0;
}
