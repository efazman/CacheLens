// Trivial victim program for probe P2 (U2): touches a buffer larger than
// L2 in a pointer-chase pattern for a fixed number of iterations, so the
// parent's cache-miss counter has something non-trivial to read if (and
// only if) it was actually armed across this program's execve.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const size_t n = 4 * 1024 * 1024 / sizeof(uint64_t);  // 4 MiB, past L2
    uint64_t *buf = malloc(n * sizeof(uint64_t));
    if (!buf) return 1;
    for (size_t i = 0; i < n; ++i) buf[i] = (i + 1) % n;
    uint64_t idx = 0;
    for (int pass = 0; pass < 200; ++pass) {
        for (size_t i = 0; i < n; ++i) idx = buf[idx];
    }
    printf("victim done, idx=%llu\n", (unsigned long long)idx);
    free(buf);
    return 0;
}
